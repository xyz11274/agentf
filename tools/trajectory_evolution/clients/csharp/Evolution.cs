using System.Net;
using System.Net.Http.Headers;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;
using System.Text.RegularExpressions;

namespace TrajectoryEvolution;

public sealed class EvolutionError(string code, int status = 0) : Exception(code)
{
    public int Status { get; } = status;
}

/// <summary>Native HTTP transport; all L0-L3 boundaries use the same InvokeAsync.</summary>
public sealed class Evolution : IDisposable
{
    private readonly Uri endpoint;
    private readonly string token;
    private readonly int timeoutMs, maxBytes, retries;
    private readonly HttpClient http;
    private readonly HashSet<string> pointGaps = new();
    private bool pointGapsFull;
    private string PointRun(JsonObject? fields) => fields?["ref"] is JsonObject reference
        && reference["run"] is JsonValue value && value.TryGetValue<string>(out var run) ? run : "";
    private JsonObject PointObserved(JsonObject? fields, JsonObject result)
    {
        var run = PointRun(fields);
        if (run.Length is > 0 and <= 256) lock (pointGaps)
        {
            if (result["degraded"]?.GetValue<bool>() == true)
            { if (pointGaps.Count < 1024) pointGaps.Add(run); else pointGapsFull = true; }
            else if (result["closed"]?.GetValue<bool>() == true || result["run_closed"]?.GetValue<bool>() == true) pointGaps.Remove(run);
        }
        return result;
    }

    public Evolution(string url, string token, int timeoutMs = 7000, int maxResponseBytes = 2097152, int retries = 1)
    {
        var trialUrl = Environment.GetEnvironmentVariable("TRAJECTORY_EVOLUTION_TRIAL_URL");
        var trialToken = Environment.GetEnvironmentVariable("TRAJECTORY_EVOLUTION_TRIAL_TOKEN");
        if (!string.IsNullOrEmpty(trialUrl) && !string.IsNullOrEmpty(trialToken)) { url = trialUrl; token = trialToken; }
        if (!Uri.TryCreate(url, UriKind.Absolute, out var uri) || uri.UserInfo != "" || uri.Query != "" || uri.Fragment != "" || uri.AbsolutePath != "/"
            || !(uri.Scheme == "https" || (uri.Scheme == "http" && new[] { "localhost", "127.0.0.1", "[::1]" }.Contains(uri.Host))))
            throw new EvolutionError("https_or_loopback_url_required");
        if (token is null || token.Length < 24 || token.Any(c => c < 33 || c > 126)) throw new EvolutionError("token_invalid");
        if (timeoutMs < 100 || timeoutMs > 300000 || maxResponseBytes < 1024 || maxResponseBytes > 16777216 || retries < 0 || retries > 3)
            throw new EvolutionError("client_limits_invalid");
        endpoint = new Uri(uri, "/v1/invoke"); this.token = token;
        this.timeoutMs = timeoutMs; maxBytes = maxResponseBytes; this.retries = retries;
        http = new HttpClient(new HttpClientHandler { AllowAutoRedirect = false, UseProxy = false }) { Timeout = Timeout.InfiniteTimeSpan };
    }

    public async Task<JsonObject> InvokeAsync(string operation, JsonObject fields, string? requestId = null, CancellationToken cancellation = default)
    {
        var request = (JsonObject)fields.DeepClone(); request["operation"] = operation; request["request_id"] = requestId ?? Guid.NewGuid().ToString("N");
        byte[] body;
        try { body = JsonSerializer.SerializeToUtf8Bytes(request); } catch { throw new EvolutionError("request_invalid"); }
        if (body.Length > 2097152) throw new EvolutionError("request_too_large");
        for (var attempt = 0; ; ++attempt)
        {
            try { return await Once(body, cancellation, timeoutMs).ConfigureAwait(false); }
            catch (EvolutionError error) { if (attempt >= retries || (error.Status > 0 && error.Status < 500) || cancellation.IsCancellationRequested) throw; }
        }
    }
    public async Task<JsonObject> PointAsync(JsonObject fields, int budgetMs = 20, CancellationToken cancellation = default)
    {
        var id = Guid.NewGuid().ToString("N");
        try
        {
            if (budgetMs < 1 || budgetMs > 5000) throw new EvolutionError("point_timeout_invalid");
            var packet = (JsonObject)fields.DeepClone(); packet["operation"] = "point"; packet["request_id"] = id;
            lock (pointGaps) if (packet.ContainsKey("ref") && (pointGapsFull || pointGaps.Contains(PointRun(fields)))) packet["capture_gap"] = true;
            var bytes = JsonSerializer.SerializeToUtf8Bytes(packet);
            if (bytes.Length > 2097152) throw new EvolutionError("request_too_large");
            return PointObserved(fields, await Once(bytes, cancellation, budgetMs).ConfigureAwait(false));
        }
        catch
        {
            var context = fields?["context"] is JsonObject parent ? (JsonObject)parent.DeepClone() : new JsonObject();
            var label = fields?["kind"] ?? context["kind"];
            var kind = label is JsonValue name && name.TryGetValue<string>(out var text) ? text : "custom";
            context["kind"] = kind; context["degraded"] = true; context["caused_by"] = new JsonArray(); context["run_id"] ??= "unobserved-" + id;
            return PointObserved(fields, new JsonObject { ["context"] = context, ["input"] = fields?["input"]?.DeepClone(), ["degraded"] = true,
                ["capture_complete"] = false, ["policy_applied"] = false, ["closed"] = false, ["ended"] = false,
                ["decision"] = kind == "loop" ? "continue" : "allow", ["reason"] = "observer_unavailable" });
        }
    }
    private async Task<JsonObject> Once(byte[] body, CancellationToken cancellation, int budgetMs)
    {
        try
        {
            using var deadline = CancellationTokenSource.CreateLinkedTokenSource(cancellation); deadline.CancelAfter(budgetMs);
            using var request = new HttpRequestMessage(HttpMethod.Post, endpoint) { Version = HttpVersion.Version11, VersionPolicy = HttpVersionPolicy.RequestVersionExact, Content = new ByteArrayContent(body) };
            request.Headers.Authorization = new AuthenticationHeaderValue("Bearer", token);
            request.Content.Headers.ContentType = new MediaTypeHeaderValue("application/json");
            using var response = await http.SendAsync(request, HttpCompletionOption.ResponseHeadersRead, deadline.Token).ConfigureAwait(false);
            using var input = await response.Content.ReadAsStreamAsync(deadline.Token).ConfigureAwait(false);
            using var bytes = new MemoryStream(); var part = new byte[8192];
            for (;;)
            {
                var n = await input.ReadAsync(part.AsMemory(), deadline.Token).ConfigureAwait(false); if (n == 0) break;
                if (bytes.Length + n > maxBytes) throw new EvolutionError("response_too_large");
                bytes.Write(part, 0, n);
            }
            JsonObject payload;
            try { payload = JsonNode.Parse(bytes.ToArray()) as JsonObject ?? throw new JsonException(); }
            catch { throw new EvolutionError("response_invalid", (int)response.StatusCode); }
            var ok = payload["ok"] is JsonValue value && value.TryGetValue<bool>(out var flag) && flag;
            if (!response.IsSuccessStatusCode || !ok)
            {
                var code = payload["error"] is JsonValue item && item.TryGetValue<string>(out var text) ? text : "rpc_failed";
                throw new EvolutionError(Regex.IsMatch(code, "^[a-z_]{1,96}$") ? code : "rpc_failed", (int)response.StatusCode);
            }
            var result = payload["result"] as JsonObject ?? throw new EvolutionError("response_invalid", (int)response.StatusCode);
            // A result must be reusable in the host's next JSON object, not owned by our envelope.
            return (JsonObject)result.DeepClone();
        }
        catch (EvolutionError) { throw; }
        catch { throw new EvolutionError("observer_unavailable"); }
    }
    public void Dispose() { http.Dispose(); }
    public override string ToString() => "EvolutionClient";
}
