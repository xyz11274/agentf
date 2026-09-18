using System.Runtime.InteropServices;
using System.Text.Json;
using System.Text.Json.Nodes;
using System.Threading;

namespace TrajectoryEvolution;

/// <summary>Zero-dependency in-process binding for the stable trajectory-evolution C ABI.</summary>
public sealed class LocalEvolution : IDisposable
{
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate nint OpenNative(
        [MarshalAs(UnmanagedType.LPUTF8Str)] string config,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string token,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string options,
        out nint error);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate nint InvokeNative(nint handle, nint request, nuint length);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate nint MetricsNative(nint handle);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate void EnabledNative(nint handle, int enabled);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate void HandleNative(nint handle);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate void FreeNative(nint value);

    public sealed record CallResult(JsonNode? Output, JsonObject Prepared, JsonObject Receipt, bool Executed);
    public sealed record LoopResult(bool Allowed, string Decision, JsonObject Prepared, JsonObject Receipt);

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

    public LocalEvolution(string libraryPath, string configPath, string token, JsonObject? options = null)
    {
        ArgumentException.ThrowIfNullOrEmpty(libraryPath); ArgumentException.ThrowIfNullOrEmpty(configPath);
        ArgumentException.ThrowIfNullOrEmpty(token);
        library = NativeLibrary.Load(libraryPath);
        try
        {
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
            if (error != 0) freeNative(error);
            if (handle == 0) throw new EvolutionError("local_initialization_failed");
            processId = Environment.ProcessId;
        }
        catch
        {
            if (handle != 0) closeNative(handle);
            NativeLibrary.Free(library); throw;
        }
    }

    private T Export<T>(string name) where T : Delegate =>
        Marshal.GetDelegateForFunctionPointer<T>(NativeLibrary.GetExport(library, name));

    private JsonObject NativeCall(InvokeNative call, string operation, JsonObject fields, string requestId)
    {
        lifecycle.EnterReadLock();
        try
        {
            if (handle == 0 || processId != Environment.ProcessId) throw new EvolutionError("local_unavailable");
            var packet = (JsonObject)fields.DeepClone(); packet["operation"] = operation; packet["request_id"] = requestId;
            byte[] body;
            try { body = JsonSerializer.SerializeToUtf8Bytes(packet); }
            catch { throw new EvolutionError("request_invalid"); }
            if (body.Length > maxRequestBytes) throw new EvolutionError("local_request_too_large");
            var input = Marshal.AllocHGlobal(body.Length);
            try
            {
                Marshal.Copy(body, 0, input, body.Length);
                var output = call(handle, input, (nuint)body.Length);
                if (output == 0) throw new EvolutionError("local_unavailable");
                string text;
                try { text = Marshal.PtrToStringUTF8(output) ?? ""; }
                finally { freeNative(output); }
                JsonObject envelope;
                try { envelope = JsonNode.Parse(text) as JsonObject ?? throw new JsonException(); }
                catch { throw new EvolutionError("response_invalid"); }
                if (envelope["ok"]?.GetValue<bool>() != true)
                    throw new EvolutionError(envelope["error"]?.GetValue<string>() ?? "local_unavailable");
                return (JsonObject)(envelope["result"]?.DeepClone() ?? throw new EvolutionError("response_invalid"));
            }
            finally { Marshal.FreeHGlobal(input); }
        }
        finally { lifecycle.ExitReadLock(); }
    }

    public JsonObject Invoke(string operation, JsonObject fields, string? requestId = null)
    {
        requestId ??= Guid.NewGuid().ToString("N");
        try { return NativeCall(invokeNative, operation, fields, requestId); }
        catch
        {
            TryReportGap(operation, fields);
            return Fallback(operation, fields, requestId);
        }
    }

    public JsonObject Manage(string operation, JsonObject fields, string? requestId = null) =>
        NativeCall(manageNative, operation, fields, requestId ?? Guid.NewGuid().ToString("N"));

    private void TryReportGap(string operation, JsonObject fields)
    {
        try
        {
            JsonObject gap;
            if (operation == "point" && fields["ref"] is JsonObject reference)
                gap = new JsonObject { ["phase"] = "gap", ["kind"] = "run",
                    ["ref"] = new JsonObject { ["run"] = reference["run"]?.DeepClone() } };
            else
            {
                var runId = fields["run_id"]?.DeepClone();
                if (runId is null && fields["context"] is JsonObject context) runId = context["run_id"]?.DeepClone();
                gap = new JsonObject { ["run_id"] = runId, ["reason"] = "observer_unavailable" };
            }
            NativeCall(invokeNative, operation == "point" ? "point" : "capture.gap", gap, Guid.NewGuid().ToString("N"));
        }
        catch { }
    }

    private static JsonObject Fallback(string operation, JsonObject fields, string requestId)
    {
        var result = new JsonObject { ["degraded"] = true, ["reason"] = "local_unavailable",
            ["capture_complete"] = false, ["policy_applied"] = false, ["input"] = fields["input"]?.DeepClone(), ["event_id"] = "" };
        if (operation == "point")
        {
            var context = fields["context"] is JsonObject original ? (JsonObject)original.DeepClone() : new JsonObject();
            var kind = fields["kind"]?.GetValue<string>() ?? context["kind"]?.GetValue<string>() ?? "custom";
            context["kind"] = kind; context["degraded"] = true; context["caused_by"] = new JsonArray();
            context["run_id"] ??= "unobserved-" + requestId;
            result["context"] = context; result["decision"] = kind == "loop" ? "continue" : "allow";
            result["closed"] = false; result["ended"] = false;
        }
        else if (operation == "run.begin") result["run_id"] = "unobserved-" + requestId;
        else if (operation == "activity.begin")
        {
            result["activity_id"] = "unobserved-" + requestId; result["decision"] = "allow";
            result["context"] = new JsonObject { ["run_id"] = fields["run_id"]?.DeepClone(),
                ["activity_id"] = result["activity_id"]?.DeepClone(), ["degraded"] = true };
        }
        else if (operation == "loop.check")
        { result["control_id"] = "unobserved-" + requestId; result["decision"] = "continue"; result["context"] = ""; }
        else if (operation is "activity.end" or "activity.abort" or "loop.commit") result["ended"] = false;
        else if (operation is "run.end" or "run.abort") { result["closed"] = false; result["evaluation_status"] = "quarantined"; }
        return result;
    }

    private string PointRun(JsonObject fields) => fields["ref"] is JsonObject reference
        && reference["run"] is JsonValue value && value.TryGetValue<string>(out var run) ? run : "";
    private JsonObject PointObserved(JsonObject fields, JsonObject result)
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

    public JsonObject Point(JsonObject fields)
    {
        var request = (JsonObject)fields.DeepClone();
        lock (pointGaps) if (request.ContainsKey("ref") && (pointGapsFull || pointGaps.Contains(PointRun(fields)))) request["capture_gap"] = true;
        return PointObserved(fields, Invoke("point", request));
    }

    public JsonNode? Call(JsonObject fields, Func<JsonNode?, JsonNode?> execute) => CallWithReceipt(fields, execute).Output;
    public CallResult CallWithReceipt(JsonObject fields, Func<JsonNode?, JsonNode?> execute)
    {
        ArgumentNullException.ThrowIfNull(fields); ArgumentNullException.ThrowIfNull(execute);
        var before = (JsonObject)fields.DeepClone(); before["phase"] = "before";
        var prepared = Point(before); var actual = prepared.ContainsKey("input") ? prepared["input"]?.DeepClone() : fields["input"]?.DeepClone();
        var identity = FinishIdentity(fields, prepared); var kind = fields["kind"]?.GetValue<string>() ?? "custom";
        if (prepared["decision"]?.GetValue<string>() == "reject")
        {
            var reason = prepared["reason"]?.GetValue<string>() ?? "policy_rejected";
            JsonNode output = new JsonObject { ["status"] = "rejected", ["reason"] = reason, ["error"] = reason };
            var after = (JsonObject)identity.DeepClone(); after["kind"] = kind; after["phase"] = "after"; after["executed"] = false;
            after["actual_input"] = actual?.DeepClone(); after["output"] = output.DeepClone();
            return new CallResult(output, prepared, Point(after), false);
        }
        JsonNode? result;
        try { result = execute(actual?.DeepClone()); }
        catch
        {
            var abort = (JsonObject)identity.DeepClone(); abort["kind"] = kind; abort["phase"] = "abort"; abort["status"] = "unknown";
            Point(abort); throw;
        }
        var completed = (JsonObject)identity.DeepClone(); completed["kind"] = kind; completed["phase"] = "after"; completed["executed"] = true;
        completed["actual_input"] = actual?.DeepClone(); completed["output"] = result?.DeepClone();
        return new CallResult(result, prepared, Point(completed), true);
    }

    public bool AllowLoop(JsonObject fields) => CheckLoop(fields).Allowed;
    public LoopResult CheckLoop(JsonObject fields)
    {
        var request = (JsonObject)fields.DeepClone(); request["kind"] = "loop";
        if (fields.ContainsKey("ref"))
        {
            request["phase"] = "guard"; var receipt = Point(request); var decision = receipt["decision"]?.GetValue<string>() ?? "continue";
            return new LoopResult(decision != "stop", decision, receipt, receipt);
        }
        request["phase"] = "before"; var prepared = Point(request); var selected = prepared["decision"]?.GetValue<string>() ?? "continue";
        var after = FinishIdentity(fields, prepared); after["kind"] = "loop"; after["phase"] = "after"; after["decision"] = selected;
        return new LoopResult(selected != "stop", selected, prepared, Point(after));
    }

    public JsonObject CaptureGap(JsonObject fields, string reason)
    {
        ArgumentNullException.ThrowIfNull(fields);
        if (string.IsNullOrEmpty(reason)) throw new ArgumentException("reason_required", nameof(reason));
        var request = (JsonObject)fields.DeepClone(); request["reason"] = reason; return Invoke("capture.gap", request);
    }
    public JsonObject Metrics()
    {
        lifecycle.EnterReadLock();
        try
        {
            if (handle == 0 || processId != Environment.ProcessId) return new JsonObject();
            var output = metricsNative(handle); if (output == 0) return new JsonObject();
            try { return JsonNode.Parse(Marshal.PtrToStringUTF8(output) ?? "{}") as JsonObject ?? new JsonObject(); }
            finally { freeNative(output); }
        }
        finally { lifecycle.ExitReadLock(); }
    }
    public void SetEnabled(bool enabled)
    {
        lifecycle.EnterReadLock();
        try { if (handle != 0 && processId == Environment.ProcessId) enabledNative(handle, enabled ? 1 : 0); }
        finally { lifecycle.ExitReadLock(); }
    }
    public void Drain()
    {
        lifecycle.EnterReadLock();
        try { if (handle != 0 && processId == Environment.ProcessId) drainNative(handle); }
        finally { lifecycle.ExitReadLock(); }
    }
    private static JsonObject FinishIdentity(JsonObject fields, JsonObject prepared)
    {
        var identity = new JsonObject();
        if (fields["ref"] is JsonNode reference) identity["ref"] = reference.DeepClone();
        else if (prepared["context"] is JsonNode context) identity["context"] = context.DeepClone();
        return identity;
    }
    public void Dispose()
    {
        lifecycle.EnterWriteLock();
        try
        {
            var owned = handle; var ownedLibrary = library; handle = 0; library = 0;
            if (owned != 0 && processId == Environment.ProcessId) closeNative(owned);
            if (ownedLibrary != 0) NativeLibrary.Free(ownedLibrary);
        }
        finally { lifecycle.ExitWriteLock(); }
    }
}
