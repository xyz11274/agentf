using System.Text.Json.Nodes;
using TrajectoryEvolution;
using var client = new Evolution(Environment.GetEnvironmentVariable("EVOLUTION_URL")!, Environment.GetEnvironmentVariable("EVOLUTION_TEST_TOKEN")!, retries: 0);
string? line;
while ((line = Console.ReadLine()) != null)
{
    var request = (JsonObject)JsonNode.Parse(line)!;
    var operation = request["operation"]!.GetValue<string>(); var id = request["request_id"]!.GetValue<string>();
    request.Remove("operation"); request.Remove("request_id");
    JsonObject reply;
    try { reply = new JsonObject { ["ok"] = true, ["result"] = await client.InvokeAsync(operation, request, id) }; }
    catch (EvolutionError error) { reply = new JsonObject { ["ok"] = false, ["error"] = error.Message, ["status"] = error.Status }; }
    Console.WriteLine(reply.ToJsonString());
}
