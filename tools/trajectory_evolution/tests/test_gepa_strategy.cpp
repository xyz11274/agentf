// test_gepa_strategy 回归验证：检查包内协议、边界条件与失败处理。
// 本地桩用于重现确定性场景，不代表真实 Agent 或远程模型的业务能力。
// 测试应同时验证正常路径与拒绝路径，避免错误数据被当成成功结果。

#include "../src/gepa/gepa_strategy.h"
#include <iostream>
#include <stdexcept>

using trajectory::evolution::integration::Json;
namespace gepa = trajectory::evolution::integration::gepa;

// 测试辅助函数：check。
// 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
void check(bool value) {
  if (!value)
    throw std::runtime_error("GEPA strategy assertion failed");
}

template <class F> void rejects(F fn) {
  try {
    fn();
  } catch (const trajectory::evolution::integration::Error &) {
    return;
  }
  throw std::runtime_error("expected GEPA strategy rejection");
}

// 将有效试验转换为逐题连续质量和代价分量，供前沿选择与父节点比较。
// 严格通过率另行保留；关键违规或硬检查失败不能靠部分成功获得可发布成绩。
Json scores(double a, double b) {
  return Json{{"a", Json{{"quality", a}, {"fitness", a}}},
              {"b", Json{{"quality", b}, {"fitness", b}}}};
}

// 测试辅助函数：node。
// 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
Json node(std::size_t id, Json parent, std::size_t depth, double a, double b) {
  return Json{{"id", id},
              {"parent", parent},
              {"depth", depth},
              {"status", "accepted"},
              {"validation", scores(a, b)}};
}

// 回归用例：main。
// 重点核验字段或诊断：attribution_previous_status、candidate_selection_strategy、top_k_pareto；异常路径不能被当成成功。
int main() {
  try {
    Json nodes = Json::Array{node(0, nullptr, 0, 0, 0), node(1, 0, 1, 1, 0), node(2, 0, 1, 0, 1),
                             node(3, 1, 2, .8, 0)};
    gepa::validate_nodes(nodes);
    check(gepa::parents(nodes.items()[0]).empty());
    auto corrected = nodes;
    corrected.items()[1]["status"] = "unobserved_changes";
    rejects([&] { gepa::validate_nodes(corrected); });
    corrected.items()[1]["attribution_previous_status"] = "accepted";
    gepa::validate_nodes(corrected);
    for (const auto id : gepa::pool(gepa::frontier_map(corrected, {"a", "b"}, Json{})))
      check(id != 1);
    check(gepa::parents(nodes.items()[1]) == std::vector<std::size_t>{0});
    const Json config{{"seed", 42}};
    const auto mapping = gepa::frontier_map(nodes, {"a", "b"}, config);
    check(gepa::pool(mapping) == std::vector<std::size_t>({1, 2}));
    auto unattributed = nodes;
    unattributed.items()[1]["validation_gain_attributed"] = false;
    const auto guarded = gepa::pool(gepa::frontier_map(unattributed, {"a", "b"}, config));
    check(std::find(guarded.begin(), guarded.end(), 1) == guarded.end());
    // node7 未通过发布归因，但训练中的控制真实命中并形成局部收益，且完整验证
    // 相对父节点没有灾难退化：它必须能进入探索父池，发布前沿仍须排除它。
    Json exploration_nodes = Json::Array{node(0, nullptr, 0, .8, .8)};
    for (std::size_t id = 1; id < 7; ++id)
      exploration_nodes.push(node(id, 0, 1, .2, .2));
    auto specialist = node(7, 0, 1, .95, .8);
    specialist["status"] = "unobserved_changes";
    specialist["validation_gain_attributed"] = false;
    specialist["optimization"] = Json{{"passed", false}};
    specialist["screening"] =
        Json{{"split", "train"},
             {"parent", scores(.8, .8)},
             {"candidate", scores(1, .8)},
             {"controls_observed", true},
             {"causal_changes", Json{{"a", Json{{"loop", 1}}}}}};
    exploration_nodes.push(specialist);
    gepa::validate_nodes(exploration_nodes);
    const auto eligibility = gepa::exploration_eligibility(exploration_nodes, 7, config);
    check(eligibility.flag("eligible", false));
    exploration_nodes.items()[7]["exploration_eligible"] = true;
    exploration_nodes.items()[7]["exploration_reason"] = eligibility["reason"];
    const auto publication_pool =
        gepa::pool(gepa::frontier_map(exploration_nodes, {"a", "b"}, config));
    const auto exploration_pool =
        gepa::pool(gepa::exploration_frontier_map(exploration_nodes, {"a", "b"}, config));
    check(std::find(publication_pool.begin(), publication_pool.end(), 7) ==
          publication_pool.end());
    check(std::find(exploration_pool.begin(), exploration_pool.end(), 7) !=
          exploration_pool.end());
    auto exploration_best = config;
    exploration_best["candidate_selection_strategy"] = "current_best";
    check(gepa::select_parent(exploration_nodes,
                              gepa::exploration_frontier_map(exploration_nodes, {"a", "b"}, config),
                              exploration_best, 0) == 7);
    auto specialist_child = node(8, 7, 2, .96, .81);
    exploration_nodes.push(specialist_child);
    gepa::validate_nodes(exploration_nodes);
    auto no_control = exploration_nodes;
    no_control.items()[7]["screening"]["controls_observed"] = false;
    check(!gepa::exploration_eligibility(no_control, 7, config).flag("eligible", true));
    auto disaster = exploration_nodes;
    disaster.items()[7]["validation"] = scores(.5, .5);
    check(!gepa::exploration_eligibility(disaster, 7, config).flag("eligible", true));
    // 节点 1 已有子节点，节点 2 是叶节点，两者共同生成一个子节点。
    const auto plan = gepa::select_merge(nodes, mapping, Json::Array{}, config, 0);
    check(plan.num("first", 99) == 1 && plan.num("second", 99) == 2 &&
          plan.num("ancestor", 99) == 0);
    check(gepa::select_merge(nodes, mapping, Json::Array{plan}, config, 0).null());
    check(
        gepa::select_merge(nodes, Json{{"a", Json::Array{1, 3}}}, Json::Array{}, config, 0).null());
    Json base{{"experience:a", "original"}, {"action_rules", "base"}, {"loop_rules", "base"}};
    auto a = base, b = base;
    a["experience:a"] = "A";
    b["action_rules"] = "B";
    auto merged = gepa::recombine(base, a, b, 1, 2, true);
    check(merged.flag("useful", false) && merged["components"].str("experience:a") == "A" &&
          merged["components"].str("action_rules") == "B");
    check(merged["sources"].num("experience:a", 99) == 1 &&
          merged["sources"].num("action_rules", 99) == 2);
    a["loop_rules"] = "A-loop";
    b["loop_rules"] = "B-loop";
    merged = gepa::recombine(base, a, b, 1, 2, false);
    check(merged.flag("useful", false) && merged["components"].str("loop_rules") == "B-loop" &&
          merged["conflicts"].items().size() == 1);
    check(!gepa::recombine(base, a, a, 1, 2, true).flag("useful", true));
    check(!gepa::recombine(base, base, b, 0, 2, true).flag("useful", true));
    auto invalid_components = b;
    invalid_components.fields().erase("loop_rules");
    rejects([&] { gepa::recombine(base, a, invalid_components, 1, 2, true); });
    auto child = node(4, 1, 2, 1, 1);
    child["parents"] = Json::Array{1, 2};
    child["ancestor"] = 0;
    child["kind"] = "merge";
    nodes.push(child);
    nodes.push(node(5, 4, 3, 1, 1));
    gepa::validate_nodes(nodes);
    check(gepa::ancestors(nodes, 5) == std::set<std::size_t>({0, 1, 2, 4}));
    for (int kind = 0; kind < 5; ++kind) {
      auto broken = nodes;
      if (kind == 0)
        broken.items()[4]["parents"] = Json::Array{1, 3};
      if (kind == 1)
        broken.items()[4]["parents"] = Json::Array{1, 1};
      if (kind == 2)
        broken.items()[4]["parents"] = Json::Array{1, 5};
      if (kind == 3)
        broken.items()[4]["ancestor"] = 3;
      if (kind == 4)
        broken.items()[4]["depth"] = 1;
      rejects([&] { gepa::validate_nodes(broken); });
    }
    check(gepa::pool(gepa::frontier_map(nodes, {"a", "b"}, config)) == std::vector<std::size_t>{4});
    auto strategies = config;
    strategies["candidate_selection_strategy"] = "current_best";
    check(gepa::select_parent(nodes, mapping, strategies, 0) == 4);
    auto constrained = nodes;
    for (auto &n : constrained.items())
      for (auto &score : n["validation"].fields()) {
        score.second["feasible_score"] = 0;
        score.second["goal_score"] = .9;
      }
    for (auto &score : constrained.items()[2]["validation"].fields()) {
      score.second["feasible_score"] = 1;
      score.second["goal_score"] = .8;
    }
    check(gepa::select_parent(constrained, gepa::frontier_map(constrained, {"a", "b"}, config),
                              strategies, 0) == 2);
    strategies["candidate_selection_strategy"] = "epsilon_greedy";
    strategies["epsilon"] = 1;
    std::set<std::size_t> selected;
    for (unsigned i = 0; i < 200; ++i)
      selected.insert(gepa::select_parent(nodes, mapping, strategies, i));
    check(selected.size() == nodes.items().size());
    strategies["candidate_selection_strategy"] = "top_k_pareto";
    strategies["top_k"] = 1;
    check(gepa::select_parent(nodes, gepa::frontier_map(nodes, {"a", "b"}, config), strategies,
                              0) == 4);
    Json frequencies{{"a", Json::Array{1}}, {"b", Json::Array{1}}, {"c", Json::Array{2}}};
    unsigned first = 0;
    for (unsigned i = 0; i < 1000; ++i)
      first += gepa::select_parent(nodes, frequencies, config, i) == 1;
    check(first > 550 && first < 800);
    auto objectives = config;
    objectives["frontier_type"] = "hybrid";
    for (auto &n : nodes.items())
      for (auto &score : n["validation"].fields())
        score.second["token_efficiency"] = .5;
    nodes.items()[2]["validation"]["a"]["token_efficiency"] = 1;
    nodes.items()[2]["validation"]["b"]["token_efficiency"] = 1;
    const auto hybrid = gepa::pool(gepa::frontier_map(nodes, {"a", "b"}, objectives));
    check(std::find(hybrid.begin(), hybrid.end(), 2) != hybrid.end());
    objectives["frontier_type"] = "cartesian";
    check(!gepa::frontier_map(nodes, {"a", "b"}, objectives).fields().empty());
    check(gepa::select_components(base, "all", 0).items().size() == 3);
    check(gepa::select_components(base, "round_robin", 0).items().front().text() == "action_rules");
    check(gepa::select_components(base, "round_robin", 1).items().front().text() == "experience:a");
    check(gepa::select_components(base, "round_robin", 2).items().front().text() == "loop_rules");
    check(gepa::select_components(base, "round_robin", 0).dump() ==
          gepa::select_components(base, "round_robin", 3).dump());
    auto orchestration = base;
    orchestration["schedule_rules"] = "base";
    check(gepa::select_components(orchestration, "round_robin", 0).items().front().text() ==
          "schedule_rules");
    check(gepa::select_components(orchestration, "round_robin", 1).items().front().text() ==
          "action_rules");
    check(gepa::select_components(orchestration, "round_robin", 2).items().front().text() ==
          "experience:a");
    check(gepa::select_components(orchestration, "round_robin", 3).items().front().text() ==
          "loop_rules");
    auto first_branch = orchestration, second_branch = orchestration;
    first_branch["schedule_rules"] = "prepare-before-image";
    second_branch["loop_rules"] = "bounded-recovery";
    const auto combined = gepa::recombine(orchestration, first_branch, second_branch, 1, 2, true);
    check(combined.flag("useful", false) &&
          combined["components"].str("schedule_rules") == "prepare-before-image" &&
          combined["components"].str("loop_rules") == "bounded-recovery");
    for (auto id : {1, 2}) {
      nodes.items()[id]["status"] = "screened";
      nodes.items()[id]["screening"] =
          Json{{"parent", scores(0, 0)}, {"candidate", scores(id == 1 ? 1 : .5, 1)}};
    }
    check(gepa::select_proposals(nodes, Json::Array{1, 2, 3}, config).size() == 2);
    auto selection = config;
    selection["proposal_selection"] = "best";
    check(gepa::select_proposals(nodes, Json::Array{1, 2}, selection) ==
          std::vector<std::size_t>{1});
    nodes.items()[1]["screening"]["parent"] = scores(.9, 1);
    check(gepa::select_proposals(nodes, Json::Array{1, 2}, selection) ==
          std::vector<std::size_t>{2});
    selection["proposal_selection"] = "top_k";
    selection["proposal_top_k"] = 2;
    check(gepa::select_proposals(nodes, Json::Array{1, 2}, selection).size() == 2);
    check(gepa::no_worse(scores(1, 1), scores(1, 0)));
    check(!gepa::no_worse(scores(1, 0), scores(1, 1)));
    check(!gepa::no_worse(Json::Object{}, scores(1, 0)));
    check(gepa::local_gain(scores(1, .5), scores(.8, .8)));
    check(!gepa::local_gain(scores(.5, .5), scores(.8, .8)));
    check(!gepa::local_gain(scores(.8, .8), scores(.8, .8)));
    auto faster = scores(1, 0), slower = scores(1, 1);
    for (auto &item : faster.fields())
      item.second["latency_efficiency"] = 1;
    for (auto &item : slower.fields())
      item.second["latency_efficiency"] = .5;
    check(gepa::local_gain(faster, slower));
    faster["a"]["quality"] = 0;
    check(!gepa::local_gain(faster, slower));
    faster["a"]["quality"] = 1;
    faster["b"].fields().erase("latency_efficiency");
    check(!gepa::local_gain(faster, slower));
    check(!gepa::local_gain(Json::Object{}, slower));
    // 饱和适应度可能偏好 [0,10] 而非 [1,1]，即使原始浪费更多。
    auto parent_loops = scores(.8, .8), child_loops = scores(1, 1);
    for (auto *values : {&parent_loops, &child_loops})
      for (auto &item : values->fields()) {
        item.second["invalid_loops"] = 1;
        item.second["loop_method"] = "model_review";
      }
    child_loops["a"]["invalid_loops"] = 0;
    child_loops["b"]["invalid_loops"] = 10;
    check(!gepa::loops_no_worse(child_loops, parent_loops));
    check(!gepa::no_worse(child_loops, parent_loops));
    child_loops["b"]["invalid_loops"] = 1;
    check(gepa::no_worse(child_loops, parent_loops));
    child_loops["b"]["loop_method"] = "rule_lower_bound";
    check(!gepa::loops_no_worse(child_loops, parent_loops));
    child_loops["b"].fields().erase("invalid_loops");
    check(!gepa::loops_no_worse(child_loops, parent_loops));
    parent_loops["b"].fields().erase("invalid_loops");
    check(!gepa::loops_no_worse(child_loops, parent_loops));
    check(gepa::loops_no_worse(scores(1, 1), scores(1, 1)));
    auto efficiency_nodes =
        Json(Json::Array{node(0, nullptr, 0, 1, 1), node(1, 0, 1, 1, 1), node(2, 0, 1, 1, 1)});
    for (auto &n : efficiency_nodes.items())
      for (auto &item : n["validation"].fields()) {
        item.second["fitness"] = n.num("id", 0) ? .8 : .9;
        item.second["latency_efficiency"] = .5;
      }
    efficiency_nodes.items()[1]["validation"]["a"]["latency_efficiency"] = .8;
    efficiency_nodes.items()[2]["validation"]["b"]["latency_efficiency"] = .8;
    const Json efficiency_frontier{{"a", Json::Array{1}}, {"b", Json::Array{2}}};
    check(
        gepa::select_merge(efficiency_nodes, efficiency_frontier, Json::Array{}, config, 0).null());
    auto exploratory = config;
    exploratory["acceptance"] = "pareto";
    check(!gepa::select_merge(efficiency_nodes, efficiency_frontier, Json::Array{}, exploratory, 0)
               .null());
    // 低于平均总分的专长节点仍可提供互补组件。
    Json specialists = Json::Array{node(0, nullptr, 0, .8, .8), node(1, 0, 1, 1, .5),
                                   node(2, 0, 1, .5, 1), node(3, 1, 2, .9, .5)};
    gepa::validate_nodes(specialists);
    check(!gepa::no_worse(specialists.items()[1]["validation"],
                          specialists.items()[0]["validation"]));
    const auto specialists_plan = gepa::select_merge(
        specialists, gepa::frontier_map(specialists, {"a", "b"}, config), Json::Array{}, config, 0);
    check(specialists_plan.num("first", 99) == 1 && specialists_plan.num("second", 99) == 2);
    specialists.items()[2]["validation"] = scores(.8, .8);
    check(gepa::select_merge(specialists, Json{{"a", Json::Array{1}}, {"b", Json::Array{2}}},
                             Json::Array{}, config, 0)
              .null());
    std::cout << "GEPA DAG, recombination, selection, components and acceptance passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
