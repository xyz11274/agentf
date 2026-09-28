// javascript 客户端：通过统一协议收集执行事件并接收策略建议。
// 传输与业务执行分离；接收回执不代表 Agent 已完成整个任务。
// 打点的输入是建议修改前的请求，执行后必须回传实际输入、输出与状态。
// 取消、超时和采集缺口需要显式处理，不能把缺失证据当成正常完成。
// 本文件包含协议验证或接入示例，不作为真实业务效果的评测成绩。

import assert from "node:assert/strict";
import {Evolution, EvolutionError, PlannedAction, PointClient} from "./evolution.mjs";

class FakePoints extends PointClient {
  // 初始化 constructor 的状态和资源；输入参数的校验与所有权在构造阶段确定。
  // 对象创建后复用既有配置，实际执行与收尾分别由后续接口负责。
  constructor(before, failAbort = false) {
    super();
    this.before = before;
    this.failAbort = failAbort;
    this.events = [];
  }

  // 提交框架无关打点，必要时在同一边界先取得策略决定，再记录实际执行结果。
  async point(fields) {
    this.events.push(fields);
    if (fields.phase === "abort" && this.failAbort)
      throw new Error("observer_failed");
    if (fields.phase === "before")
      return {input : fields.input, ...this.before};
    return {event_id : "receipt-1"};
  }
}

{
  const points = new FakePoints({decision : "reject", reason : "policy_rejected"});
  let calls = 0;
  const result =
      await points.call({kind : "action", ref : {run : "run", key : "call"}, input : {value : 1}},
                        () => { ++calls; });
  assert.equal(calls, 0);
  assert.deepEqual(result,
                   {status : "rejected", reason : "policy_rejected", error : "policy_rejected"});
  assert.equal(points.events.at(-1).executed, false);
}

{
  const points = new FakePoints({decision : "allow"}, true);
  const business = new Error("business_failure");
  await assert.rejects(points.call({kind : "model", ref : {run : "run", key : "call"}, input : {}},
                                   () => { throw business; }),
                       error => error === business);
  assert.equal(points.events.at(-1).phase, "abort");
}

{
  const observations = [];
  const step = {
    handle : {step_id : "step"},
    run : {
      observe : async (...args) => {
        observations.push(args);
        throw new Error("observer_failed");
      }
    },
  };
  const action = new PlannedAction(
      step, {id : "action", call_id : "call", decision : "allow", reason : "", event_id : "event"},
      {id : "action", operation : "tool", arguments : {}});
  const business = new Error("tool_failure");
  await assert.rejects(action.execute(async () => { throw business; }),
                       error => error === business);
  assert.equal(observations.length, 1);
  assert.equal(observations[0][1].status, "failed");
  await assert.rejects(action.execute(async () => ({})), /local_action_already_executed/);
}

{
  const client = new Evolution({
    url : "http://127.0.0.1:1",
    token : "fixture-client-token-00000001",
    timeoutMs : 100,
    retries : 0,
  });
  await assert.rejects(client.invoke("point", {value : Number.NaN}),
                       error =>
                           error instanceof EvolutionError && error.message === "request_invalid");
  const circular = {};
  circular.self = circular;
  await assert.rejects(client.invoke("point", circular),
                       error =>
                           error instanceof EvolutionError && error.message === "request_invalid");
}

console.log("JavaScript client contract passed");
