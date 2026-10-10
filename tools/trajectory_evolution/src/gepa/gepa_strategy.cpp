// 搜索节点选择：依据质量、效率及约束状态维护候选集合和比较结果。
// 父子关系表示生成来源，是否保留节点由评测证据决定，不要求叶节点自动胜过父节点。
// 分数相近或目标冲突时保留可比较信息，避免仅凭一次随机回答判定改进。

// 搜索树的父子关系、节点融合和排序在此实现，不直接启动 Agent。
#include "gepa_strategy.h"

#include <limits>
#include <random>

namespace trajectory::evolution::integration::gepa {
namespace {
// 本文件只负责 GEPA 搜索中的“选择与组合”，不执行模型调用，也不改变运行时策略。
// 所有判断都基于已落盘的节点、逐题验证分数和显式配置，因此中断恢复后应得到
// 与中断前相同的父节点、前沿和融合结果。
double number(const Json &value, double fallback = 0) {
  return std::holds_alternative<double>(value.value) ? std::get<double>(value.value) : fallback;
}

// 候选排序使用稳定的多目标字典序：质量和适应度优先，随后才比较循环与时延。
// 这里仅决定搜索顺序，不等价于发布门禁；发布仍需独立留出集复核。
bool better(const Json &a, const Json &b) {
  const auto feasible = mean(a, "feasible_score") - mean(b, "feasible_score");
  if (std::abs(feasible) > 1e-12 && mean(a, "feasible_score") > -1e9 &&
      mean(b, "feasible_score") > -1e9)
    return feasible > 0;
  const auto goal = mean(a, "goal_score") - mean(b, "goal_score");
  if (std::abs(goal) > 1e-12 && mean(a, "goal_score") > -1e9 && mean(b, "goal_score") > -1e9)
    return goal > 0;
  const auto robustness = mean(a, "robustness_score") - mean(b, "robustness_score");
  if (std::abs(robustness) > 1e-12 && mean(a, "robustness_score") > -1e9 &&
      mean(b, "robustness_score") > -1e9)
    return robustness > 0;
  const auto difference = mean(a, "quality") - mean(b, "quality");
  return difference > 1e-12 ||
         (std::abs(difference) <= 1e-12 && mean(a, "fitness") > mean(b, "fitness") + 1e-12);
}
}

// 输入是 task_id -> 指标对象。缺失字段按零处理，以免不完整评测被误当成高分。
double mean(const Json &scores, const std::string &field) {
  if (!scores.object() || scores.fields().empty())
    return -1e9;
  double total = 0;
  for (const auto &item : scores.fields()) {
    if (!item.second.has(field))
      return -1e9;
    total += number(item.second[field], -1e9);
  }
  return total / scores.fields().size();
}

// 循环目标采用逐题约束，而不是只比较均值。这样可避免少数任务的大幅改善
// 掩盖另一些任务无效循环显著增加的退化。
bool loops_no_worse(const Json &candidate, const Json &parent) {
  if (!candidate.object() || !parent.object() || candidate.fields().empty() ||
      candidate.fields().size() != parent.fields().size())
    return false;
  for (const auto &item : parent.fields())
    if (!candidate.has(item.first))
      return false;
  std::size_t observed = 0;
  for (const auto &item : parent.fields()) {
    const auto &a = candidate[item.first];
    const auto &b = item.second;
    if (!a.has("invalid_loops") && !b.has("invalid_loops"))
      continue;
    if (!a.has("invalid_loops") || !b.has("invalid_loops") || a["invalid_loops"].null() ||
        b["invalid_loops"].null() || a.str("loop_method") != b.str("loop_method"))
      return false;
    ++observed;
  }
  // 关闭循环目标时没有可比较的计数；部分观测不能被当作零值。
  return !observed || (observed == parent.fields().size() &&
                       mean(candidate, "invalid_loops") <= mean(parent, "invalid_loops") + 1e-12);
}

// Pareto 接受条件：质量、适应度以及循环三类核心目标均不得倒退。
bool no_worse(const Json &candidate, const Json &parent) {
  if (!loops_no_worse(candidate, parent))
    return false;
  return mean(candidate, "quality") >= mean(parent, "quality") - 1e-12 &&
         mean(candidate, "fitness") >= mean(parent, "fitness") - 1e-12;
}

// “局部专长”允许节点总体均值不占优，但至少在一个任务上形成超过阈值的改进。
// 该信号用于保留多样分支，最终能否发布仍由全局验证与留出门禁决定。
bool local_gain(const Json &candidate, const Json &parent, double minimum) {
  if (!candidate.object() || !parent.object() || parent.fields().empty() ||
      candidate.fields().size() != parent.fields().size())
    return false;
  bool gained = false;
  for (const auto &task : parent.fields()) {
    if (!candidate.has(task.first))
      return false;
    const auto &a = candidate[task.first];
    const auto &b = task.second;
    for (const auto *key : {"quality", "fitness"})
      if (!a.has(key) || !b.has(key) || a[key].null() || b[key].null())
        return false;
    if (a.has("invalid_loops") || b.has("invalid_loops"))
      if (!a.has("invalid_loops") || !b.has("invalid_loops") || a["invalid_loops"].null() ||
          b["invalid_loops"].null() || a.str("loop_method") != b.str("loop_method"))
        return false;
    const auto quality = number(a["quality"]) - number(b["quality"]);
    gained |= quality > minimum;
    for (const auto *key : {"fitness", "loop_efficiency", "token_efficiency", "latency_efficiency",
                            "goal_score", "robustness_score"}) {
      if (a.has(key) != b.has(key) || (a.has(key) && (a[key].null() || b[key].null())))
        return false;
      // 保留在特定任务上真正改进的专长节点，不能把同一任务失败换来的速度收益当作改进。
      if (a.has(key) && quality >= -1e-12)
        gained |= number(a[key]) > number(b[key]) + minimum;
    }
  }
  return gained;
}

// 统一读取单亲变异和双亲融合节点，调用方无需根据节点类型分支。
std::vector<std::size_t> parents(const Json &node) {
  std::vector<std::size_t> result;
  if (node.has("parents")) {
    for (const auto &id : node["parents"].items())
      result.push_back(id.integer());
  } else if (!node["parent"].null())
    result.push_back(node["parent"].integer());
  return result;
}

// 迭代展开祖先而非递归，避免深搜索树造成调用栈增长；同时用集合阻断坏数据中的环。
std::set<std::size_t> ancestors(const Json &nodes, std::size_t index) {
  std::set<std::size_t> result;
  auto pending = parents(nodes.items().at(index));
  while (!pending.empty()) {
    const auto id = pending.back();
    pending.pop_back();
    require(id < index, "gepa_tree_invalid");
    if (result.insert(id).second)
      for (auto p : parents(nodes.items().at(id))) {
        require(p < id, "gepa_tree_invalid");
        pending.push_back(p);
      }
  }
  return result;
}

bool exploration_parent(const Json &node) {
  return (node.str("status") == "accepted" &&
          (!node.has("validation_gain_attributed") ||
           node.flag("validation_gain_attributed", false))) ||
         node.flag("exploration_eligible", false);
}

// 搜索父节点可以保留专长分支，但不能把一个已在 Val 上整体退化的节点当作
// 通用父版本。这里以 L0 的完整验证为锚点，给单批测量噪声留 10% 资源余量；
// 这不是发布门禁，仍允许后续从质量/loop 专长继续探索。
bool safe_exploration_candidate(const Json &nodes, std::size_t index, const Json &config) {
  if (!nodes.array() || index >= nodes.items().size() || index == 0)
    return index == 0;
  const auto &node = nodes.items()[index], &root = nodes.items().front();
  if (!node["validation"].object() || node["validation"].fields().empty() ||
      !root["validation"].object() || root["validation"].fields().empty())
    return false;
  if (node.str("status") == "unobserved_changes" &&
      !node.flag("validation_gain_attributed", false)) {
    // 未归因节点可以作为少量专长探索，但必须先证明没有相对 L0 的整体灾难退化。
    if (!node.flag("exploration_eligible", false))
      return false;
  } else if (node.str("status") != "accepted") {
    return false;
  }
  // 未命中的新增规则仍需披露；只要筛选阶段有本次改动的真实应用，
  // 它不阻断非发布探索。完整覆盖仍由最终门禁决定。
  if (node.str("status") == "unobserved_changes" &&
      !node["screening"].flag("controls_observed", false))
    return false;
  const auto quality = mean(node["validation"], "quality");
  const auto root_quality = mean(root["validation"], "quality");
  const auto allowed_drop =
      std::clamp(number(config["exploration_max_quality_drop"], 0.15), 0.0, 0.15);
  if (quality + allowed_drop < root_quality - 1e-12)
    return false;
  const auto loops = mean(node["validation"], "invalid_loops");
  const auto root_loops = mean(root["validation"], "invalid_loops");
  if (loops > -1e8 && root_loops > -1e8 && loops > root_loops * 1.10 + 1e-9)
    return false;
  for (const auto *field : {"duration_ms", "tokens"}) {
    const auto value = mean(node["validation"], field);
    const auto baseline = mean(root["validation"], field);
    if (value > -1e8 && baseline > -1e8 && value > baseline * 1.10 + 1e-9)
      return false;
  }
  return true;
}

Json exploration_eligibility(const Json &nodes, std::size_t index, const Json &config) {
  auto result = [](bool eligible, const char *reason) {
    return Json{{"eligible", eligible}, {"reason", reason}};
  };
  if (!nodes.array() || index >= nodes.items().size())
    return result(false, "node_unavailable");
  const auto &node = nodes.items()[index];
  if (node.str("status") == "accepted" &&
      (!index || !node.has("validation_gain_attributed") ||
       node.flag("validation_gain_attributed", false)))
    return result(true, "accepted");
  // Pool 可以带着归因疑点进入独立 Val；探索父节点必须在 Val 中看到
  // 本次改动实际应用并产生局部收益，不能把批次整体随机改善当成策略效果。
  if (node.str("status") != "unobserved_changes")
    return result(false, "status_not_explorable");
  const auto ids = parents(node);
  if (ids.empty() || ids.size() > 2 ||
      std::any_of(ids.begin(), ids.end(), [&](auto parent) { return parent >= index; }))
    return result(false, "parent_unavailable");
  auto attributed_gain = [&](const Json &candidate_scores, const Json &parent_scores,
                             const Json &changes) {
    Json candidate = Json::Object{}, previous = Json::Object{};
    if (!changes.object() || changes.fields().empty())
      return false;
    for (const auto &task : changes.fields()) {
      bool applied = false;
      if (task.second.object())
        for (const auto &boundary : task.second.fields())
          applied |= number(boundary.second, 0) > 0;
      if (applied && candidate_scores.has(task.first) && parent_scores.has(task.first)) {
        candidate[task.first] = candidate_scores[task.first];
        previous[task.first] = parent_scores[task.first];
      }
    }
    return local_gain(candidate, previous, number(config["min_improvement"], 0.000001));
  };
  if (ids.size() == 1) {
    const auto &screening = node["screening"];
    if (node.str("kind") != "reflection" || screening.str("split", "train") != "train" ||
        !screening.flag("controls_observed", false))
      return result(false, "training_control_not_applied");
    if (!attributed_gain(node["validation"], nodes.items()[ids.front()]["validation"],
                         node["validation_applied_changes"][std::to_string(ids.front())]))
      return result(false, "validation_control_gain_missing");
  } else {
    if (node.str("kind") != "merge" || !node["validation_applied_changes"].object())
      return result(false, "merge_contribution_missing");
    bool validation_gain = false;
    for (const auto parent : ids)
      validation_gain |= attributed_gain(
          node["validation"], nodes.items()[parent]["validation"],
          node["validation_applied_changes"][std::to_string(parent)]);
    if (!validation_gain)
      return result(false, "validation_merge_gain_missing");
  }
  const auto &validation = node["validation"];
  const auto &seed_validation = nodes.items().front()["validation"];
  if (!validation.object() || validation.fields().empty() || !seed_validation.object() ||
      validation.fields().size() != seed_validation.fields().size())
    return result(false, "validation_incomplete");
  for (const auto &task : seed_validation.fields())
    if (!validation.has(task.first) || !validation[task.first].has("quality") ||
        !task.second.has("quality"))
      return result(false, "validation_incomplete");
  const auto quality = mean(validation, "quality");
  const auto allowed_drop =
      std::clamp(number(config["exploration_max_quality_drop"], 0.15), 0.0, 0.15);
  double reference_quality = mean(seed_validation, "quality");
  for (const auto parent : ids) {
    const auto &parent_validation = nodes.items()[parent]["validation"];
    if (!parent_validation.object() ||
        parent_validation.fields().size() != validation.fields().size())
      return result(false, "validation_incomplete");
    reference_quality = std::max(reference_quality, mean(parent_validation, "quality"));
  }
  if (quality <= -1e9 || reference_quality <= -1e9 ||
      quality + allowed_drop < reference_quality - 1e-12)
    return result(false, "validation_quality_disaster");
  auto eligible = result(true, ids.size() == 1 ? "validation_control_gain_with_safety_guard"
                                               : "validation_merge_gain_with_safety_guard");
  eligible["quality"] = quality;
  eligible["reference_quality"] = reference_quality;
  eligible["maximum_quality_drop"] = allowed_drop;
  return eligible;
}

// 恢复搜索前先验证整棵树。任何悬空父节点、向前引用或重复节点编号都必须
// 在选择阶段前暴露，不能让损坏的持久化状态悄悄影响下一代。
void validate_nodes(Json &nodes) {
  require(nodes.array() && !nodes.items().empty(), "gepa_tree_invalid");
  for (std::size_t i = 0; i < nodes.items().size(); ++i) {
    auto &node = nodes.items()[i];
    const auto ids = parents(node);
    require(node.num("id", SIZE_MAX) == i && ids.size() <= 2 && (i ? !ids.empty() : ids.empty()),
            "gepa_tree_invalid");
    std::set<std::size_t> unique;
    std::size_t depth = 0;
    for (auto id : ids) {
      require(id < i && unique.insert(id).second, "gepa_tree_invalid");
      const auto &parent = nodes.items()[id];
      // 归因纠正不删除历史父子边；新父节点仍只从 accepted 节点中选择。
      require(exploration_parent(parent) ||
                  (parent.str("status") == "unobserved_changes" &&
                   parent.str("attribution_previous_status") == "accepted"),
              "gepa_tree_invalid");
      depth = std::max(depth, nodes.items()[id].num("depth", 0) + 1);
    }
    require(node.num("depth", SIZE_MAX) == depth &&
                (i ? node.num("parent", SIZE_MAX) == ids.front() : node["parent"].null()),
            "gepa_tree_invalid");
    if (!node.has("parents")) {
      node["parents"] = Json::Array{};
      for (auto id : ids)
        node["parents"].push(id);
    }
    if (!node.has("kind"))
      node["kind"] = i ? "reflection" : "seed";
    require((ids.size() == 2) == (node.str("kind") == "merge"), "gepa_tree_invalid");
    if (ids.size() == 2) {
      const auto a = ancestors(nodes, ids[0]), b = ancestors(nodes, ids[1]);
      const auto base = node.num("ancestor", SIZE_MAX);
      require(!a.count(ids[1]) && !b.count(ids[0]) && a.count(base) && b.count(base),
              "gepa_tree_invalid");
    }
  }
}

// 按样本组织前沿节点成绩，帮助识别不同节点的互补优势。
Json frontier_map(const Json &nodes, const std::vector<std::string> &tasks, const Json &config) {
  // 为每道验证题分别维护非支配前沿。这样总体均值一般、但在特定任务上有效的
  // 分支仍可成为父节点，避免搜索过早塌缩为单一路径。
  Json mapping = Json::Object{};
  const auto mode = config.str("frontier_type", "instance");
  auto add = [&](const std::string &key, const std::string &task, const std::string &field) {
    double best = -std::numeric_limits<double>::infinity();
    Json ids = Json::Array{};
    for (std::size_t i = 0; i < nodes.items().size(); ++i) {
      const auto &node = nodes.items()[i];
      // 非根节点即使被旧版错误标为 accepted，只要已经明确无法把验证收益
      // 归因到策略应用，就不能继续进入父节点池。启动迁移会修正其状态，
      // 这里再保留一道防线，避免异常中断留下的半迁移检查点污染下一代。
      if (node.str("status") != "accepted" ||
          (i && node.has("validation_gain_attributed") &&
           !node.flag("validation_gain_attributed", false)))
        continue;
      const auto value = task.empty() ? mean(node["validation"], field)
                                      : number(node["validation"][task][field], -1e9);
      if (value <= -1e9)
        continue;
      if (value > best + 1e-12) {
        ids = Json::Array{};
        best = value;
      }
      if (value >= best - 1e-12)
        ids.push(i);
    }
    if (!ids.items().empty())
      mapping[key] = ids;
  };
  if (mode == "instance" || mode == "hybrid")
    for (const auto &task : tasks)
      add("task:" + task, task, "fitness");
  if (mode == "objective" || mode == "hybrid") {
    add("objective:quality", {}, "quality");
    for (const auto *field : {"feasible_score", "goal_score", "robustness_score", "loop_efficiency",
                              "token_efficiency", "latency_efficiency"})
      add(std::string("objective:") + field, {}, field);
  }
  if (mode == "cartesian")
    for (const auto &task : tasks)
      for (const auto *field :
           {"quality", "loop_efficiency", "token_efficiency", "latency_efficiency"})
        add("pair:" + Json(Json::Array{task, field}).dump(), task, field);
  // 删除被“不差于它且覆盖范围包含它”的候选支配的节点；覆盖与分数相同时
  // 保留一个确定性代表，避免同一前沿被重复扩张。
  if (config.flag("prune_dominated", true)) {
    std::map<std::size_t, std::set<std::string>> coverage;
    for (const auto &item : mapping.fields())
      for (const auto &id : item.second.items())
        coverage[id.integer()].insert(item.first);
    std::set<std::size_t> removed;
    for (const auto &a : coverage)
      for (const auto &b : coverage)
        if (a.first != b.first) {
          const auto &av = nodes.items()[a.first]["validation"];
          const auto &bv = nodes.items()[b.first]["validation"];
          if (std::includes(b.second.begin(), b.second.end(), a.second.begin(), a.second.end()) &&
              no_worse(bv, av) && (b.second != a.second || better(bv, av) || b.first < a.first)) {
            removed.insert(a.first);
            break;
          }
        }
    for (auto &item : mapping.fields()) {
      Json ids = Json::Array{};
      for (const auto &id : item.second.items())
        if (!removed.count(id.integer()))
          ids.push(id);
      require(!ids.items().empty(), "gepa_frontier_empty");
      item.second = ids;
    }
  }
  return mapping;
}

Json exploration_frontier_map(const Json &nodes, const std::vector<std::string> &tasks,
                              const Json &config) {
  auto eligible = nodes;
  for (std::size_t i = 0; i < eligible.items().size(); ++i) {
    auto &node = eligible.items()[i];
    if (!exploration_parent(node))
      continue;
    if (!safe_exploration_candidate(eligible, i, config)) {
      if (i)
        node["status"] = "exploration_ineligible";
      continue;
    }
    node["status"] = "accepted";
    node["validation_gain_attributed"] = true;
  }
  auto mapping = frontier_map(eligible, tasks, config);
  if (!eligible.items().empty())
    mapping["anchor:root"] = Json::Array{0};
  std::size_t strict = 0;
  for (std::size_t i = 1; i < nodes.items().size(); ++i)
    if (nodes.items()[i].str("status") == "accepted" &&
        (!nodes.items()[i].has("validation_gain_attributed") ||
         nodes.items()[i].flag("validation_gain_attributed", false)) &&
        better(nodes.items()[i]["validation"], nodes.items()[strict]["validation"]))
      strict = i;
  mapping["anchor:strict_winner"] = Json::Array{strict};
  return mapping;
}

// 建立可供父节点选择的候选集合，保留配置允许的历史和前沿节点。
std::vector<std::size_t> pool(const Json &mapping) {
  // 将逐题前沿去重为候选池；节点在多道题命中时仍只保留一次。
  std::set<std::size_t> ids;
  for (const auto &item : mapping.fields())
    for (const auto &id : item.second.items())
      ids.insert(id.integer());
  return {ids.begin(), ids.end()};
}

// 按候选在样本上的表现选择父节点，而非只沿最后生成的叶节点继续搜索。
std::size_t select_parent(const Json &nodes, const Json &mapping, const Json &config,
                          std::size_t round) {
  // 父节点选择只在当前前沿池内进行。轮询模式保证弱势专长分支仍有繁殖机会，
  // 最优模式则用于收敛阶段集中预算；两种模式都不会读取最终留出集。
  auto ids = pool(mapping);
  require(!ids.empty(), "gepa_frontier_empty");
  const auto strategy = config.str("candidate_selection_strategy", "pareto");
  std::vector<std::size_t> accepted;
  for (const auto i : ids)
    if (i < nodes.items().size() && exploration_parent(nodes.items()[i]))
      accepted.push_back(i);
  require(!accepted.empty(), "gepa_frontier_empty");
  auto ranking = [&](auto a, auto b) {
    return better(nodes.items()[a]["validation"], nodes.items()[b]["validation"]);
  };
  std::stable_sort(accepted.begin(), accepted.end(), ranking);
  if (strategy == "current_best")
    return accepted.front();
  std::mt19937 random(static_cast<unsigned>(config.num("seed", 42) + round));
  if (strategy == "epsilon_greedy")
    return std::bernoulli_distribution(number(config["epsilon"], .1))(random)
               ? accepted[std::uniform_int_distribution<std::size_t>(0,
                                                                     accepted.size() - 1)(random)]
               : accepted.front();
  if (strategy == "top_k_pareto") {
    std::stable_sort(ids.begin(), ids.end(), ranking);
    ids.resize(std::min(ids.size(), config.num("top_k", 5)));
  }
  std::vector<double> weights;
  for (auto id : ids) {
    double weight = 0;
    for (const auto &item : mapping.fields())
      for (const auto &candidate : item.second.items())
        if (candidate.integer() == id)
          ++weight;
    if (strategy == "fair") {
      weights.push_back(1.0 / (1.0 + nodes.items()[id].num("selections", 0)));
      continue;
    }
    // Pareto 前沿保留局部专长，但不能让已经反复失败的根/弱分支持续吞掉
    // 反思预算。质量因子和新鲜度因子只改变抽样概率，不改变前沿成员资格。
    const auto quality = mean(nodes.items()[id]["validation"], "quality");
    const auto quality_factor = quality <= -1e8
                                    ? 0.25
                                    : std::max(0.25, std::min(1.25, 0.5 + quality));
    const auto loop_factor = mean(nodes.items()[id]["validation"], "invalid_loops") <= -1e8
                                 ? 1.0
                                 : 1.0 / (1.0 +
                                          0.10 * std::max(0.0, mean(
                                                           nodes.items()[id]["validation"],
                                                           "invalid_loops")));
    const auto freshness = 1.0 / (1.0 + 0.35 * nodes.items()[id].num("selections", 0));
    weights.push_back(std::max(1e-6, weight * quality_factor * loop_factor * freshness));
  }
  return ids[std::discrete_distribution<std::size_t>(weights.begin(), weights.end())(random)];
}

// 挑选具有互补证据的多个父节点，为下一代融合提供来源。
Json select_merge(const Json &nodes, const Json &mapping, const Json &attempts, const Json &config,
                  std::size_t round) {
  // 合并只能从前沿节点中选取：单亲扩张探索当前最优，双亲合并探索互补改进。
  // 双亲候选必须来自不同、互不为祖先的分支，并共享一个可追溯祖先。
  // attempts 记录已试组合，确保恢复后不会反复花费模型预算生成同一对子节点。
  const auto ids = pool(mapping);
  std::vector<std::array<std::size_t, 2>> pairs;
  for (std::size_t a = 0; a < ids.size(); ++a)
    for (std::size_t b = a + 1; b < ids.size(); ++b)
      pairs.push_back({ids[a], ids[b]});
  std::mt19937 random(static_cast<unsigned>(config.num("seed", 42) + round));
  std::shuffle(pairs.begin(), pairs.end(), random);
  for (const auto &pair : pairs) {
    const auto a = ancestors(nodes, pair[0]), b = ancestors(nodes, pair[1]);
    if (a.count(pair[1]) || b.count(pair[0]))
      continue;
    std::vector<std::size_t> common;
    std::set_intersection(a.begin(), a.end(), b.begin(), b.end(), std::back_inserter(common));
    std::stable_sort(common.begin(), common.end(), [&](auto x, auto y) {
      return nodes.items()[x].num("depth", 0) > nodes.items()[y].num("depth", 0);
    });
    for (auto base : common) {
      // 前沿专长节点的整体均值可能较低，但会在不同任务上各有提升。
      // 因而要求相对共同祖先存在真实局部增益；生成的子节点仍须分别胜过两个父节点。
      auto gain = [&](std::size_t id) {
        const auto &candidate = nodes.items()[id]["validation"];
        const auto &ancestor = nodes.items()[base]["validation"];
        if (config.str("acceptance") == "pareto")
          return local_gain(candidate, ancestor, number(config["min_improvement"], 0.000001));
        if (candidate.fields().size() != ancestor.fields().size())
          return false;
        bool improved = false;
        for (const auto &task : ancestor.fields()) {
          if (!candidate.has(task.first))
            return false;
          improved |=
              better(Json{{task.first, candidate[task.first]}}, Json{{task.first, task.second}});
        }
        return improved;
      };
      if (!gain(pair[0]) || !gain(pair[1]))
        continue;
      bool seen = false;
      for (const auto &attempt : attempts.items())
        seen |= attempt.num("first", SIZE_MAX) == pair[0] &&
                attempt.num("second", SIZE_MAX) == pair[1] &&
                attempt.num("ancestor", SIZE_MAX) == base;
      if (!seen)
        return Json{{"first", pair[0]}, {"second", pair[1]}, {"ancestor", base}};
    }
  }
  return nullptr;
}

// 组合多个父节点的有效组件，并保留来源关系；融合后的策略仍需重新执行评测。
Json recombine(const Json &ancestor, const Json &first, const Json &second, std::size_t first_id,
               std::size_t second_id, bool prefer_first) {
  require(ancestor.object() && first.object() && second.object() &&
              first.fields().size() == ancestor.fields().size() &&
              second.fields().size() == ancestor.fields().size(),
          "gepa_components_changed");
  Json result{
      {"components", Json::Object{}}, {"sources", Json::Object{}}, {"conflicts", Json::Array{}}};
  // 采用三方合并：共同祖先提供基线，两个父节点各自相对祖先的改动才有资格进入子代。
  // 当双方同时改写同一组件时记录冲突，并按调用方给定的稳定优先级选择，保证可复现。
  std::set<std::size_t> contributors;
  bool complementary = false;
  for (const auto &item : ancestor.fields()) {
    const auto &key = item.first;
    require(first.has(key) && second.has(key), "gepa_components_changed");
    const auto old = item.second.dump(), a = first[key].dump(), b = second[key].dump();
    bool use_first = prefer_first;
    if (a == b)
      use_first = true;
    else if (a == old) {
      use_first = false;
      complementary = true;
    } else if (b == old) {
      use_first = true;
      complementary = true;
    } else
      result["conflicts"].push(key);
    const auto source = use_first ? first_id : second_id;
    result["components"][key] = use_first ? first[key] : second[key];
    result["sources"][key] = source;
    if (a != b)
      contributors.insert(source);
  }
  // 名义上的双亲边必须实际合并两个分支的改动，不能退化为单亲复制。
  result["useful"] = complementary && contributors.size() == 2;
  return result;
}

// 选择本轮要修改的策略组件，避免一次改写所有能力而失去归因。
Json select_components(const Json &components, const std::string &strategy, std::size_t cursor) {
  Json selected = Json::Array{};
  if (strategy == "round_robin") {
    // 组件轮询将 L1 经验、L2 编排、L3 循环控制交错安排，防止经验条目数量较多时
    // 长期挤占控制策略的变异预算。
    // 让可执行控制规则与经验改写交替进入候选。经验集合很大时，也不能在再次
    // 探索 L2/L3 前耗尽所有有效轮次。
    std::vector<std::string> experiences;
    for (const auto &item : components.fields())
      if (item.first != "action_rules" && item.first != "loop_rules" &&
          item.first != "schedule_rules")
        experiences.push_back(item.first);
    if (experiences.empty()) {
      for (const auto *name : {"schedule_rules", "action_rules", "loop_rules"})
        if (components.has(name))
          selected.push(name);
    } else {
      for (std::size_t i = 0; i < experiences.size();) {
        if (components.has("schedule_rules"))
          selected.push("schedule_rules");
        if (components.has("action_rules"))
          selected.push("action_rules");
        selected.push(experiences[i++]);
        if (components.has("loop_rules"))
          selected.push("loop_rules");
        if (i < experiences.size())
          selected.push(experiences[i++]);
      }
    }
  } else
    for (const auto &item : components.fields())
      selected.push(item.first);
  require(!selected.items().empty(), "gepa_components_empty");
  if (strategy == "round_robin")
    return Json::Array{selected.items()[cursor % selected.items().size()]};
  return selected;
}

// 从候选提案中选择本轮评测对象，保留搜索预算和组件覆盖限制。
std::vector<std::size_t> select_proposals(const Json &nodes, const Json &ids, const Json &config) {
  std::vector<std::size_t> result;
  for (const auto &id : ids.items())
    if (nodes.items().at(id.integer()).str("status") == "screened")
      result.push_back(id.integer());
  // 预筛选只缩小需要做昂贵完整验证的集合；未通过筛选的节点仍保留在搜索树中，
  // 便于审计失败原因，但不会进入发布候选。
  const auto strategy = config.str("proposal_selection", "all_improvements");
  if (strategy != "all_improvements") {
    auto improvement = [&](auto id) {
      const auto &screening = nodes.items()[id]["screening"];
      return Json{{"gain", Json{{"quality", mean(screening["candidate"], "quality") -
                                                mean(screening["parent"], "quality")},
                                {"fitness", mean(screening["candidate"], "fitness") -
                                                mean(screening["parent"], "fitness")}}}};
    };
    std::stable_sort(result.begin(), result.end(),
                     [&](auto a, auto b) { return better(improvement(a), improvement(b)); });
    result.resize(std::min(result.size(),
                           strategy == "best" ? std::size_t(1) : config.num("proposal_top_k", 2)));
  }
  return result;
}
} // 命名空间结束：trajectory::evolution::integration::gepa
