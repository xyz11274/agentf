// 编排控制：根据规则与依赖关系规划动作或子任务的执行顺序。
// 先检查可用能力和动作身份，再决定并行、串行或跳过；不能凭文字名称猜测能力。
// 独立分支继续执行，依赖失败的分支保留明确原因和可回放状态。
// 此处是对外声明，具体实现位于 src；修改接口时需考虑现有调用方与持久化版本。

#pragma once

#include "runtime_policy.pb.h"
#include <string>
#include <vector>

namespace trajectory::evolution {

// 检查编排规则是否与运行时动作契约一致，避免生成后永远无法命中。
void validate_schedule_rules(const v1::RuntimeOptions &options);
// 结合可用技能和依赖生成串并行安排；不把模型建议直接当作已提交任务。
v1::SchedulePlan plan_schedule(const v1::RuntimeOptions &options, const v1::RuntimeQuery &query);

// 按批次持久化此状态，调用方复用现有运行事务串行化更新。
// 工作线程不能占着执行槽等待其他线程完成。
v1::ScheduleState begin_schedule(const v1::SchedulePlan &plan);
// 计算此刻真正可执行的动作集合，已终止或依赖未完成的动作不重复派发。
std::vector<std::string> ready_schedule(const v1::ScheduleState &state);
// 只更新本动作与受其依赖影响的后续节点，独立分支保留自己的状态。
v1::ScheduleState advance_schedule(v1::ScheduleState state, const std::string &action_id,
                                   const std::string &status);
// 完成表示计划中的动作已进入终态，不等于所有业务目标成功。
bool schedule_finished(const v1::ScheduleState &state);

} // 命名空间结束：trajectory::evolution
