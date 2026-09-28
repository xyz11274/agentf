// csharp 客户端：通过统一协议收集执行事件并接收策略建议。
// 传输与业务执行分离；接收回执不代表 Agent 已完成整个任务。
// 打点的输入是建议修改前的请求，执行后必须回传实际输入、输出与状态。
// 取消、超时和采集缺口需要显式处理，不能把缺失证据当成正常完成。

using System.Net;
using System.Net.Http.Headers;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;
using System.Text.RegularExpressions;

namespace TrajectoryEvolution;

public sealed class EvolutionError

(string code, int status = 0) : Exception(code) {
  public int Status { get; } = status;
}

/// <summary>原生 HTTP 传输，全部 L0–L3 边界共用同一 InvokeAsync 协议。</summary>
public sealed class Evolution : IDisposable {
  public sealed record CallResult(JsonNode? Output, JsonObject Prepared, JsonObject Receipt,
                                  bool Executed);
  public sealed record LoopResult(bool Allowed, string Decision, JsonObject Prepared,
                                  JsonObject Receipt);

  /// <summary>绑定现有接收函数，交付确认不代表运行已经完成。</summary>
  // 异步驱动隔离评测接收循环，输入交给宿主既有回调，完成由正常打点确认。
  public static async Task<bool> EvaluateAsync(Func<JsonNode?, Task> receive,
                                               CancellationToken cancellation = default) {
    if (Environment.GetEnvironmentVariable("TRAJECTORY_EVOLUTION_EVALUATION_ENTRY") != "1")
      return false;
    using var client =
        new Evolution(Environment.GetEnvironmentVariable("TRAJECTORY_EVOLUTION_TRIAL_URL") ?? "",
                      Environment.GetEnvironmentVariable("TRAJECTORY_EVOLUTION_TRIAL_TOKEN") ?? "",
                      5000, 2097152, 0);
    var consumer = Guid.NewGuid().ToString("N");
    try {
      for (var index = 0;;) {
        var packet = await client.InvokeAsync(
            "evaluation.next", new JsonObject { ["consumer"] = consumer, ["index"] = index },
            cancellation: cancellation);
        var state = packet["state"]?.GetValue<string>();
        if (state == "done")
          return true;
        if (state == "waiting") {
          await Task.Delay(100, cancellation);
          continue;
        }
        if (state != "input" || packet["index"]?.GetValue<int>() != index)
          throw new EvolutionError("evaluation_entry_failed");
        await receive(packet["input"]);
        ++index;
      }
    } catch {
      try {
        await client.InvokeAsync("evaluation.fail", new JsonObject { ["consumer"] = consumer });
      } catch {
      }
      throw new EvolutionError("evaluation_entry_failed");
    }
  }

  private readonly Uri endpoint;
  private readonly string token;
  private readonly int timeoutMs, maxBytes, retries;
  private readonly HttpClient http;
  private readonly HashSet<string> pointGaps = new();
  private bool pointGapsFull;
  // 提取打点的运行身份，供降级跟踪和收尾使用。
  private string PointRun(JsonObject? fields) => fields?["ref"] is JsonObject reference
                                                         && reference["run"] is JsonValue value
                                                         && value.TryGetValue<string>(out var run)
                                                     ? run
                                                     : "";

  // 跟踪本地观测是否完整，运行终结时清理对应状态。
  private JsonObject PointObserved(JsonObject? fields, JsonObject result) {
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

  // 初始化 Evolution 的状态和资源；输入参数的校验与所有权在构造阶段确定。
  // 对象创建后复用既有配置，实际执行与收尾分别由后续接口负责。
  public Evolution(string url, string token, int timeoutMs = 7000, int maxResponseBytes = 2097152,
                   int retries = 1) {
    var trialUrl = Environment.GetEnvironmentVariable("TRAJECTORY_EVOLUTION_TRIAL_URL");
    var trialToken = Environment.GetEnvironmentVariable("TRAJECTORY_EVOLUTION_TRIAL_TOKEN");
    if (!string.IsNullOrEmpty(trialUrl) && !string.IsNullOrEmpty(trialToken)) {
      url = trialUrl;
      token = trialToken;
    }
    if (!Uri.TryCreate(url, UriKind.Absolute, out var uri) || uri.UserInfo != "" ||
        uri.Query != "" || uri.Fragment != "" || uri.AbsolutePath != "/" ||
        !(uri.Scheme == "https" ||
          (uri.Scheme == "http" && new[] { "localhost", "127.0.0.1", "[::1]" }.Contains(uri.Host))))
      throw new EvolutionError("https_or_loopback_url_required");
    if (token is null || token.Length < 24 || token.Any(c => c < 33 || c > 126))
      throw new EvolutionError("token_invalid");
    if (timeoutMs < 100 || timeoutMs > 300000 || maxResponseBytes < 1024 ||
        maxResponseBytes > 16777216 || retries < 0 || retries > 3)
      throw new EvolutionError("client_limits_invalid");
    endpoint = new Uri(uri, "/v1/invoke");
    this.token = token;
    this.timeoutMs = timeoutMs;
    maxBytes = maxResponseBytes;
    this.retries = retries;
    http = new HttpClient(new HttpClientHandler { AllowAutoRedirect = false, UseProxy = false }) {
      Timeout = Timeout.InfiniteTimeSpan
    };
  }

  // 异步提交组件请求，使用统一超时和幂等身份。
  public async Task<JsonObject> InvokeAsync(string operation, JsonObject fields,
                                            string? requestId = null,
                                            CancellationToken cancellation = default) {
    var request = (JsonObject)fields.DeepClone();
    request["operation"] = operation;
    request["request_id"] = requestId ?? Guid.NewGuid().ToString("N");
    byte[] body;
    try {
      body = JsonSerializer.SerializeToUtf8Bytes(request);
    } catch {
      throw new EvolutionError("request_invalid");
    }
    if (body.Length > 2097152)
      throw new EvolutionError("request_too_large");
    for (var attempt = 0;; ++attempt) {
      try {
        return await Once(body, cancellation, timeoutMs).ConfigureAwait(false);
      } catch (EvolutionError error) {
        if (attempt >= retries || (error.Status > 0 && error.Status < 500) ||
            cancellation.IsCancellationRequested)
          throw;
      }
    }
  }

  /// <summary>包装一次既有 JSON 调用，业务委托最多执行并等待一次。</summary>
  // 异步包装一次业务调用，先取得策略决定，再执行并记录结果；业务回调不重试。
  public async Task<JsonNode?>
  CallAsync(JsonObject fields, Func<JsonNode?, Task<JsonNode?>> execute, int budgetMs = 20,
            CancellationToken cancellation = default) =>
      (await CallWithReceiptAsync(fields, execute, budgetMs, cancellation).ConfigureAwait(false))
          .Output;

  /// <summary>包装业务调用，同时返回前置策略决定和结束回执。</summary>
  // 异步返回业务结果与前后两个回执，观测异常不能替换业务异常。
  public async Task<CallResult> CallWithReceiptAsync(JsonObject fields,
                                                     Func<JsonNode?, Task<JsonNode?>> execute,
                                                     int budgetMs = 20,
                                                     CancellationToken cancellation = default) {
    ArgumentNullException.ThrowIfNull(fields);
    ArgumentNullException.ThrowIfNull(execute);
    var before = (JsonObject)fields.DeepClone();
    before["phase"] = "before";
    var prepared = await PointAsync(before, budgetMs, cancellation).ConfigureAwait(false);
    var actual = prepared.ContainsKey("input") ? prepared["input"]?.DeepClone()
                                               : fields["input"]?.DeepClone();
    var identity = FinishIdentity(fields, prepared);
    var kind = fields["kind"] is JsonValue label && label.TryGetValue<string>(out var name)
                   ? name
                   : "custom";
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
      var receipt = await PointAsync(after, budgetMs, cancellation).ConfigureAwait(false);
      return new CallResult(output, prepared, receipt, false);
    }
    JsonNode ? result;
    try {
      result = await execute(actual?.DeepClone()).ConfigureAwait(false);
    } catch {
      var abort = (JsonObject)identity.DeepClone();
      abort["kind"] = kind;
      abort["phase"] = "abort";
      abort["status"] = "unknown";
      abort["output"] = null;
      await PointAsync(abort, budgetMs, CancellationToken.None).ConfigureAwait(false);
      throw;
    }
    var completed = (JsonObject)identity.DeepClone();
    completed["kind"] = kind;
    completed["phase"] = "after";
    completed["executed"] = true;
    completed["actual_input"] = actual?.DeepClone();
    completed["output"] = result?.DeepClone();
    var terminal = await PointAsync(completed, budgetMs, cancellation).ConfigureAwait(false);
    return new CallResult(result, prepared, terminal, true);
  }

  // 异步取得并提交循环决定，保留宿主循环的原执行方式。
  public async Task<bool> AllowLoopAsync(JsonObject fields, int budgetMs = 20,
                                         CancellationToken cancellation = default) =>
      (await CheckLoopAsync(fields, budgetMs, cancellation).ConfigureAwait(false)).Allowed;

  // 异步返回循环决定和完整回执，便于观察真实应用情况。
  public async Task<LoopResult> CheckLoopAsync(JsonObject fields, int budgetMs = 20,
                                               CancellationToken cancellation = default) {
    ArgumentNullException.ThrowIfNull(fields);
    var request = (JsonObject)fields.DeepClone();
    request["kind"] = "loop";
    if (fields.ContainsKey("ref")) {
      request["phase"] = "guard";
      var receipt = await PointAsync(request, budgetMs, cancellation).ConfigureAwait(false);
      var decision = receipt["decision"]?.GetValue<string>() ?? "continue";
      return new LoopResult(decision != "stop", decision, receipt, receipt);
    }
    request["phase"] = "before";
    var prepared = await PointAsync(request, budgetMs, cancellation).ConfigureAwait(false);
    var selected = prepared["decision"]?.GetValue<string>() ?? "continue";
    var after = FinishIdentity(fields, prepared);
    after["kind"] = "loop";
    after["phase"] = "after";
    after["decision"] = selected;
    var terminal = await PointAsync(after, budgetMs, cancellation).ConfigureAwait(false);
    return new LoopResult(selected != "stop", selected, prepared, terminal);
  }

  // 异步上报不含敏感正文的采集缺口。
  public Task<JsonObject> CaptureGapAsync(JsonObject fields, string reason,
                                          CancellationToken cancellation = default) {
    ArgumentNullException.ThrowIfNull(fields);
    if (string.IsNullOrEmpty(reason))
      throw new ArgumentException("reason_required", nameof(reason));
    var request = (JsonObject)fields.DeepClone();
    request["reason"] = reason;
    return InvokeAsync("capture.gap", request, cancellation: cancellation);
  }

  // 异步获取隔离评测下一轮输入，等待前一轮真实结束。
  public Task<JsonObject> EvaluationNextAsync(string consumer, int index,
                                              CancellationToken cancellation = default) =>
      InvokeAsync("evaluation.next", new JsonObject { ["consumer"] = consumer, ["index"] = index },
                  cancellation: cancellation);
  // 异步记录评测入口失败，使用固定诊断。
  public Task<JsonObject>
  EvaluationFailAsync(string consumer, CancellationToken cancellation = default) => InvokeAsync(
      "evaluation.fail", new JsonObject { ["consumer"] = consumer }, cancellation: cancellation);

  // 依据准备回执构造对应结束身份，保证前后打点配对。
  private static JsonObject FinishIdentity(JsonObject fields, JsonObject prepared) {
    var identity = new JsonObject();
    if (fields["ref"] is JsonNode reference)
      identity["ref"] = reference.DeepClone();
    else if (prepared["context"] is JsonNode context)
      identity["context"] = context.DeepClone();
    return identity;
  }

  // 异步提交打点并返回策略与采集回执，失败时按协议降级。
  public async Task<JsonObject> PointAsync(JsonObject fields, int budgetMs = 20,
                                           CancellationToken cancellation = default) {
    var id = Guid.NewGuid().ToString("N");
    try {
      if (budgetMs < 1 || budgetMs > 5000)
        throw new EvolutionError("point_timeout_invalid");
      var packet = (JsonObject)fields.DeepClone();
      packet["operation"] = "point";
      packet["request_id"] = id;
      lock (pointGaps) if (packet.ContainsKey("ref") &&
                           (pointGapsFull || pointGaps.Contains(PointRun(fields))))
          packet["capture_gap"] = true;
      var bytes = JsonSerializer.SerializeToUtf8Bytes(packet);
      if (bytes.Length > 2097152)
        throw new EvolutionError("request_too_large");
      return PointObserved(fields, await Once(bytes, cancellation, budgetMs).ConfigureAwait(false));
    } catch {
      var context = fields?["context"] is JsonObject parent ? (JsonObject)parent.DeepClone()
                                                            : new JsonObject();
      var label = fields?["kind"] ?? context["kind"];
      var kind =
          label is JsonValue name && name.TryGetValue<string>(out var text) ? text : "custom";
      context["kind"] = kind;
      context["degraded"] = true;
      context["caused_by"] = new JsonArray();
      context["run_id"] ??= "unobserved-" + id;
      return PointObserved(
          fields,
          new JsonObject { ["context"] = context, ["input"] = fields?["input"]?.DeepClone(),
                           ["degraded"] = true, ["capture_complete"] = false,
                           ["policy_applied"] = false, ["closed"] = false, ["ended"] = false,
                           ["decision"] = kind == "loop" ? "continue" : "allow",
                           ["reason"] = "observer_unavailable" });
    }
  }

  // 在统一预算内执行一次传输尝试，重试由外层幂等逻辑控制。
  private async Task<JsonObject> Once(byte[] body, CancellationToken cancellation, int budgetMs) {
    try {
      using var deadline = CancellationTokenSource.CreateLinkedTokenSource(cancellation);
      deadline.CancelAfter(budgetMs);
      using var request = new HttpRequestMessage(HttpMethod.Post, endpoint) {
        Version = HttpVersion.Version11, VersionPolicy = HttpVersionPolicy.RequestVersionExact,
        Content = new ByteArrayContent(body)
      };
      request.Headers.Authorization = new AuthenticationHeaderValue("Bearer", token);
      request.Content.Headers.ContentType = new MediaTypeHeaderValue("application/json");
      using var response =
          await http.SendAsync(request, HttpCompletionOption.ResponseHeadersRead, deadline.Token)
              .ConfigureAwait(false);
      using var input =
          await response.Content.ReadAsStreamAsync(deadline.Token).ConfigureAwait(false);
      using var bytes = new MemoryStream();
      var part = new byte[8192];
      for (;;) {
        var n = await input.ReadAsync(part.AsMemory(), deadline.Token).ConfigureAwait(false);
        if (n == 0)
          break;
        if (bytes.Length + n > maxBytes)
          throw new EvolutionError("response_too_large");
        bytes.Write(part, 0, n);
      }
      JsonObject payload;
      try {
        payload = JsonNode.Parse(bytes.ToArray()) as JsonObject ?? throw new JsonException();
      } catch {
        throw new EvolutionError("response_invalid", (int)response.StatusCode);
      }
      var ok = payload["ok"] is JsonValue value && value.TryGetValue<bool>(out var flag) && flag;
      if (!response.IsSuccessStatusCode || !ok) {
        var code = payload["error"] is JsonValue item && item.TryGetValue<string>(out var text)
                       ? text
                       : "rpc_failed";
        throw new EvolutionError(Regex.IsMatch(code, "^[a-z_]{1,96}$") ? code : "rpc_failed",
                                 (int)response.StatusCode);
      }
      var result = payload["result"] as JsonObject ??
                   throw new EvolutionError("response_invalid", (int)response.StatusCode);
      // 返回结果应能放入宿主的下一个 JSON 对象，不能依赖当前信封的生命周期。
      return (JsonObject)result.DeepClone();
    } catch (EvolutionError) {
      throw;
    } catch {
      throw new EvolutionError("observer_unavailable");
    }
  }

  // 释放句柄及网络资源，禁止在释放后继续使用对象。
  public void Dispose() {
    http.Dispose();
  }

  // 返回不包含凭据与业务正文的安全诊断表示。
  public override string ToString() => "EvolutionClient";
}
