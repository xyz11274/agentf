// 边界运行时：维护 run/step/action 的生命周期与策略应用证据。
// 打点既采集事实又可能返回策略决策，必须区分建议输入、实际输入和实际执行结果。
// 异步完成由显式终点确认，不能用派发回执关闭整个 run。

#pragma once

#include "integration_json.h"
#include "trajectory_evolution/policy_runtime.h"

namespace trajectory::evolution::integration {

// 框架无关边界操作与兼容入口共享持久化、认证和评测，
// 但不复用其步骤或派发调度器。
void validate_boundary_configuration(const Json &config);
// 描述框架无关的输入与控制能力，供调用方发现可用插入点。
Json describe_boundary_contract(const Json &config);
// 从已记录边界事件统计运行指标，不能把未执行的策略建议计入调用次数。
Json boundary_metrics(const Json &events);
// 把边界打点状态整理为标准轨迹，保留执行、因果与结束信息。
Json boundary_trace(const Json &run);
// 限制查询文本规模，保留原业务请求不受检索裁剪影响。
std::string bounded_query_text(const Json &value, const Json &config, bool task);
// 从运行开始打点构造规范请求，绑定身份和上下文，不接管宿主调度器。
Json boundary_begin_request(const Json &request, const std::string &run_id);
// 建立单次运行的状态、策略快照与采集上下文，后续打点复用同一身份。
void initialize_boundary_run(Json &run, const Json &request);
// 按协议操作名分发打点；所有状态迁移经此入口校验，避免不同语言客户端绕过 run 的因果与关闭约束。
Json dispatch_boundary(Json &run, const Json &request, const Json &config,
                       const PolicyRuntime *cached_policy = nullptr);

} // 命名空间结束：trajectory::evolution::integration
