// 策略运行时：解释固定版本的 L1 经验、L2 编排和 L3 循环控制。
// 读取策略不会触发在线模型反思；决策使用当前请求、结构条件和执行事实。
// 每次应用都返回归属信息，便于区分规则命中、建议被采纳与真实执行效果。
// 此处是对外声明，具体实现位于 src；修改接口时需考虑现有调用方与持久化版本。

#pragma once

#include "runtime_policy.pb.h"
#include "trajectory_evolution/shadow_runtime.h"

namespace trajectory::evolution {

// 编译阶段把经验快照和经校验的选项固定为一个不可变策略版本；后续运行时只读取该版本。
// 同一 policy_id 在不同打点处共享同一套 L1、L2、L3 规则，具体决定仍取决于当前查询。
// 将快照与批准项编译成固定版本，校验能力等级、作用域和内容版本。
v1::RuntimePolicy compile_runtime_policy(v1::ShadowPolicySnapshot snapshot,
                                         const v1::RuntimeOptions &options);

// 纯粹、不可变的 C++ 控制层；宿主持有执行和循环状态。
class PolicyRuntime {
public:
  // 初始化 PolicyRuntime 的状态和资源；输入参数的校验与所有权在构造阶段确定。
  // 对象创建后复用既有配置，实际执行与收尾分别由后续接口负责。
  explicit PolicyRuntime(v1::RuntimePolicy policy);

  // 返回当前不可变策略对象，不触发重新编译或远程调用。
  const v1::RuntimePolicy &policy() const {
    return policy_;
  }

  // 决策函数没有副作用：它只把当前打点查询映射为建议、控制或循环结论。
  // 业务状态只能在宿主随后提交实际执行结果时推进，不能把策略建议误记为已发生事实。
  // 运行时读取当前边界事实并产生有界决策；这里不调用远程模型进行语义裁判。
  v1::RuntimeDecision decide(const v1::RuntimeQuery &query) const;
  // 重新计算控制决定，不能把调用方传入的计划直接当作证据。
  v1::RuntimeReceipt verify(const v1::RuntimeQuery &query,
                            const v1::RuntimeObservation &observation) const;
  // 每次调用的原生请求布局只读取绑定字段，
  // 不会修改冻结策略、授权、身份或上下文预算。
  v1::RuntimeReceipt verify(const v1::RuntimeQuery &query,
                            const v1::RuntimeObservation &observation,
                            const v1::RuntimeOptions &request_binding) const;

private:
  const v1::RuntimePolicy policy_;
  const ShadowRuntime shadow_;
  std::set<std::string> approved_ids_;
};

} // 命名空间结束：trajectory::evolution
