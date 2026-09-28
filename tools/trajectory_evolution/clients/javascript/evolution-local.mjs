// javascript 客户端：通过统一协议收集执行事件并接收策略建议。
// 原生入口复用包内 C ABI；注意句柄生命周期与跨语言内存释放。
// 打点的输入是建议修改前的请求，执行后必须回传实际输入、输出与状态。
// 取消、超时和采集缺口需要显式处理，不能把缺失证据当成正常完成。

import {createRequire} from 'node:module';
import {randomUUID} from 'node:crypto';
import {PointClient} from './evolution.mjs';

// 应用启动时创建一次；invoke 不使用 Promise、HTTP 或子进程。
export class LocalEvolution extends PointClient {
  // 初始化 constructor 的状态和资源；输入参数的校验与所有权在构造阶段确定。
  // 对象创建后复用既有配置，实际执行与收尾分别由后续接口负责。
  constructor({library, config, token, ...options}) {
    super();
    this.native = createRequire(import.meta.url)(library);
    this.handle = this.native.open(config, token, JSON.stringify(options));
    this.limit = options.max_request_bytes ?? 65536;
  }

  // 检查数据大小与遍历限制，避免热路径复制过大输入。
  fits(value, budget, depth = 0) {
    if (depth > 32 || budget < 0)
      return -1;
    if (typeof value === 'string')
      return budget - value.length * 6 - 2;
    if (value === null || typeof value === 'boolean')
      return budget - 32;
    if (typeof value === 'number')
      return Number.isFinite(value) ? budget - 32 : -1;
    if (typeof value !== 'object')
      return -1;
    for (const key of Object.keys(value)) {
      budget = this.fits(key, budget - 2, depth + 1);
      budget = this.fits(value[key], budget, depth + 1);
      if (budget < 0)
        return -1;
    }
    return budget - 2;
  }

  // 执行一次组件协议调用，检查响应信封并保持请求身份一致。
  invoke(operation, fields = {}, request_id = randomUUID()) {
    fields = fields && typeof fields === 'object' && !Array.isArray(fields) ? fields : {};
    const fallback = {
      degraded : true,
      reason : 'local_unavailable',
      capture_complete : false,
      policy_applied : false,
      input : fields.input,
      event_id : ''
    };
    if (operation === 'point') {
      const context = {...(fields.context ?? {})}, kind = fields.kind ?? context.kind ?? 'custom';
      Object.assign(context, {
        kind,
        degraded : true,
        caused_by : [],
        run_id : context.run_id ?? `unobserved-${request_id}`
      });
      Object.assign(fallback, {
        context,
        decision : kind === 'loop' ? 'continue' : 'allow',
        closed : false,
        ended : false
      });
    }
    if (operation === 'run.begin')
      fallback.run_id = `unobserved-${request_id}`;
    if (operation === 'activity.begin')
      Object.assign(fallback, {
        activity_id : `unobserved-${request_id}`,
        decision : 'allow',
        context :
            {run_id : fields.run_id, activity_id : `unobserved-${request_id}`, degraded : true}
      });
    if (operation === 'loop.check')
      Object.assign(fallback,
                    {control_id : `unobserved-${request_id}`, decision : 'continue', context : ''});
    if ([ 'activity.end', 'activity.abort', 'loop.commit' ].includes(operation))
      fallback.ended = false;
    if ([ 'run.end', 'run.abort' ].includes(operation))
      Object.assign(fallback, {closed : false, evaluation_status : 'quarantined'});
    try {
      const packet = {...fields, operation, request_id};
      if (this.fits(packet, this.limit) < 0)
        throw new Error('local_request_too_large');
      const response = JSON.parse(this.native.invoke(this.handle, JSON.stringify(packet)));
      if (response.ok !== true)
        throw new Error('local_unavailable');
      return response.result.degraded ? {...fallback, ...response.result} : response.result;
    } catch {
      try {
        const gap =
            operation === 'point' && fields.ref
                ? {operation : 'point', phase : 'gap', kind : 'run', ref : {run : fields.ref.run}}
                : {
                    operation : 'capture.gap',
                    run_id : fields.run_id ?? fields.context?.run_id,
                    reason : 'observer_unavailable'
                  };
        this.native.invoke(this.handle, JSON.stringify({...gap, request_id : randomUUID()}));
      } catch { /* 安全降级，不暴露输入或原生异常正文。 */
      }
      return fallback;
    }
  }

  // 提交框架无关打点，必要时在同一边界先取得策略决定，再记录实际执行结果。
  point(fields, request_id = randomUUID()) {
    fields = this.pointFields(fields);
    return this.pointObserved(fields, this.invoke('point', fields, request_id));
  }

  // 切换组件附加能力，不更改宿主原有业务和安全逻辑。
  setEnabled(enabled) {
    this.native.setEnabled(this.handle, Boolean(enabled));
  }

  // 读取组件自身的采集和排队指标，不把观测指标当作业务任务得分。
  metrics() {
    return JSON.parse(this.native.metrics(this.handle));
  }

  // 等待当前待持久化工作完成，用于维护或关闭；不要放在主 Agent 每个调用的热路径。
  drain() {
    this.native.drain(this.handle);
  } // 仅供维护使用，可能等待存储完成。

  // 关闭资源并等待本实现要求的在途操作完成，重复关闭按协议处理。
  close() {
    this.native.close(this.handle);
  } // 只在所有调用方停止后关闭。
}
