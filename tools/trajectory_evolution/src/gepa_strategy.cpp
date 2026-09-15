#include "gepa_strategy.h"

#include <limits>
#include <random>

namespace trajectory::evolution::integration::gepa {
namespace {
double number(const Json& value, double fallback = 0) {
  return std::holds_alternative<double>(value.value) ? std::get<double>(value.value) : fallback;
}
bool better(const Json& a, const Json& b) {
  const auto difference = mean(a, "quality") - mean(b, "quality");
  return difference > 1e-12 || (std::abs(difference) <= 1e-12 && mean(a, "fitness") > mean(b, "fitness") + 1e-12);
}
}

double mean(const Json& scores, const std::string& field) {
  if(!scores.object() || scores.fields().empty()) return -1e9;
  double total = 0;
  for(const auto& item : scores.fields()) {
    if(!item.second.has(field)) return -1e9;
    total += number(item.second[field], -1e9);
  }
  return total / scores.fields().size();
}
bool loops_no_worse(const Json& candidate, const Json& parent) {
  if(!candidate.object() || !parent.object() || candidate.fields().empty()
      || candidate.fields().size() != parent.fields().size()) return false;
  for(const auto& item : parent.fields()) if(!candidate.has(item.first)) return false;
  std::size_t observed = 0;
  for(const auto& item : parent.fields()) {
    const auto& a = candidate[item.first]; const auto& b = item.second;
    if(!a.has("invalid_loops") && !b.has("invalid_loops")) continue;
    if(!a.has("invalid_loops") || !b.has("invalid_loops") || a["invalid_loops"].null()
        || b["invalid_loops"].null() || a.str("loop_method") != b.str("loop_method")) return false;
    ++observed;
  }
  // A disabled loop objective has no counts. Partial observations are not zero.
  return !observed || (observed == parent.fields().size()
      && mean(candidate, "invalid_loops") <= mean(parent, "invalid_loops") + 1e-12);
}
bool no_worse(const Json& candidate, const Json& parent) {
  if(!loops_no_worse(candidate, parent)) return false;
  return mean(candidate, "quality") >= mean(parent, "quality") - 1e-12
      && mean(candidate, "fitness") >= mean(parent, "fitness") - 1e-12;
}
bool local_gain(const Json& candidate, const Json& parent, double minimum) {
  if(!candidate.object() || !parent.object() || parent.fields().empty()
      || candidate.fields().size() != parent.fields().size()) return false;
  bool gained = false;
  for(const auto& task : parent.fields()) {
    if(!candidate.has(task.first)) return false;
    const auto& a = candidate[task.first]; const auto& b = task.second;
    for(const auto* key : {"quality", "fitness"})
      if(!a.has(key) || !b.has(key) || a[key].null() || b[key].null()) return false;
    if(a.has("invalid_loops") || b.has("invalid_loops"))
      if(!a.has("invalid_loops") || !b.has("invalid_loops") || a["invalid_loops"].null()
          || b["invalid_loops"].null() || a.str("loop_method") != b.str("loop_method")) return false;
    const auto quality = number(a["quality"]) - number(b["quality"]);
    gained |= quality > minimum;
    for(const auto* key : {"fitness", "loop_efficiency", "token_efficiency", "latency_efficiency"}) {
      if(a.has(key) != b.has(key) || (a.has(key) && (a[key].null() || b[key].null()))) return false;
      // Preserve specialists, not speed gains obtained by failing the same task.
      if(a.has(key) && quality >= -1e-12) gained |= number(a[key]) > number(b[key]) + minimum;
    }
  }
  return gained;
}
std::vector<std::size_t> parents(const Json& node) {
  std::vector<std::size_t> result;
  if(node.has("parents")) {
    for(const auto& id : node["parents"].items()) result.push_back(id.integer());
  } else if(!node["parent"].null()) result.push_back(node["parent"].integer());
  return result;
}
std::set<std::size_t> ancestors(const Json& nodes, std::size_t index) {
  std::set<std::size_t> result;
  auto pending = parents(nodes.items().at(index));
  while(!pending.empty()) {
    const auto id = pending.back(); pending.pop_back();
    require(id < index, "gepa_tree_invalid");
    if(result.insert(id).second) for(auto p : parents(nodes.items().at(id))) {
      require(p < id, "gepa_tree_invalid"); pending.push_back(p);
    }
  }
  return result;
}
void validate_nodes(Json& nodes) {
  require(nodes.array() && !nodes.items().empty(), "gepa_tree_invalid");
  for(std::size_t i = 0; i < nodes.items().size(); ++i) {
    auto& node = nodes.items()[i]; const auto ids = parents(node);
    require(node.num("id", SIZE_MAX) == i && ids.size() <= 2 && (i ? !ids.empty() : ids.empty()), "gepa_tree_invalid");
    std::set<std::size_t> unique; std::size_t depth = 0;
    for(auto id : ids) {
      require(id < i && unique.insert(id).second && nodes.items()[id].str("status") == "accepted", "gepa_tree_invalid");
      depth = std::max(depth, nodes.items()[id].num("depth", 0) + 1);
    }
    require(node.num("depth", SIZE_MAX) == depth
        && (i ? node.num("parent", SIZE_MAX) == ids.front() : node["parent"].null()), "gepa_tree_invalid");
    if(!node.has("parents")) { node["parents"] = Json::Array{}; for(auto id : ids) node["parents"].push(id); }
    if(!node.has("kind")) node["kind"] = i ? "reflection" : "seed";
    require((ids.size() == 2) == (node.str("kind") == "merge"), "gepa_tree_invalid");
    if(ids.size() == 2) {
      const auto a = ancestors(nodes, ids[0]), b = ancestors(nodes, ids[1]);
      const auto base = node.num("ancestor", SIZE_MAX);
      require(!a.count(ids[1]) && !b.count(ids[0]) && a.count(base) && b.count(base), "gepa_tree_invalid");
    }
  }
}
Json frontier_map(const Json& nodes, const std::vector<std::string>& tasks, const Json& config) {
  Json mapping = Json::Object{};
  const auto mode = config.str("frontier_type", "instance");
  auto add = [&](const std::string& key, const std::string& task, const std::string& field) {
    double best = -std::numeric_limits<double>::infinity(); Json ids = Json::Array{};
    for(std::size_t i = 0; i < nodes.items().size(); ++i) {
      const auto& node = nodes.items()[i]; if(node.str("status") != "accepted") continue;
      const auto value = task.empty() ? mean(node["validation"], field)
          : number(node["validation"][task][field], -1e9);
      if(value <= -1e9) continue;
      if(value > best + 1e-12) { ids = Json::Array{}; best = value; }
      if(value >= best - 1e-12) ids.push(i);
    }
    if(!ids.items().empty()) mapping[key] = ids;
  };
  if(mode == "instance" || mode == "hybrid") for(const auto& task : tasks) add("task:" + task, task, "fitness");
  if(mode == "objective" || mode == "hybrid") {
    add("objective:quality", {}, "quality");
    for(const auto* field : {"loop_efficiency", "token_efficiency", "latency_efficiency"}) add(std::string("objective:") + field, {}, field);
  }
  if(mode == "cartesian") for(const auto& task : tasks)
    for(const auto* field : {"quality", "loop_efficiency", "token_efficiency", "latency_efficiency"})
      add("pair:" + Json(Json::Array{task, field}).dump(), task, field);
  // Remove candidates whose frontier coverage is contained in an at-least-as-good
  // candidate. Equal coverage/scores keep one deterministic representative.
  if(config.flag("prune_dominated", true)) {
    std::map<std::size_t, std::set<std::string>> coverage;
    for(const auto& item : mapping.fields()) for(const auto& id : item.second.items()) coverage[id.integer()].insert(item.first);
    std::set<std::size_t> removed;
    for(const auto& a : coverage) for(const auto& b : coverage) if(a.first != b.first) {
      const auto& av = nodes.items()[a.first]["validation"]; const auto& bv = nodes.items()[b.first]["validation"];
      if(std::includes(b.second.begin(), b.second.end(), a.second.begin(), a.second.end())
          && no_worse(bv, av) && (b.second != a.second || better(bv, av) || b.first < a.first)) {
        removed.insert(a.first); break;
      }
    }
    for(auto& item : mapping.fields()) {
      Json ids = Json::Array{}; for(const auto& id : item.second.items()) if(!removed.count(id.integer())) ids.push(id);
      require(!ids.items().empty(), "gepa_frontier_empty"); item.second = ids;
    }
  }
  return mapping;
}
std::vector<std::size_t> pool(const Json& mapping) {
  std::set<std::size_t> ids;
  for(const auto& item : mapping.fields()) for(const auto& id : item.second.items()) ids.insert(id.integer());
  return {ids.begin(), ids.end()};
}
std::size_t select_parent(const Json& nodes, const Json& mapping, const Json& config, std::size_t round) {
  auto ids = pool(mapping); require(!ids.empty(), "gepa_frontier_empty");
  const auto strategy = config.str("candidate_selection_strategy", "pareto");
  std::vector<std::size_t> accepted;
  for(std::size_t i = 0; i < nodes.items().size(); ++i) if(nodes.items()[i].str("status") == "accepted") accepted.push_back(i);
  auto ranking = [&](auto a, auto b) { return better(nodes.items()[a]["validation"], nodes.items()[b]["validation"]); };
  std::stable_sort(accepted.begin(), accepted.end(), ranking);
  if(strategy == "current_best") return accepted.front();
  std::mt19937 random(static_cast<unsigned>(config.num("seed", 42) + round));
  if(strategy == "epsilon_greedy")
    return std::bernoulli_distribution(number(config["epsilon"], .1))(random)
        ? accepted[std::uniform_int_distribution<std::size_t>(0, accepted.size() - 1)(random)] : accepted.front();
  if(strategy == "top_k_pareto") {
    std::stable_sort(ids.begin(), ids.end(), ranking);
    ids.resize(std::min(ids.size(), config.num("top_k", 5)));
  }
  std::vector<double> weights;
  for(auto id : ids) {
    double weight = 0;
    for(const auto& item : mapping.fields()) for(const auto& candidate : item.second.items()) if(candidate.integer() == id) ++weight;
    weights.push_back(strategy == "fair" ? 1.0 / (1.0 + nodes.items()[id].num("selections", 0)) : weight);
  }
  return ids[std::discrete_distribution<std::size_t>(weights.begin(), weights.end())(random)];
}
Json select_merge(const Json& nodes, const Json& mapping, const Json& attempts, const Json& config, std::size_t round) {
  const auto ids = pool(mapping); std::vector<std::array<std::size_t, 2>> pairs;
  for(std::size_t a = 0; a < ids.size(); ++a) for(std::size_t b = a + 1; b < ids.size(); ++b) pairs.push_back({ids[a], ids[b]});
  std::mt19937 random(static_cast<unsigned>(config.num("seed", 42) + round));
  std::shuffle(pairs.begin(), pairs.end(), random);
  for(const auto& pair : pairs) {
    const auto a = ancestors(nodes, pair[0]), b = ancestors(nodes, pair[1]);
    if(a.count(pair[1]) || b.count(pair[0])) continue;
    std::vector<std::size_t> common;
    std::set_intersection(a.begin(), a.end(), b.begin(), b.end(), std::back_inserter(common));
    std::stable_sort(common.begin(), common.end(), [&](auto x, auto y) { return nodes.items()[x].num("depth", 0) > nodes.items()[y].num("depth", 0); });
    for(auto base : common) {
      // Frontier specialists may lose on average while improving different tasks.
      // Require a real local gain; the combined child still has to beat both parents.
      auto gain = [&](std::size_t id) {
        const auto& candidate = nodes.items()[id]["validation"];
        const auto& ancestor = nodes.items()[base]["validation"];
        if(config.str("acceptance") == "pareto")
          return local_gain(candidate, ancestor, number(config["min_improvement"], 0.000001));
        if(candidate.fields().size() != ancestor.fields().size()) return false;
        bool improved = false;
        for(const auto& task : ancestor.fields()) {
          if(!candidate.has(task.first)) return false;
          improved |= better(Json{{task.first, candidate[task.first]}}, Json{{task.first, task.second}});
        }
        return improved;
      };
      if(!gain(pair[0]) || !gain(pair[1])) continue;
      bool seen = false;
      for(const auto& attempt : attempts.items()) seen |= attempt.num("first", SIZE_MAX) == pair[0]
          && attempt.num("second", SIZE_MAX) == pair[1] && attempt.num("ancestor", SIZE_MAX) == base;
      if(!seen) return Json{{"first", pair[0]}, {"second", pair[1]}, {"ancestor", base}};
    }
  }
  return nullptr;
}
Json recombine(const Json& ancestor, const Json& first, const Json& second,
    std::size_t first_id, std::size_t second_id, bool prefer_first) {
  require(ancestor.object() && first.object() && second.object()
      && first.fields().size() == ancestor.fields().size() && second.fields().size() == ancestor.fields().size(), "gepa_components_changed");
  Json result{{"components", Json::Object{}}, {"sources", Json::Object{}}, {"conflicts", Json::Array{}}};
  std::set<std::size_t> contributors; bool complementary = false;
  for(const auto& item : ancestor.fields()) {
    const auto& key = item.first;
    require(first.has(key) && second.has(key), "gepa_components_changed");
    const auto old = item.second.dump(), a = first[key].dump(), b = second[key].dump();
    bool use_first = prefer_first;
    if(a == b) use_first = true;
    else if(a == old) { use_first = false; complementary = true; }
    else if(b == old) { use_first = true; complementary = true; }
    else result["conflicts"].push(key);
    const auto source = use_first ? first_id : second_id;
    result["components"][key] = use_first ? first[key] : second[key];
    result["sources"][key] = source;
    if(a != b) contributors.insert(source);
  }
  // A nominal two-parent edge must actually combine changes from both branches.
  result["useful"] = complementary && contributors.size() == 2;
  return result;
}
Json select_components(const Json& components, const std::string& strategy, std::size_t cursor) {
  Json selected = Json::Array{};
  for(const auto& item : components.fields()) selected.push(item.first);
  require(!selected.items().empty(), "gepa_components_empty");
  if(strategy == "round_robin") return Json::Array{selected.items()[cursor % selected.items().size()]};
  return selected;
}
std::vector<std::size_t> select_proposals(const Json& nodes, const Json& ids, const Json& config) {
  std::vector<std::size_t> result;
  for(const auto& id : ids.items()) if(nodes.items().at(id.integer()).str("status") == "screened") result.push_back(id.integer());
  const auto strategy = config.str("proposal_selection", "all_improvements");
  if(strategy != "all_improvements") {
    auto improvement = [&](auto id) {
      const auto& screening = nodes.items()[id]["screening"];
      return Json{{"gain", Json{{"quality", mean(screening["candidate"], "quality") - mean(screening["parent"], "quality")},
          {"fitness", mean(screening["candidate"], "fitness") - mean(screening["parent"], "fitness")}}}};
    };
    std::stable_sort(result.begin(), result.end(), [&](auto a, auto b) { return better(improvement(a), improvement(b)); });
    result.resize(std::min(result.size(), strategy == "best" ? std::size_t(1) : config.num("proposal_top_k", 2)));
  }
  return result;
}
}  // namespace trajectory::evolution::integration::gepa
