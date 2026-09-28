// 异步派发运行时：维护批次、依赖和分支完成状态。
// 提交工作与完成工作分开；回调只负责交付，后续打点确认执行结果。
// 串并行调整不改变分支身份，失败只影响声明依赖于它的后续工作。

#pragma once

#include "instrumentation.h"

namespace trajectory::evolution::integration {

// 派发回调只负责提交工作；完成结果由后续打点到达，
// 可能来自其他进程，全部波次状态由运行时持有。
Json dispatch_batch_point(Json &run, const Json &request, const Json &config,
                          const PolicyRuntime *cached_policy);

} // 命名空间结束：trajectory::evolution::integration
