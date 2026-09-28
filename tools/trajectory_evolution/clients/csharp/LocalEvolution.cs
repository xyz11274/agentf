// csharp 客户端：通过统一协议收集执行事件并接收策略建议。
// 原生入口复用包内 C ABI；注意句柄生命周期与跨语言内存释放。
// 打点的输入是建议修改前的请求，执行后必须回传实际输入、输出与状态。
// 取消、超时和采集缺口需要显式处理，不能把缺失证据当成正常完成。

using System.Runtime.InteropServices;
using System.Text.Json;
using System.Text.Json.Nodes;
using System.Threading;

namespace TrajectoryEvolution;

/// <summary>稳定 C 接口的进程内绑定，不额外依赖网络客户端。</summary>
public sealed class LocalEvolution : IDisposable {
  [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
  private delegate nint OpenNative([MarshalAs(UnmanagedType.LPUTF8Str)] string config,
                                   [MarshalAs(UnmanagedType.LPUTF8Str)] string token,
                                   [MarshalAs(UnmanagedType.LPUTF8Str)] string options,
                                   out nint error);
  [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
  private delegate nint InvokeNative(nint handle, nint request, nuint length);
  [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
  private delegate nint MetricsNative(nint handle);
  [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
  private delegate void EnabledNative(nint handle, int enabled);
  [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
  private delegate void HandleNative(nint handle);
  [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
  private delegate void FreeNative(nint value);

  public sealed record CallResult(JsonNode? Output, JsonObject Prepared, JsonObject Receipt,
                                  bool Executed);
  public sealed record LoopResult(bool Allowed, string Decision, JsonObject Prepared,
                                  JsonObject Receipt);

  private nint library;
  private nint handle;
  private readonly int processId, maxRequestBytes;
  private readonly InvokeNative invokeNative, manageNative;
  private readonly MetricsNative metricsNative;
  private readonly EnabledNative enabledNative;
  private readonly HandleNative drainNative = null!, closeNative = null!;
  private readonly FreeNative freeNative;
  private readonly HashSet<string> pointGaps = new();
  private readonly ReaderWriterLockSlim lifecycle = new();
  private bool pointGapsFull;

  // 初始化 LocalEvolution 的状态和资源；输入参数的校验与所有权在构造阶段确定。
  // 对象创建后复用既有配置，实际执行与收尾分别由后续接口负责。
  public LocalEvolution(string libraryPath, string configPath, string token,
                        JsonObject? options = null) {
    ArgumentException.ThrowIfNullOrEmpty(libraryPath);
    ArgumentException.ThrowIfNullOrEmpty(configPath);
    ArgumentException.ThrowIfNullOrEmpty(token);
    library = NativeLibrary.Load(libraryPath);
    try {
      var open = Export<OpenNative>("te_local_open");
      invokeNative = Export<InvokeNative>("te_local_invoke");
      manageNative = Export<InvokeNative>("te_local_manage");
      metricsNative = Export<MetricsNative>("te_local_metrics");
      enabledNative = Export<EnabledNative>("te_local_set_enabled");
      drainNative = Export<HandleNative>("te_local_drain");
      closeNative = Export<HandleNative>("te_local_close");
      freeNative = Export<FreeNative>("te_local_free");
      maxRequestBytes = options?["max_request_bytes"]?.GetValue<int>() ?? 65536;
      var optionsText = options?.ToJsonString() ?? "{}";
      handle = open(configPath, token, optionsText, out var error);
      if (error != 0)
        freeNative(error);
      if (handle == 0)
        throw new EvolutionError("local_initialization_failed");
      processId = Environment.ProcessId;
    } catch {
      if (handle != 0)
        closeNative(handle);
      NativeLibrary.Free(library);
      throw;
    }
  }

  // 按名称解析原生函数导出，缺失接口时直接报告初始化失败。
  private T Export<T>(string name)
      where T : Delegate => Marshal.GetDelegateForFunctionPointer<T>(NativeLibrary.GetExport(library,
                name));

  // 在句柄生命周期保护下调用原生函数并管理返回值内存。
  private JsonObject NativeCall(InvokeNative call, string operation, JsonObject fields,
  string requestId) {
    lifecycle.EnterReadLock();
    try {
      if (handle == 0 || processId != Environment.ProcessId)
        throw new EvolutionError("local_unavailable");
      var packet = (JsonObject)fields.DeepClone();
      packet["operation"] = operation;
      packet["request_id"] = requestId;
      byte[] body;
      try {
        body = JsonSerializer.SerializeToUtf8Bytes(packet);
      } catch {
        throw new EvolutionError("request_invalid");
      }
      if (body.Length > maxRequestBytes)
        throw new EvolutionError("local_request_too_large");
      var input = Marshal.AllocHGlobal(body.Length);
      try {
        Marshal.Copy(body, 0, input, body.Length);
        var output = call(handle, input, (nuint)body.Length);
        if (output == 0)
          throw new EvolutionError("local_unavailable");
        string text;
        try {
          text = Marshal.PtrToStringUTF8(output) ?? "";
        } finally {
          freeNative(output);
        }
        JsonObject envelope;
        try {
          envelope = JsonNode.Parse(text) as JsonObject ?? throw new JsonException();
        } catch {
          throw new EvolutionError("response_invalid");
        }
        if (envelope["ok"]?.GetValue<bool>() != true)
          throw new EvolutionError(envelope["error"]?.GetValue<string>() ?? "local_unavailable");
        return (JsonObject)(envelope["result"]?.DeepClone() ??
                            throw new EvolutionError("response_invalid"));
      } finally {
        Marshal.FreeHGlobal(input);
      }
    } finally {
      lifecycle.ExitReadLock();
    }
  }

  // 执行当前客户端协议请求，校验响应并保留请求身份。
  public JsonObject Invoke(string operation, JsonObject fields, string? requestId = null) {
    requestId ??= Guid.NewGuid().ToString("N");
    try {
      return NativeCall(invokeNative, operation, fields, requestId);
    } catch {
      TryReportGap(operation, fields);
      return Fallback(operation, fields, requestId);
    }
  }

  // 执行后台评测管理操作，不能用于主 Agent 低延迟边界。
  public JsonObject Manage(string operation, JsonObject fields, string? requestId = null) =>
      NativeCall(manageNative, operation, fields, requestId ?? Guid.NewGuid().ToString("N"));

  // 尽力报告不含业务内容的采集缺口，失败不能递归触发更多业务异常。
  private void TryReportGap(string operation, JsonObject fields) {
    try {
      JsonObject gap;
      if (operation == "point" && fields["ref"] is JsonObject reference)
        gap =
            new JsonObject { ["phase"] = "gap", ["kind"] = "run",
                             ["ref"] = new JsonObject { ["run"] = reference["run"]?.DeepClone() } };
      else {
        var runId = fields["run_id"]?.DeepClone();
        if (runId is null && fields["context"] is JsonObject context)
          runId = context["run_id"]?.DeepClone();
        gap = new JsonObject { ["run_id"] = runId, ["reason"] = "observer_unavailable" };
      }
      NativeCall(invokeNative, operation == "point" ? "point" : "capture.gap", gap,
                 Guid.NewGuid().ToString("N"));
    } catch {
    }
  }

  // 观测不可用时返回原始请求与降级状态，不生成假成功回执。
  private static JsonObject Fallback(string operation, JsonObject fields, string requestId) {
    var result = new JsonObject { ["degraded"] = true,
                                  ["reason"] = "local_unavailable",
                                  ["capture_complete"] = false,
                                  ["policy_applied"] = false,
                                  ["input"] = fields["input"]?.DeepClone(),
                                  ["event_id"] = "" };
    if (operation == "point") {
      var context = fields["context"] is JsonObject original ? (JsonObject)original.DeepClone()
                                                             : new JsonObject();
      var kind =
          fields["kind"]?.GetValue<string>() ?? context["kind"]?.GetValue<string>() ?? "custom";
      context["kind"] = kind;
      context["degraded"] = true;
      context["caused_by"] = new JsonArray();
      context["run_id"] ??= "unobserved-" + requestId;
      result["context"] = context;
      result["decision"] = kind == "loop" ? "continue" : "allow";
      result["closed"] = false;
      result["ended"] = false;
    } else if (operation == "run.begin")
      result["run_id"] = "unobserved-" + requestId;
    else if (operation == "activity.begin") {
      result["activity_id"] = "unobserved-" + requestId;
      result["decision"] = "allow";
      result["context"] = new JsonObject { ["run_id"] = fields["run_id"]?.DeepClone(),
                                           ["activity_id"] = result["activity_id"]?.DeepClone(),
                                           ["degraded"] = true };
    } else if (operation == "loop.check") {
      result["control_id"] = "unobserved-" + requestId;
      result["decision"] = "continue";
      result["context"] = "";
    } else if (operation is "activity.end" or "activity.abort" or "loop.commit")
      result["ended"] = false;
    else if (operation is "run.end" or "run.abort") {
      result["closed"] = false;
      result["evaluation_status"] = "quarantined";
    }
    return result;
  }

  // 提取打点的运行身份，供降级跟踪和收尾使用。
  private string PointRun(JsonObject fields) => fields["ref"] is JsonObject reference
                                                        && reference["run"] is JsonValue value
                                                        && value.TryGetValue<string>(out var run)
                                                    ? run
                                                    : "";

  // 跟踪本地观测是否完整，运行终结时清理对应状态。
  private JsonObject PointObserved(JsonObject fields, JsonObject result) {
    var run = PointRun(fields);
    if (run.Length is > 0 and <= 256)
      lock (pointGaps) {
        if (result["degraded"]?.GetValue<bool>() == true) {
          if (pointGaps.Count < 1024)
            pointGaps.Add(run);
          else
            pointGapsFull = true;
        } else if (result["closed"]?.GetValue<bool>() == true ||
                   result["run_closed"]?.GetValue<bool>() == true)
          pointGaps.Remove(run);
      }
    return result;
  }

  // 提交单个打点，保持策略准备在业务执行前、结果采集在执行后。
  public JsonObject Point(JsonObject fields) {
    var request = (JsonObject)fields.DeepClone();
    lock (pointGaps) if (request.ContainsKey("ref") &&
                         (pointGapsFull || pointGaps.Contains(PointRun(fields))))
        request["capture_gap"] = true;
    return PointObserved(fields, Invoke("point", request));
  }

  // 最多执行一次宿主业务操作，策略拒绝时不调用业务回调。
  public JsonNode? Call(JsonObject fields, Func<JsonNode?, JsonNode?> execute) =>
      CallWithReceipt(fields, execute).Output;

  // 返回原业务结果及前后回执，便于检查注入是否真正采用。
  public CallResult CallWithReceipt(JsonObject fields, Func<JsonNode?, JsonNode?> execute) {
    ArgumentNullException.ThrowIfNull(fields);
    ArgumentNullException.ThrowIfNull(execute);
    var before = (JsonObject)fields.DeepClone();
    before["phase"] = "before";
    var prepared = Point(before);
    var actual = prepared.ContainsKey("input") ? prepared["input"]?.DeepClone()
                                               : fields["input"]?.DeepClone();
    var identity = FinishIdentity(fields, prepared);
    var kind = fields["kind"]?.GetValue<string>() ?? "custom";
    if (prepared["decision"]?.GetValue<string>() == "reject") {
      var reason = prepared["reason"]?.GetValue<string>() ?? "policy_rejected";
      JsonNode output =
          new JsonObject { ["status"] = "rejected", ["reason"] = reason, ["error"] = reason };
      var after = (JsonObject)identity.DeepClone();
      after["kind"] = kind;
      after["phase"] = "after";
      after["executed"] = false;
      after["actual_input"] = actual?.DeepClone();
      after["output"] = output.DeepClone();
      return new CallResult(output, prepared, Point(after), false);
    }
    JsonNode ? result;
    try {
      result = execute(actual?.DeepClone());
    } catch {
      var abort = (JsonObject)identity.DeepClone();
      abort["kind"] = kind;
      abort["phase"] = "abort";
      abort["status"] = "unknown";
      Point(abort);
      throw;
    }
    var completed = (JsonObject)identity.DeepClone();
    completed["kind"] = kind;
    completed["phase"] = "after";
    completed["executed"] = true;
    completed["actual_input"] = actual?.DeepClone();
    completed["output"] = result?.DeepClone();
    return new CallResult(result, prepared, Point(completed), true);
  }

  // 取得并提交循环决定，返回是否允许继续。
  public bool AllowLoop(JsonObject fields) => CheckLoop(fields).Allowed;

  // 返回循环守卫与提交回执，保留两种打点协议。
  public LoopResult CheckLoop(JsonObject fields) {
    var request = (JsonObject)fields.DeepClone();
    request["kind"] = "loop";
    if (fields.ContainsKey("ref")) {
      request["phase"] = "guard";
      var receipt = Point(request);
      var decision = receipt["decision"]?.GetValue<string>() ?? "continue";
      return new LoopResult(decision != "stop", decision, receipt, receipt);
    }
    request["phase"] = "before";
    var prepared = Point(request);
    var selected = prepared["decision"]?.GetValue<string>() ?? "continue";
    var after = FinishIdentity(fields, prepared);
    after["kind"] = "loop";
    after["phase"] = "after";
    after["decision"] = selected;
    return new LoopResult(selected != "stop", selected, prepared, Point(after));
  }

  // 显式记录采集缺口，不将缺失轨迹作为完整样本。
  public JsonObject CaptureGap(JsonObject fields, string reason) {
    ArgumentNullException.ThrowIfNull(fields);
    if (string.IsNullOrEmpty(reason))
      throw new ArgumentException("reason_required", nameof(reason));
    var request = (JsonObject)fields.DeepClone();
    request["reason"] = reason;
    return Invoke("capture.gap", request);
  }

  // 读取组件运行指标，不触发业务执行或模型请求。
  public JsonObject Metrics() {
    lifecycle.EnterReadLock();
    try {
      if (handle == 0 || processId != Environment.ProcessId)
        return new JsonObject();
      var output = metricsNative(handle);
      if (output == 0)
        return new JsonObject();
      try {
        return JsonNode.Parse(Marshal.PtrToStringUTF8(output) ?? "{}") as JsonObject ??
               new JsonObject();
      } finally {
        freeNative(output);
      }
    } finally {
      lifecycle.ExitReadLock();
    }
  }

  // 切换附加进化能力，宿主原执行流程仍可独立运行。
  public void SetEnabled(bool enabled) {
    lifecycle.EnterReadLock();
    try {
      if (handle != 0 && processId == Environment.ProcessId)
        enabledNative(handle, enabled ? 1 : 0);
    } finally {
      lifecycle.ExitReadLock();
    }
  }

  // 等待已接受的缓冲工作完成，仅在维护或关闭阶段使用。
  public void Drain() {
    lifecycle.EnterReadLock();
    try {
      if (handle != 0 && processId == Environment.ProcessId)
        drainNative(handle);
    } finally {
      lifecycle.ExitReadLock();
    }
  }

  // 依据准备回执构造对应结束身份，保证前后打点配对。
  private static JsonObject FinishIdentity(JsonObject fields, JsonObject prepared) {
    var identity = new JsonObject();
    if (fields["ref"] is JsonNode reference)
      identity["ref"] = reference.DeepClone();
    else if (prepared["context"] is JsonNode context)
      identity["context"] = context.DeepClone();
    return identity;
  }

  // 释放句柄及网络资源，禁止在释放后继续使用对象。
  public void Dispose() {
    lifecycle.EnterWriteLock();
    try {
      var owned = handle;
      var ownedLibrary = library;
      handle = 0;
      library = 0;
      if (owned != 0 && processId == Environment.ProcessId)
        closeNative(owned);
      if (ownedLibrary != 0)
        NativeLibrary.Free(ownedLibrary);
    } finally {
      lifecycle.ExitWriteLock();
    }
  }
}
