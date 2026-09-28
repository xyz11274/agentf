// 通用打点解释：将语言无关的请求映射到标准事件与策略边界。
// 所有宿主关联信息通过公开字段传递，不读取具体框架的内部对象。
// 输入验证、身份绑定和起止关系在此收口，调用方不必实现额外观察器。

#pragma once

#include "integration_json.h"
#include "trajectory_evolution/policy_runtime.h"

namespace trajectory::evolution::integration {

// 所有语言客户端使用同一 JSON 信封，不包含宿主调度器或框架类型。
Json point_request(const Json &request, const Json &config);
// 把组件结果转换为宿主可消费的打点回执，保留降级与实际控制状态。
Json point_result(const Json &request, const Json &normalized, Json result);
// 在观测组件不可用时生成受限降级结果，保持原业务输入并标记采集缺口。
Json point_fallback(const Json &request, const std::string &reason);
// 验证采集与注入配置，保留框架无关的协议边界。
void validate_instrumentation(const Json &config);
// 可选宿主标识替代跨进程手工保存上下文。
std::string point_run_key(const Json &request);
// 提取调用级稳定键，区分同一运行内并发操作。
std::string point_call_key(const Json &request);
// 生成传输请求幂等键，重复请求不能创建第二个业务调用。
std::string point_request_key(const Json &request);
// 把调用方稳定引用解析为内部执行身份；异步跨进程传递仍使用公开引用字段。
Json resolve_point_reference(const Json &request, Json &run, const std::string &run_id);
// 保存起点的关联信息，终点必须能找到对应起点，不能串接同名的其他调用。
void record_point_reference(Json &run, const Json &request, const Json &result);
// 按稳定引用恢复并推进派发打点，适配异步、跨线程和跨进程返回。
Json dispatch_referenced_point(Json &run, const Json &request, const Json &config,
                               const PolicyRuntime *cached_policy = nullptr);
// 异常路径释放或标记未闭合引用，避免正常路径之外的 return 留下悬挂状态。
void abort_point_reference(Json &run, const Json &request, const Json &config,
                           const PolicyRuntime *cached_policy = nullptr) noexcept;

} // 命名空间结束：trajectory::evolution::integration
