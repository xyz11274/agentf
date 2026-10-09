// 搜索节点选择：依据质量、效率及约束状态维护候选集合和比较结果。
// 父子关系表示生成来源，是否保留节点由评测证据决定，不要求叶节点自动胜过父节点。
// 分数相近或目标冲突时保留可比较信息，避免仅凭一次随机回答判定改进。

#pragma once

// 搜索树节点可从叶节点和旁支节点选父，融合时保留可追溯的祖先关系。

#include "integration_json.h"
#include <array>
#include <set>

namespace trajectory::evolution::integration::gepa {

// 输入是 task_id -> 指标对象。缺失或无效指标返回不可比较的低值。
double mean(const Json &scores, const std::string &field);
// 同一完整评测集合的 quality/strict 保护，不要求 fitness 或 loop 同时改善。
bool global_core_no_worse(const Json &candidate, const Json &reference);
// 仅比较完全相同的逐题评测集合。全局质量与 strict 均不退时才允许替换
// elite；目标达标代理不能抵消它们的下降。这是偏序，不作为 std::sort 比较器。
bool global_better(const Json &candidate, const Json &incumbent);
// 循环目标采用逐题约束，而不是只比较均值。这样可避免少数任务的大幅改善
// 掩盖另一些任务无效循环显著增加的退化。
bool loops_no_worse(const Json &candidate, const Json &parent);
// Pareto 接受条件：质量、适应度以及循环三类核心目标均不得倒退。
bool no_worse(const Json &candidate, const Json &parent);
// 调用方传入真实触发的同题子集。至少两题的净 strict/quality 收益，或质量和
// strict 不退的净无效循环减少，才支持局部探索；资源波动不独立构成收益。
bool local_gain(const Json &candidate, const Json &parent, double minimum = 0.000001);
// 统一读取单亲变异和双亲融合节点，调用方无需根据节点类型分支。
std::vector<std::size_t> parents(const Json &node);
// 迭代展开祖先而非递归，避免深搜索树造成调用栈增长；同时用集合阻断坏数据中的环。
std::set<std::size_t> ancestors(const Json &nodes, std::size_t index);
// 恢复搜索前先验证整棵树。任何悬空父节点、向前引用或重复节点编号都必须
// 在选择阶段前暴露，不能让损坏的持久化状态悄悄影响下一代。
void validate_nodes(Json &nodes);
// 判断节点能否作为后续反思的父节点。探索资格只影响搜索，不代表可发布。
bool exploration_parent(const Json &node);
// 根据已落盘的训练因果证据和完整验证成绩计算探索资格及原因。
Json exploration_eligibility(const Json &nodes, std::size_t index, const Json &config);
// 按样本组织前沿节点成绩，帮助识别不同节点的互补优势。
Json frontier_map(const Json &nodes, const std::vector<std::string> &tasks, const Json &config);
// 探索前沿可包含有真实局部收益、但尚未满足发布门槛的安全专长节点。
Json exploration_frontier_map(const Json &nodes, const std::vector<std::string> &tasks,
                              const Json &config);
// 建立可供父节点选择的候选集合，保留配置允许的历史和前沿节点。
std::vector<std::size_t> pool(const Json &mapping);
// 按候选在样本上的表现选择父节点，而非只沿最后生成的叶节点继续搜索。
std::size_t select_parent(const Json &nodes, const Json &mapping, const Json &config,
                          std::size_t round);
// 挑选具有互补证据的多个父节点，为下一代融合提供来源。
Json select_merge(const Json &nodes, const Json &mapping, const Json &attempts, const Json &config,
                  std::size_t round);
// 组合多个父节点的有效组件，并保留来源关系；融合后的策略仍需重新执行评测。
Json recombine(const Json &ancestor, const Json &first, const Json &second, std::size_t first_id,
               std::size_t second_id, bool prefer_first);
// 选择本轮要修改的策略组件，避免一次改写所有能力而失去归因。
Json select_components(const Json &components, const std::string &strategy, std::size_t cursor);
// 从候选提案中选择本轮评测对象，保留搜索预算和组件覆盖限制。
std::vector<std::size_t> select_proposals(const Json &nodes, const Json &ids, const Json &config);

} // 命名空间结束：trajectory::evolution::integration::gepa
