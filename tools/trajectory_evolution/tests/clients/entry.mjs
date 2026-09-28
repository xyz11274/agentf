// entry 回归验证：检查包内协议、边界条件与失败处理。
// 本地桩用于重现确定性场景，不代表真实 Agent 或远程模型的业务能力。
// 测试应同时验证正常路径与拒绝路径，避免错误数据被当成成功结果。

// 确定性的入口协议测试桩，不是 Agent 能力基准实现。
import {Evolution, evaluate} from "../../clients/javascript/evolution.mjs";
const client = new Evolution(
    {url : "https://deployment.invalid", token : "deployment-placeholder-credential", retries : 0});
await evaluate(async message => {
  if (message.history.length !== message.turn * 2 + 1)
    throw new Error("history_invalid");
  const run = await client.point({
    phase : "before",
    kind : "run",
    ref : {run : message.key},
    conversation_id : message.conversation,
    input : message.text
  },
                                 5000);
  if (run.degraded)
    throw new Error("capture_failed");
  const result = await client.point({
    phase : "after",
    kind : "run",
    ref : {run : message.key},
    output : {answer : "20", state : {value : 20}},
    business_status : "succeeded"
  },
                                    5000);
  if (result.degraded)
    throw new Error("capture_failed");
});
