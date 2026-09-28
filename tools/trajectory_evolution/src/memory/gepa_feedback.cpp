#include "gepa_feedback.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <map>
#include <set>
#include <openssl/sha.h>

namespace trajectory::evolution::integration {
namespace {

// 指纹用于去重和绑定来源；反馈中保留真实题目 ID 便于回链，不保存任务正文。
std::string fingerprint(const std::string &text) {
  unsigned char result[SHA256_DIGEST_LENGTH];
  SHA256(reinterpret_cast<const unsigned char *>(text.data()), text.size(), result);
  std::string value;
  for (auto byte : result) {
    value += "0123456789abcdef"[byte >> 4];
    value += "0123456789abcdef"[byte & 15];
  }
  return value;
}

// 同一修改、同一来源和评测口径复用身份；节点编号或进程重启不创造新经验。
std::string record_id(const Json &record) {
  return fingerprint(Json{{"before", record["before"]}, {"after", record["after"]},
                          {"sources", record["sources"]},
                          {"background", record["background"]},
                          {"context", record["comparison_context"]}}.dump());
}

double metric(const Json &row, const char *key) {
  const auto &value = row[key];
  if (const auto *n = std::get_if<double>(&value.value)) return *n;
  return double(row.num(key, 0));
}

// 质量/严格通过/无效循环的退步优先记录，不能用 token 下降掩盖业务损失。
std::string outcome(const Json &before, const Json &after) {
  const auto quality = metric(after, "quality") - metric(before, "quality");
  const auto pass = metric(after, "strict_pass_rate") - metric(before, "strict_pass_rate");
  const bool loops_up = before.has("invalid_loops") && after.has("invalid_loops") &&
      metric(after, "invalid_loops") > metric(before, "invalid_loops") + 1e-12;
  if (quality < -1e-12 || pass < -1e-12 || loops_up) return "negative";
  if (quality > 1e-12 || metric(after, "fitness") > metric(before, "fitness") + 1e-6)
    return "positive";
  if (metric(after, "fitness") < metric(before, "fitness") - 1e-6) return "negative";
  return "neutral";
}

bool usable(const dataset::v1::EvaluationTrial &trial) {
  return trial.split() == "train" && trial.variant_id() == "candidate" &&
      trial.error_code().empty() &&
      (trial.verdict() == "passed" || trial.verdict() == "failed") &&
      (trial.execution_status() == "completed" || trial.execution_status() == "failed");
}

} // 结束当前匿名命名空间。

Json make_gepa_memory_feedback(
    const Json &before, const Json &after, const v1::ShadowPolicySnapshot &snapshot,
    const dataset::v1::TaskEvaluationReport &parent,
    const dataset::v1::TaskEvaluationReport &candidate,
    const Json &parent_scores, const Json &candidate_scores, const Json &applied,
    const std::string &dataset_version, const std::string &comparison_context,
    bool screening_passed) {
  if (!parent.complete() || !candidate.complete() || !before.object() || !after.object() ||
      !parent_scores.object() || !candidate_scores.object() ||
      parent_scores.fields().empty() || candidate_scores.fields().empty() ||
      !applied.object() || applied.fields().empty()) return Json();

  Json previous = Json::Object{}, next = Json::Object{};
  std::set<std::string> source_ids;
  for (const auto &component : before.fields()) {
    if (!after.has(component.first) || component.second.dump() == after[component.first].dump())
      continue;
    previous[component.first] = component.second;
    next[component.first] = after[component.first];
    if (component.first.rfind("experience:", 0) == 0)
      source_ids.insert(component.first.substr(11));
    else for (const auto *rules : {&component.second, &after[component.first]})
      if (rules->array())
      for (const auto &rule : rules->items())
        if (!rule.str("memory_id").empty()) source_ids.insert(rule.str("memory_id"));
  }
  if (previous.fields().empty() || source_ids.empty()) return Json();
  Json background = Json::Object{};
  for (const auto &component : before.fields()) {
    if (previous.has(component.first) || component.second.null() ||
        (component.second.array() && component.second.items().empty())) continue;
    background[component.first] = fingerprint(component.second.dump());
    if (component.first.rfind("experience:", 0) == 0)
      source_ids.insert(component.first.substr(11));
    else if (component.second.array())
      for (const auto &rule : component.second.items())
        if (!rule.str("memory_id").empty()) source_ids.insert(rule.str("memory_id"));
  }
  // 只改一个组件仍可能依赖未改动的 B/C，不能把局部效果外推为原经验的全局效果。
  const bool isolated = previous.fields().size() == 1 && source_ids.size() == 1 &&
      background.fields().empty() && previous.fields().begin()->first.rfind("experience:", 0) == 0;
  Json sources = Json::Array{};
  for (const auto &id : source_ids) {
    const auto found = std::find_if(snapshot.experiences().begin(), snapshot.experiences().end(),
        [&](const auto &source) { return source.memory_id() == id; });
    if (found == snapshot.experiences().end()) return Json();
    sources.push(Json{{"memory_id", id}, {"content_revision", found->content_revision()},
                      {"application_shared", found->application_shared()}});
  }
  Json record{{"before", previous}, {"after", next}, {"sources", sources},
              {"background", background},
              {"comparison_context", comparison_context}, {"split", "train"},
              {"screening_passed", screening_passed}, {"tasks", Json::Object{}},
              {"attribution", isolated ? "isolated_memory" : previous.fields().size() == 1
                                    ? "conditional_component" : "component_bundle"}};
  // 重复次数先由搜索评分器聚合；这里以任务版本为独立支持单位，不把一次重试算一票。
  for (const auto &trial : candidate.trials()) {
    if (!usable(trial) || !applied.has(trial.task_id()) ||
        !parent_scores.has(trial.task_id()) || !candidate_scores.has(trial.task_id())) continue;
    const auto found = std::find_if(parent.trials().begin(), parent.trials().end(),
        [&](const auto &p) { return usable(p) && p.task_id() == trial.task_id() &&
            p.task_version() == trial.task_version() && p.repetition() == trial.repetition(); });
    if (found == parent.trials().end()) continue;
    bool observed = false;
    for (const auto &boundary : applied[trial.task_id()].fields())
      observed |= applied[trial.task_id()].num(boundary.first, 0) > 0;
    if (!observed) continue;
    // 数据集增量导出会换版本，但同题正文版本没变就仍是同一份支持证据。
    const auto task_key = fingerprint(trial.task_id() + '\n' + trial.task_version());
    auto &task = record["tasks"][task_key];
    task["task_id"] = trial.task_id();
    task["task_version"] = trial.task_version();
    task["dataset_version"] = dataset_version;
    task[outcome(parent_scores[trial.task_id()], candidate_scores[trial.task_id()])] = true;
  }
  if (record["tasks"].fields().empty()) return Json();
  record["report_ids"] = Json::Object{{parent.run_id(), true}, {candidate.run_id(), true}};
  record["id"] = record_id(record);
  return Json{{"schema_version", 1}, {"user_id", snapshot.identity().user_id()},
              {"records", Json::Object{{record.str("id"), record}}}};
}

bool gepa_feedback_sources_match(const Json &record, const v1::ShadowPolicySnapshot &snapshot) {
  if (!record["sources"].array() || record["sources"].items().empty()) return false;
  for (const auto &ref : record["sources"].items()) {
    bool present = false;
    for (const auto &source : snapshot.experiences())
      present |= source.memory_id() == ref.str("memory_id") &&
          source.content_revision() == ref.str("content_revision") &&
          source.application_shared() == ref.flag("application_shared", false);
    if (!present) return false;
  }
  return true;
}

bool gepa_feedback_context_matches(const Json &record, const Json &components) {
  if (!record["background"].object() || !components.object()) return false;
  Json background = Json::Object{};
  for (const auto &component : components.fields()) {
    if (record["before"].has(component.first) || component.second.null() ||
        (component.second.array() && component.second.items().empty())) continue;
    background[component.first] = fingerprint(component.second.dump());
  }
  return background.dump() == record["background"].dump();
}

Json merge_gepa_memory_feedback(const Json &current, const Json &incoming,
                                const v1::ShadowPolicySnapshot &snapshot,
                                std::size_t minimum_pairs) {
  if (incoming.null() || !incoming.has("records") || incoming["records"].fields().empty())
    return current;
  require(incoming.num("schema_version", 0) == 1 && incoming["records"].object() &&
              incoming.str("user_id") == snapshot.identity().user_id(),
          "gepa_memory_feedback_identity_invalid");
  Json result = current;
  if (!result.has("records"))
    result = Json{{"schema_version", 1}, {"user_id", snapshot.identity().user_id()},
                  {"records", Json::Object{}}};
  require(result.num("schema_version", 0) == 1 && result["records"].object() &&
              result.str("user_id") == snapshot.identity().user_id(),
          "gepa_memory_feedback_store_invalid");
  // 接收端沿用搜索门槛，不能因使用默认值而提升未达标提案。
  minimum_pairs = std::max({std::size_t(1), incoming.num("minimum_pairs", minimum_pairs),
                            result.num("minimum_pairs", 1)});
  require(minimum_pairs <= 10000, "gepa_memory_feedback_support_limit");
  result["minimum_pairs"] = minimum_pairs;
  require(incoming["records"].fields().size() <= 128, "gepa_memory_feedback_limit");
  for (const auto &item : incoming["records"].fields()) {
    const auto &record = item.second;
    require(record.str("id") == item.first && record_id(record) == item.first &&
                record.str("split") == "train" && record["tasks"].object() &&
                record["tasks"].fields().size() <= 4096,
            "gepa_memory_feedback_record_invalid");
    if (!gepa_feedback_sources_match(record, snapshot)) continue;
    auto &stored = result["records"][item.first];
    const auto previous = stored.dump();
    if (!stored.object()) stored = record;
    stored["screening_passed"] = stored.flag("screening_passed", false) ||
                                 record.flag("screening_passed", false);
    for (const auto &task : record["tasks"].fields()) {
      require(fingerprint(task.second.str("task_id") + '\n' + task.second.str("task_version")) ==
                  task.first, "gepa_memory_feedback_task_invalid");
      // 达到容量仍合并已有任务的反证；不能裁掉旧键再重复增加支持票。
      if (!stored["tasks"].has(task.first) && stored["tasks"].fields().size() >= 4096)
        continue;
      auto &observation = stored["tasks"][task.first];
      if (!observation.object()) observation = task.second;
      for (const auto *kind : {"positive", "negative", "neutral"})
        if (task.second.flag(kind, false)) observation[kind] = true;
    }
    for (const auto &report : record["report_ids"].fields())
      stored["report_ids"][report.first] = true;
    // 报告引用可裁剪，任务去重键不能随意丢失，否则旧批次重放会制造新支持。
    while (stored["report_ids"].fields().size() > 256)
      stored["report_ids"].fields().erase(stored["report_ids"].fields().begin());
    std::size_t positive = 0, negative = 0;
    for (const auto &task : stored["tasks"].fields()) {
      positive += task.second.flag("positive", false);
      negative += task.second.flag("negative", false);
    }
    stored["positive_task_count"] = positive;
    stored["negative_task_count"] = negative;
    stored["status"] = negative ? (positive ? "conflicting" : "regressed")
                         : positive >= minimum_pairs &&
                                   stored.flag("screening_passed", false)
                             ? "supported" : "insufficient_support";
    if (stored.dump() != previous)
      stored["updated_ms"] = std::size_t(std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch()).count());
  }
  // 复用既有库的容量约束；优先保留可用修订，淘汰最旧的未支持探索记录。
  while (result["records"].fields().size() > 128) {
    auto &records = result["records"].fields();
    records.erase(std::min_element(records.begin(), records.end(), [](const auto &a, const auto &b) {
      const bool supported_a = a.second.str("status") == "supported";
      const bool supported_b = b.second.str("status") == "supported";
      if (supported_a != supported_b) return !supported_a;
      return a.second.num("updated_ms", 0) < b.second.num("updated_ms", 0);
    }));
  }
  return result;
}

agentflow::trajectory::evaluation::MemoryEvaluationFeedback gepa_source_feedback(
    const agentflow::trajectory::evaluation::EvolutionMemoryStore &memory,
    const v1::ShadowExperience &source,
    const agentflow::trajectory::evaluation::MemoryEvaluationFeedback &ordinary) {
  namespace ep = agentflow::trajectory::evaluation;
  const auto feedback = json(memory.gepa_feedback());
  if (!feedback["records"].object()) return ordinary;
  std::map<std::string, std::pair<bool, bool>> tasks;
  std::set<std::string> reports;
  for (const auto &item : feedback["records"].fields()) {
    const auto &record = item.second;
    const auto key = "experience:" + source.memory_id();
    if (record.str("attribution") != "isolated_memory" ||
        !record["before"].has(key) || !record["after"].has(key) ||
        !record["sources"].array() || record["sources"].items().size() != 1) continue;
    const auto &ref = record["sources"].items().front();
    if (ref.str("memory_id") != source.memory_id() ||
        ref.str("content_revision") != source.content_revision() ||
        ref.flag("application_shared", false) != source.application_shared()) continue;
    if (!record["background"].object() || !record["background"].fields().empty()) continue;
    const auto &before = record["before"][key], &after = record["after"][key];
    const bool removed = !before.null() && after.null();
    const bool added = before.null() && !after.null();
    if (!removed && !added) continue;
    const auto &active = removed ? before : after;
    // 只有原正文、原适用范围的消融才评价原经验。A 改成 A' 后变差不能降级 A。
    if (!active.string() || active.text() != source.content()) continue;
    for (const auto &task : record["tasks"].fields()) {
      auto &result = tasks[task.first];
      result.first |= task.second.flag(removed ? "negative" : "positive", false);
      result.second |= task.second.flag(removed ? "positive" : "negative", false);
    }
    reports.insert("gepa:" + item.first);
  }
  if (reports.empty()) return ordinary;
  ep::MemoryEvaluationFeedback result;
  if (ordinary.content_revision() == source.content_revision()) result = ordinary;
  result.set_content_revision(source.content_revision());
  std::size_t positive = 0, negative = 0;
  for (const auto &task : tasks) {
    positive += task.second.first;
    negative += task.second.second;
  }
  result.set_positive_trial_count(std::max<std::uint64_t>(positive, result.positive_trial_count()));
  result.set_negative_trial_count(std::max<std::uint64_t>(negative, result.negative_trial_count()));
  for (const auto &id : result.report_ids()) reports.insert(id);
  result.clear_report_ids();
  for (const auto &id : reports) result.add_report_ids(id);
  const auto minimum = std::max<std::size_t>(1, feedback.num("minimum_pairs", 2));
  positive = result.positive_trial_count();
  negative = result.negative_trial_count();
  result.set_status(positive && negative ? "conflicting_trial_results"
                    : negative >= minimum ? "regressed"
                    : positive >= minimum ? "validated" : "review_needed");
  return result;
}

void update_gepa_memory_states(agentflow::trajectory::evaluation::EvolutionMemoryStore &memory,
                               const v1::ShadowPolicySnapshot &snapshot) {
  namespace ep = agentflow::trajectory::evaluation;
  for (auto &entry : *memory.mutable_entries()) {
    for (const auto &source : snapshot.experiences()) {
      if (source.application_shared() || source.memory_id() != entry.memory_id()) continue;
      const auto feedback = gepa_source_feedback(memory, source, entry.evaluation_feedback());
      if (feedback.SerializeAsString() == entry.evaluation_feedback().SerializeAsString()) break;
      *entry.mutable_evaluation_feedback() = feedback;
      if (feedback.status() == "validated")
        entry.set_status(ep::EVOLUTION_MEMORY_STATUS_VALIDATED);
      else if (feedback.status() == "regressed")
        entry.set_status(ep::EVOLUTION_MEMORY_STATUS_ARCHIVED);
      else if (entry.status() == ep::EVOLUTION_MEMORY_STATUS_VALIDATED &&
               feedback.status() == "conflicting_trial_results")
        entry.set_status(ep::EVOLUTION_MEMORY_STATUS_CANDIDATE);
      break;
    }
  }
}

} // 命名空间结束：trajectory::evolution::integration
