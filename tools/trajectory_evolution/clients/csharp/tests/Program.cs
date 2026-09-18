using System.Net;
using System.Net.Sockets;
using System.Text;
using System.Text.Json.Nodes;
using TrajectoryEvolution;

static void Require(bool value, string message) { if (!value) throw new Exception(message); }

if (args.Length == 3 && args[0] == "--native")
{
    using var local = new LocalEvolution(args[1], args[2], "fixture-principal-alpha-00000001",
        new JsonObject { ["background"] = false });
    var reference = new JsonObject { ["run"] = "csharp-native-contract" };
    var begun = local.Point(new JsonObject { ["phase"] = "before", ["kind"] = "run", ["ref"] = reference.DeepClone(),
        ["input"] = "native contract", ["level"] = "L0", ["capabilities"] = new JsonArray() });
    Require(begun["degraded"]?.GetValue<bool>() != true, "native run degraded");
    var calls = 0;
    var observed = local.CallWithReceipt(new JsonObject { ["phase"] = "before", ["kind"] = "action",
        ["ref"] = new JsonObject { ["run"] = "csharp-native-contract", ["key"] = "action" },
        ["name"] = "lookup", ["input"] = new JsonObject { ["query"] = "data" } }, input => { ++calls; return input; });
    Require(calls == 1 && observed.Executed, "native business call was not executed exactly once");
    Require(observed.Receipt["degraded"]?.GetValue<bool>() != true, "native after receipt degraded");
    var closed = local.Point(new JsonObject { ["phase"] = "after", ["kind"] = "run", ["ref"] = reference.DeepClone(), ["output"] = "done" });
    Require(closed["closed"]?.GetValue<bool>() == true, "native run did not close");
    local.Drain(); Require(local.Metrics()["degraded_calls"]?.GetValue<double>() == 0, "native degraded calls recorded");
    var callers = Enumerable.Range(0, 6).Select(_ => Task.Run(() => {
        for (var i = 0; i < 100; ++i) local.Invoke("contract.describe", new JsonObject());
    })).ToArray();
    await Task.Delay(1); local.Dispose(); local.Dispose(); await Task.WhenAll(callers);
    Console.WriteLine("C# native client contract tests passed");
    return;
}

var socket = new TcpListener(IPAddress.Loopback, 0); socket.Start();
var port = ((IPEndPoint)socket.LocalEndpoint).Port; socket.Stop();
using var listener = new HttpListener(); listener.Prefixes.Add($"http://127.0.0.1:{port}/"); listener.Start();
using var stop = new CancellationTokenSource(); var aborts = 0;
var serving = Task.Run(async () =>
{
    while (!stop.IsCancellationRequested)
    {
        HttpListenerContext context;
        try { context = await listener.GetContextAsync().WaitAsync(stop.Token); }
        catch (OperationCanceledException) { break; }
        try
        {
            var request = (JsonObject)(await JsonNode.ParseAsync(context.Request.InputStream))!;
            var result = new JsonObject();
            var operation = request["operation"]?.GetValue<string>();
            if (operation == "point")
            {
                var phase = request["phase"]?.GetValue<string>();
                if (phase == "before")
                {
                    result["decision"] = request["name"]?.GetValue<string>() == "reject" ? "reject" : "allow";
                    result["reason"] = "fixture_reject"; result["input"] = request["input"]?.DeepClone();
                    result["context"] = new JsonObject { ["run_id"] = "run-1", ["kind"] = request["kind"]?.DeepClone() };
                }
                else if (phase == "guard") result["decision"] = "stop";
                else { result["event_id"] = "receipt-" + phase; result["closed"] = false; }
                if (phase == "abort") Interlocked.Increment(ref aborts);
            }
            else if (operation == "capture.gap") result["capture_complete"] = false;
            else if (operation == "evaluation.next") result["state"] = "done";
            else if (operation == "evaluation.fail") result["failed"] = true;
            var bytes = Encoding.UTF8.GetBytes(new JsonObject { ["ok"] = true, ["result"] = result }.ToJsonString());
            context.Response.StatusCode = 200; context.Response.ContentType = "application/json";
            context.Response.ContentLength64 = bytes.Length; await context.Response.OutputStream.WriteAsync(bytes);
        }
        finally { context.Response.Close(); }
    }
});

try
{
    using var client = new Evolution($"http://127.0.0.1:{port}", "fixture-client-contract-token", 1000, 65536, 0);
    static JsonObject Fields(string name) => new() {
        ["kind"] = "action", ["name"] = name, ["input"] = new JsonObject { ["value"] = 7 }
    };

    var calls = 0;
    var allowed = await client.CallWithReceiptAsync(Fields("allow"), input => {
        ++calls; return Task.FromResult<JsonNode?>(new JsonObject { ["value"] = input?["value"]?.GetValue<int>() });
    }, 500);
    Require(calls == 1 && allowed.Executed, "allowed call was not executed exactly once");
    Require(allowed.Output?["value"]?.GetValue<int>() == 7, "allowed output changed");
    Require(allowed.Receipt["event_id"]?.GetValue<string>() == "receipt-after", "after receipt missing");

    calls = 0;
    var rejected = await client.CallWithReceiptAsync(Fields("reject"), input => {
        ++calls; return Task.FromResult(input);
    }, 500);
    Require(calls == 0 && !rejected.Executed, "rejected call executed");
    Require(rejected.Output?["status"]?.GetValue<string>() == "rejected", "reject result missing");
    Require(rejected.Receipt["event_id"]?.GetValue<string>() == "receipt-after", "reject receipt missing");

    var original = new InvalidOperationException("business-secret");
    try
    {
        await client.CallWithReceiptAsync(Fields("allow"), _ => Task.FromException<JsonNode?>(original), 500);
        throw new Exception("business exception was swallowed");
    }
    catch (InvalidOperationException observed) { Require(ReferenceEquals(original, observed), "business exception was replaced"); }
    Require(aborts == 1, "abort was not recorded");

    var loop = await client.CheckLoopAsync(new JsonObject {
        ["ref"] = new JsonObject { ["run"] = "r", ["key"] = "k" }
    }, 500);
    Require(!loop.Allowed && loop.Decision == "stop", "loop decision changed");
    Require(!(await client.CaptureGapAsync(new JsonObject { ["run_id"] = "r" }, "missing_boundary"))["capture_complete"]!.GetValue<bool>(), "gap missing");
    Require((await client.EvaluationNextAsync("consumer", 0))["state"]!.GetValue<string>() == "done", "evaluation next missing");
    Require((await client.EvaluationFailAsync("consumer"))["failed"]!.GetValue<bool>(), "evaluation fail missing");
    Console.WriteLine("C# client contract tests passed");
}
finally
{
    stop.Cancel(); listener.Stop(); await serving;
}
