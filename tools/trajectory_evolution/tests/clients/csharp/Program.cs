// Program 回归验证：检查包内协议、边界条件与失败处理。
// 本地桩用于重现确定性场景，不代表真实 Agent 或远程模型的业务能力。
// 测试应同时验证正常路径与拒绝路径，避免错误数据被当成成功结果。

using System.Text.Json.Nodes;
using TrajectoryEvolution;
using var client = new Evolution(Environment.GetEnvironmentVariable("EVOLUTION_URL")!,
                                 Environment.GetEnvironmentVariable("EVOLUTION_TEST_TOKEN")!,
                                 retries: 0);
string? line;
while ((line = Console.ReadLine()) != null) {
  var request = (JsonObject)JsonNode.Parse(line)!;
  var operation = request["operation"]!.GetValue<string>();
  var id = request["request_id"]!.GetValue<string>();
  request.Remove("operation");
  request.Remove("request_id");
  JsonObject reply;
  try {
    reply = new JsonObject { ["ok"] = true,
                             ["result"] = await client.InvokeAsync(operation, request, id) };
  } catch (EvolutionError error) {
    reply = new JsonObject { ["ok"] = false, ["error"] = error.Message, ["status"] = error.Status };
  }
  Console.WriteLine(reply.ToJsonString());
}
