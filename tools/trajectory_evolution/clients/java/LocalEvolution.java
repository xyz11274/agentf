import com.fasterxml.jackson.databind.JsonNode;
import com.fasterxml.jackson.databind.ObjectMapper;
import com.fasterxml.jackson.databind.node.ObjectNode;
import com.sun.jna.IntegerType;
import com.sun.jna.Library;
import com.sun.jna.Native;
import com.sun.jna.Pointer;
import com.sun.jna.ptr.PointerByReference;
import java.nio.charset.StandardCharsets;
import java.util.HashMap;
import java.util.HashSet;
import java.util.Map;
import java.util.Set;
import java.util.UUID;
import java.util.concurrent.locks.Lock;
import java.util.concurrent.locks.ReentrantReadWriteLock;

/** Typed, in-process Java binding for the stable trajectory-evolution C ABI. */
public final class LocalEvolution implements AutoCloseable {
    private interface NativeApi extends Library {
        Pointer te_local_open(String config, String bearer, String options, PointerByReference error);
        Pointer te_local_invoke(Pointer handle, byte[] request, SizeT length);
        Pointer te_local_manage(Pointer handle, byte[] request, SizeT length);
        Pointer te_local_metrics(Pointer handle);
        void te_local_set_enabled(Pointer handle, int enabled);
        void te_local_drain(Pointer handle);
        void te_local_close(Pointer handle);
        void te_local_free(Pointer value);
    }

    /** JNA has no built-in size_t whose width is independent of C long. */
    public static final class SizeT extends IntegerType {
        private static final long serialVersionUID = 1L;
        public SizeT() { this(0); }
        public SizeT(long value) { super(Native.SIZE_T_SIZE, value, true); }
    }

    @FunctionalInterface
    public interface CallExecutor { JsonNode execute(JsonNode input) throws Exception; }

    public static final class CallResult {
        public final JsonNode output;
        public final ObjectNode prepared, receipt;
        public final boolean executed;
        private CallResult(JsonNode output, ObjectNode prepared, ObjectNode receipt, boolean executed) {
            this.output = output; this.prepared = prepared; this.receipt = receipt; this.executed = executed;
        }
    }

    public static final class LoopResult {
        public final boolean allowed;
        public final String decision;
        public final ObjectNode prepared, receipt;
        private LoopResult(boolean allowed, String decision, ObjectNode prepared, ObjectNode receipt) {
            this.allowed = allowed; this.decision = decision; this.prepared = prepared; this.receipt = receipt;
        }
    }

    public static final class Failure extends RuntimeException {
        private static final long serialVersionUID = 1L;
        Failure(String code) { super(code); }
    }

    private interface NativeCall { Pointer call(Pointer handle, byte[] request, SizeT length); }
    private static final ObjectMapper JSON = new ObjectMapper();
    private final NativeApi nativeApi;
    private final long processId;
    private final int maxRequestBytes;
    private final Set<String> pointGaps = new HashSet<>();
    private final ReentrantReadWriteLock lifecycle = new ReentrantReadWriteLock(true);
    private boolean pointGapsFull;
    private volatile Pointer handle;

    public LocalEvolution(String libraryPath, String configPath, String token) {
        this(libraryPath, configPath, token, JSON.createObjectNode());
    }

    public LocalEvolution(String libraryPath, String configPath, String token, ObjectNode options) {
        if (libraryPath == null || libraryPath.isEmpty() || configPath == null || configPath.isEmpty()
            || token == null || token.isEmpty()) throw new IllegalArgumentException("local_configuration_missing");
        ObjectNode selected = options == null ? JSON.createObjectNode() : options.deepCopy();
        maxRequestBytes = selected.path("max_request_bytes").asInt(65536);
        Map<String, Object> loadOptions = new HashMap<>();
        loadOptions.put(Library.OPTION_STRING_ENCODING, StandardCharsets.UTF_8.name());
        try { nativeApi = Native.load(libraryPath, NativeApi.class, loadOptions); }
        catch (UnsatisfiedLinkError | SecurityException error) { throw new Failure("local_initialization_failed"); }
        PointerByReference nativeError = new PointerByReference();
        final String encoded;
        try { encoded = JSON.writeValueAsString(selected); }
        catch (Exception error) { throw new Failure("local_initialization_failed"); }
        Pointer opened = nativeApi.te_local_open(configPath, token, encoded, nativeError);
        Pointer detail = nativeError.getValue();
        if (detail != null) nativeApi.te_local_free(detail);
        if (opened == null) throw new Failure("local_initialization_failed");
        handle = opened;
        processId = ProcessHandle.current().pid();
    }

    private String consume(Pointer value) {
        if (value == null) throw new Failure("local_unavailable");
        try { return value.getString(0, StandardCharsets.UTF_8.name()); }
        finally { nativeApi.te_local_free(value); }
    }

    private ObjectNode nativeCall(NativeCall call, String operation, ObjectNode fields, String requestId) {
        ObjectNode packet = fields.deepCopy();
        packet.put("operation", operation); packet.put("request_id", requestId);
        final byte[] body;
        try { body = JSON.writeValueAsBytes(packet); }
        catch (Exception error) { throw new Failure("request_invalid"); }
        if (body.length > maxRequestBytes) throw new Failure("local_request_too_large");
        final String response;
        Lock read = lifecycle.readLock(); read.lock();
        try {
            Pointer owned = handle;
            if (owned == null || processId != ProcessHandle.current().pid()) throw new Failure("local_unavailable");
            response = consume(call.call(owned, body, new SizeT(body.length)));
        } finally { read.unlock(); }
        final JsonNode decoded;
        try { decoded = JSON.readTree(response); }
        catch (Failure error) { throw error; }
        catch (Exception error) { throw new Failure("response_invalid"); }
        if (decoded == null || !decoded.isObject()) throw new Failure("response_invalid");
        if (!decoded.path("ok").asBoolean(false)) {
            String code = decoded.path("error").asText("local_unavailable");
            throw new Failure(code.matches("[a-z_]{1,96}") ? code : "local_unavailable");
        }
        if (!decoded.path("result").isObject()) throw new Failure("response_invalid");
        return (ObjectNode)decoded.get("result");
    }

    public ObjectNode invoke(String operation, ObjectNode fields) {
        return invoke(operation, fields, UUID.randomUUID().toString());
    }

    public ObjectNode invoke(String operation, ObjectNode fields, String requestId) {
        if (operation == null || operation.isEmpty() || fields == null || requestId == null || requestId.isEmpty())
            throw new IllegalArgumentException("operation_fields_and_request_id_required");
        try { return nativeCall(nativeApi::te_local_invoke, operation, fields, requestId); }
        catch (RuntimeException error) {
            tryReportGap(operation, fields);
            return fallback(operation, fields, requestId);
        }
    }

    /** Blocking evaluation administration; never call this on an Agent boundary. */
    public ObjectNode manage(String operation, ObjectNode fields) {
        return manage(operation, fields, UUID.randomUUID().toString());
    }

    public ObjectNode manage(String operation, ObjectNode fields, String requestId) {
        if (operation == null || !operation.startsWith("agent_evaluation.") || fields == null)
            throw new IllegalArgumentException("management_operation_required");
        return nativeCall(nativeApi::te_local_manage, operation, fields, requestId);
    }

    /** Isolated evaluation entry is cross-process HTTP; reuse its typed Java driver. */
    public static boolean evaluate(Evolution.Receiver receive) throws Evolution.Failure {
        return Evolution.evaluate(receive);
    }

    private void tryReportGap(String operation, ObjectNode fields) {
        try {
            ObjectNode gap = JSON.createObjectNode();
            String gapOperation;
            if ("point".equals(operation) && fields.path("ref").isObject()) {
                gapOperation = "point"; gap.put("phase", "gap").put("kind", "run");
                gap.set("ref", JSON.createObjectNode().set("run", copy(fields.path("ref").get("run"))));
            } else {
                gapOperation = "capture.gap";
                JsonNode run = fields.get("run_id");
                if (run == null && fields.path("context").isObject()) run = fields.path("context").get("run_id");
                gap.set("run_id", copy(run)); gap.put("reason", "observer_unavailable");
            }
            nativeCall(nativeApi::te_local_invoke, gapOperation, gap, UUID.randomUUID().toString());
        } catch (RuntimeException ignored) { }
    }

    private static JsonNode copy(JsonNode value) { return value == null ? null : value.deepCopy(); }

    private static ObjectNode fallback(String operation, ObjectNode fields, String requestId) {
        ObjectNode result = JSON.createObjectNode().put("degraded", true).put("reason", "local_unavailable")
            .put("capture_complete", false).put("policy_applied", false).put("event_id", "");
        result.set("input", copy(fields.get("input")));
        if ("point".equals(operation)) {
            ObjectNode context = fields.path("context").isObject()
                ? ((ObjectNode)fields.path("context")).deepCopy() : JSON.createObjectNode();
            String kind = fields.path("kind").asText(context.path("kind").asText("custom"));
            context.put("kind", kind).put("degraded", true); context.set("caused_by", JSON.createArrayNode());
            if (!context.hasNonNull("run_id")) context.put("run_id", "unobserved-" + requestId);
            result.set("context", context); result.put("decision", "loop".equals(kind) ? "continue" : "allow");
            result.put("closed", false).put("ended", false);
        } else if ("run.begin".equals(operation)) result.put("run_id", "unobserved-" + requestId);
        else if ("activity.begin".equals(operation)) {
            result.put("activity_id", "unobserved-" + requestId).put("decision", "allow");
            result.set("context", JSON.createObjectNode().set("run_id", copy(fields.get("run_id"))));
            ((ObjectNode)result.get("context")).put("activity_id", "unobserved-" + requestId).put("degraded", true);
        } else if ("loop.check".equals(operation)) {
            result.put("control_id", "unobserved-" + requestId).put("decision", "continue").put("context", "");
        } else if ("activity.end".equals(operation) || "activity.abort".equals(operation) || "loop.commit".equals(operation)) {
            result.put("ended", false);
        } else if ("run.end".equals(operation) || "run.abort".equals(operation)) {
            result.put("closed", false).put("evaluation_status", "quarantined");
        }
        return result;
    }

    private String pointRun(ObjectNode fields) {
        JsonNode run = fields.path("ref").get("run"); return run == null ? "" : run.asText("");
    }

    private ObjectNode pointObserved(ObjectNode fields, ObjectNode result) {
        String run = pointRun(fields);
        if (!run.isEmpty() && run.length() <= 256) synchronized (pointGaps) {
            if (result.path("degraded").asBoolean(false)) {
                if (pointGaps.size() < 1024) pointGaps.add(run); else pointGapsFull = true;
            } else if (result.path("closed").asBoolean(false) || result.path("run_closed").asBoolean(false)) pointGaps.remove(run);
        }
        return result;
    }

    public ObjectNode point(ObjectNode fields) {
        if (fields == null) throw new IllegalArgumentException("fields_required");
        ObjectNode request = fields.deepCopy();
        synchronized (pointGaps) {
            if (request.has("ref") && (pointGapsFull || pointGaps.contains(pointRun(fields)))) request.put("capture_gap", true);
        }
        return pointObserved(fields, invoke("point", request));
    }

    public JsonNode call(ObjectNode fields, CallExecutor execute) throws Exception {
        return callWithReceipt(fields, execute).output;
    }

    /** Wrap one business call. The executor is invoked zero or one time, never retried. */
    public CallResult callWithReceipt(ObjectNode fields, CallExecutor execute) throws Exception {
        if (fields == null || execute == null) throw new IllegalArgumentException("fields_and_executor_required");
        ObjectNode before = fields.deepCopy().put("phase", "before");
        ObjectNode prepared = point(before);
        JsonNode actual = prepared.has("input") ? copy(prepared.get("input")) : copy(fields.get("input"));
        ObjectNode identity = finishIdentity(fields, prepared);
        String kind = fields.path("kind").asText("custom");
        if ("reject".equals(prepared.path("decision").asText())) {
            String reason = prepared.path("reason").asText("policy_rejected");
            ObjectNode output = JSON.createObjectNode().put("status", "rejected").put("reason", reason).put("error", reason);
            ObjectNode after = identity.deepCopy().put("kind", kind).put("phase", "after").put("executed", false);
            after.set("actual_input", copy(actual)); after.set("output", output.deepCopy());
            return new CallResult(output, prepared, point(after), false);
        }
        final JsonNode output;
        try { output = execute.execute(copy(actual)); }
        catch (Exception error) {
            ObjectNode abort = identity.deepCopy().put("kind", kind).put("phase", "abort").put("status", "unknown");
            abort.putNull("output"); point(abort); throw error;
        }
        ObjectNode after = identity.deepCopy().put("kind", kind).put("phase", "after").put("executed", true);
        after.set("actual_input", copy(actual)); after.set("output", copy(output));
        return new CallResult(output, prepared, point(after), true);
    }

    public boolean allowLoop(ObjectNode fields) { return checkLoop(fields).allowed; }

    public LoopResult checkLoop(ObjectNode fields) {
        if (fields == null) throw new IllegalArgumentException("fields_required");
        ObjectNode request = fields.deepCopy().put("kind", "loop");
        if (fields.has("ref")) {
            ObjectNode receipt = point(request.put("phase", "guard"));
            String decision = receipt.path("decision").asText("continue");
            return new LoopResult(!"stop".equals(decision), decision, receipt, receipt);
        }
        ObjectNode prepared = point(request.put("phase", "before"));
        String decision = prepared.path("decision").asText("continue");
        ObjectNode after = finishIdentity(fields, prepared).put("kind", "loop").put("phase", "after").put("decision", decision);
        return new LoopResult(!"stop".equals(decision), decision, prepared, point(after));
    }

    public ObjectNode captureGap(ObjectNode fields, String reason) {
        if (fields == null || reason == null || reason.isEmpty()) throw new IllegalArgumentException("fields_and_reason_required");
        return invoke("capture.gap", fields.deepCopy().put("reason", reason));
    }

    private static ObjectNode finishIdentity(ObjectNode fields, ObjectNode prepared) {
        ObjectNode identity = JSON.createObjectNode();
        if (fields.has("ref")) identity.set("ref", copy(fields.get("ref")));
        else if (prepared.path("context").isObject()) identity.set("context", prepared.path("context").deepCopy());
        return identity;
    }

    public ObjectNode metrics() {
        final String response;
        Lock read = lifecycle.readLock(); read.lock();
        try {
            Pointer owned = handle;
            if (owned == null || processId != ProcessHandle.current().pid()) return JSON.createObjectNode();
            response = consume(nativeApi.te_local_metrics(owned));
        } catch (RuntimeException error) { return JSON.createObjectNode(); }
        finally { read.unlock(); }
        try {
            JsonNode result = JSON.readTree(response);
            return result != null && result.isObject() ? (ObjectNode)result : JSON.createObjectNode();
        } catch (Exception error) { return JSON.createObjectNode(); }
    }

    public void setEnabled(boolean enabled) {
        Lock read = lifecycle.readLock(); read.lock();
        try {
            Pointer owned = handle;
            if (owned != null && processId == ProcessHandle.current().pid()) nativeApi.te_local_set_enabled(owned, enabled ? 1 : 0);
        } finally { read.unlock(); }
    }

    /** Application maintenance/shutdown only; never call at a business boundary. */
    public void drain() {
        Lock read = lifecycle.readLock(); read.lock();
        try {
            Pointer owned = handle;
            if (owned != null && processId == ProcessHandle.current().pid()) nativeApi.te_local_drain(owned);
        } finally { read.unlock(); }
    }

    @Override public void close() {
        Lock write = lifecycle.writeLock(); write.lock();
        try {
            Pointer owned = handle; handle = null;
            if (owned != null && processId == ProcessHandle.current().pid()) nativeApi.te_local_close(owned);
        } finally { write.unlock(); }
    }
}
