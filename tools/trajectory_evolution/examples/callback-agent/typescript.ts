// typescript 接入示例：演示宿主如何在已有执行边界调用公开接口。
// 示例用于说明接入方式，实际执行和业务工具仍由宿主自身负责。
// 需要保留真实请求、结果及因果关联，不能用示例常量代替生产运行证据。

// 无需 AgentFlow 运行时或模型账户即可运行。
// 接入真实业务时只替换示例中的模型与工具回调。
import {Evolution} from "../../clients/typescript/evolution.ts";
import type {Run, Step, ObjectValue} from "../../clients/typescript/evolution.ts";

export async function conversation(client: Evolution, conversationId: string): Promise<Run> {
  const run = await client.start({
    conversationId,
    input : "视频生成缺少图片素材，同时检索资料",
    level : "L3",
    capabilities : [ "model", "actions", "loop" ]
  });
  let rounds = 0;

  // 该函数由真实队列、图路由或回调调度。
  // 不要求接管或包装整个 Agent 的调用入口。
  async function onNextStep(): Promise<void> {
    const step = await run.step({image : "missing"});
    ++rounds;
    if (step.decision === "stop") {
      await step.end({stopped : true});
      return;
    }
    if (rounds > 14)
      throw new Error("example requires a bounded loop rule");
    const handle = await step.prepareModel(
        {system : "你是任务助手。", messages : [ {role : "user", content : "视频生成缺少素材"} ]});
    // 主动在异步边界传递可序列化句柄，验证跨进程上下文。
    const saved = JSON.parse(JSON.stringify({runId : run.id, step : step.handle, model : handle}));
    await new Promise<void>((resolve, reject) => setTimeout(() => {
                              const resumed = client.resume(saved.runId).resumeStep(saved.step);
                              onModelResult(resumed, saved.model).then(resolve, reject);
                            }, 0));
    await step.end({branch_results_collected : true});
    await new Promise<void>(resolve => setTimeout(resolve, 0));
    return onNextStep();
  }

  // 在模型返回时记录真实输出，再由宿主继续其原业务流程。
  async function onModelResult(step: Step, handle: ObjectValue): Promise<void> {
    // 这是固定的模型返回，不是付费模型评测。
    await step.observeModel(handle as any, handle.request,
                            {answer : "执行独立任务，素材不足的分支保留错误"});
    const planned = await step.actions([
      {id : "slow", operation : "lookup", arguments : {topic : "资料"}},
      {id : "video", operation : "video", arguments : {}},
      {id : "fast-failure", operation : "lookup", arguments : {topic : "模拟网络失败"}},
    ]);
    const settled = await Promise.allSettled(planned.map(action => action.execute(async spec => {
      if (spec.id === "fast-failure")
        throw new Error("simulated network failure");
      await new Promise(resolve => setTimeout(resolve, spec.id === "slow" ? 15 : 1));
      return {found : spec.id};
    })));
    const results =
        settled.map((result, index) => result.status === "fulfilled"
                                           ? result.value
                                           : {id : planned[index].action.id, status : "failed"});
    if (results.find(r => r.id === "slow")?.status !== "completed")
      throw new Error("independent branch was lost");
    if (results.find(r => r.id === "fast-failure")?.status !== "failed")
      throw new Error("failure was hidden");
  }

  try {
    await onNextStep();
    await run.finish({rounds, answer : "检索完成，视频素材不足"}, {businessStatus : "partial"});
  } catch (error) {
    await run.abort({reason : "example_aborted"}).catch(() => {});
    throw error;
  }
  return run;
}

if (import.meta.url === new URL(process.argv[1], "file:").href) {
  const client = new Evolution({
    url : process.env.EVOLUTION_URL ?? "http://127.0.0.1:8766",
    token : process.env.EVOLUTION_USER_A_TOKEN ?? ""
  });
  const run = await conversation(client, "example-typescript-" + crypto.randomUUID());
  console.log(JSON.stringify({run_id : run.id, ...(await run.status())}));
}
