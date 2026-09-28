// 控制规则生成：把经验和观测到的宿主能力转成候选编排与循环规则。
// 可修改范围来自公开打点能力与动作描述，不从基线成绩凭空推导宿主能力。
// 生成后仍需校验作用域、动作参数和运行证据，规则存在不代表真实生效。

#pragma once

// 规则是否生成、是否获授权、是否实际命中，是三件需要分别验证的事。

#include "integration_json.h"
#include "runtime_policy.pb.h"
#include "evaluation_dataset.pb.h"

namespace trajectory::evolution::integration {
// 验证控制学习配置与可用动作契约；不从评测基线臆造宿主能力。
void validate_control_learning(const Json &options);
// 构建控制生成所需的最小输入：可用动作、允许修改的字段及相关反馈。
Json control_generation_input(const v1::ShadowPolicySnapshot &snapshot,
                              const v1::RuntimeOptions &base, const Json &options);
// 把经过验证的控制草案写入候选策略；不改变源策略对象的既有身份约定。
v1::RuntimeOptions apply_control_draft(const v1::ShadowPolicySnapshot &snapshot,
                                       const v1::RuntimeOptions &base, const Json &options,
                                       const Json &draft);
// 校验生成规则是否落在宿主声明的执行能力范围，避免发布无法执行的控制。
bool generated_controls_authorized(const v1::RuntimePolicy &candidate,
                                   const v1::RuntimePolicy &current, const Json &options);
// 移除本轮模型生成的控制部分，保留其余策略内容，供替换或消融比较。
v1::RuntimeOptions without_generated_controls(v1::RuntimeOptions options);
// 统计真实控制介入，不统计规则存在或 L1 注入回执。
Json control_coverage(const Json &artifact, const dataset::v1::TaskEvaluationReport &report);
// 合并不同来源的控制覆盖证据，同一机会不会因重复上报而重复计数。
Json merge_control_coverage(Json coverage, const Json &previous);
} // 命名空间结束：trajectory::evolution::integration
