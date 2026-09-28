// typescript 客户端：通过统一协议收集执行事件并接收策略建议。
// 传输与业务执行分离；接收回执不代表 Agent 已完成整个任务。
// 打点的输入是建议修改前的请求，执行后必须回传实际输入、输出与状态。
// 取消、超时和采集缺口需要显式处理，不能把缺失证据当成正常完成。

/* 框架无关的 L0–L3 客户端，不依赖 AgentFlow 或模型 SDK。 */
export type Level = "L0"|"L1"|"L2"|"L3";
export type Status = "completed"|"failed"|"cancelled"|"unknown";
export type Capability = "model"|"actions"|"loop";
export type ObjectValue = Record<string, any>;
export type ModelHandle = {
  call_id: string; request : ObjectValue; event_id : string
};
export type StepHandle = {
  step_id: string; decision : "continue" | "replan" | "stop"; event_id : string
};
export type Action = {
  id: string; operation : string; arguments : ObjectValue
};
export type ActionHandle = {
  id: string; call_id : string; decision : "allow" | "reject"; reason : string; event_id : string
};

export class EvolutionError extends Error {
  status: number;

  // 初始化 constructor 的状态和资源；输入参数的校验与所有权在构造阶段确定。
  // 对象创建后复用既有配置，实际执行与收尾分别由后续接口负责。
  constructor(code: string, status = 0) {
    super(code);
    this.name = "EvolutionError";
    this.status = status;
  }
}

/* 在原有事件循环中绑定接收入口；非评测试验期间不做额外执行。 */
export async function evaluate(receive: (input: any) => any): Promise<boolean> {
  const env = (globalThis as any).process?.env;
  if (env?.TRAJECTORY_EVOLUTION_EVALUATION_ENTRY !== "1")
    return false;
  const client = new Evolution({
    url : env.TRAJECTORY_EVOLUTION_TRIAL_URL,
    token : env.TRAJECTORY_EVOLUTION_TRIAL_TOKEN,
    timeoutMs : 5000,
    retries : 0
  });
  const consumer = crypto.randomUUID();
  try {
    for (let index = 0;;) {
      const packet = await client.invoke("evaluation.next", {consumer, index});
      if (packet.state === "done")
        return true;
      if (packet.state === "waiting") {
        await new Promise(resolve => setTimeout(resolve, 100));
        continue;
      }
      if (packet.state !== "input" || packet.index !== index)
        throw new Error("evaluation_entry_response_invalid");
      await receive(packet.input);
      ++index;
    }
  } catch {
    await client.invoke("evaluation.fail", {consumer}).catch(() => {});
    throw new EvolutionError("evaluation_entry_failed");
  }
}

export abstract class PointClient {
  private gaps = new Set<string>();
  private gapsFull = false;
  abstract point(fields: ObjectValue, ...options: any[]): any;

  // 补齐打点协议字段与稳定请求身份，避免重试生成新的业务身份。
  protected pointFields(fields: ObjectValue): ObjectValue {
    const run = fields?.ref?.run;
    return typeof run === "string" && (this.gapsFull || this.gaps.has(run))
               ? {...fields, capture_gap : true}
               : fields;
  }

  // 跟踪本地采集缺口与运行收尾，保证观测丢失最终可见。
  protected pointObserved(fields: ObjectValue, result: ObjectValue): ObjectValue {
    const run = fields?.ref?.run;
    if (typeof run === "string" && run.length <= 256) {
      if (result.degraded) {
        if (this.gaps.size < 1024)
          this.gaps.add(run);
        else
          this.gapsFull = true;
      } else if (result.closed || result.run_closed)
        this.gaps.delete(run);
    }
    return result;
  }

  // 包装宿主业务调用：先查询策略，再最多执行一次原业务函数，最后记录结果。
  async call(fields: ObjectValue, execute: (input: any) => any, ...options: any[]): Promise<any> {
    const prepared = await this.point({...fields, phase : "before"}, ...options);
    const identity = fields.ref ? {ref : fields.ref} : {context : prepared.context};
    const actual = prepared.input ?? fields.input;
    let output;
    if (prepared.decision === "reject")
      output = {
        status : "rejected",
        reason : prepared.reason ?? "policy_rejected",
        error : prepared.reason ?? "policy_rejected"
      };
    else {
      try {
        output = await execute(actual);
      } catch (error) {
        try {
          await this.point(
              {...identity, kind : fields.kind, phase : "abort", status : "unknown", output : null},
              ...options);
        } catch { /* 观测失败不能替换业务异常。 */
        }
        throw error;
      }
    }
    await this.point({
      ...identity,
      kind : fields.kind,
      phase : "after",
      executed : prepared.decision !== "reject",
      actual_input : actual,
      output
    },
                     ...options);
    return output;
  }

  // 提交循环边界的真实控制决定，并返回宿主是否继续迭代。
  async allowLoop(fields: ObjectValue, ...options: any[]): Promise<boolean> {
    const result = await this.point({...fields, phase : "guard", kind : "loop"}, ...options);
    return result.decision !== "stop";
  }
}

export class Evolution extends PointClient {
  #url: string;
  #token: string;
  #timeout: number;
  #maxBytes: number;
  #retries: number;

  // 初始化 constructor 的状态和资源；输入参数的校验与所有权在构造阶段确定。
  // 对象创建后复用既有配置，实际执行与收尾分别由后续接口负责。
  constructor(options: {
    url: string; token : string;
    timeoutMs?: number;
    maxResponseBytes?: number;
    retries?: number
  }) {
    super();
    const env = (globalThis as any).process?.env;
    if (env?.TRAJECTORY_EVOLUTION_TRIAL_URL && env?.TRAJECTORY_EVOLUTION_TRIAL_TOKEN)
      options = {
        ...options,
        url : env.TRAJECTORY_EVOLUTION_TRIAL_URL,
        token : env.TRAJECTORY_EVOLUTION_TRIAL_TOKEN
      };
    const url = new URL(options.url);
    if (url.username || url.password || url.search || url.hash ||
        !["", "/"].includes(url.pathname) ||
        (url.protocol !== "https:" &&
         !(url.protocol === "http:" &&
           [ "localhost", "127.0.0.1", "[::1]" ].includes(url.hostname))))
      throw new EvolutionError("https_or_loopback_url_required");
    if (options.token.length < 24 || /[\r\n]/.test(options.token))
      throw new EvolutionError("token_invalid");
    this.#url = url.origin + "/v1/invoke";
    this.#token = options.token;
    this.#timeout = options.timeoutMs ?? 7000;
    this.#maxBytes = options.maxResponseBytes ?? 2097152;
    this.#retries = options.retries ?? 1;
    if (!Number.isInteger(this.#timeout) || this.#timeout < 100 || this.#timeout > 300000 ||
        !Number.isInteger(this.#maxBytes) || this.#maxBytes < 1024 || this.#maxBytes > 16777216 ||
        !Number.isInteger(this.#retries) || this.#retries < 0 || this.#retries > 3)
      throw new EvolutionError("client_limits_invalid");
  }

  // 执行一次组件协议调用，检查响应信封并保持请求身份一致。
  async invoke(operation: string, fields: ObjectValue,
               requestId: string = crypto.randomUUID()): Promise<ObjectValue> {
    return this.send(operation, fields, requestId, this.#timeout, this.#retries);
  }

  // 提交框架无关打点，必要时在同一边界先取得策略决定，再记录实际执行结果。
  async point(fields: ObjectValue, timeoutMs = 20,
              requestId: string = crypto.randomUUID()): Promise<ObjectValue> {
    fields = this.pointFields(fields);
    try {
      if (!Number.isInteger(timeoutMs) || timeoutMs < 1 || timeoutMs > 5000)
        throw new Error("point_timeout_invalid");
      return this.pointObserved(fields, await this.send("point", fields, requestId, timeoutMs, 0));
    } catch {
      const context = {...(fields?.context ?? {})}, kind = fields?.kind ?? context.kind ?? "custom";
      Object.assign(context, {
        kind,
        degraded : true,
        caused_by : [],
        run_id : context.run_id ?? `unobserved-${requestId}`
      });
      return this.pointObserved(fields, {
        context,
        input : fields?.input,
        degraded : true,
        capture_complete : false,
        policy_applied : false,
        decision : kind === "loop" ? "continue" : "allow",
        closed : false,
        ended : false,
        reason : "observer_unavailable"
      });
    }
  }

  // 发送一份已构造的协议请求，保持请求身份、超时和响应上限。
  private async send(operation: string, fields: ObjectValue, requestId: string, timeoutMs: number,
                     retries: number): Promise<ObjectValue> {
    let body: string;
    try {
      body = JSON.stringify({...fields, operation, request_id : requestId}, (_key, value) => {
        if (typeof value === "number" && !Number.isFinite(value))
          throw new Error("non_finite_number");
        return value;
      });
    } catch {
      throw new EvolutionError("request_invalid");
    }
    if (new TextEncoder().encode(body).length > 2097152)
      throw new EvolutionError("request_too_large");
    for (let attempt = 0;; ++attempt) {
      try {
        const response = await fetch(this.#url, {
          method : "POST",
          redirect : "error",
          cache : "no-store",
          headers : {"Content-Type" : "application/json", Authorization : "Bearer " + this.#token},
          body,
          signal : AbortSignal.timeout(timeoutMs)
        });
        const reader = response.body?.getReader();
        if (!reader)
          throw new EvolutionError("response_missing");
        const chunks: Uint8Array[] = [];
        let length = 0;
        try {
          for (;;) {
            const part = await reader.read();
            if (part.done)
              break;
            length += part.value.length;
            if (length > this.#maxBytes)
              throw new EvolutionError("response_too_large");
            chunks.push(part.value);
          }
        } finally {
          await reader.cancel().catch(() => {});
          reader.releaseLock();
        }
        const bytes = new Uint8Array(length);
        let pos = 0;
        for (const chunk of chunks) {
          bytes.set(chunk, pos);
          pos += chunk.length;
        }
        let payload;
        try {
          payload = JSON.parse(new TextDecoder("utf-8", {fatal : true}).decode(bytes));
        } catch {
          throw new EvolutionError("response_invalid", response.status);
        }
        if (!response.ok || payload.ok !== true) {
          const code = typeof payload.error === "string" && /^[a-z_]{1,96}$/.test(payload.error)
                           ? payload.error
                           : "rpc_failed";
          throw new EvolutionError(code, response.status);
        }
        if (!payload.result || typeof payload.result !== "object" || Array.isArray(payload.result))
          throw new EvolutionError("response_invalid", response.status);
        return payload.result;
      } catch (error) {
        const failure =
            error instanceof EvolutionError ? error : new EvolutionError("observer_unavailable");
        if (attempt >= retries || (failure.status > 0 && failure.status < 500))
          throw failure;
        // 只重试同一个持久化远程请求，绝不重试宿主的模型或工具回调。
      }
    }
  }

  // 开始一个新的规范运行上下文，绑定应用、用户与会话身份。
  async start(options: {
    conversationId: string; input : unknown; level : Level; capabilities : Capability[]
  }): Promise<Run> {
    const result = await this.invoke("task.begin", {
      conversation_id : options.conversationId,
      input : options.input,
      level : options.level,
      capabilities : options.capabilities
    });
    return this.resume(result.run_id);
  }

  // 根据既有运行句柄恢复上下文，适配异步和跨进程任务。
  resume(runId: string): Run {
    return new Run(this, runId);
  }
}

export class Run {
  client: Evolution;
  id: string;
  telemetryError: string|null = null;

  // 初始化 constructor 的状态和资源；输入参数的校验与所有权在构造阶段确定。
  // 对象创建后复用既有配置，实际执行与收尾分别由后续接口负责。
  constructor(client: Evolution, id: string) {
    this.client = client;
    this.id = id;
  }

  // 执行一次组件协议调用，检查响应信封并保持请求身份一致。
  async invoke(operation: string, fields: ObjectValue = {}): Promise<ObjectValue> {
    return this.client.invoke(operation, {...fields, run_id : this.id});
  }

  // 开始当前步骤并取得规范上下文，后续观测和结束复用该身份。
  async step(state: unknown,
             options: {executionId?: string; actorId?: string; causedBy?: string[]} = {}):
      Promise<Step> {
    const handle = await this.invoke("step.begin", {
      state,
      execution_id : options.executionId ?? "main",
      ...(options.actorId ? {actor_id : options.actorId} : {}),
      ...(options.causedBy ? {caused_by : options.causedBy} : {})
    });
    return this.resumeStep(handle as StepHandle);
  }

  // 从传递的句柄恢复既有步骤，不能创建重复执行身份。
  resumeStep(handle: StepHandle): Step {
    return new Step(this, handle);
  }

  // 报告当前上下文的采集缺口，使后续收尾知道记录不完整。
  async gap(reason: "observer_unavailable"|"privacy_rejected"|"missing_boundary"): Promise<void> {
    this.telemetryError = reason;
    await this.invoke("capture.gap", {reason});
  }

  // 记录真实观测结果并推进当前状态；未执行的建议不能作为观测输入。
  async observe(operation: string, fields: ObjectValue): Promise<ObjectValue|null> {
    try {
      return await this.invoke(operation, fields);
    } catch (error) {
      const privacy =
          error instanceof EvolutionError &&
          [ "credential_value_rejected", "sensitive_field_rejected" ].includes(error.message);
      try {
        await this.gap(privacy ? "privacy_rejected" : "observer_unavailable");
      } catch { /* 任务关闭时也会提交本地的不完整标记。 */
      }
      // 只以未知数据关闭当前调用，不取消并行兄弟分支，
      // 也不重发被拒绝的内容；capture.gap 将该运行隔离出学习流程。
      if (operation === "model.observe" || operation === "action.observe") {
        const fallback: ObjectValue = {
          step_id : fields.step_id,
          call_id : fields.call_id,
          status : "unknown",
          output : null,
          error : {code : "observation_unavailable", retryability : "unknown"}
        };
        if (operation === "model.observe") {
          fallback.request = {};
          fallback.transport_allowed = fields.transport_allowed === true;
        } else
          fallback.executed = fields.executed === true;
        try {
          await this.invoke(operation, fallback);
        } catch { /* 服务不可用仍应保留不完整状态。 */
        }
      }
      return null;
    }
  }

  // 返回当前运行或作业状态，不触发额外执行或重新评测。
  status(): Promise<ObjectValue> {
    return this.invoke("task.status");
  }

  // 请求重试允许恢复的后台评测，不重试宿主原业务动作。
  retryEvaluation(): Promise<ObjectValue> {
    return this.invoke("evaluation.retry");
  }

  // 提交实际完成状态；未确认的异步任务不能因为派发函数返回就结束。
  finish(output: unknown, options: {
    status?: Status;
    businessStatus?: "succeeded" | "failed" | "partial" | "unknown"
  } = {}): Promise<ObjectValue> {
    return this.invoke(this.telemetryError ? "task.abort" : "task.end", {
      output : output ?? null,
      status : this.telemetryError ? "unknown" : (options.status ?? "completed"),
      business_status : options.businessStatus ?? "unknown",
      capture_complete : !this.telemetryError
    });
  }

  // 以明确中止状态收尾，保留采集完整性和业务异常的区别。
  abort(output: unknown = null,
        status: "failed"|"cancelled"|"unknown" = "cancelled"): Promise<ObjectValue> {
    return this.invoke("task.abort", {output, status});
  }
}

export class Step {
  run: Run;
  handle: StepHandle;

  // 初始化 constructor 的状态和资源；输入参数的校验与所有权在构造阶段确定。
  // 对象创建后复用既有配置，实际执行与收尾分别由后续接口负责。
  constructor(run: Run, handle: StepHandle) {
    this.run = run;
    this.handle = handle;
  }

  // 提取组件给出的控制决定，缺省按接口约定处理，不能推断已应用。
  get decision(): StepHandle["decision"] {
    return this.handle.decision;
  }

  // 先取得当前策略增强后的模型请求，宿主必须发送返回的新请求体才能使注入生效。
  prepareModel(request: ObjectValue): Promise<ModelHandle> {
    return this.run.invoke("model.prepare", {step_id : this.handle.step_id, request}) as
           Promise<ModelHandle>;
  }

  // 记录真实模型响应、用量和执行状态，不能以请求已发送推断调用成功。
  observeModel(handle: ModelHandle, actualRequest: ObjectValue, output: unknown,
               status: Status = "completed", transportAllowed = true): Promise<ObjectValue|null> {
    return this.run.observe("model.observe", {
      step_id : this.handle.step_id,
      call_id : handle.call_id,
      request : actualRequest,
      output : output ?? null,
      status,
      transport_allowed : transportAllowed,
      ...(status === "failed" ? {error : {code : "model_call_failed", retryability : "unknown"}}
                              : {})
    });
  }

  // 包装一次真实模型调用，先准备注入请求，再记录原始调用结果。
  async model<T>(request: ObjectValue,
                 send: (preparedRequest: ObjectValue) => Promise<T>): Promise<T> {
    const handle = await this.prepareModel(request);
    let output: T;
    try {
      output = await send(handle.request);
    } catch (error) {
      await this.observeModel(handle, handle.request, null, "failed");
      throw error;
    }
    await this.observeModel(handle, handle.request, output);
    return output;
  }

  // 按宿主动作接口执行或派发已允许的动作，并记录真实结果。
  async actions(actions: Action[]): Promise<PlannedAction[]> {
    if (!actions.length)
      return [];
    const result =
        await this.run.invoke("actions.prepare", {step_id : this.handle.step_id, actions});
    const byId = new Map<string, Action>(actions.map(a => [a.id, a]));
    if (!Array.isArray(result.actions) || result.actions.length !== actions.length)
      throw new EvolutionError("action_plan_invalid");
    return result.actions.map((handle: ActionHandle) => {
      const action = byId.get(handle.id);
      if (!action)
        throw new EvolutionError("action_plan_invalid");
      byId.delete(handle.id);
      return new PlannedAction(this, handle, action);
    });
  }

  // 记录宿主提供的实际业务状态变化，不在组件内猜测状态。
  recordState(before: unknown, after: unknown): Promise<ObjectValue> {
    return this.run.invoke("state.record", {step_id : this.handle.step_id, before, after});
  }

  // 关闭当前步骤或运行，保留实际终态及未完成分支检查。
  end(output: unknown = null, status: Status = "completed"): Promise<ObjectValue> {
    return this.run.invoke(
        "step.end", {step_id : this.handle.step_id, output, status, decision : this.decision});
  }
}

export class PlannedAction {
  step: Step;
  handle: ActionHandle;
  action: Action;
  #executed = false;

  // 初始化 constructor 的状态和资源；输入参数的校验与所有权在构造阶段确定。
  // 对象创建后复用既有配置，实际执行与收尾分别由后续接口负责。
  constructor(step: Step, handle: ActionHandle, action: Action) {
    this.step = step;
    this.handle = handle;
    this.action = action;
  }

  // 记录真实观测结果并推进当前状态；未执行的建议不能作为观测输入。
  observe(output: unknown, status: Status, executed: boolean): Promise<ObjectValue|null> {
    return this.step.run.observe("action.observe", {
      step_id : this.step.handle.step_id,
      call_id : this.handle.call_id,
      output : output ?? null,
      status,
      executed,
      ...(status === "failed" ? {error : {code : "action_failed", retryability : "unknown"}} : {})
    });
  }

  // 调用宿主提供的真实执行入口，返回和异常保持原业务语义。
  async execute(send: (action: Action) => Promise<unknown>): Promise<ObjectValue> {
    if (this.#executed)
      throw new EvolutionError("local_action_already_executed");
    this.#executed = true;
    let output: unknown = null, status: Status = "cancelled";
    const executed = this.handle.decision === "allow";
    if (executed) {
      try {
        output = await send(this.action);
        status = "completed";
      } catch (error) {
        try {
          await this.observe(null, "failed", true);
        } catch { /* 观测失败不能替换业务异常。 */
        }
        throw error;
      }
    } else
      output = {error : "policy_rejected", reason : this.handle.reason};
    const receipt = await this.observe(output, status, executed);
    return {id : this.action.id, output, status, event_id : receipt?.event_id ?? null};
  }
}
