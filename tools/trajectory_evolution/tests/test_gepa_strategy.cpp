#include "../src/gepa/gepa_strategy.h"
#include <iostream>
#include <stdexcept>

using trajectory::evolution::integration::Json;
namespace gepa = trajectory::evolution::integration::gepa;
void check(bool value) { if(!value) throw std::runtime_error("GEPA strategy assertion failed"); }
template<class F> void rejects(F fn) {
  try { fn(); } catch(const trajectory::evolution::integration::Error&) { return; }
  throw std::runtime_error("expected GEPA strategy rejection");
}
Json scores(double a, double b) {
  return Json{{"a", Json{{"quality", a}, {"fitness", a}}},
      {"b", Json{{"quality", b}, {"fitness", b}}}};
}
Json node(std::size_t id, Json parent, std::size_t depth, double a, double b) {
  return Json{{"id", id}, {"parent", parent}, {"depth", depth}, {"status", "accepted"}, {"validation", scores(a, b)}};
}
int main() {
  try {
    Json nodes = Json::Array{node(0, nullptr, 0, 0, 0), node(1, 0, 1, 1, 0),
        node(2, 0, 1, 0, 1), node(3, 1, 2, .8, 0)};
    gepa::validate_nodes(nodes);
    check(gepa::parents(nodes.items()[0]).empty());
    check(gepa::parents(nodes.items()[1]) == std::vector<std::size_t>{0});
    const Json config{{"seed", 42}};
    const auto mapping = gepa::frontier_map(nodes, {"a", "b"}, config);
    check(gepa::pool(mapping) == std::vector<std::size_t>({1, 2}));
    // Node 1 already has a child; node 2 is a leaf. Both contribute to one child.
    const auto plan = gepa::select_merge(nodes, mapping, Json::Array{}, config, 0);
    check(plan.num("first", 99) == 1 && plan.num("second", 99) == 2 && plan.num("ancestor", 99) == 0);
    check(gepa::select_merge(nodes, mapping, Json::Array{plan}, config, 0).null());
    check(gepa::select_merge(nodes, Json{{"a", Json::Array{1, 3}}}, Json::Array{}, config, 0).null());
    Json base{{"experience:a", "original"}, {"action_rules", "base"}, {"loop_rules", "base"}};
    auto a = base, b = base; a["experience:a"] = "A"; b["action_rules"] = "B";
    auto merged = gepa::recombine(base, a, b, 1, 2, true);
    check(merged.flag("useful", false) && merged["components"].str("experience:a") == "A"
        && merged["components"].str("action_rules") == "B");
    check(merged["sources"].num("experience:a", 99) == 1 && merged["sources"].num("action_rules", 99) == 2);
    a["loop_rules"] = "A-loop"; b["loop_rules"] = "B-loop";
    merged = gepa::recombine(base, a, b, 1, 2, false);
    check(merged.flag("useful", false) && merged["components"].str("loop_rules") == "B-loop"
        && merged["conflicts"].items().size() == 1);
    check(!gepa::recombine(base, a, a, 1, 2, true).flag("useful", true));
    check(!gepa::recombine(base, base, b, 0, 2, true).flag("useful", true));
    auto invalid_components = b; invalid_components.fields().erase("loop_rules");
    rejects([&] { gepa::recombine(base, a, invalid_components, 1, 2, true); });
    auto child = node(4, 1, 2, 1, 1);
    child["parents"] = Json::Array{1, 2}; child["ancestor"] = 0; child["kind"] = "merge";
    nodes.push(child); nodes.push(node(5, 4, 3, 1, 1));
    gepa::validate_nodes(nodes);
    check(gepa::ancestors(nodes, 5) == std::set<std::size_t>({0, 1, 2, 4}));
    for(int kind = 0; kind < 5; ++kind) {
      auto broken = nodes;
      if(kind == 0) broken.items()[4]["parents"] = Json::Array{1, 3};
      if(kind == 1) broken.items()[4]["parents"] = Json::Array{1, 1};
      if(kind == 2) broken.items()[4]["parents"] = Json::Array{1, 5};
      if(kind == 3) broken.items()[4]["ancestor"] = 3;
      if(kind == 4) broken.items()[4]["depth"] = 1;
      rejects([&] { gepa::validate_nodes(broken); });
    }
    check(gepa::pool(gepa::frontier_map(nodes, {"a", "b"}, config)) == std::vector<std::size_t>{4});
    auto strategies = config; strategies["candidate_selection_strategy"] = "current_best";
    check(gepa::select_parent(nodes, mapping, strategies, 0) == 4);
    auto constrained = nodes;
    for(auto& n : constrained.items()) for(auto& score : n["validation"].fields()) {
      score.second["feasible_score"] = 0; score.second["goal_score"] = .9;
    }
    for(auto& score : constrained.items()[2]["validation"].fields()) {
      score.second["feasible_score"] = 1; score.second["goal_score"] = .8;
    }
    check(gepa::select_parent(constrained, gepa::frontier_map(constrained, {"a", "b"}, config), strategies, 0) == 2);
    strategies["candidate_selection_strategy"] = "epsilon_greedy"; strategies["epsilon"] = 1;
    std::set<std::size_t> selected;
    for(unsigned i = 0; i < 200; ++i) selected.insert(gepa::select_parent(nodes, mapping, strategies, i));
    check(selected.size() == nodes.items().size());
    strategies["candidate_selection_strategy"] = "top_k_pareto"; strategies["top_k"] = 1;
    check(gepa::select_parent(nodes, gepa::frontier_map(nodes, {"a", "b"}, config), strategies, 0) == 4);
    Json frequencies{{"a", Json::Array{1}}, {"b", Json::Array{1}}, {"c", Json::Array{2}}};
    unsigned first = 0;
    for(unsigned i = 0; i < 1000; ++i) first += gepa::select_parent(nodes, frequencies, config, i) == 1;
    check(first > 550 && first < 800);
    auto objectives = config; objectives["frontier_type"] = "hybrid";
    for(auto& n : nodes.items()) for(auto& score : n["validation"].fields()) score.second["token_efficiency"] = .5;
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
    check(gepa::select_components(base, "round_robin", 0).dump() == gepa::select_components(base, "round_robin", 3).dump());
    auto orchestration = base; orchestration["schedule_rules"] = "base";
    check(gepa::select_components(orchestration, "round_robin", 0).items().front().text() == "schedule_rules");
    check(gepa::select_components(orchestration, "round_robin", 1).items().front().text() == "action_rules");
    check(gepa::select_components(orchestration, "round_robin", 2).items().front().text() == "experience:a");
    check(gepa::select_components(orchestration, "round_robin", 3).items().front().text() == "loop_rules");
    auto first_branch = orchestration, second_branch = orchestration;
    first_branch["schedule_rules"] = "prepare-before-image"; second_branch["loop_rules"] = "bounded-recovery";
    const auto combined = gepa::recombine(orchestration, first_branch, second_branch, 1, 2, true);
    check(combined.flag("useful", false) && combined["components"].str("schedule_rules") == "prepare-before-image"
        && combined["components"].str("loop_rules") == "bounded-recovery");
    for(auto id : {1, 2}) {
      nodes.items()[id]["status"] = "screened";
      nodes.items()[id]["screening"] = Json{{"parent", scores(0, 0)}, {"candidate", scores(id == 1 ? 1 : .5, 1)}};
    }
    check(gepa::select_proposals(nodes, Json::Array{1, 2, 3}, config).size() == 2);
    auto selection = config; selection["proposal_selection"] = "best";
    check(gepa::select_proposals(nodes, Json::Array{1, 2}, selection) == std::vector<std::size_t>{1});
    nodes.items()[1]["screening"]["parent"] = scores(.9, 1);
    check(gepa::select_proposals(nodes, Json::Array{1, 2}, selection) == std::vector<std::size_t>{2});
    selection["proposal_selection"] = "top_k"; selection["proposal_top_k"] = 2;
    check(gepa::select_proposals(nodes, Json::Array{1, 2}, selection).size() == 2);
    check(gepa::no_worse(scores(1, 1), scores(1, 0)));
    check(!gepa::no_worse(scores(1, 0), scores(1, 1)));
    check(!gepa::no_worse(Json::Object{}, scores(1, 0)));
    check(gepa::local_gain(scores(1, .5), scores(.8, .8)));
    check(!gepa::local_gain(scores(.5, .5), scores(.8, .8)));
    check(!gepa::local_gain(scores(.8, .8), scores(.8, .8)));
    auto faster = scores(1, 0), slower = scores(1, 1);
    for(auto& item : faster.fields()) item.second["latency_efficiency"] = 1;
    for(auto& item : slower.fields()) item.second["latency_efficiency"] = .5;
    check(gepa::local_gain(faster, slower));
    faster["a"]["quality"] = 0;
    check(!gepa::local_gain(faster, slower));
    faster["a"]["quality"] = 1; faster["b"].fields().erase("latency_efficiency");
    check(!gepa::local_gain(faster, slower));
    check(!gepa::local_gain(Json::Object{}, slower));
    // Saturating fitness can prefer [0, 10] to [1, 1], despite more raw waste.
    auto parent_loops = scores(.8, .8), child_loops = scores(1, 1);
    for(auto* values : {&parent_loops, &child_loops}) for(auto& item : values->fields()) {
      item.second["invalid_loops"] = 1;
      item.second["loop_method"] = "model_review";
    }
    child_loops["a"]["invalid_loops"] = 0; child_loops["b"]["invalid_loops"] = 10;
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
    auto efficiency_nodes = Json(Json::Array{node(0, nullptr, 0, 1, 1), node(1, 0, 1, 1, 1), node(2, 0, 1, 1, 1)});
    for(auto& n : efficiency_nodes.items()) for(auto& item : n["validation"].fields()) {
      item.second["fitness"] = n.num("id", 0) ? .8 : .9;
      item.second["latency_efficiency"] = .5;
    }
    efficiency_nodes.items()[1]["validation"]["a"]["latency_efficiency"] = .8;
    efficiency_nodes.items()[2]["validation"]["b"]["latency_efficiency"] = .8;
    const Json efficiency_frontier{{"a", Json::Array{1}}, {"b", Json::Array{2}}};
    check(gepa::select_merge(efficiency_nodes, efficiency_frontier, Json::Array{}, config, 0).null());
    auto exploratory = config; exploratory["acceptance"] = "pareto";
    check(!gepa::select_merge(efficiency_nodes, efficiency_frontier, Json::Array{}, exploratory, 0).null());
    // Below-average specialists can supply complementary components to one child.
    Json specialists = Json::Array{node(0, nullptr, 0, .8, .8), node(1, 0, 1, 1, .5),
        node(2, 0, 1, .5, 1), node(3, 1, 2, .9, .5)};
    gepa::validate_nodes(specialists);
    check(!gepa::no_worse(specialists.items()[1]["validation"], specialists.items()[0]["validation"]));
    const auto specialists_plan = gepa::select_merge(specialists,
        gepa::frontier_map(specialists, {"a", "b"}, config), Json::Array{}, config, 0);
    check(specialists_plan.num("first", 99) == 1 && specialists_plan.num("second", 99) == 2);
    specialists.items()[2]["validation"] = scores(.8, .8);
    check(gepa::select_merge(specialists, Json{{"a", Json::Array{1}}, {"b", Json::Array{2}}},
        Json::Array{}, config, 0).null());
    std::cout << "GEPA DAG, recombination, selection, components and acceptance passed\n";
    return 0;
  } catch(const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
