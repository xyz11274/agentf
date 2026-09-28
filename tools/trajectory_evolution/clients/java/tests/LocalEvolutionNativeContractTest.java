// java 客户端：通过统一协议收集执行事件并接收策略建议。
// 原生入口复用包内 C ABI；注意句柄生命周期与跨语言内存释放。
// 打点的输入是建议修改前的请求，执行后必须回传实际输入、输出与状态。
// 取消、超时和采集缺口需要显式处理，不能把缺失证据当成正常完成。
// 本文件包含协议验证或接入示例，不作为真实业务效果的评测成绩。

import static org.junit.jupiter.api.Assertions.*;
import static org.junit.jupiter.api.Assumptions.assumeTrue;

import com.fasterxml.jackson.databind.ObjectMapper;
import com.fasterxml.jackson.databind.node.ObjectNode;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.Future;
import java.util.concurrent.atomic.AtomicInteger;
import org.junit.jupiter.api.Test;

final class LocalEvolutionNativeContractTest {
  private static final ObjectMapper JSON = new ObjectMapper();

  // 测试辅助函数：realSharedLibraryPreservesBusinessAndReceipts。
  // 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
  @Test
  void realSharedLibraryPreservesBusinessAndReceipts() throws Exception {
    String library = System.getProperty("evolution.native.library");
    String config = System.getProperty("evolution.native.config");
    assumeTrue(library != null && config != null, "native library and config were not requested");
    LocalEvolution local = new LocalEvolution(library, config, "fixture-principal-alpha-00000001",
                                              JSON.createObjectNode().put("background", true));
    try {
      ObjectNode reference = JSON.createObjectNode().put("run", "java-native-contract");
      ObjectNode begin = JSON.createObjectNode()
                             .put("phase", "before")
                             .put("kind", "run")
                             .put("input", "native contract")
                             .put("level", "L0");
      begin.set("ref", reference.deepCopy());
      begin.set("capabilities", JSON.createArrayNode());
      ObjectNode begun = local.point(begin);
      assertFalse(begun.path("degraded").asBoolean(true), begun.toString());

      AtomicInteger calls = new AtomicInteger();
      ObjectNode action = JSON.createObjectNode()
                              .put("phase", "before")
                              .put("kind", "action")
                              .put("name", "lookup");
      action.set("ref",
                 JSON.createObjectNode().put("run", "java-native-contract").put("key", "action"));
      action.set("input", JSON.createObjectNode().put("query", "data"));
      LocalEvolution.CallResult observed = local.callWithReceipt(action, input -> {
        calls.incrementAndGet();
        return input;
      });
      assertEquals(1, calls.get());
      assertTrue(observed.executed);
      assertFalse(observed.receipt.path("degraded").asBoolean(true), observed.receipt.toString());

      ObjectNode loop = JSON.createObjectNode();
      loop.set("ref",
               JSON.createObjectNode().put("run", "java-native-contract").put("key", "loop"));
      loop.put("state", "same");
      LocalEvolution.LoopResult decision = local.checkLoop(loop);
      assertTrue(decision.allowed);
      assertEquals("continue", decision.decision);

      ObjectNode end =
          JSON.createObjectNode().put("phase", "after").put("kind", "run").put("output", "done");
      end.set("ref", reference.deepCopy());
      assertTrue(local.point(end).path("closed").asBoolean(false));
      local.drain();
      // drain 将运行状态持久化，轨迹导出仍由后台完成。
      // 保留测试客户端，直到学习器完成归档。
      Path users = Path.of(JSON.readTree(Path.of(config).toFile()).path("data_dir").asText())
                       .resolve("by-user");
      long deadline = System.nanoTime() + 5_000_000_000L;
      boolean archived = false;
      while (!archived && System.nanoTime() < deadline) {
        try (var files = Files.walk(users)) {
          archived = files.anyMatch(p -> p.toString().endsWith(".trace.json"));
        }
        if (!archived)
          Thread.sleep(10);
      }
      assertTrue(archived, "background trace archive did not complete");
      assertEquals(0, local.metrics().path("degraded_calls").asInt(-1));
    } finally {
      local.close();
    }

    // 观测失败不能触发业务重试，也不能替换原业务操作。
    AtomicInteger degradedCalls = new AtomicInteger();
    ObjectNode offline = JSON.createObjectNode().put("kind", "action");
    offline.set("input", JSON.createObjectNode().put("value", 7));
    LocalEvolution.CallResult degraded = local.callWithReceipt(offline, input -> {
      degradedCalls.incrementAndGet();
      return input;
    });
    assertEquals(1, degradedCalls.get());
    assertTrue(degraded.executed);
    assertTrue(degraded.prepared.path("degraded").asBoolean(false));
    assertTrue(degraded.receipt.path("degraded").asBoolean(false));
    local.close();
  }

  // 测试辅助函数：closeCannotRaceAnActiveNativeCall。
  // 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
  @Test
  void closeCannotRaceAnActiveNativeCall() throws Exception {
    String library = System.getProperty("evolution.native.library");
    String config = System.getProperty("evolution.native.config");
    assumeTrue(library != null && config != null, "native library and config were not requested");
    LocalEvolution local = new LocalEvolution(library, config, "fixture-principal-alpha-00000001",
                                              JSON.createObjectNode().put("background", false));
    ExecutorService workers = Executors.newFixedThreadPool(8);
    CountDownLatch start = new CountDownLatch(1);
    List<Future<?>> calls = new ArrayList<>();
    try {
      for (int worker = 0; worker < 6; ++worker)
        calls.add(workers.submit(() -> {
          start.await();
          for (int i = 0; i < 500; ++i)
            assertTrue(local.invoke("contract.describe", JSON.createObjectNode()).isObject());
          return null;
        }));
      calls.add(workers.submit(() -> {
        start.await();
        for (int i = 0; i < 100; ++i) {
          local.metrics();
          local.setEnabled(true);
          if (i % 25 == 0)
            local.drain();
        }
        return null;
      }));
      Future<?> closer = workers.submit(() -> {
        start.await();
        local.close();
        local.close();
        return null;
      });
      start.countDown();
      for (Future<?> call : calls)
        call.get();
      closer.get();
      assertTrue(local.invoke("contract.describe", JSON.createObjectNode())
                     .path("degraded")
                     .asBoolean(false));
      assertTrue(local.metrics().isEmpty());
    } finally {
      local.close();
      workers.shutdownNow();
    }
  }
}
