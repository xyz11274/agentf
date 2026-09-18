import com.fasterxml.jackson.databind.JsonNode;
import com.fasterxml.jackson.databind.ObjectMapper;
import com.fasterxml.jackson.databind.node.ObjectNode;
import java.io.ByteArrayOutputStream;
import java.net.Proxy;
import java.net.ProxySelector;
import java.net.SocketAddress;
import java.net.URI;
import java.net.http.HttpClient;
import java.net.http.HttpRequest;
import java.net.http.HttpResponse;
import java.nio.ByteBuffer;
import java.time.Duration;
import java.util.List;
import java.util.UUID;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.Flow;
import java.util.concurrent.TimeUnit;

/** Native Java transport for the entire L0-L3 protocol. No AgentFlow dependency. */
public final class Evolution {
    @FunctionalInterface
    public interface Receiver { void accept(JsonNode input) throws Exception; }
    @FunctionalInterface
    public interface CallExecutor { JsonNode execute(JsonNode input) throws Exception; }

    /** Full observation outcome. {@link #call} keeps the output-only API. */
    public static final class CallResult {
        public final JsonNode output, prepared, receipt;
        public final boolean executed;
        private CallResult(JsonNode output, JsonNode prepared, JsonNode receipt, boolean executed) {
            this.output = output; this.prepared = prepared; this.receipt = receipt; this.executed = executed;
        }
    }
    /** Full loop outcome for hosts that need both decision receipts. */
    public static final class LoopResult {
        public final boolean allowed;
        public final String decision;
        public final JsonNode prepared, receipt;
        private LoopResult(boolean allowed, String decision, JsonNode prepared, JsonNode receipt) {
            this.allowed = allowed; this.decision = decision; this.prepared = prepared; this.receipt = receipt;
        }
    }


    /** Call on a valid host thread. An asynchronous dispatch may return immediately;
     * normal run-end points, not this callback, determine completion. */
    public static boolean evaluate(Receiver receive) throws Failure {
        if (!"1".equals(System.getenv("TRAJECTORY_EVOLUTION_EVALUATION_ENTRY"))) return false;
        Evolution client = new Evolution(System.getenv("TRAJECTORY_EVOLUTION_TRIAL_URL"),
            System.getenv("TRAJECTORY_EVOLUTION_TRIAL_TOKEN"), 5000, 2097152, 0);
        String consumer = UUID.randomUUID().toString();
        try {
            for (int index = 0;;) {
                ObjectNode request = JSON.createObjectNode().put("consumer", consumer).put("index", index);
                JsonNode packet = client.invoke("evaluation.next", request);
                String state = packet.path("state").asText();
                if (state.equals("done")) return true;
                if (state.equals("waiting")) { Thread.sleep(100); continue; }
                if (!state.equals("input") || packet.path("index").asInt(-1) != index) throw new Failure("evaluation_entry_failed", 0);
                receive.accept(packet.get("input"));
                ++index;
            }
        } catch (Exception error) {
            try { client.invoke("evaluation.fail", JSON.createObjectNode().put("consumer", consumer)); } catch (Exception ignored) { }
            if (error instanceof InterruptedException) Thread.currentThread().interrupt();
            throw new Failure("evaluation_entry_failed", 0);
        }
    }
    public static final class Failure extends Exception {
        public final int status;
        Failure(String code, int status) { super(code); this.status = status; }
    }
    private static final ObjectMapper JSON = new ObjectMapper();
    private final URI endpoint;
    private final String token;
    private final int timeoutMs, maxBytes, retries;
    private final HttpClient http;
    private final java.util.Set<String> pointGaps = new java.util.HashSet<>();
    private boolean pointGapsFull;

    private synchronized boolean pointHasGap(ObjectNode fields) {
        return pointGapsFull || pointGaps.contains(fields.path("ref").path("run").asText());
    }
    private synchronized JsonNode pointObserved(ObjectNode fields, JsonNode result) {
        String run = fields == null ? "" : fields.path("ref").path("run").asText();
        if (!run.isEmpty() && run.length() <= 256) {
            if (result.path("degraded").asBoolean()) {
                if (pointGaps.size() < 1024) pointGaps.add(run); else pointGapsFull = true;
            } else if (result.path("closed").asBoolean() || result.path("run_closed").asBoolean()) pointGaps.remove(run);
        }
        return result;
    }

    public Evolution(String url, String token, int timeoutMs, int maxResponseBytes, int retries) throws Failure {
        String trialUrl = System.getenv("TRAJECTORY_EVOLUTION_TRIAL_URL"), trialToken = System.getenv("TRAJECTORY_EVOLUTION_TRIAL_TOKEN");
        if (trialUrl != null && !trialUrl.isEmpty() && trialToken != null && !trialToken.isEmpty()) { url = trialUrl; token = trialToken; }
        URI uri;
        try { uri = URI.create(url); } catch (Exception ignored) { throw new Failure("https_or_loopback_url_required", 0); }
        String host = uri.getHost();
        boolean local = "localhost".equals(host) || "127.0.0.1".equals(host) || "[::1]".equals(host);
        if (host == null || uri.getRawUserInfo() != null || uri.getRawQuery() != null || uri.getRawFragment() != null
            || !("".equals(uri.getPath()) || "/".equals(uri.getPath()))
            || !("https".equals(uri.getScheme()) || ("http".equals(uri.getScheme()) && local)))
            throw new Failure("https_or_loopback_url_required", 0);
        if (token == null || token.length() < 24 || token.chars().anyMatch(c -> c < 33 || c > 126)) throw new Failure("token_invalid", 0);
        if (timeoutMs < 100 || timeoutMs > 300000 || maxResponseBytes < 1024 || maxResponseBytes > 16777216 || retries < 0 || retries > 3)
            throw new Failure("client_limits_invalid", 0);
        this.endpoint = uri.resolve("/v1/invoke"); this.token = token;
        this.timeoutMs = timeoutMs; this.maxBytes = maxResponseBytes; this.retries = retries;
        this.http = HttpClient.newBuilder().version(HttpClient.Version.HTTP_1_1)
            .followRedirects(HttpClient.Redirect.NEVER).connectTimeout(Duration.ofMillis(timeoutMs))
            .proxy(new ProxySelector() {
                @Override public List<Proxy> select(URI uri) { return List.of(Proxy.NO_PROXY); }
                @Override public void connectFailed(URI uri, SocketAddress address, java.io.IOException error) { }
            }).build();
    }

    public JsonNode invoke(String operation, ObjectNode fields) throws Failure {
        return invoke(operation, fields, UUID.randomUUID().toString());
    }
    public JsonNode invoke(String operation, ObjectNode fields, String requestId) throws Failure {
        if (fields == null) throw new Failure("object_required", 0);
        ObjectNode request = fields.deepCopy(); request.put("operation", operation); request.put("request_id", requestId);
        byte[] body;
        try { body = JSON.writeValueAsBytes(request); } catch (Exception ignored) { throw new Failure("request_invalid", 0); }
        if (body.length > 2097152) throw new Failure("request_too_large", 0);
        for (int attempt = 0; ; ++attempt) {
            try { return once(body, timeoutMs); }
            catch (Failure error) { if (attempt >= retries || (error.status > 0 && error.status < 500)) throw error; }
        }
    }
    /** Wrap one existing JSON call. The business executor is invoked at most once. */
    public JsonNode call(ObjectNode fields, CallExecutor execute) throws Exception {
        return callWithReceipt(fields, execute, 20).output;
    }
    public JsonNode call(ObjectNode fields, CallExecutor execute, int budgetMs) throws Exception {
        return callWithReceipt(fields, execute, budgetMs).output;
    }
    public CallResult callWithReceipt(ObjectNode fields, CallExecutor execute, int budgetMs) throws Exception {
        if (fields == null || execute == null) throw new IllegalArgumentException("fields_and_executor_required");
        ObjectNode before = fields.deepCopy(); before.put("phase", "before");
        JsonNode prepared = point(before, budgetMs);
        JsonNode actual = prepared.has("input") ? prepared.get("input") : fields.get("input");
        ObjectNode identity = finishIdentity(fields, prepared);
        String kind = fields.path("kind").asText("custom");
        if ("reject".equals(prepared.path("decision").asText())) {
            String reason = prepared.path("reason").asText("policy_rejected");
            ObjectNode output = JSON.createObjectNode().put("status", "rejected").put("reason", reason).put("error", reason);
            ObjectNode after = identity.deepCopy(); after.put("kind", kind).put("phase", "after").put("executed", false);
            after.set("actual_input", actual); after.set("output", output);
            return new CallResult(output, prepared, point(after, budgetMs), false);
        }
        final JsonNode output;
        try { output = execute.execute(actual); }
        catch (Exception error) {
            ObjectNode abort = identity.deepCopy(); abort.put("kind", kind).put("phase", "abort").put("status", "unknown");
            abort.putNull("output"); point(abort, budgetMs);
            throw error;
        }
        ObjectNode after = identity.deepCopy(); after.put("kind", kind).put("phase", "after").put("executed", true);
        after.set("actual_input", actual); after.set("output", output);
        return new CallResult(output, prepared, point(after, budgetMs), true);
    }

    public boolean allowLoop(ObjectNode fields) { return allowLoop(fields, 20); }
    public boolean allowLoop(ObjectNode fields, int budgetMs) { return checkLoop(fields, budgetMs).allowed; }
    /** Evaluate and commit one loop decision, exposing both protocol receipts. */
    public LoopResult checkLoop(ObjectNode fields, int budgetMs) {
        if (fields == null) throw new IllegalArgumentException("fields_required");
        ObjectNode request = fields.deepCopy(); request.put("kind", "loop");
        if (fields.has("ref")) {
            request.put("phase", "guard");
            JsonNode receipt = point(request, budgetMs);
            String decision = receipt.path("decision").asText("continue");
            return new LoopResult(!"stop".equals(decision), decision, receipt, receipt);
        }
        request.put("phase", "before");
        JsonNode prepared = point(request, budgetMs);
        String decision = prepared.path("decision").asText("continue");
        ObjectNode after = finishIdentity(fields, prepared);
        after.put("kind", "loop").put("phase", "after").put("decision", decision);
        JsonNode receipt = point(after, budgetMs);
        return new LoopResult(!"stop".equals(decision), decision, prepared, receipt);
    }

    public JsonNode captureGap(ObjectNode fields, String reason) throws Failure {
        if (fields == null || reason == null || reason.isEmpty()) throw new IllegalArgumentException("fields_and_reason_required");
        ObjectNode request = fields.deepCopy(); request.put("reason", reason);
        return invoke("capture.gap", request);
    }
    public JsonNode evaluationNext(String consumer, int index) throws Failure {
        return invoke("evaluation.next", JSON.createObjectNode().put("consumer", consumer).put("index", index));
    }
    public JsonNode evaluationFail(String consumer) throws Failure {
        return invoke("evaluation.fail", JSON.createObjectNode().put("consumer", consumer));
    }
    private static ObjectNode finishIdentity(ObjectNode fields, JsonNode prepared) {
        ObjectNode identity = JSON.createObjectNode();
        if (fields.has("ref")) identity.set("ref", fields.get("ref").deepCopy());
        else if (prepared.path("context").isObject()) identity.set("context", prepared.get("context").deepCopy());
        return identity;
    }

    public JsonNode point(ObjectNode fields) { return point(fields, 20); }
    public JsonNode point(ObjectNode fields, int budgetMs) {
        String id = UUID.randomUUID().toString();
        try {
            if (budgetMs < 1 || budgetMs > 5000 || fields == null) throw new IllegalArgumentException();
            ObjectNode packet = fields.deepCopy(); packet.put("operation", "point"); packet.put("request_id", id);
            if (packet.has("ref") && pointHasGap(fields)) packet.put("capture_gap", true);
            byte[] bytes = JSON.writeValueAsBytes(packet);
            if (bytes.length > 2097152) throw new IllegalArgumentException();
            return pointObserved(fields, once(bytes, budgetMs));
        } catch (Exception ignored) {
            ObjectNode context = fields != null && fields.path("context").isObject()
                ? ((ObjectNode) fields.get("context")).deepCopy() : JSON.createObjectNode();
            String kind = fields == null ? "custom" : fields.path("kind").asText(context.path("kind").asText("custom"));
            context.put("kind", kind); context.put("degraded", true); context.putArray("caused_by");
            if (!context.has("run_id")) context.put("run_id", "unobserved-" + id);
            ObjectNode result = JSON.createObjectNode(); result.set("context", context);
            result.set("input", fields == null ? null : fields.get("input")); result.put("degraded", true);
            result.put("capture_complete", false); result.put("policy_applied", false); result.put("closed", false); result.put("ended", false);
            result.put("decision", "loop".equals(kind) ? "continue" : "allow"); result.put("reason", "observer_unavailable");
            return pointObserved(fields, result);
        }
    }
    private static final class BoundedBody implements HttpResponse.BodySubscriber<byte[]> {
        private final CompletableFuture<byte[]> body = new CompletableFuture<>();
        private final ByteArrayOutputStream bytes = new ByteArrayOutputStream();
        private final int limit;
        private Flow.Subscription subscription;
        BoundedBody(int limit) { this.limit = limit; }
        @Override public CompletionStage<byte[]> getBody() { return body; }
        @Override public void onSubscribe(Flow.Subscription value) { subscription = value; value.request(1); }
        @Override public void onNext(List<ByteBuffer> items) {
            for (ByteBuffer item : items) {
                if (item.remaining() > limit - bytes.size()) {
                    subscription.cancel(); body.completeExceptionally(new Failure("response_too_large", 0)); return;
                }
                byte[] part = new byte[item.remaining()]; item.get(part); bytes.write(part, 0, part.length);
            }
            subscription.request(1);
        }
        @Override public void onError(Throwable error) { body.completeExceptionally(new Failure("observer_unavailable", 0)); }
        @Override public void onComplete() { body.complete(bytes.toByteArray()); }
    }
    private JsonNode once(byte[] body, int budgetMs) throws Failure {
        CompletableFuture<HttpResponse<byte[]>> pending = null;
        try {
            HttpRequest request = HttpRequest.newBuilder(endpoint).timeout(Duration.ofMillis(budgetMs))
                .header("Content-Type", "application/json").header("Authorization", "Bearer " + token)
                .POST(HttpRequest.BodyPublishers.ofByteArray(body)).build();
            pending = http.sendAsync(request, info -> new BoundedBody(maxBytes));
            // Includes connection, request upload, headers and the entire bounded body.
            HttpResponse<byte[]> response = pending.get(budgetMs, TimeUnit.MILLISECONDS);
            int status = response.statusCode();
            JsonNode payload;
            try { payload = JSON.readTree(response.body()); } catch (Exception ignored) { throw new Failure("response_invalid", status); }
            if (payload == null || !payload.isObject()) throw new Failure("response_invalid", status);
            if (status < 200 || status >= 300 || !payload.path("ok").asBoolean(false)) {
                String code = payload.path("error").asText("rpc_failed");
                throw new Failure(code.matches("[a-z_]{1,96}") ? code : "rpc_failed", status);
            }
            if (!payload.path("result").isObject()) throw new Failure("response_invalid", status);
            return payload.get("result");
        } catch (Failure error) { throw error; }
        catch (InterruptedException ignored) { Thread.currentThread().interrupt(); throw new Failure("observer_unavailable", 0); }
        catch (Exception error) {
            if (error.getCause() instanceof Failure) throw (Failure)error.getCause();
            throw new Failure("observer_unavailable", 0);
        }
        finally { if (pending != null && !pending.isDone()) pending.cancel(true); }
    }
    @Override public String toString() { return "EvolutionClient"; }
}
