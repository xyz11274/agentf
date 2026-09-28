// probe 回归验证：检查包内协议、边界条件与失败处理。
// 本地桩用于重现确定性场景，不代表真实 Agent 或远程模型的业务能力。
// 测试应同时验证正常路径与拒绝路径，避免错误数据被当成成功结果。

import readline from "node:readline";
import {Evolution, EvolutionError} from "../../clients/javascript/evolution.mjs";
const client = new Evolution(
    {url : process.env.EVOLUTION_URL, token : process.env.EVOLUTION_TEST_TOKEN, retries : 0});
for await (const line of readline.createInterface({input : process.stdin})) {
  const {operation, request_id, ...fields} = JSON.parse(line);
  try {
    console.log(
        JSON.stringify({ok : true, result : await client.invoke(operation, fields, request_id)}));
  } catch (error) {
    if (!(error instanceof EvolutionError))
      throw error;
    console.log(JSON.stringify({ok : false, error : error.message, status : error.status}));
  }
}
