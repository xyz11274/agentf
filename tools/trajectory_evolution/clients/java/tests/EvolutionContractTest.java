// java 客户端：通过统一协议收集执行事件并接收策略建议。
// 传输与业务执行分离；接收回执不代表 Agent 已完成整个任务。
// 打点的输入是建议修改前的请求，执行后必须回传实际输入、输出与状态。
// 取消、超时和采集缺口需要显式处理，不能把缺失证据当成正常完成。
// 本文件包含协议验证或接入示例，不作为真实业务效果的评测成绩。

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

  // 开始一个新的规范运行上下文，绑定应用、用户与会话身份。
  @BeforeEach
  void start() throws Exception {
    server = HttpServer.create(new InetSocketAddress("127.0.0.1", 0), 0);
    server.createContext("/v1/invoke", this::reply);
    server.start();
    client = new Evolution("http://127.0.0.1:" + server.getAddress().getPort(),
                           "fixture-client-contract-token", 1000, 65536, 0);
  }

  // 请求停止当前工作并完成资源收尾；不将停止信号解释为业务成功。
  @AfterEach
  void stop() {
    server.stop(0);
  }

  // 测试辅助函数：reply。
  // 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
  private void reply(HttpExchange exchange) {
    try {
      ObjectNode request = (ObjectNode)JSON.readTree(exchange.getRequestBody());
      ObjectNode result = JSON.createObjectNode();
      String operation = request.path("operation").asText();
      if ("point".equals(operation)) {
        String phase = request.path("phase").asText();
        String kind = request.path("kind").asText("custom");
        if ("before".equals(phase)) {
          result.put("decision",
                     "reject".equals(request.path("name").asText()) ? "reject" : "allow");
          result.put("reason", "fixture_reject");
          result.set("input", request.get("input"));
          result.set("context", JSON.createObjectNode().put("run_id", "run-1").put("kind", kind));
        } else if ("guard".equals(phase))
          result.put("decision", "stop");
        else {
          result.put("event_id", "receipt-" + phase);
          result.put("closed", false);
        }
        if ("abort".equals(phase))
          aborts.incrementAndGet();
      } else if ("capture.gap".equals(operation))
        result.put("capture_complete", false);
      else if ("evaluation.next".equals(operation))
        result.put("state", "done");
      else if ("evaluation.fail".equals(operation))
        result.put("failed", true);
      ObjectNode envelope = JSON.createObjectNode().put("ok", true).set("result", result);
      byte[] body = JSON.writeValueAsBytes(envelope);
      exchange.getResponseHeaders().add("Content-Type", "application/json");
      exchange.sendResponseHeaders(200, body.length);
      exchange.getResponseBody().write(body);
    } catch (Exception ignored) {
    } finally {
      exchange.close();
    }
  }

  // 验证或读取当前对象的字段集合，拒绝与协议不符的结构。
  private static ObjectNode fields(String name) {
    return JSON.createObjectNode()
        .put("kind", "action")
        .put("name", name)
        .set("input", JSON.createObjectNode().put("value", 7));
  }

  // 测试辅助函数：callRunsOnceAndExposesReceipt。
  // 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
  @Test
  void callRunsOnceAndExposesReceipt() throws Exception {
    AtomicInteger calls = new AtomicInteger();
    Evolution.CallResult result = client.callWithReceipt(fields("allow"), input -> {
      calls.incrementAndGet();
      return JSON.createObjectNode().put("value", input.path("value").asInt());
    }, 500);
    assertEquals(1, calls.get());
    assertTrue(result.executed);
    assertEquals(7, result.output.path("value").asInt());
    assertEquals("receipt-after", result.receipt.path("event_id").asText());
  }

  // 测试辅助函数：rejectSkipsBusinessCallAndStillRecordsAfter。
  // 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
  @Test
  void rejectSkipsBusinessCallAndStillRecordsAfter() throws Exception {
    AtomicInteger calls = new AtomicInteger();
    Evolution.CallResult result = client.callWithReceipt(fields("reject"), input -> {
      calls.incrementAndGet();
      return input;
    }, 500);
    assertEquals(0, calls.get());
    assertFalse(result.executed);
    assertEquals("rejected", result.output.path("status").asText());
    assertEquals("receipt-after", result.receipt.path("event_id").asText());
  }

  // 测试辅助函数：businessExceptionIsUnchangedAndAbortIsRecorded。
  // 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
  @Test
  void businessExceptionIsUnchangedAndAbortIsRecorded() {
    Exception original = new Exception("business-secret");
    Exception observed = assertThrows(
        Exception.class,
        () -> client.callWithReceipt(fields("allow"), input -> { throw original; }, 500));
    assertSame(original, observed);
    assertEquals(1, aborts.get());
  }

  // 测试辅助函数：loopAndAdministrativeHelpersExposeResults。
  // 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
  @Test
  void loopAndAdministrativeHelpersExposeResults() throws Exception {
    ObjectNode loop =
        JSON.createObjectNode().set("ref", JSON.createObjectNode().put("run", "r").put("key", "k"));
    Evolution.LoopResult decision = client.checkLoop(loop, 500);
    assertFalse(decision.allowed);
    assertEquals("stop", decision.decision);
    assertFalse(client.captureGap(JSON.createObjectNode().put("run_id", "r"), "missing_boundary")
                    .path("capture_complete")
                    .asBoolean(true));
    assertEquals("done", client.evaluationNext("consumer", 0).path("state").asText());
    assertTrue(client.evaluationFail("consumer").path("failed").asBoolean());
  }
}
