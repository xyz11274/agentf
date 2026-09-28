// java 客户端：通过统一协议收集执行事件并接收策略建议。
// 原生入口复用包内 C ABI；注意句柄生命周期与跨语言内存释放。
// 打点的输入是建议修改前的请求，执行后必须回传实际输入、输出与状态。
// 取消、超时和采集缺口需要显式处理，不能把缺失证据当成正常完成。

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

/* 稳定 C 接口的类型化、进程内 Java 绑定。 */
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

  /* JNA 没有宽度独立于 C long 的内置 size_t，需要在此明确映射。 */
  public static final class SizeT extends IntegerType {
    private static final long serialVersionUID = 1L;

    // 初始化 SizeT 的状态和资源；输入参数的校验与所有权在构造阶段确定。
    // 对象创建后复用既有配置，实际执行与收尾分别由后续接口负责。
    public SizeT() {
      this(0);
    }

    // 初始化 SizeT 的状态和资源；输入参数的校验与所有权在构造阶段确定。
    // 对象创建后复用既有配置，实际执行与收尾分别由后续接口负责。
    public SizeT(long value) {
      super(Native.SIZE_T_SIZE, value, true);
    }
  }

  @FunctionalInterface
  public interface CallExecutor {
    JsonNode execute(JsonNode input) throws Exception;
  }

  public static final class CallResult {
    public final JsonNode output;
    public final ObjectNode prepared, receipt;
    public final boolean executed;

    // 初始化 CallResult 的状态和资源；输入参数的校验与所有权在构造阶段确定。
    // 对象创建后复用既有配置，实际执行与收尾分别由后续接口负责。
    private CallResult(JsonNode output, ObjectNode prepared, ObjectNode receipt, boolean executed) {
      this.output = output;
      this.prepared = prepared;
      this.receipt = receipt;
      this.executed = executed;
    }
  }

  public static final class LoopResult {
    public final boolean allowed;
    public final String decision;
    public final ObjectNode prepared, receipt;

    // 初始化 LoopResult 的状态和资源；输入参数的校验与所有权在构造阶段确定。
    // 对象创建后复用既有配置，实际执行与收尾分别由后续接口负责。
    private LoopResult(boolean allowed, String decision, ObjectNode prepared, ObjectNode receipt) {
      this.allowed = allowed;
      this.decision = decision;
      this.prepared = prepared;
      this.receipt = receipt;
    }
  }

  public static final class Failure extends RuntimeException {
    private static final long serialVersionUID = 1L;

    // 保存接口失败的固定类型，供调用方区分传输与业务问题。
    Failure(String code) {
      super(code);
    }
  }

  private interface NativeCall {
    Pointer call(Pointer handle, byte[] request, SizeT length);
  }
  private static final ObjectMapper JSON = new ObjectMapper();
  private final NativeApi nativeApi;
  private final long processId;
  private final int maxRequestBytes;
  private final Set<String> pointGaps = new HashSet<>();
  private final ReentrantReadWriteLock lifecycle = new ReentrantReadWriteLock(true);
  private boolean pointGapsFull;
  private volatile Pointer handle;

  // 初始化 LocalEvolution 的状态和资源；输入参数的校验与所有权在构造阶段确定。
  // 对象创建后复用既有配置，实际执行与收尾分别由后续接口负责。
  public LocalEvolution(String libraryPath, String configPath, String token) {
    this(libraryPath, configPath, token, JSON.createObjectNode());
  }

  // 初始化 LocalEvolution 的状态和资源；输入参数的校验与所有权在构造阶段确定。
  // 对象创建后复用既有配置，实际执行与收尾分别由后续接口负责。
  public LocalEvolution(String libraryPath, String configPath, String token, ObjectNode options) {
    if (libraryPath == null || libraryPath.isEmpty() || configPath == null ||
        configPath.isEmpty() || token == null || token.isEmpty())
      throw new IllegalArgumentException("local_configuration_missing");
    ObjectNode selected = options == null ? JSON.createObjectNode() : options.deepCopy();
    maxRequestBytes = selected.path("max_request_bytes").asInt(65536);
    Map<String, Object> loadOptions = new HashMap<>();
    loadOptions.put(Library.OPTION_STRING_ENCODING, StandardCharsets.UTF_8.name());
    try {
      nativeApi = Native.load(libraryPath, NativeApi.class, loadOptions);
    } catch (UnsatisfiedLinkError | SecurityException error) {
      throw new Failure("local_initialization_failed");
    }
    PointerByReference nativeError = new PointerByReference();
    final String encoded;
    try {
      encoded = JSON.writeValueAsString(selected);
    } catch (Exception error) {
      throw new Failure("local_initialization_failed");
    }
    Pointer opened = nativeApi.te_local_open(configPath, token, encoded, nativeError);
    Pointer detail = nativeError.getValue();
    if (detail != null)
      nativeApi.te_local_free(detail);
    if (opened == null)
      throw new Failure("local_initialization_failed");
    handle = opened;
    processId = ProcessHandle.current().pid();
  }

  // 复制并解析原生返回字符串，随后由同一动态库释放内存。
  private String consume(Pointer value) {
    if (value == null)
      throw new Failure("local_unavailable");
    try {
      return value.getString(0, StandardCharsets.UTF_8.name());
    } finally {
      nativeApi.te_local_free(value);
    }
  }

  // 调用原生函数并复制其响应，按库约定释放返回值。
  private ObjectNode nativeCall(NativeCall call, String operation, ObjectNode fields,
                                String requestId) {
    ObjectNode packet = fields.deepCopy();
    packet.put("operation", operation);
    packet.put("request_id", requestId);
    final byte[] body;
    try {
      body = JSON.writeValueAsBytes(packet);
    } catch (Exception error) {
      throw new Failure("request_invalid");
    }
    if (body.length > maxRequestBytes)
      throw new Failure("local_request_too_large");
    final String response;
    Lock read = lifecycle.readLock();
    read.lock();
    try {
      Pointer owned = handle;
      if (owned == null || processId != ProcessHandle.current().pid())
        throw new Failure("local_unavailable");
      response = consume(call.call(owned, body, new SizeT(body.length)));
    } finally {
      read.unlock();
    }
    final JsonNode decoded;
    try {
      decoded = JSON.readTree(response);
    } catch (Failure error) {
      throw error;
    } catch (Exception error) {
      throw new Failure("response_invalid");
    }
    if (decoded == null || !decoded.isObject())
      throw new Failure("response_invalid");
    if (!decoded.path("ok").asBoolean(false)) {
      String code = decoded.path("error").asText("local_unavailable");
      throw new Failure(code.matches("[a-z_]{1,96}") ? code : "local_unavailable");
    }
    if (!decoded.path("result").isObject())
      throw new Failure("response_invalid");
    return (ObjectNode)decoded.get("result");
  }

  // 执行一次组件协议调用，检查响应信封并保持请求身份一致。
  public ObjectNode invoke(String operation, ObjectNode fields) {
    return invoke(operation, fields, UUID.randomUUID().toString());
  }

  // 执行一次组件协议调用，检查响应信封并保持请求身份一致。
  public ObjectNode invoke(String operation, ObjectNode fields, String requestId) {
    if (operation == null || operation.isEmpty() || fields == null || requestId == null ||
        requestId.isEmpty())
      throw new IllegalArgumentException("operation_fields_and_request_id_required");
    try {
      return nativeCall(nativeApi::te_local_invoke, operation, fields, requestId);
    } catch (RuntimeException error) {
      tryReportGap(operation, fields);
      return fallback(operation, fields, requestId);
    }
  }

  /* 阻塞式评测管理入口，不要在 Agent 业务打点调用。 */
  // 执行评测等阻塞式管理操作，与主 Agent 热路径分离。
  public ObjectNode manage(String operation, ObjectNode fields) {
    return manage(operation, fields, UUID.randomUUID().toString());
  }

  // 执行评测等阻塞式管理操作，与主 Agent 热路径分离。
  public ObjectNode manage(String operation, ObjectNode fields, String requestId) {
    if (operation == null || !operation.startsWith("agent_evaluation.") || fields == null)
      throw new IllegalArgumentException("management_operation_required");
    return nativeCall(nativeApi::te_local_manage, operation, fields, requestId);
  }

  /* 隔离评测入口通过跨进程 HTTP 通信，复用类型化 Java 驱动。 */
  // 通过本模块的评测入口处理请求，执行与采集生命周期由对应契约约束。
  public static boolean evaluate(Evolution.Receiver receive) throws Evolution.Failure {
    return Evolution.evaluate(receive);
  }

  // 尽力提交最小采集缺口标记，不能重发被隐私检查拒绝的内容。
  private void tryReportGap(String operation, ObjectNode fields) {
    try {
      ObjectNode gap = JSON.createObjectNode();
      String gapOperation;
      if ("point".equals(operation) && fields.path("ref").isObject()) {
        gapOperation = "point";
        gap.put("phase", "gap").put("kind", "run");
        gap.set("ref", JSON.createObjectNode().set("run", copy(fields.path("ref").get("run"))));
      } else {
        gapOperation = "capture.gap";
        JsonNode run = fields.get("run_id");
        if (run == null && fields.path("context").isObject())
          run = fields.path("context").get("run_id");
        gap.set("run_id", copy(run));
        gap.put("reason", "observer_unavailable");
      }
      nativeCall(nativeApi::te_local_invoke, gapOperation, gap, UUID.randomUUID().toString());
    } catch (RuntimeException ignored) {
    }
  }

  // 复制跨接口传递的数据，避免返回值引用即将释放的临时缓冲区。
  private static JsonNode copy(JsonNode value) {
    return value == null ? null : value.deepCopy();
  }

  // 构造保持原业务继续执行的降级响应，并记录采集不完整。
  private static ObjectNode fallback(String operation, ObjectNode fields, String requestId) {
    ObjectNode result = JSON.createObjectNode()
                            .put("degraded", true)
                            .put("reason", "local_unavailable")
                            .put("capture_complete", false)
                            .put("policy_applied", false)
                            .put("event_id", "");
    result.set("input", copy(fields.get("input")));
    if ("point".equals(operation)) {
      ObjectNode context = fields.path("context").isObject()
                               ? ((ObjectNode)fields.path("context")).deepCopy()
                               : JSON.createObjectNode();
      String kind = fields.path("kind").asText(context.path("kind").asText("custom"));
      context.put("kind", kind).put("degraded", true);
      context.set("caused_by", JSON.createArrayNode());
      if (!context.hasNonNull("run_id"))
        context.put("run_id", "unobserved-" + requestId);
      result.set("context", context);
      result.put("decision", "loop".equals(kind) ? "continue" : "allow");
      result.put("closed", false).put("ended", false);
    } else if ("run.begin".equals(operation))
      result.put("run_id", "unobserved-" + requestId);
    else if ("activity.begin".equals(operation)) {
      result.put("activity_id", "unobserved-" + requestId).put("decision", "allow");
      result.set("context", JSON.createObjectNode().set("run_id", copy(fields.get("run_id"))));
      ((ObjectNode)result.get("context"))
          .put("activity_id", "unobserved-" + requestId)
          .put("degraded", true);
    } else if ("loop.check".equals(operation)) {
      result.put("control_id", "unobserved-" + requestId)
          .put("decision", "continue")
          .put("context", "");
    } else if ("activity.end".equals(operation) || "activity.abort".equals(operation) ||
               "loop.commit".equals(operation)) {
      result.put("ended", false);
    } else if ("run.end".equals(operation) || "run.abort".equals(operation)) {
      result.put("closed", false).put("evaluation_status", "quarantined");
    }
    return result;
  }

  // 提取打点所属运行身份，用于关联缺口、回执和结束状态。
  private String pointRun(ObjectNode fields) {
    JsonNode run = fields.path("ref").get("run");
    return run == null ? "" : run.asText("");
  }

  // 跟踪本地采集缺口与运行收尾，保证观测丢失最终可见。
  private ObjectNode pointObserved(ObjectNode fields, ObjectNode result) {
    String run = pointRun(fields);
    if (!run.isEmpty() && run.length() <= 256)
      synchronized (pointGaps) {
        if (result.path("degraded").asBoolean(false)) {
          if (pointGaps.size() < 1024)
            pointGaps.add(run);
          else
            pointGapsFull = true;
        } else if (result.path("closed").asBoolean(false) ||
                   result.path("run_closed").asBoolean(false))
          pointGaps.remove(run);
      }
    return result;
  }

  // 提交框架无关打点，必要时在同一边界先取得策略决定，再记录实际执行结果。
  public ObjectNode point(ObjectNode fields) {
    if (fields == null)
      throw new IllegalArgumentException("fields_required");
    ObjectNode request = fields.deepCopy();
    synchronized (pointGaps) {
      if (request.has("ref") && (pointGapsFull || pointGaps.contains(pointRun(fields))))
        request.put("capture_gap", true);
    }
    return pointObserved(fields, invoke("point", request));
  }

  // 包装宿主业务调用：先查询策略，再最多执行一次原业务函数，最后记录结果。
  public JsonNode call(ObjectNode fields, CallExecutor execute) throws Exception {
    return callWithReceipt(fields, execute).output;
  }

  /* 包装一次业务调用；执行器仅运行零次或一次，永不自动重试业务操作。 */
  // 最多执行一次业务函数，并返回实际业务结果、原业务错误和两个打点回执。
  public CallResult callWithReceipt(ObjectNode fields, CallExecutor execute) throws Exception {
    if (fields == null || execute == null)
      throw new IllegalArgumentException("fields_and_executor_required");
    ObjectNode before = fields.deepCopy().put("phase", "before");
    ObjectNode prepared = point(before);
    JsonNode actual =
        prepared.has("input") ? copy(prepared.get("input")) : copy(fields.get("input"));
    ObjectNode identity = finishIdentity(fields, prepared);
    String kind = fields.path("kind").asText("custom");
    if ("reject".equals(prepared.path("decision").asText())) {
      String reason = prepared.path("reason").asText("policy_rejected");
      ObjectNode output = JSON.createObjectNode()
                              .put("status", "rejected")
                              .put("reason", reason)
                              .put("error", reason);
      ObjectNode after =
          identity.deepCopy().put("kind", kind).put("phase", "after").put("executed", false);
      after.set("actual_input", copy(actual));
      after.set("output", output.deepCopy());
      return new CallResult(output, prepared, point(after), false);
    }
    final JsonNode output;
    try {
      output = execute.execute(copy(actual));
    } catch (Exception error) {
      ObjectNode abort =
          identity.deepCopy().put("kind", kind).put("phase", "abort").put("status", "unknown");
      abort.putNull("output");
      point(abort);
      throw error;
    }
    ObjectNode after =
        identity.deepCopy().put("kind", kind).put("phase", "after").put("executed", true);
    after.set("actual_input", copy(actual));
    after.set("output", copy(output));
    return new CallResult(output, prepared, point(after), true);
  }

  // 提交循环边界的真实控制决定，并返回宿主是否继续迭代。
  public boolean allowLoop(ObjectNode fields) {
    return checkLoop(fields).allowed;
  }

  // 返回循环守卫决定及回执，区分引用式和上下文式提交。
  public LoopResult checkLoop(ObjectNode fields) {
    if (fields == null)
      throw new IllegalArgumentException("fields_required");
    ObjectNode request = fields.deepCopy().put("kind", "loop");
    if (fields.has("ref")) {
      ObjectNode receipt = point(request.put("phase", "guard"));
      String decision = receipt.path("decision").asText("continue");
      return new LoopResult(!"stop".equals(decision), decision, receipt, receipt);
    }
    ObjectNode prepared = point(request.put("phase", "before"));
    String decision = prepared.path("decision").asText("continue");
    ObjectNode after = finishIdentity(fields, prepared)
                           .put("kind", "loop")
                           .put("phase", "after")
                           .put("decision", decision);
    return new LoopResult(!"stop".equals(decision), decision, prepared, point(after));
  }

  // 报告当前运行的采集缺口，使后台知道这份轨迹不完整。
  public ObjectNode captureGap(ObjectNode fields, String reason) {
    if (fields == null || reason == null || reason.isEmpty())
      throw new IllegalArgumentException("fields_and_reason_required");
    return invoke("capture.gap", fields.deepCopy().put("reason", reason));
  }

  // 构造与开始打点匹配的结束身份，确保引用和上下文协议不混用。
  private static ObjectNode finishIdentity(ObjectNode fields, ObjectNode prepared) {
    ObjectNode identity = JSON.createObjectNode();
    if (fields.has("ref"))
      identity.set("ref", copy(fields.get("ref")));
    else if (prepared.path("context").isObject())
      identity.set("context", prepared.path("context").deepCopy());
    return identity;
  }

  // 读取组件自身的采集和排队指标，不把观测指标当作业务任务得分。
  public ObjectNode metrics() {
    final String response;
    Lock read = lifecycle.readLock();
    read.lock();
    try {
      Pointer owned = handle;
      if (owned == null || processId != ProcessHandle.current().pid())
        return JSON.createObjectNode();
      response = consume(nativeApi.te_local_metrics(owned));
    } catch (RuntimeException error) {
      return JSON.createObjectNode();
    } finally {
      read.unlock();
    }
    try {
      JsonNode result = JSON.readTree(response);
      return result != null && result.isObject() ? (ObjectNode)result : JSON.createObjectNode();
    } catch (Exception error) {
      return JSON.createObjectNode();
    }
  }

  // 切换组件附加能力，不更改宿主原有业务和安全逻辑。
  public void setEnabled(boolean enabled) {
    Lock read = lifecycle.readLock();
    read.lock();
    try {
      Pointer owned = handle;
      if (owned != null && processId == ProcessHandle.current().pid())
        nativeApi.te_local_set_enabled(owned, enabled ? 1 : 0);
    } finally {
      read.unlock();
    }
  }

  /* 仅用于应用维护或关闭，不应放在业务边界。 */
  // 等待当前待持久化工作完成，用于维护或关闭；不要放在主 Agent 每个调用的热路径。
  public void drain() {
    Lock read = lifecycle.readLock();
    read.lock();
    try {
      Pointer owned = handle;
      if (owned != null && processId == ProcessHandle.current().pid())
        nativeApi.te_local_drain(owned);
    } finally {
      read.unlock();
    }
  }

  // 关闭资源并等待本实现要求的在途操作完成，重复关闭按协议处理。
  @Override
  public void close() {
    Lock write = lifecycle.writeLock();
    write.lock();
    try {
      Pointer owned = handle;
      handle = null;
      if (owned != null && processId == ProcessHandle.current().pid())
        nativeApi.te_local_close(owned);
    } finally {
      write.unlock();
    }
  }
}
