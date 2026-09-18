import static org.junit.jupiter.api.Assertions.*;

import com.fasterxml.jackson.databind.JsonNode;
import com.fasterxml.jackson.databind.ObjectMapper;
import com.fasterxml.jackson.databind.node.ObjectNode;
import com.sun.net.httpserver.HttpExchange;
import com.sun.net.httpserver.HttpServer;
import java.net.InetSocketAddress;
import java.nio.charset.StandardCharsets;
import java.util.concurrent.atomic.AtomicInteger;
import org.junit.jupiter.api.AfterEach;
import org.junit.jupiter.api.BeforeEach;
import org.junit.jupiter.api.Test;

final class EvolutionContractTest {
    private static final ObjectMapper JSON = new ObjectMapper();
    private HttpServer server;
    private Evolution client;
    private final AtomicInteger aborts = new AtomicInteger();

    @BeforeEach void start() throws Exception {
        server = HttpServer.create(new InetSocketAddress("127.0.0.1", 0), 0);
        server.createContext("/v1/invoke", this::reply);
        server.start();
        client = new Evolution("http://127.0.0.1:" + server.getAddress().getPort(),
            "fixture-client-contract-token", 1000, 65536, 0);
    }
    @AfterEach void stop() { server.stop(0); }

    private void reply(HttpExchange exchange) {
        try {
            ObjectNode request = (ObjectNode)JSON.readTree(exchange.getRequestBody());
            ObjectNode result = JSON.createObjectNode();
            String operation = request.path("operation").asText();
            if ("point".equals(operation)) {
                String phase = request.path("phase").asText();
                String kind = request.path("kind").asText("custom");
                if ("before".equals(phase)) {
                    result.put("decision", "reject".equals(request.path("name").asText()) ? "reject" : "allow");
                    result.put("reason", "fixture_reject"); result.set("input", request.get("input"));
                    result.set("context", JSON.createObjectNode().put("run_id", "run-1").put("kind", kind));
                } else if ("guard".equals(phase)) result.put("decision", "stop");
                else { result.put("event_id", "receipt-" + phase); result.put("closed", false); }
                if ("abort".equals(phase)) aborts.incrementAndGet();
            } else if ("capture.gap".equals(operation)) result.put("capture_complete", false);
            else if ("evaluation.next".equals(operation)) result.put("state", "done");
            else if ("evaluation.fail".equals(operation)) result.put("failed", true);
            ObjectNode envelope = JSON.createObjectNode().put("ok", true).set("result", result);
            byte[] body = JSON.writeValueAsBytes(envelope);
            exchange.getResponseHeaders().add("Content-Type", "application/json");
            exchange.sendResponseHeaders(200, body.length); exchange.getResponseBody().write(body);
        } catch (Exception ignored) { }
        finally { exchange.close(); }
    }

    private static ObjectNode fields(String name) {
        return JSON.createObjectNode().put("kind", "action").put("name", name).set("input", JSON.createObjectNode().put("value", 7));
    }

    @Test void callRunsOnceAndExposesReceipt() throws Exception {
        AtomicInteger calls = new AtomicInteger();
        Evolution.CallResult result = client.callWithReceipt(fields("allow"), input -> {
            calls.incrementAndGet(); return JSON.createObjectNode().put("value", input.path("value").asInt());
        }, 500);
        assertEquals(1, calls.get()); assertTrue(result.executed);
        assertEquals(7, result.output.path("value").asInt()); assertEquals("receipt-after", result.receipt.path("event_id").asText());
    }

    @Test void rejectSkipsBusinessCallAndStillRecordsAfter() throws Exception {
        AtomicInteger calls = new AtomicInteger();
        Evolution.CallResult result = client.callWithReceipt(fields("reject"), input -> {
            calls.incrementAndGet(); return input;
        }, 500);
        assertEquals(0, calls.get()); assertFalse(result.executed);
        assertEquals("rejected", result.output.path("status").asText());
        assertEquals("receipt-after", result.receipt.path("event_id").asText());
    }

    @Test void businessExceptionIsUnchangedAndAbortIsRecorded() {
        Exception original = new Exception("business-secret");
        Exception observed = assertThrows(Exception.class,
            () -> client.callWithReceipt(fields("allow"), input -> { throw original; }, 500));
        assertSame(original, observed); assertEquals(1, aborts.get());
    }

    @Test void loopAndAdministrativeHelpersExposeResults() throws Exception {
        ObjectNode loop = JSON.createObjectNode().set("ref", JSON.createObjectNode().put("run", "r").put("key", "k"));
        Evolution.LoopResult decision = client.checkLoop(loop, 500);
        assertFalse(decision.allowed); assertEquals("stop", decision.decision);
        assertFalse(client.captureGap(JSON.createObjectNode().put("run_id", "r"), "missing_boundary").path("capture_complete").asBoolean(true));
        assertEquals("done", client.evaluationNext("consumer", 0).path("state").asText());
        assertTrue(client.evaluationFail("consumer").path("failed").asBoolean());
    }
}
