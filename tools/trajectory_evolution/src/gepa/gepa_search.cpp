// GEPA 搜索负责候选生成、评测调度与节点选择；宿主执行交给统一评测器。
#include "gepa_search.h"
#include "gepa_strategy.h"
#include "control_generation.h"
#include "failure_ledger.h"
#include "holdout_baseline_cache.h"
#include "schedule_evidence.h"
#include "shared_memory.h"
#include "evaluation_contracts.h"
#include "local_support.h"
#include "model_json.h"
#include "agentflow/trajectory/runtime_config.h"
#include "trajectory_evolution/agent_evaluation.h"

#include <chrono>
#include <cmath>
#include <fstream>
#include <limits>
#include <random>
#include <set>
#include <openssl/sha.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace trajectory::evolution::integration {
namespace {
namespace fs = std::filesystem;
namespace legacy = agentflow::trajectory;
using Report = dataset::v1::TaskEvaluationReport;

std::string hash(const std::string& bytes) {
  unsigned char out[SHA256_DIGEST_LENGTH];
  require(SHA256(reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size(), out), "gepa_hash_failed");
  std::string result;
  for(auto c : out) { result += "0123456789abcdef"[c >> 4]; result += "0123456789abcdef"[c & 15]; }
  return result;
}
std::size_t now() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}
void check_path(const fs::path& path) {
  fs::path prefix;
  for(const auto& part : fs::absolute(path)) { prefix /= part; require(!fs::is_symlink(prefix), "gepa_symlink_rejected"); }
}
std::string read(const fs::path& path, std::size_t limit = 4194304) {
  check_path(path);
  std::ifstream stream(path, std::ios::binary | std::ios::ate);
  require(stream && stream.tellg() >= 0 && static_cast<std::size_t>(stream.tellg()) <= limit, "gepa_file_unavailable");
  std::string bytes(static_cast<std::size_t>(stream.tellg()), '\0'); stream.seekg(0);
  require(bytes.empty() || bool(stream.read(bytes.data(), bytes.size())), "gepa_read_failed"); return bytes;
}
void write(const fs::path& path, const std::string& bytes) {
  check_path(path); fs::create_directories(path.parent_path());
  require(::chmod(path.parent_path().c_str(), 0700) == 0, "gepa_permission_failed");
  std::string pending = path.string() + ".pending.XXXXXX";
  int fd = ::mkstemp(pending.data()); require(fd >= 0, "gepa_write_failed");
  try {
    for(std::size_t offset = 0; offset < bytes.size();) {
      const auto n = ::write(fd, bytes.data() + offset, bytes.size() - offset); require(n > 0, "gepa_write_failed"); offset += n;
    }
    require(::fsync(fd) == 0, "gepa_write_failed"); ::close(fd); fd = -1;
    require(::rename(pending.c_str(), path.c_str()) == 0, "gepa_write_failed");
    int parent = ::open(path.parent_path().c_str(), O_RDONLY | O_DIRECTORY);
    if(parent >= 0) { ::fsync(parent); ::close(parent); }
  } catch(...) { if(fd >= 0) ::close(fd); ::unlink(pending.c_str()); throw; }
}
double number(const Json& value, double fallback = 0) {
  return std::holds_alternative<double>(value.value) ? std::get<double>(value.value) : fallback;
}
std::size_t bound(const Json& c, const char* key, std::size_t fallback, std::size_t low, std::size_t high) {
  const auto n = c.num(key, fallback); require(n >= low && n <= high, "gepa_limit_invalid"); return n;
}
bool scored(const Report& report) {
  return report.complete() && report.trials_size() && std::all_of(report.trials().begin(), report.trials().end(), [](const auto& t) {
    return t.verdict() == "passed" || t.verdict() == "failed";
  });
}

// 在保留具体文本的同时限制单字段和整包大小，避免反思上下文失控。
std::string clip(std::string value, std::size_t limit) {
  if(value.size() <= limit) return value;
  while(limit && (static_cast<unsigned char>(value[limit]) & 0xc0) == 0x80) --limit;
  value.resize(limit); return value + " [truncated]";
}
Json excerpt(const Json& value, unsigned depth = 0) {
  if(value.string()) return clip(value.text(), 2048);
  if(depth >= 12) return "[depth limit]";
  if(value.array()) {
    Json result = Json::Array{};
    for(const auto& item : value.items()) { if(result.items().size() == 24) { result.push("[items truncated]"); break; } result.push(excerpt(item, depth + 1)); }
    return result;
  }
  if(value.object()) {
    Json result = Json::Object{};
    for(const auto& item : value.fields()) { if(result.fields().size() == 32) { result["_truncated"] = true; break; } result[item.first] = excerpt(item.second, depth + 1); }
    return result;
  }
  return value;
}
Json compact_excerpt(const Json& value, unsigned depth = 0) {
  if(value.string()) return clip(value.text(), 512);
  if(depth >= 8) return "[depth limit]";
  if(value.array()) {
    Json result = Json::Array{};
    for(const auto& item : value.items()) {
      if(result.items().size() == 8) { result.push("[items truncated]"); break; }
      result.push(compact_excerpt(item, depth + 1));
    }
    return result;
  }
  if(value.object()) {
    Json result = Json::Object{};
    for(const auto& item : value.fields()) {
      if(result.fields().size() == 16) { result["_truncated"] = true; break; }
      result[item.first] = compact_excerpt(item.second, depth + 1);
    }
    return result;
  }
  return value;
}
Json parse_model_object(const std::string& response, bool* recovered = nullptr) {
  if(recovered) *recovered = false;
  const auto body = legacy::unwrap_model_json(response);
  try {
    auto value = Json::parse(body);
    require(value.object(), "invalid_json");
    return value;
  } catch(const std::exception&) {
    // 部分 OpenAI 兼容服务即使指定 response_format=json_object，仍会在有效 JSON
    // 对象前后附带简短说明。这里只接受唯一一个括号平衡且可解析的对象；多个对象
    // 仍视为歧义，不能猜测哪一个才是模型结果。
  }
  Json only;
  std::size_t count = 0, start = 0, depth = 0;
  bool quoted = false, escaped = false;
  for(std::size_t i = 0; i < body.size(); ++i) {
    const char c = body[i];
    if(depth && quoted) {
      if(escaped) escaped = false;
      else if(c == '\\') escaped = true;
      else if(c == '"') quoted = false;
      continue;
    }
    if(depth && c == '"') { quoted = true; continue; }
    if(c == '{') {
      if(!depth) start = i;
      ++depth;
    } else if(c == '}' && depth && !--depth) {
      try {
        auto candidate = Json::parse(body.substr(start, i - start + 1));
        if(candidate.object()) { only = std::move(candidate); ++count; }
      } catch(const std::exception&) {}
    }
  }
  require(count == 1, "invalid_json");
  if(recovered) *recovered = true;
  return only;
}
Json all_source_experiences(const Json& artifact) {
  Json result = Json::Array{};
  const auto policy = proto<v1::RuntimePolicy>(artifact["policy"]);
  for(const auto& item : policy.snapshot().experiences()) {
    if(!item.control_target_type().empty()) continue;
    bool approved = false;
    for(const auto& ref : policy.options().approved_experiences())
      approved |= ref.memory_id() == item.memory_id() && ref.content_revision() == item.content_revision();
    if(!approved) continue;
    Json value{{"memory_id", item.memory_id()}, {"scope", item.target_scope()},
        {"condition", item.trigger_pattern()}, {"content", item.content()}};
    for(const auto& rewrite : policy.options().experience_overrides())
      if(rewrite.source_experience().memory_id() == item.memory_id()) {
        value["content"] = rewrite.content();
        if(!rewrite.trigger_pattern().empty()) value["condition"] = value["trigger_pattern"] = rewrite.trigger_pattern();
        if(!rewrite.observed_operations().empty()) {
          value["observed_operations"] = Json::Array{};
          for(const auto& operation : rewrite.observed_operations()) value["observed_operations"].push(operation);
        }
      }
    for(const auto& disabled : policy.options().disabled_injections())
      if(disabled.memory_id() == item.memory_id() && disabled.content_revision() == item.content_revision()) {
        value["content"] = nullptr;
        value.fields().erase("trigger_pattern"); value.fields().erase("observed_operations");
      }
    result.push(value);
  }
  return result;
}
// 公共关系固定 PublishedExperience 的版本；批准和应用回执固定 ShadowExperience
// 的版本。两种 hash 不可直接比较，只有内容与双边批准均有效时才合并搜索轴。
std::map<std::pair<std::string, std::string>, std::string> canonical_experience_aliases(
    const v1::RuntimePolicy& policy) {
  std::map<std::pair<std::string, std::string>, std::string> result;
  std::map<std::string, const v1::ShadowExperience*> entries;
  std::set<std::pair<std::string, std::string>> approved;
  for(const auto& item : policy.snapshot().experiences()) entries[item.memory_id()] = &item;
  for(const auto& ref : policy.options().approved_experiences())
    approved.emplace(ref.memory_id(), ref.content_revision());
  for(const auto& relation : policy.snapshot().cross_relations()) {
    const auto own = entries.find(relation.private_memory_id()), shared = entries.find(relation.shared_memory_id());
    if(relation.relation() != "duplicate" || own == entries.end() || shared == entries.end()
        || own->second->application_shared() || !shared->second->application_shared()
        || !own->second->control_target_type().empty() || !shared->second->control_target_type().empty()
        || own->second->target_scope() != shared->second->target_scope()
        || own->second->content_revision() != relation.private_content_revision()
        || !approved.count({own->first, own->second->content_revision()})
        || !approved.count({shared->first, shared->second->content_revision()})) continue;
    v1::PublishedExperience published;
    published.set_memory_id(shared->first); published.set_target_scope(shared->second->target_scope());
    published.set_trigger_pattern(shared->second->trigger_pattern()); published.set_content(shared->second->content());
    if(shared_content_revision(published) == relation.shared_content_revision())
      result.emplace(std::make_pair(shared->first, shared->second->content_revision()), own->first);
  }
  return result;
}
Json source_experiences(const Json& artifact) {
  auto result = all_source_experiences(artifact);
  const auto aliases = canonical_experience_aliases(proto<v1::RuntimePolicy>(artifact["policy"]));
  std::set<std::string> shadowed_shared;
  for(const auto& alias : aliases) shadowed_shared.insert(alias.first.first);
  Json filtered = Json::Array{};
  for(const auto& item : result.items()) if(!shadowed_shared.count(item.str("memory_id"))) filtered.push(item);
  return filtered;
}

// 仅含内容的组件保持旧搜索所使用的线缆格式，确保历史树能够继续回放。
Json experience_component(const Json& source) {
  if(!source.has("trigger_pattern") && !source.has("observed_operations")) return source["content"];
  Json result{{"content", source["content"]}};
  for(const auto* field : {"trigger_pattern", "observed_operations"})
    if(source.has(field)) result[field] = source[field];
  return result;
}
std::string experience_content(const Json& value) {
  return value.null() ? std::string{} : value.object() ? value.str("content") : value.text();
}

Json observed_target_options(const std::string& scope, const std::string& operation, const Json& arguments) {
  Json observed{{"action_rules", Json::Array{Json{{"rule_id", "$operation-budget"},
      {"target_scope", scope}, {"operation", operation}, {"max_attempts", 16}}}}};
  if(arguments.object()) for(const auto& argument : arguments.fields()) {
    if(observed["action_rules"].items().size() >= 32) break;
    const auto& value = argument.second;
    if(!identifier(argument.first) || value.null() || value.object() || value.array()
        || (value.string() && (value.text().empty() || value.text().size() > 1024))) continue;
    observed["action_rules"].push(Json{{"rule_id", argument.first}, {"target_scope", scope},
        {"operation", operation}, {"failure_key_arguments", Json::Array{argument.first}}});
  }
  return observed;
}

Json feedback_trial(const dataset::v1::EvaluationTrial& trial, const Json& controls) {
  const auto raw = json(trial), evidence = json(trial.evidence());
  Json result{{"verdict", trial.verdict()}, {"checks", excerpt(raw["checks"])},
      {"metrics", json(trial.metrics())}, {"execution_status", trial.execution_status()}};
  auto& view = result["evidence"];
  for(const auto* field : {"output", "state", "artifacts", "loop_review"})
    if(evidence.has(field)) view[field] = excerpt(evidence[field]);
  const auto traces = evidence.has("traces") ? evidence["traces"]
      : evidence.has("trace") ? Json(Json::Array{evidence["trace"]}) : Json(Json::Array{});
  for(const auto& trace : traces.items()) {
    Json run{{"run_id", trace["identity"]["run_id"]}, {"actions", Json::Array{}}};
    std::map<std::string, Json> starts, step_starts, action_controls, loop_controls, experience_controls;
    Json target_ledger = Json::Object{}, target_attempts = Json::Object{};
    for(const auto& event : trace["events"].items()) {
      if(event.str("kind") == "action.start" && event["attributes"].str("type") == "schedule.action"
          && !event["attributes"].str("target_scope").empty()) {
        const auto scope = event["attributes"].str("target_scope"), operation = event.str("operation");
        const auto observed = observed_target_options(scope, operation, event["input"]);
        target_attempts[event.str("action_id")] = Json{{"options", observed}, {"scope", scope}, {"operation", operation},
            {"execution_id", event["action_id"]},
            {"entries", target_failure_snapshot(observed, scope, operation, event["input"], target_ledger, event.str("action_id"))}};
      }
      if(event.str("kind") == "action.end") {
        const auto& frame = static_cast<const Json&>(target_attempts)[event.str("action_id")];
        settle_target_event(target_ledger, frame, frame["options"], event);
      }
      if(event.str("kind") == "action.start") starts[event.str("action_id")] = event;
      if(event.str("kind") == "step.start") step_starts[event.str("step_id")] = event;
      if(event.str("kind") == "advice.applied" && event["attributes"]["receipt"].str("boundary") == "llm")
        experience_controls[event.str("step_id")]["application_status"] = event["application"]["status"];
      const auto& decision = event["attributes"]["decision"];
      const auto& query = decision["query"];
      if(event["attributes"].str("type") != "policy.query") continue;
      if(query.str("boundary") == "llm" && !event.str("step_id").empty()) {
        Json advice{{"target_scope", query["advice_query"]["target_scope"]}, {"memory_ids", Json::Array{}}};
        if(decision["experiences"].array())
          for(const auto& source : decision["experiences"].items()) advice["memory_ids"].push(source["memory_id"]);
        if(query.has("observed_operations")) advice["observed_operations"] = query["observed_operations"];
        experience_controls[event.str("step_id")] = advice;
      }
      if(query.str("boundary") == "schedule" && query["actions"].array()) {
        auto schedule_query = proto<v1::RuntimeQuery>(query);
        const auto start = step_starts.find(event.str("step_id"));
        if(start != step_starts.end()) project_schedule_evidence(schedule_query, start->second["input"]);
        Json plan{{"query_event_id", event["event_id"]}, {"target_scope", query["advice_query"]["target_scope"]},
            {"actions", Json::Array{}}, {"effective_plan", decision["schedule"]}};
        // 文本条件匹配 Run 输入，不是某个技能的派发 prompt；给出真实匹配文本。
        plan["task_text"] = clip(query["advice_query"].str("task"), 2048);
        const auto schedule_input = json(schedule_query);
        for(const auto& action : schedule_input["actions"].items()) {
          Json item{{"action_id", action["action_id"]}, {"operation", action["operation"]}};
          if(action.has("after")) item["after"] = action["after"];
          if(action.has("arguments")) item["arguments"] = action["arguments"];
          plan["actions"].push(item);
        }
        run["schedule_plans"].push(plan);
      }
      if(query.str("boundary") == "loop" && !event.str("step_id").empty()) {
        const auto& progress = decision["progress"];
        const auto& previous = query["previous"];
        const auto fingerprint = query.str("state_fingerprint");
        const bool counted = !query.flag("pending_actions", false) && !fingerprint.empty()
            && progress.str("fingerprint") == fingerprint
            && progress["iteration"].dump() == query["iteration"].dump();
        // 在重规划重置计数器前暴露运行时计数。模型评审的浪费是另一类信号；
        // 缺失或关闭的计数器必须保持未知，不能补成零。
        Json control{{"query_event_id", event["event_id"]}, {"target_scope", query["advice_query"]["target_scope"]},
            {"observed_no_progress", counted ? Json(std::max({query.num("repeated_action_steps", 0), query.num("repeated_failure_steps", 0),
                previous.str("fingerprint") == fingerprint ? previous.num("no_progress", 0) + 1 : std::size_t(0)})) : Json()},
            {"pending_actions", query.flag("pending_actions", false)}, {"proposed_decision", progress["decision"]}};
        control["failed_actions"] = query.num("failed_actions", 0);
        control["repeated_failure_steps"] = query.num("repeated_failure_steps", 0);
        if(query.has("error_codes")) control["error_codes"] = query["error_codes"];
        if(query.has("rejected_actions")) control["rejected_actions"] = query["rejected_actions"];
        if(query.has("can_finalize")) control["can_finalize"] = query["can_finalize"];
        // Protobuf JSON 会将 uint64 的 iteration 表示为十进制字符串。
        control["iteration"] = static_cast<std::size_t>(proto<v1::RuntimeQuery>(query).iteration());
        if(progress.has("rule_id")) control["rule_id"] = progress["rule_id"];
        loop_controls[event.str("step_id")] = std::move(control);
      }
      if(query.str("boundary") != "actions" || !query["actions"].array()) continue;
      for(const auto& planned : query["actions"].items()) {
        if(planned.str("action_id").empty()) continue;
        Json control{{"query_event_id", event["event_id"]}, {"target_scope", query["advice_query"]["target_scope"]}};
        if(planned.has("schema_required_arguments")) control["schema_required_arguments"] = planned["schema_required_arguments"];
        // 必须在展示裁剪前判断参数是否存在，避免真实参数被截断后误判为缺失。
        const auto& configured = controls["actions"][planned.str("operation")];
        const auto& required = configured.array() ? configured : planned["schema_required_arguments"];
        control["missing_required_arguments"] = Json::Array{};
        if(required.array()) for(const auto& name : required.items()) {
          const auto& value = planned["arguments"][name.text()];
          if(value.null() || (value.string() && value.text().empty())
              || (value.array() && value.items().empty()) || (value.object() && value.fields().empty()))
            control["missing_required_arguments"].push(name);
        }
        if(planned.has("prior_failures")) control["prior_failures"] = planned["prior_failures"];
        const auto observed = observed_target_options(query["advice_query"].str("target_scope"),
            planned.str("operation"), planned["arguments"]);
        const auto prior = target_failure_snapshot(observed, query["advice_query"].str("target_scope"),
            planned.str("operation"), planned["arguments"], target_ledger, query.str("execution_id"));
        target_attempts[planned.str("action_id")] = Json{{"entries", prior}, {"options", observed},
            {"scope", query["advice_query"]["target_scope"]}, {"operation", planned["operation"]},
            {"execution_id", query["execution_id"]}};
        control["prior_operation_rounds"] = prior["$operation-budget"].num("count", 0);
        for(const auto& attempt : prior.fields()) if(attempt.first != "$operation-budget" && attempt.second.num("count", 0))
          control["failure_key_candidates"].push(Json{{"argument", attempt.first}, {"prior_failures", attempt.second["count"]},
              {"cross_execution", !attempt.second.str("execution_id").empty()
                  && attempt.second.str("execution_id") != query.str("execution_id")}});
        if(planned.has("target_failures")) control["target_failures"] = planned["target_failures"];
        if(decision["actions"].array()) for(const auto& outcome : decision["actions"].items())
          if(outcome.str("action_id") == planned.str("action_id")) control["decision"] = outcome["decision"];
        action_controls[planned.str("action_id")] = std::move(control);
      }
    }
    for(const auto& event : trace["events"].items()) {
      const auto kind = event.str("kind");
      if(kind != "action.end" && kind != "run.start" && kind != "run.end" && kind != "loop.decision") continue;
      const auto found = starts.find(event.str("action_id"));
      const auto start = found == starts.end() ? Json(Json::Object{}) : found->second;
      Json action = Json::Object{};
      for(const auto* field : {"event_id", "kind", "actor_id", "execution_id", "loop_id", "iteration",
                              "status", "error", "decision", "reason", "outcome"})
        if(event.has(field)) action[field] = excerpt(event[field]);
      const auto operation = event.str("operation", start.str("operation"));
      if(!operation.empty()) action["operation"] = operation;
      if(event["attributes"].has("executed")) action["executed"] = event["attributes"]["executed"];
      const bool model = event["attributes"].str("semantic") == "model"
          || start["attributes"].str("semantic") == "model";
      if(kind == "action.end") {
        action["semantic"] = event["attributes"].str("semantic", start["attributes"].str("semantic"));
        const auto step = step_starts.find(event.str("step_id"));
        if(start["attributes"]["extensions"].flag("guard_only", false)
            || (step != step_starts.end() && step->second["attributes"]["extensions"].flag("guard_only", false)))
          action["guard_only"] = true;
        const auto control = action_controls.find(event.str("action_id"));
        action["action_control"] = control == action_controls.end() ? Json() : control->second;
        if(!model && event["attributes"].flag("observed_failure", event.str("status") == "failed")) {
          const auto code = event["attributes"].str("observed_error_code", structured_error_code(event["output"]));
          if(!code.empty()) action["terminal_error_code_candidate"] = code;
        }
        if(model) {
          const auto advice = experience_controls.find(event.str("step_id"));
          if(advice != experience_controls.end()) action["experience_query"] = advice->second;
        }
      }
      if(kind == "loop.decision") {
        const auto control = loop_controls.find(event.str("step_id"));
        action["loop_control"] = control == loop_controls.end() ? Json() : control->second;
      }
      // 反思只使用任务/评分标准、真实决策和工具结果。重放的模型消息历史及供应商
      // 封装不会产生新动作，因此不应占用反思上下文。
      if(!model) {
        if(event.has("input")) action["input"] = excerpt(event["input"]);
        else if(start.has("input")) action["input"] = excerpt(start["input"]);
      }
      if(event.has("output")) {
        const auto& output = event["output"];
        action["output"] = excerpt(output);
        if(model && output["choices"].array()) {
          action["output"] = Json::Array{};
          for(const auto& choice : output["choices"].items()) {
            Json message = Json::Object{};
            for(const auto* field : {"content", "tool_calls", "function_call", "refusal"})
              if(choice["message"].has(field)) message[field] = excerpt(choice["message"][field]);
            if(choice.has("finish_reason")) message["finish_reason"] = choice["finish_reason"];
            action["output"].push(message);
          }
        }
      }
      run["actions"].push(action);
    }
    view["runs"].push(run);
  }
  return result;
}

// 归因只认已应用回执及其固定版本，不把查询、相邻失败或公共库近似文本当成使用证明。
Json selected_experience_applications(const dataset::v1::EvaluationTrial& trial,
    const Json& selected, const Json& artifact) {
  Json result{{"status", "not_observed"}, {"count", 0}, {"points", Json::Array{}}};
  const auto policy = proto<v1::RuntimePolicy>(artifact["policy"]);
  const auto aliases = canonical_experience_aliases(policy);
  std::map<std::string, std::string> revisions;
  for(const auto& source : policy.snapshot().experiences())
    for(const auto& approved : policy.options().approved_experiences())
      if(source.memory_id() == approved.memory_id() && source.content_revision() == approved.content_revision())
        revisions[source.memory_id()] = source.content_revision();
  const auto evidence = json(trial.evidence());
  const auto traces = evidence["traces"].array() ? evidence["traces"] : Json(Json::Array{evidence["trace"]});
  std::size_t count = 0;
  for(const auto& trace : traces.items()) for(const auto& event : trace["events"].items()) {
    const auto& receipt = event["attributes"]["receipt"];
    if(event.str("kind") != "advice.applied" || event["application"].str("status") != "applied"
        || receipt.str("boundary") != "llm" || !receipt["experiences"].array()
        || trace["identity"].str("run_id").empty() || event.str("event_id").empty()) continue;
    for(const auto& source : receipt["experiences"].items()) {
      const auto actual = source.str("memory_id"), revision = source.str("content_revision");
      const auto known = revisions.find(actual);
      if(known == revisions.end() || known->second != revision) continue;
      const auto alias = aliases.find({actual, revision});
      const auto canonical = alias == aliases.end() ? actual : alias->second;
      if(std::none_of(selected.items().begin(), selected.items().end(), [&](const auto& component) {
        return component.text() == "experience:" + canonical;
      })) continue;
      ++count;
      if(result["points"].items().size() == 8) continue;
      Json point{{"run_id", trace["identity"]["run_id"]}, {"receipt_event_id", event["event_id"]},
          {"memory_id", canonical}, {"content_revision", revisions.at(canonical)}};
      if(canonical != actual) {
        point["receipt_memory_id"] = actual; point["receipt_content_revision"] = revision;
      }
      for(const auto* field : {"actor_id", "execution_id"}) if(event.has(field)) point[field] = event[field];
      result["points"].push(point);
    }
  }
  result["count"] = count;
  if(count) result["status"] = "observed";
  if(count > result["points"].items().size()) result["truncated"] = true;
  return result;
}

// 完整 trial 超出反思预算时，仍保留带回执的 L1 证据；相邻动作只提供上下文，
// 不能被错误地当作因果归因。
Json compact_experience_feedback_trial(const dataset::v1::EvaluationTrial& trial,
    const Json& full, const Json& selected, std::size_t byte_limit, const Json& artifact = Json()) {
  Json summary{{"verdict", full["verdict"]}, {"checks", compact_excerpt(full["checks"])},
      {"output", compact_excerpt(json(trial.evidence())["output"])},
      {"metrics", full["metrics"]}, {"evidence_truncated", true}};
  const auto evidence = json(trial.evidence());
  const auto traces = evidence.has("traces") ? evidence["traces"]
      : evidence.has("trace") ? Json(Json::Array{evidence["trace"]}) : Json(Json::Array{});
  if(!full["evidence"]["runs"].array()) return summary;
  std::map<std::pair<std::string, std::string>, std::string> canonical;
  if(artifact["policy"].object())
    canonical = canonical_experience_aliases(proto<v1::RuntimePolicy>(artifact["policy"]));
  auto selected_memory = [&](const Json& source) {
    auto id = source.str("memory_id");
    const auto alias = canonical.find({id, source.str("content_revision")});
    if(alias != canonical.end()) id = alias->second;
    for(const auto& component : selected.items())
      if(component.text() == "experience:" + id) return id;
    return std::string{};
  };
  struct Point { std::size_t run, action; Json query, receipt; };
  std::vector<Point> points;
  std::vector<std::map<std::string, Json>> events;
  std::vector<std::map<std::string, std::size_t>> positions;
  std::vector<std::map<std::string, std::size_t>> query_positions;
  const auto& runs = full["evidence"]["runs"].items();
  for(std::size_t r = 0; r < traces.items().size() && r < runs.size(); ++r) {
    std::map<std::string, Json> by_id, queries, receipts;
    positions.emplace_back();
    query_positions.emplace_back();
    for(const auto& event : traces.items()[r]["events"].items()) {
      positions.back()[event.str("event_id")] = positions.back().size();
      by_id[event.str("event_id")] = event;
      if(event["attributes"].str("type") == "policy.query"
          && event["attributes"]["decision"]["query"].str("boundary") == "llm") {
        queries[event.str("step_id")] = event;
        query_positions.back()[event.str("step_id")] = positions.back().at(event.str("event_id"));
      }
      if(event.str("kind") == "advice.applied" && event["application"].str("status") == "applied"
          && event["attributes"]["receipt"].str("boundary") == "llm")
        receipts[event.str("step_id")] = event;
    }
    events.push_back(std::move(by_id));
    for(std::size_t a = 0; a < runs[r]["actions"].items().size(); ++a) {
      const auto& action = runs[r]["actions"].items()[a];
      if(action.str("semantic") != "model") continue;
      const auto found = events.back().find(action.str("event_id"));
      if(found == events.back().end()) continue;
      const auto step = found->second.str("step_id");
      const auto q = queries.find(step), receipt = receipts.find(step);
      if(q == queries.end() || receipt == receipts.end()) continue;
      bool relevant = false;
      const auto& used = receipt->second["attributes"]["receipt"]["experiences"];
      if(used.array()) for(const auto& source : used.items())
        relevant |= !selected_memory(source).empty();
      if(relevant) points.push_back({r, a, q->second, receipt->second});
    }
  }
  if(points.empty()) return summary;
  Json base{{"verdict", full["verdict"]}, {"metrics", full["metrics"]},
      {"execution_status", full["execution_status"]}, {"evidence_truncated", true}};
  std::map<std::size_t, std::map<std::size_t, Json>> kept;
  auto render = [&] {
    auto result = base; result["evidence"]["runs"] = Json::Array{};
    for(const auto& run : kept) {
      Json view{{"run_id", runs[run.first]["run_id"]}, {"actions", Json::Array{}}};
      for(const auto& action : run.second) view["actions"].push(action.second);
      result["evidence"]["runs"].push(view);
    }
    return result;
  };
  auto insert = [&](std::size_t r, std::size_t a, const Json& action) {
    if(kept[r].count(a)) return true;
    kept[r][a] = action;
    if(render().dump().size() <= byte_limit) return true;
    kept[r].erase(a); if(kept[r].empty()) kept.erase(r);
    return false;
  };
  // 对每个 run/source 优先保留最近一次应用，再按时间回溯更早的观测点。
  std::vector<Point> ordered, rest;
  std::set<std::pair<std::size_t, std::string>> represented;
  for(auto it = points.rbegin(); it != points.rend(); ++it) {
    bool first = false;
    for(const auto& source : it->receipt["attributes"]["receipt"]["experiences"].items()) {
      const auto id = selected_memory(source);
      if(!id.empty()) first |= represented.emplace(it->run, id).second;
    }
    (first ? ordered : rest).push_back(*it);
  }
  ordered.insert(ordered.end(), rest.begin(), rest.end());
  std::vector<Point> retained;
  for(const auto& point : ordered) {
    if(retained.size() == 8) break;
    const auto& source = runs[point.run]["actions"].items()[point.action];
    auto action = compact_excerpt(source);
    const auto& query = point.query["attributes"]["decision"]["query"];
    action["experience_query"] = source["experience_query"];
    auto& view = action["experience_query"];
    view["query_event_id"] = point.query["event_id"];
    view["receipt_event_id"] = point.receipt["event_id"];
    view["applied_memory_ids"] = Json::Array{};
    for(const auto& ref : point.receipt["attributes"]["receipt"]["experiences"].items())
      view["applied_memory_ids"].push(ref["memory_id"]);
    view["state_excerpt"] = clip(query["advice_query"].str("state"), 512);
    if(!insert(point.run, point.action, action)) {
      view["state_excerpt"] = clip(query["advice_query"].str("state"), 128);
      action.fields().erase("output");
      if(!insert(point.run, point.action, action)) continue;
    }
    retained.push_back(point);
  }
  for(const auto& point : retained) {
    const auto& actions = runs[point.run]["actions"].items();
    const auto& model = events[point.run].at(actions[point.action].str("event_id"));
    auto same_branch = [&](const Json& event) {
      if(model.str("actor_id").empty() || model.str("execution_id").empty()
          || event.str("actor_id") != model.str("actor_id")
          || event.str("execution_id") != model.str("execution_id")) return false;
      const auto& a = model["attributes"]["extensions"]["point_reference"];
      const auto& b = event["attributes"]["extensions"]["point_reference"];
      return a.object() == b.object() && (!a.object() || a.str("parent", "root") == b.str("parent", "root"));
    };
    std::size_t next_query = SIZE_MAX;
    for(std::size_t i = point.action + 1; i < actions.size(); ++i) {
      const auto event = events[point.run].find(actions[i].str("event_id"));
      if(actions[i].str("semantic") != "model" || event == events[point.run].end() || !same_branch(event->second)) continue;
      const auto q = query_positions[point.run].find(event->second.str("step_id"));
      if(q != query_positions[point.run].end()) next_query = q->second;
      break;
    }
    for(const int direction : {-1, 1}) {
      std::size_t count = 0;
      for(int i = static_cast<int>(point.action) + direction; i >= 0 && i < static_cast<int>(actions.size()); i += direction) {
        const auto& nearby = actions[i];
        const auto event = events[point.run].find(nearby.str("event_id"));
        if(event == events[point.run].end() || !same_branch(event->second)) continue;
        if(nearby.str("semantic") == "model") break;
        if(nearby.str("semantic") != "action") continue;
        if(direction > 0 && positions[point.run].at(nearby.str("event_id")) >= next_query) break;
        if(direction < 0) {
          const auto& observed = point.query["attributes"]["decision"]["query"]["observed_operations"];
          if(positions[point.run].at(nearby.str("event_id")) >= positions[point.run].at(point.query.str("event_id"))
              || !nearby.flag("executed", false) || !observed.array()
              || std::none_of(observed.items().begin(), observed.items().end(),
                  [&](const auto& op) { return op.text() == nearby.str("operation"); })) continue;
        }
        // 委派 ACK 不一定含业务结果。只沿本次动作的直接因果链补齐批次结果，
        // 不借用时间相邻的兄弟批次，也不把批次结束误当作业务成功。
        const auto& delegate = event->second;
        const auto& parent = delegate["attributes"]["extensions"]["point_reference"];
        if(direction < 0 && delegate["attributes"].flag("delegates_schedule", false)
            && !parent.str("key").empty() && delegate["caused_by"].array()) {
          std::size_t result_count = 0;
          for(const auto& cause : delegate["caused_by"].items()) {
            const auto batch = events[point.run].find(cause.text());
            if(batch == events[point.run].end()) continue;
            const auto& end = batch->second;
            const auto& ref = end["attributes"]["extensions"]["point_reference"];
            if(end.str("kind") != "step.end" || end.str("step_id").empty()
                || end.str("actor_id") != delegate.str("actor_id")
                || end.str("execution_id") != delegate.str("execution_id")
                || ref.str("parent") != parent.str("key") || ref.str("run") != parent.str("run")
                || positions[point.run].at(end.str("event_id")) >= positions[point.run].at(delegate.str("event_id"))
                || !end["caused_by"].array()) continue;
            for(const auto& result_id : end["caused_by"].items()) {
              if(result_count == 4) break;
              const auto found = events[point.run].find(result_id.text());
              if(found == events[point.run].end()) continue;
              const auto& result = found->second;
              if(result.str("kind") != "action.end" || result["attributes"].str("type") != "schedule.action"
                  || result.str("step_id") != end.str("step_id")
                  || result.str("actor_id") != end.str("actor_id") || result.str("execution_id") != end.str("execution_id")
                  || !result["attributes"].flag("executed", false)
                  || (result.str("status") != "completed" && result.str("status") != "failed")
                  || positions[point.run].at(result.str("event_id")) >= positions[point.run].at(end.str("event_id"))) continue;
              const auto row = std::find_if(actions.begin(), actions.end(), [&](const auto& action) {
                return action.str("event_id") == result.str("event_id");
              });
              if(row == actions.end()) continue;
              auto linked = compact_excerpt(*row);
              linked["dispatch_result_of"] = delegate["event_id"];
              linked["schedule_end_event_id"] = end["event_id"];
              if(insert(point.run, static_cast<std::size_t>(row - actions.begin()), linked)) ++result_count;
            }
          }
        }
        auto context = compact_excerpt(nearby);
        context["temporal_context_only"] = true;
        if(!insert(point.run, static_cast<std::size_t>(i), context) || ++count == 2) break;
      }
    }
  }
  if(kept.empty()) return summary;
  auto result = render();
  for(const auto* field : {"checks", "output"}) {
    auto candidate = result; candidate[field] = summary[field];
    if(candidate.dump().size() <= byte_limit) result = std::move(candidate);
  }
  return result;
}

bool action_control_evidence(const Json& action) {
  const auto& control = action["action_control"];
  if(action.str("kind") != "action.end" || !control.object()
      || control.str("decision") != "allow") return false;
  if(control.num("prior_failures", 0) >= 2
      && (action.flag("guard_only", false) || action.flag("executed", true))) return true;
  // 普通重复工具调用可能分别完成不同工作，不能凭次数占满机会窗口。
  // 总尝试预算只优先考虑重复派发检查点；具体循环证据仍保留给 L3。
  if(control.num("prior_operation_rounds", 0) > 0 && action.flag("guard_only", false)) return true;
  if(action.has("terminal_error_code_candidate") && action.flag("executed", false)) return true;
  if(action.flag("executed", true) && control["failure_key_candidates"].array())
    for(const auto& target : control["failure_key_candidates"].items())
      if(target.flag("cross_execution", false)) return true;
  return action.flag("executed", true) && control["missing_required_arguments"].array()
      && !control["missing_required_arguments"].items().empty();
}

std::string iteration_identity(const Json& run, const Json& action) {
  if(run.str("run_id").empty() || action.str("execution_id").empty() || action.str("loop_id").empty()
      || !action.has("iteration")) return {};
  // 与 task_evaluation.cpp::loop_inventory 对齐：Protobuf 规范化后，数值形式和
  // 十进制字符串形式的 uint64 都表示同一次迭代。
  const auto event = proto<v1::TraceEvent>(Json{{"iteration", action["iteration"]}});
  if(!event.iteration()) return {};
  const auto iteration = json(event)["iteration"];
  return hash(Json(Json::Array{run["run_id"], action["execution_id"], action["loop_id"], iteration}).dump());
}

// 只从完整原始轨迹确认末轮是一次纯文本交付；工具工作、缺失结束和未知格式不作推断。
bool answer_only_iteration(const Json& raw, const std::string& run_id,
    const std::string& execution, const std::string& loop, std::size_t iteration) {
  const auto traces = raw["traces"].array() ? raw["traces"] : Json(Json::Array{raw["trace"]});
  for(const auto& trace : traces.items()) {
    if(trace["identity"].str("run_id") != run_id || !trace["events"].array()) continue;
    std::set<std::string> starts, ends;
    bool completed = false;
    for(const auto& event : trace["events"].items()) {
      if(event.str("kind") == "run.end")
        completed = event["outcome"].str("execution_status", event.str("status")) == "completed";
      if(event.str("execution_id") != execution || event.str("loop_id") != loop || event["iteration"].null()) continue;
      if(proto<v1::TraceEvent>(Json{{"iteration", event["iteration"]}}).iteration() != iteration) continue;
      const auto kind = event.str("kind");
      const auto& attributes = event["attributes"];
      if(kind == "step.start" && attributes.str("type") != "boundary.control"
          && attributes.str("semantic") != "model") return false;
      if(kind != "action.start" && kind != "action.end") continue;
      if(attributes.str("semantic") != "model" || event.str("action_id").empty()) return false;
      if(kind == "action.start") { if(!starts.insert(event.str("action_id")).second) return false; continue; }
      if(event.str("status") != "completed" || !attributes.flag("executed", false)
          || !ends.insert(event.str("action_id")).second) return false;
      const auto& choices = event["output"]["choices"];
      if(!choices.array() || choices.items().size() != 1) return false;
      const auto& choice = choices.items().front(); const auto& message = choice["message"];
      if(choice.str("finish_reason") != "stop" || !message["content"].string() || message.str("content").empty()
          || !message["function_call"].null()
          || (!message["tool_calls"].null() && (!message["tool_calls"].array() || !message["tool_calls"].items().empty()))) return false;
    }
    return completed && starts.size() == 1 && starts == ends;
  }
  return false;
}

// 在截断前将完整的语义迭代评审绑定到精简结构行。被评审的后缀只描述这次训练观测；
// 后续评测仍须判断由此得到的 actor 级上限能否泛化。
void bind_iteration_assessments(Json& trial, const Json& raw_evidence) {
  auto& evidence = trial["evidence"];
  // feedback_trial 有意只摘录展示字段；授权必须改用评测器验证过的完整原始评审。
  const auto& review = raw_evidence["loop_review"];
  if(review.str("status") != "complete" || !review["iteration_reviews"].array()
      || review["observed_iterations"].null() || !evidence["runs"].array()) return;
  std::map<std::string, Json> assessments;
  for(const auto& item : review["iteration_reviews"].items()) {
    if(!item.object()) return;
    const auto id = item.str("iteration_id"), decision = item.str("decision"), reason = item.str("reason");
    if(id.empty() || (decision != "keep" && decision != "waste") || reason.empty()
        || !assessments.emplace(id, Json{{"iteration_id", id}, {"decision", decision},
             {"reason", clip(reason, 512)}}).second) return;
  }
  if(assessments.size() != static_cast<std::size_t>(review.num("observed_iterations", 0))) return;

  struct Bound { Json* action; std::string id; std::string group; std::size_t iteration; bool loop; };
  std::vector<Bound> bound;
  std::set<std::string> matched;
  std::map<std::string, std::map<std::size_t, std::string>> decisions;
  std::map<std::string, std::vector<std::size_t>> loops;
  for(auto& run : evidence["runs"].items()) for(auto& action : run["actions"].items()) {
    const auto id = iteration_identity(run, action);
    if(id.empty()) continue;
    const auto found = assessments.find(id);
    const bool loop = action.str("kind") == "loop.decision" && action["loop_control"].object();
    const auto iteration = static_cast<std::size_t>(
        proto<v1::TraceEvent>(Json{{"iteration", action["iteration"]}}).iteration());
    bound.push_back(Bound{&action, id, run.str("run_id") + "\n" + action.str("execution_id")
        + "\n" + action.str("loop_id"), iteration, loop});
    const auto current_index = bound.size() - 1;
    auto& current = bound[current_index];
    if(found != assessments.end()) {
      matched.insert(id);
      if(iteration) {
        const auto inserted = decisions[current.group].emplace(iteration, found->second.str("decision"));
        if(!inserted.second && inserted.first->second != found->second.str("decision")) return;
      }
    }
    if(loop && iteration) loops[current.group].push_back(current_index);
  }
  // 过期、截断或采用不同规范化方式的评审，不能授权上限，也不能意外标注到另一执行上。
  if(matched.size() != assessments.size()) return;

  std::map<std::string, std::size_t> waste_after;
  std::map<std::string, std::size_t> final_answers;
  for(const auto& group : decisions) {
    if(group.second.empty()) continue;
    auto first = group.second.rbegin();
    if(first->second == "keep" && group.second.size() > 1) {
      const auto indices = loops.find(group.first);
      if(indices == loops.end() || indices->second.empty()) continue;
      const auto& sample = *bound[indices->second.front()].action;
      const auto run_end = group.first.find('\n');
      if(!answer_only_iteration(raw_evidence, group.first.substr(0, run_end),
          sample.str("execution_id"), sample.str("loop_id"), first->first)) continue;
      final_answers[group.first] = first->first;
      ++first;
    }
    if(first->second != "waste") continue;
    std::size_t first_waste = first->first;
    for(++first; first != group.second.rend() && first->second == "waste"; ++first) first_waste = first->first;
    if(!first_waste) continue;
    const auto cap = first_waste - 1;
    bool complete_suffix = true;
    for(const auto index : loops[group.first]) {
      const auto& row = bound[index];
      if(row.iteration <= cap) continue;
      if(final_answers.count(group.first) && row.iteration == final_answers[group.first]) continue;
      const auto assessment = assessments.find(row.id);
      complete_suffix &= assessment != assessments.end() && assessment->second.str("decision") == "waste";
    }
    if(complete_suffix) waste_after[group.first] = cap;
  }
  for(auto& item : bound) {
    const auto assessment = assessments.find(item.id);
    if(assessment != assessments.end()) (*item.action)["iteration_assessment"] = assessment->second;
    const bool finalizing = final_answers.count(item.group);
    if(item.loop && waste_after.count(item.group)
        && (!finalizing || (item.iteration < final_answers[item.group]
            && (*item.action)["loop_control"].flag("can_finalize", false)))) {
      (*item.action)["loop_control"]["reviewed_waste_after"] = waste_after[item.group];
      if(finalizing) (*item.action)["loop_control"]["require_finalization"] = true;
    }
  }
  // 同名 actor 的另一次执行可能仍有必要工作；在展示裁剪前保留完整反例下界。
  // 有收尾能力时仅豁免已验证的末尾纯答复，不能把工具工作一并省掉。
  std::map<std::pair<std::string, std::string>, std::pair<std::size_t, std::size_t>> required;
  std::map<std::string, std::set<std::pair<std::string, std::string>>> scopes;
  std::set<std::string> finalizable;
  for(const auto& item : bound) if(item.loop) {
    const auto& control = (*item.action)["loop_control"];
    const auto key = std::make_pair(control.str("target_scope"), item.action->str("actor_id"));
    if(!key.first.empty() && !key.second.empty()) scopes[item.group].insert(key);
    if(control.flag("can_finalize", false)) finalizable.insert(item.group);
  }
  for(auto& item : bound) {
    const auto known_scope = scopes.find(item.group);
    if(known_scope == scopes.end()) continue;
    const bool answer = final_answers.count(item.group) && final_answers[item.group] == item.iteration;
    if(answer) {
      (*item.action)["reviewed_final_answer"] = true;
      if(item.loop) (*item.action)["loop_control"]["reviewed_final_answer"] = true;
    }
    const auto assessment = assessments.find(item.id);
    if(assessment == assessments.end() || assessment->second.str("decision") != "keep") continue;
    // 某轮缺少 loop 打点时，仍沿同 run/execution/loop 的确切归属保护已执行工作。
    for(const auto& key : known_scope->second) {
      if(!item.action->str("actor_id").empty() && item.action->str("actor_id") != key.second) continue;
      auto& floor = required[key];
      floor.first = std::max(floor.first, item.iteration);
      if(finalizable.count(item.group) && !answer) floor.second = std::max(floor.second, item.iteration);
    }
  }
  evidence["loop_requirements"] = Json::Array{};
  for(const auto& entry : required) for(const bool finish : {false, true})
    evidence["loop_requirements"].push(Json{{"target_scope", entry.first.first}, {"actor_id", entry.first.second},
        {"require_finalization", finish}, {"required_iteration_floor", finish ? entry.second.second : entry.second.first}});
  for(auto& item : bound) if(item.loop) {
    auto& control = (*item.action)["loop_control"];
    const auto found = required.find({control.str("target_scope"), item.action->str("actor_id")});
    if(found != required.end()) control["required_iteration_floor"] = control.flag("require_finalization", false)
        ? found->second.second : found->second.first;
  }
  // keep 评审属于整轮：仅有一个明确逻辑动作时才向该动作归因。
  // 混合动作轮保留未知；按真实 action_id 去重，不按操作名称合并并发分支。
  std::map<std::string, const Json*> raw_actions;
  const auto index_trace = [&](const Json& trace) {
    if(trace["events"].array()) for(const auto& event : trace["events"].items())
      if(event.str("kind") == "action.end" && !event.str("event_id").empty())
        raw_actions[trace["identity"].str("run_id") + '\n' + event.str("event_id")] = &event;
  };
  if(raw_evidence["traces"].array()) for(const auto& trace : raw_evidence["traces"].items()) index_trace(trace);
  else index_trace(raw_evidence["trace"]);
  std::map<std::string, std::set<std::string>> logical_actions;
  std::set<const Json*> delegated;
  std::set<std::string> independent_delivery;
  std::size_t action_ordinal = 0;
  for(const auto& item : bound) {
    auto& action = *item.action;
    action.fields().erase("reviewed_required_action");
    if(action.str("kind") != "action.end") continue;
    const auto raw = raw_actions.find(item.group.substr(0, item.group.find('\n')) + '\n' + action.str("event_id"));
    if(action.str("semantic") == "model") {
      if(raw != raw_actions.end() && (*raw->second)["output"]["choices"].array())
        for(const auto& choice : (*raw->second)["output"]["choices"].items()) {
          const auto& content = choice["message"]["content"];
          if(choice.str("finish_reason") == "stop" && (!content.string() || !content.text().empty()))
            independent_delivery.insert(item.id);
        }
      continue;
    }
    if(raw != raw_actions.end() && (*raw->second)["attributes"].flag("delegates_schedule", false)) {
      delegated.insert(item.action); continue;
    }
    const auto action_id = raw == raw_actions.end() ? std::string{} : raw->second->str("action_id");
    logical_actions[item.id].insert(action_id.empty() ? "event:" + action.str("event_id")
        + ":" + std::to_string(action_ordinal++) : "action:" + action_id);
  }
  for(const auto& item : bound)
    if(item.action->str("kind") == "action.end" && item.action->str("semantic") != "model"
        && !delegated.count(item.action) && logical_actions[item.id].size() == 1
        && !independent_delivery.count(item.id)
        && (*item.action)["iteration_assessment"].str("decision") == "keep")
      (*item.action)["reviewed_required_action"] = true;
  // 操作预算作用于同 scope/operation；必要的后续调用不能被另一条轨迹的冗余后缀覆盖。
  // 取各自账本计数的最大下界，不把独立 Run 或并发分支的次数相加。
  std::map<std::pair<std::string, std::string>, std::size_t> action_required;
  for(const auto& item : bound) {
    const auto& action = *item.action; const auto& control = action["action_control"];
    if(action.str("kind") != "action.end" || action.str("semantic") == "model"
        || !action.flag("reviewed_required_action", false)
        || control.str("decision") != "allow" || !control.has("prior_operation_rounds")
        || control.str("target_scope").empty() || action.str("operation").empty()) continue;
    auto& floor = action_required[{control.str("target_scope"), action.str("operation")}];
    floor = std::max(floor, control.num("prior_operation_rounds", 0) + 1);
  }
  evidence["action_requirements"] = Json::Array{};
  for(const auto& entry : action_required)
    evidence["action_requirements"].push(Json{{"target_scope", entry.first.first}, {"operation", entry.first.second},
        {"required_attempt_floor", entry.second}});
  // 在裁剪前定位更早的冗余派发。时间后缀还须满足计数边界：
  // 并发 keep/waste 可能具有相同 prior，不能用同一预算误伤必要分支。
  for(auto& run : evidence["runs"].items()) {
    std::map<std::pair<std::string, std::string>, std::vector<Json*>> guards;
    for(auto& action : run["actions"].items()) {
      const auto& control = static_cast<const Json&>(action)["action_control"];
      if(action.str("kind") == "action.end" && action.flag("guard_only", false)
          && !action.flag("executed", true) && control.object()
          && control.str("decision") == "allow" && !control.str("target_scope").empty()
          && !action.str("operation").empty())
        guards[{control.str("target_scope"), action.str("operation")}].push_back(&action);
    }
    for(const auto& group : guards) for(std::size_t i = 0; i < group.second.size(); ++i) {
      auto& candidate = *group.second[i];
      const auto prior = candidate["action_control"].num("prior_operation_rounds", 0);
      if(!prior || candidate["iteration_assessment"].str("decision") != "waste") continue;
      bool safe = true;
      for(std::size_t j = 0; j < group.second.size(); ++j) {
        const auto& other = *group.second[j];
        const auto decision = other["iteration_assessment"].str("decision");
        safe &= (decision == "keep" || decision == "waste")
            && other["action_control"].has("prior_operation_rounds")
            && (j < i || decision == "waste")
            && (decision != "keep" || other["action_control"].num("prior_operation_rounds", 0) < prior);
      }
      if(safe) {
        candidate["action_control"]["reviewed_budget_prior"] = prior;
        break;
      }
    }
  }
}

void merge_action_requirements(Json& target, const Json& source) {
  if(!source.array()) return;
  if(!target.array()) target = Json::Array{};
  for(const auto& row : source.items()) {
    auto found = std::find_if(target.items().begin(), target.items().end(), [&](const auto& previous) {
      return previous.str("target_scope") == row.str("target_scope") && previous.str("operation") == row.str("operation");
    });
    if(found == target.items().end()) target.push(row);
    else (*found)["required_attempt_floor"] = std::max(found->num("required_attempt_floor", 0), row.num("required_attempt_floor", 0));
  }
}

std::size_t required_attempt_floor(const Json& input, const Json& binding) {
  std::size_t floor = 1;
  const auto merge = [&](const Json& rows) {
    if(rows.array()) for(const auto& row : rows.items())
      if(row.str("target_scope") == binding.str("target_scope") && row.str("operation") == binding.str("operation"))
        floor = std::max(floor, row.num("required_attempt_floor", 0));
  };
  merge(input["controls"]["action_requirements"]);
  if(input["feedback"].array()) for(const auto& row : input["feedback"].items()) {
    const auto& evidence = row["trial"]["evidence"];
    merge(evidence["action_requirements"]);
    if(evidence["runs"].array()) for(const auto& run : evidence["runs"].items())
      if(run["actions"].array()) for(const auto& action : run["actions"].items()) {
        const auto& control = action["action_control"];
        if(action.str("kind") == "action.end" && action.str("semantic") != "model"
            && action.flag("reviewed_required_action", false) && control.str("decision") == "allow"
            && control.has("prior_operation_rounds") && control.str("target_scope") == binding.str("target_scope")
            && action.str("operation") == binding.str("operation"))
          floor = std::max(floor, control.num("prior_operation_rounds", 0) + 1);
      }
  }
  return floor;
}

void merge_loop_requirements(Json& target, const Json& source) {
  if(!source.array()) return;
  if(!target.array()) target = Json::Array{};
  for(const auto& row : source.items()) {
    auto found = std::find_if(target.items().begin(), target.items().end(), [&](const auto& previous) {
      return previous.str("target_scope") == row.str("target_scope") && previous.str("actor_id") == row.str("actor_id")
          && previous.flag("require_finalization", false) == row.flag("require_finalization", false);
    });
    if(found == target.items().end()) target.push(row);
    else (*found)["required_iteration_floor"] = std::max(found->num("required_iteration_floor", 0), row.num("required_iteration_floor", 0));
  }
}

std::size_t required_iteration_floor(const Json& input, const Json& control, const std::string& actor,
    std::size_t fallback = 0) {
  auto floor = control.num("required_iteration_floor", fallback);
  const auto& requirements = input["controls"]["loop_requirements"];
  if(requirements.array()) for(const auto& row : requirements.items())
    if(row.str("target_scope") == control.str("target_scope") && row.str("actor_id") == actor
        && row.flag("require_finalization", false) == control.flag("require_finalization", false))
      floor = std::max(floor, row.num("required_iteration_floor", 0));
  return floor;
}

// 同一分支的重试护栏与恢复预算需要一起优化；不能把并发兄弟的失败串起来。
Json recovery_components(const Json& selected, const Json& input, const Json& available) {
  if(selected.items().size() != 1 || !available.has("action_rules") || !available.has("loop_rules")
      || (selected.items().front().text() != "action_rules" && selected.items().front().text() != "loop_rules")) return selected;
  for(const auto& feedback : input["feedback"].items()) {
    const auto& runs = feedback["trial"]["evidence"]["runs"]; if(!runs.array()) continue;
    for(const auto& run : runs.items()) {
      if(!run["actions"].array()) continue;
      for(const auto& action : run["actions"].items()) {
        if(!action_control_evidence(action) || action.str("execution_id").empty()
            || (!action.has("terminal_error_code_candidate") && action["action_control"].num("prior_failures", 0) < 2)) continue;
        for(const auto& step : run["actions"].items()) {
          const auto& loop = step["loop_control"];
          if(!loop.object() || loop.flag("pending_actions", false)
              || step.str("execution_id") != action.str("execution_id") || step.str("actor_id") != action.str("actor_id")
              || loop.str("target_scope") != action["action_control"].str("target_scope")) continue;
          if(loop.str("proposed_decision") == "replan" || loop.num("repeated_failure_steps", 0))
            return Json::Array{"action_rules", "loop_rules"};
        }
      }
    }
  }
  return selected;
}

Json control_feedback_trial(const dataset::v1::EvaluationTrial& trial, const Json& selected, const Json& controls) {
  auto full = feedback_trial(trial, controls);
  bind_iteration_assessments(full, json(trial.evidence()));
  auto chosen = [&](const char* component) {
    return std::any_of(selected.items().begin(), selected.items().end(),
        [&](const auto& item) { return item.text() == component; });
  };
  const bool actions = chosen("action_rules"), loops = chosen("loop_rules"), schedules = chosen("schedule_rules");
  Json checks = Json::Array{};
  if(!full["checks"].null()) for(const auto& check : full["checks"].items()) {
    if(checks.items().size() == 8) break;
    Json item = Json::Object{};
    for(const auto* field : {"check_id", "status", "attempts"}) if(check.has(field)) item[field] = check[field];
    if(check.has("reason")) item["reason"] = clip(check.str("reason"), 512);
    checks.push(item);
  }
  Json result{{"verdict", full["verdict"]}, {"checks", checks}, {"metrics", full["metrics"]},
      {"execution_status", full["execution_status"]}, {"evidence_truncated", true},
      {"control_evidence_preserved", true}};
  if(!trial.error_code().empty()) result["error_code"] = trial.error_code();
  auto& evidence = result["evidence"];
  if(loops && full["evidence"].has("loop_review"))
    evidence["loop_review"] = compact_excerpt(full["evidence"]["loop_review"]);
  evidence["runs"] = Json::Array{};
  // 评测异常可能没有检查项或轨迹；保留异常，不伪造可执行控制证据。
  if(full["evidence"]["runs"].null()) {
    result["control_evidence_preserved"] = false;
    return result;
  }
  for(const auto& run : full["evidence"]["runs"].items()) {
    Json view{{"run_id", run["run_id"]}, {"actions", Json::Array{}}};
    if(schedules && run["schedule_plans"].array()) {
      view["schedule_plans"] = Json::Array{};
      std::vector<std::pair<std::size_t, Json>> preferred, other, kept;
      std::size_t ordinal = 0;
      for(const auto& plan : run["schedule_plans"].items()) {
        std::set<std::string> instances;
        for(const auto& action : plan["actions"].items())
          if(!action.str("operation").empty() && !action.str("action_id").empty()) instances.insert(action.str("action_id"));
        auto compact = compact_excerpt(plan);
        // 实例 ID 和宿主依赖本身已是不含内容的视图；裁剪该数组可能隐藏唯一的
        // 生产者/消费者配对。
        compact["actions"] = plan["actions"];
        (instances.size() > 1 ? preferred : other).emplace_back(ordinal++, std::move(compact));
      }
      for(const auto* rows : {&preferred, &other}) for(const auto& plan : *rows) {
        if(kept.size() == 8) break;
        kept.push_back(plan);
      }
      std::sort(kept.begin(), kept.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
      for(const auto& plan : kept) view["schedule_plans"].push(plan.second);
    }
    std::vector<std::pair<std::size_t, Json>> dispatches, opportunities, primary, secondary;
    std::size_t ordinal = 0;
    for(const auto& action : run["actions"].items()) {
      const auto order = ordinal++;
      bool keep = false, important = false, actionable = false;
      if(actions && action.str("kind") == "action.end" && action["action_control"].object()) keep = true;
      if(actions && action_control_evidence(action)) actionable = true;
      if(loops && action.str("kind") == "loop.decision" && action["loop_control"].object()) {
        keep = true;
        const auto& control = action["loop_control"];
        important = (!control["observed_no_progress"].null() && control.num("observed_no_progress", 0))
            || control.num("failed_actions", 0)
            || control.num("repeated_failure_steps", 0) || control.str("proposed_decision", "continue") != "continue";
        const auto reviewed_cap = std::max({controls.num("min_iterations", 2),
            control.num("reviewed_waste_after", 0), control.num("required_iteration_floor", 0)});
        actionable = important || (controls.flag("aggressive_controls", false)
            && control.has("reviewed_waste_after") && control.num("iteration", 0) > reviewed_cap);
      }
      if((loops || schedules) && action.str("kind") == "action.end"
          && action.str("semantic") != "model" && !action.str("operation").empty()) {
        keep = true;
        important |= action.str("status") == "failed" || action.has("error");
      }
      if(!keep) continue;
      Json item = Json::Object{};
      for(const auto* field : {"event_id", "kind", "actor_id", "execution_id", "loop_id", "iteration", "status",
                              "error", "decision", "reason", "outcome", "operation", "executed",
                              "semantic", "guard_only", "input", "output", "action_control", "loop_control",
                              "iteration_assessment", "terminal_error_code_candidate"})
        if(action.has(field)) item[field] = compact_excerpt(action[field]);
      // 选择器和模型都会使用这类小型类型化证据；不能把被省略的条目改写成“参数或
      // 谓词不存在”，否则会改变可执行含义。
      if(action["action_control"].object()) item["action_control"] = action["action_control"];
      if(action["loop_control"].object()) item["loop_control"] = action["loop_control"];
      // 上层重复派发不能被同一子分支的大量重试挤出窗口；总长度上限不变。
      const bool redispatch = actions && actionable && action.flag("guard_only", false)
          && action["action_control"].num("prior_operation_rounds", 0) > 0;
      (redispatch ? dispatches : actionable ? opportunities : important ? primary : secondary)
          .emplace_back(order, std::move(item));
    }
    std::vector<std::pair<std::size_t, Json>> chosen_actions;
    for(const auto* rows : {&dispatches, &opportunities, &primary, &secondary}) for(const auto& action : *rows) {
      if(chosen_actions.size() == 16) break;
      chosen_actions.push_back(action);
    }
    std::sort(chosen_actions.begin(), chosen_actions.end(),
        [](const auto& a, const auto& b) { return a.first < b.first; });
    for(const auto& action : chosen_actions) view["actions"].push(action.second);
    if(view["actions"].items().size() || view["schedule_plans"].array()) evidence["runs"].push(view);
  }
  return result;
}

// 搜索允许未改动的旧规则继续等待适用场景；新规则仍必须有真实应用证据。
// 这里只控制候选留存，winner 和发布门禁仍检查完整策略的覆盖率。
bool control_changes_observed(const Json& coverage, const Json& inherited) {
  for(const auto& rule : coverage["rules"].fields())
    if(!inherited["rules"].has(rule.first) && !rule.second.num("applied", 0)) return false;
  return true;
}

// 增量草案只在搜索入口展开；索引始终指向父数组，未提及的独立规则原样保留。
// 旧 controls 仍表示完整替换，编译、导出和宿主策略协议不增加新字段。
Json expand_control_updates(const Json& draft, const Json& before, const Json& selected) {
  if(!draft.has("control_updates")) return draft;
  require(!draft.has("controls") && draft["control_updates"].array(), "gepa_draft_invalid");
  std::set<std::string> allowed;
  for(const auto& component : selected.items()) allowed.insert(component.text());
  std::map<std::string, std::map<std::size_t, Json>> replacements;
  std::map<std::string, Json::Array> additions;
  std::set<std::string> touched, added;
  for(const auto& change : draft["control_updates"].items()) {
    keys(change, {"component", "op", "index", "value"});
    require(change.object() && change["component"].string() && change["op"].string(), "gepa_draft_invalid");
    const auto component = change.str("component"), operation = change.str("op");
    require((component == "action_rules" || component == "loop_rules" || component == "schedule_rules")
        && allowed.count(component) && before.has(component) && before[component].array(), "gepa_component_not_authorized");
    require(operation == "add" || operation == "set" || operation == "remove", "gepa_draft_invalid");
    require(operation == "remove" ? !change.has("value") : change["value"].object(), "gepa_draft_invalid");
    touched.insert(component);
    if(operation == "add") {
      require(!change.has("index") && added.insert(component + "\n" + change["value"].dump()).second, "gepa_draft_invalid");
      additions[component].push_back(change["value"]);
    } else {
      require(std::holds_alternative<double>(change["index"].value), "gepa_draft_invalid");
      const auto index = number(change["index"]);
      require(std::isfinite(index) && index >= 0 && std::floor(index) == index
          && index < before[component].items().size(), "gepa_draft_invalid");
      require(replacements[component].emplace(static_cast<std::size_t>(index),
          operation == "remove" ? Json() : change["value"]).second, "gepa_draft_invalid");
    }
  }
  auto result = draft;
  result.fields().erase("control_updates");
  for(const auto& component : touched) {
    auto& output = result["controls"][component]; output = Json::Array{};
    const auto& changes = replacements[component];
    const auto& original = before[component].items();
    for(std::size_t index = 0; index < original.size(); ++index) {
      const auto changed = changes.find(index);
      if(changed == changes.end()) output.push(original[index]);
      else if(!changed->second.null()) output.push(changed->second);
    }
    for(const auto& value : additions[component]) output.push(value);
  }
  // 空更新也是明确的弃权，沿用既有空 experiences 的无改动路径。
  if(touched.empty() && !result.has("experience") && !result.has("experiences")) result["experiences"] = Json::Array{};
  return result;
}

// 请求只展示本轮可改的组件；权限仍由原始配置和候选编译校验决定。
void restrict_reflection_components(Json& packet, const Json& selected) {
  std::set<std::string> allowed;
  for(const auto& item : selected.items()) allowed.insert(item.text());
  packet["selected_components"] = selected;
  const bool controls = allowed.count("action_rules") || allowed.count("loop_rules") || allowed.count("schedule_rules");
  if(controls && packet["experiences"].array() && packet["components"].object()) {
    const auto& values = packet["components"].fields();
    for(auto& source : packet["experiences"].items()) {
      const auto key = "experience:" + source.str("memory_id");
      const auto found = values.find(key);
      if(allowed.count(key) || found == values.end()) continue;
      // 控制子请求裁掉经验组件前保留当前正文；历史正文不能复活已停用项。
      if(found->second.null()) source.fields().erase("content");
      else source["content"] = experience_content(found->second);
    }
  }
  if(packet["components"].object()) {
    auto& values = packet["components"].fields();
    for(auto it = values.begin(); it != values.end();)
      if(!allowed.count(it->first)) it = values.erase(it); else ++it;
  }
  for(const auto* category : {"action", "loop", "schedule"}) {
    const std::string prefix(category);
    if(allowed.count(prefix + "_rules")) continue;
    if(packet["controls"].object()) {
      packet["controls"].fields().erase(prefix + "_primitives");
      packet["controls"].fields().erase("base_" + prefix + "_rules");
    }
  }
  if(!controls) {
    packet.fields().erase("controls");
    if(packet["experiences"].array()) {
      auto& sources = packet["experiences"].items();
      sources.erase(std::remove_if(sources.begin(), sources.end(), [&](const auto& item) {
        return !allowed.count("experience:" + item.str("memory_id"));
      }), sources.end());
    }
  }
}

std::string control_binding_id(const Json& input, Json binding) {
  auto packet = input;
  if(input["controls"].object()) {
    packet["controls"].fields().erase("editable_bindings");
    if(input["controls"]["action_primitives"].array())
      for(auto& primitive : packet["controls"]["action_primitives"].items())
        if(primitive.object()) primitive.fields().erase("observed_bindings");
  }
  binding.fields().erase("binding_id");
  return "control:" + hash(hash(packet.dump()) + '\0' + binding.dump()).substr(0, 24);
}

// 数值范围来自当前观察与父规则交集；仅说明预算，不替代来源或组件授权。
Json control_binding_budget(const Json& prepared, const Json& binding) {
  const bool action = binding.str("component") == "action_rules" && binding.str("primitive_id") == "operation_budget";
  const bool loop = binding.str("component") == "loop_rules" && binding.str("primitive_id") == "bounded_iteration_cap";
  if(!action && !loop) return Json();
  const auto field = action ? "max_attempts" : "max_iterations";
  const auto minimum = action ? required_attempt_floor(prepared, binding) : std::max({prepared["controls"]["limits"].num("min_iterations", 2),
      binding.num("reviewed_waste_after", 0), binding.num("required_iteration_floor", 0)});
  const auto observed = action ? binding.num("prior_operation_rounds", 0) : binding.num("observed_iteration", 0);
  auto maximum = std::min<std::size_t>(action ? 16 : 10000, action ? observed : observed ? observed - 1 : 0);
  std::size_t matched = 0, existing_maximum = 0;
  const Json* current = nullptr;
  const auto& rules = prepared["components"][binding.str("component")];
  if(rules.array()) for(const auto& rule : rules.items()) {
    if((action && (rule.str("operation") != binding.str("operation") || !rule.num("max_attempts", 0)))
        || (loop && rule.str("actor_id") != binding.str("actor_id"))) continue;
    std::size_t sources = 0; std::string scope;
    for(const auto* values : {&prepared["experiences"], &prepared["controls"]["control_sources"]})
      if(values->array()) for(const auto& source : values->items())
        if(source.str("memory_id") == rule.str("memory_id")) { ++sources; scope = source.str("scope"); }
    if(sources == 1 && scope == binding.str("target_scope")) {
      ++matched; existing_maximum = rule.num(field, 0); current = &rule;
    }
  }
  if(matched == 1) maximum = std::min(maximum, existing_maximum);
  Json result{{"field", field}, {"min", minimum}, {"max", maximum}};
  if(loop) {
    // current 是父策略现值，min/max 是允许的新值；未知条件不伪装成无条件预算。
    if(!matched) {
      std::size_t exact = 0, generic = 0;
      if(prepared["controls"]["base_loop_rules"].array())
        for(const auto& rule : prepared["controls"]["base_loop_rules"].items()) {
          if(rule.str("target_scope") != binding.str("target_scope")
              || (!rule.str("actor_id").empty() && rule.str("actor_id") != binding.str("actor_id"))) continue;
          if(rule.str("actor_id").empty()) { ++generic; if(!exact) current = &rule; }
          else { ++exact; current = &rule; }
        }
      if(exact != 1 && (exact || generic != 1)) current = nullptr;
      // 通用生成规则可能覆盖通用基础规则，但 resolver 不从该来源继承；不猜其生效条件。
      if(current && current->str("actor_id").empty() && rules.array()) for(const auto& rule : rules.items()) {
        if(!rule.str("actor_id").empty()) continue;
        for(const auto* sources : {&prepared["experiences"], &prepared["controls"]["control_sources"]})
          if(sources->array()) for(const auto& source : sources->items())
            if(source.str("memory_id") == rule.str("memory_id") && source.str("scope") == binding.str("target_scope"))
              current = nullptr;
      }
    }
    if(current && matched <= 1 && current->num(field, 0)
        && !current->flag("after_action_rejection", false) && !current->flag("require_finalization", false)
        && (!(*current)["error_codes"].array() || (*current)["error_codes"].items().empty())
        && (!current->has("memory_id") || current->str("match_mode") == "predicate"))
      result["current"] = (*current)[field];
  }
  return result;
}

// 将当前训练包中的真实控制机会投影为 request-local typed binding；不增加原语或运行时授权。
Json observed_operation_bindings(const Json& input, std::size_t byte_limit) {
  if(!input["controls"].object()) return input;
  auto packet = input;
  packet["controls"].fields().erase("editable_bindings");
  Json* budget = nullptr;
  if(input["controls"]["action_primitives"].array())
    for(auto& primitive : packet["controls"]["action_primitives"].items()) {
      if(!primitive.object()) continue;
      primitive.fields().erase("observed_bindings");
      if(primitive.str("primitive_id") == "operation_budget" && !budget) budget = &primitive;
    }
  const auto selected = [&](const char* component) {
    return input["selected_components"].array()
        && std::any_of(input["selected_components"].items().begin(), input["selected_components"].items().end(),
            [&](const auto& item) { return item.string() && item.text() == component; });
  };
  bool iteration_cap = false;
  if(input["controls"]["loop_primitives"].array())
    for(const auto& primitive : input["controls"]["loop_primitives"].items())
      iteration_cap |= primitive.object() && primitive.str("primitive_id") == "bounded_iteration_cap";

  std::vector<Json> action_candidates, legacy_action_candidates, loop_candidates;
  if(budget && selected("action_rules") && input["feedback"].array()) {
    std::map<std::pair<std::string, std::string>, Json> observed, reviewed;
    for(std::size_t row = 0; row < input["feedback"].items().size(); ++row) {
      const auto& runs = input["feedback"].items()[row]["trial"]["evidence"]["runs"];
      if(!runs.array()) continue;
      for(std::size_t run = 0; run < runs.items().size(); ++run) {
        const auto& actions = runs.items()[run]["actions"];
        if(!actions.array()) continue;
        for(std::size_t index = 0; index < actions.items().size(); ++index) {
          const auto& action = actions.items()[index]; const auto& control = action["action_control"];
          if(action.str("kind") != "action.end" || action.str("semantic") == "model"
              || !std::holds_alternative<bool>(action["guard_only"].value) || !action["guard_only"].boolean()
              || !std::holds_alternative<bool>(action["executed"].value) || action["executed"].boolean()
              || !control.object() || control.str("decision") != "allow" || control.str("query_event_id").empty()
              || control.str("target_scope").empty() || action.str("operation").empty()
              || !control.num("prior_operation_rounds", 0)) continue;
          const auto key = std::make_pair(control.str("target_scope"), action.str("operation"));
          const auto rounds = control.num("prior_operation_rounds", 0);
          const Json binding{{"operation", key.second}, {"target_scope", key.first}, {"prior_operation_rounds", rounds},
              {"evidence_ref", "/feedback/" + std::to_string(row) + "/trial/evidence/runs/" + std::to_string(run)
                  + "/actions/" + std::to_string(index)}};
          if(control.has("reviewed_budget_prior") && control.num("reviewed_budget_prior", 0) == rounds
              && rounds >= required_attempt_floor(input, binding)
              && (!reviewed.count(key) || reviewed[key].num("prior_operation_rounds", 0) > rounds)) {
            reviewed[key] = binding;
            reviewed[key]["reviewed_redundant_suffix"] = true;
          }
          const auto previous = observed.find(key);
          if(previous != observed.end() && previous->second.num("prior_operation_rounds", 0) >= rounds) continue;
          // 保留某个真实观察的最大值和原位置，不把不同 Run 的轮数相加。
          observed[key] = binding;
        }
      }
    }
    for(const auto& entry : observed) {
      legacy_action_candidates.push_back(entry.second);
      action_candidates.push_back(reviewed.count(entry.first) ? reviewed[entry.first] : entry.second);
    }
    std::stable_sort(action_candidates.begin(), action_candidates.end(), [](const auto& a, const auto& b) {
      if(a.flag("reviewed_redundant_suffix", false) != b.flag("reviewed_redundant_suffix", false))
        return a.flag("reviewed_redundant_suffix", false);
      return a.num("prior_operation_rounds", 0) > b.num("prior_operation_rounds", 0);
    });
    std::stable_sort(legacy_action_candidates.begin(), legacy_action_candidates.end(), [](const auto& a, const auto& b) {
      return a.num("prior_operation_rounds", 0) > b.num("prior_operation_rounds", 0);
    });
  }

  if(iteration_cap && selected("loop_rules") && input["controls"].flag("aggressive_controls", false)
      && input["feedback"].array()) {
    std::map<std::pair<std::string, std::string>, std::size_t> highest_keep;
    for(const auto& feedback : input["feedback"].items()) {
      const auto& runs = feedback["trial"]["evidence"]["runs"];
      if(!runs.array()) continue;
      for(const auto& run : runs.items()) {
        if(!run["actions"].array()) continue;
        for(const auto& action : run["actions"].items()) {
          const auto& control = action["loop_control"];
          if(action.str("kind") != "loop.decision" || !control.object()
              || control.str("target_scope").empty() || action.str("actor_id").empty()
              || action["iteration_assessment"].str("decision") != "keep") continue;
          auto& value = highest_keep[{control.str("target_scope"), action.str("actor_id")}];
          value = std::max(value, control.num("iteration", 0));
        }
      }
    }
    std::map<std::pair<std::string, std::string>, Json> observed;
    const auto minimum = input["controls"]["limits"].num("min_iterations", 2);
    for(std::size_t row = 0; row < input["feedback"].items().size(); ++row) {
      const auto& runs = input["feedback"].items()[row]["trial"]["evidence"]["runs"];
      if(!runs.array()) continue;
      for(std::size_t run = 0; run < runs.items().size(); ++run) {
        const auto& actions = runs.items()[run]["actions"];
        if(!actions.array()) continue;
        for(std::size_t index = 0; index < actions.items().size(); ++index) {
          const auto& action = actions.items()[index]; const auto& control = action["loop_control"];
          const auto proposed = control.str("proposed_decision", "continue");
          if(action.str("kind") != "loop.decision" || !control.object() || control.flag("pending_actions", false)
              || (proposed != "continue" && proposed != "replan") || !control.has("reviewed_waste_after")
              || control.str("target_scope").empty() || action.str("actor_id").empty()) continue;
          const auto iteration = control.num("iteration", 0);
          const auto waste_after = control.num("reviewed_waste_after", 0);
          const auto key = std::make_pair(control.str("target_scope"), action.str("actor_id"));
          const auto floor = required_iteration_floor(input, control, action.str("actor_id"), highest_keep[key]);
          if(iteration <= std::max({minimum, waste_after, floor})) continue;
          const auto previous = observed.find(key);
          if(previous != observed.end() && previous->second.num("observed_iteration", 0) >= iteration) continue;
          observed[key] = Json{{"actor_id", key.second}, {"target_scope", key.first},
              {"observed_iteration", iteration}, {"reviewed_waste_after", waste_after},
              {"required_iteration_floor", floor},
              {"highest_reviewed_keep_iteration", highest_keep[key]},
              {"evidence_ref", "/feedback/" + std::to_string(row) + "/trial/evidence/runs/" + std::to_string(run)
                  + "/actions/" + std::to_string(index)}};
          if(control.flag("require_finalization", false)) observed[key]["require_finalization"] = true;
        }
      }
    }
    for(const auto& entry : observed) loop_candidates.push_back(entry.second);
    std::stable_sort(loop_candidates.begin(), loop_candidates.end(), [](const auto& a, const auto& b) {
      return a.num("observed_iteration", 0) > b.num("observed_iteration", 0);
    });
  }

  auto& editable = packet["controls"]["editable_bindings"]; editable = Json::Array{};
  const auto add_editable = [&](Json binding, const char* component, const char* primitive) {
    binding["component"] = component; binding["primitive_id"] = primitive;
    binding["budget"] = control_binding_budget(packet, binding);
    if(binding["budget"].num("min", 0) > binding["budget"].num("max", 0)) return;
    binding["binding_id"] = control_binding_id(packet, binding);
    editable.push(std::move(binding));
    if(packet.dump().size() > byte_limit) editable.items().pop_back();
  };
  for(std::size_t index = 0; editable.items().size() < 6
      && (index < action_candidates.size() || index < loop_candidates.size()); ++index) {
    if(index < action_candidates.size())
      add_editable(action_candidates[index], "action_rules", "operation_budget");
    if(editable.items().size() < 6 && index < loop_candidates.size())
      add_editable(loop_candidates[index], "loop_rules", "bounded_iteration_cap");
  }
  if(editable.items().empty()) packet["controls"].fields().erase("editable_bindings");

  // 保留旧提示视图供 concrete 草稿兼容；typed binding 优先占用剩余预算。
  if(budget && !legacy_action_candidates.empty()) {
    auto& bindings = (*budget)["observed_bindings"]; bindings = Json::Array{};
    for(const auto& binding : legacy_action_candidates) {
      bindings.push(binding);
      if(packet.dump().size() > byte_limit) bindings.items().pop_back();
      if(bindings.items().size() == 4) break;
    }
    if(bindings.items().empty()) budget->fields().erase("observed_bindings");
  }
  return packet;
}

Json resolve_control_binding_updates(const Json& draft, const Json& prepared, Json* diagnostic = nullptr) {
  if(diagnostic) *diagnostic = Json();
  if(!draft.has("control_binding_updates")) return draft;
  std::size_t update_index = 0;
  Json budget;
  const auto check = [&](bool valid, const char* reason) {
    if(valid) return;
    if(diagnostic) {
      *diagnostic = Json{{"reason", reason}, {"update_index", update_index}};
      if(budget.object()) (*diagnostic)["budget"] = budget;
    }
    throw Error("control_binding_invalid");
  };
  check(!draft.has("control_updates") && !draft.has("controls"), "mixed_protocol");
  check(draft["control_binding_updates"].array(), "updates_not_array");
  const auto& bindings = prepared["controls"]["editable_bindings"];
  check(bindings.array(), "bindings_unavailable");
  Json result = draft; result.fields().erase("control_binding_updates");
  result["control_updates"] = Json::Array{};
  std::set<std::string> used;
  const auto scope = [&](const std::string& memory_id) {
    std::string value; std::size_t matches = 0;
    if(prepared["experiences"].array()) for(const auto& source : prepared["experiences"].items())
      if(source.str("memory_id") == memory_id) { value = source.str("scope"); ++matches; }
    if(prepared["controls"]["control_sources"].array())
      for(const auto& source : prepared["controls"]["control_sources"].items())
        if(source.str("memory_id") == memory_id) { value = source.str("scope"); ++matches; }
    check(matches == 1 && !value.empty(), "source_missing_or_ambiguous"); return value;
  };
  const auto whole = [&](const Json& value, const char* field) {
    check(std::holds_alternative<double>(value[field].value), "budget_not_integer");
    const auto numeric = std::get<double>(value[field].value);
    check(std::isfinite(numeric) && std::floor(numeric) == numeric, "budget_not_integer");
    check(numeric >= 0 && numeric <= 10000, "budget_out_of_range");
    return static_cast<std::size_t>(numeric);
  };
  for(const auto& update : draft["control_binding_updates"].items()) {
    budget = Json();
    check(update.object(), "update_not_object");
    const std::set<std::string> allowed{"binding_id", "memory_id", "max_attempts", "max_iterations", "reason"};
    for(const auto& field : update.fields()) check(allowed.count(field.first), "unknown_update_field");
    check(update["binding_id"].string() && update["memory_id"].string() && update["reason"].string()
        && !update.str("binding_id").empty() && !update.str("memory_id").empty()
        && !update.str("reason").empty(), "required_field_invalid");
    check(used.insert(update.str("binding_id")).second, "duplicate_binding_update");
    const Json* binding = nullptr;
    for(const auto& candidate : bindings.items()) if(candidate.str("binding_id") == update.str("binding_id")) {
      check(!binding, "binding_ambiguous"); binding = &candidate;
    }
    check(binding != nullptr, "unknown_binding_id");
    check(control_binding_id(prepared, *binding) == binding->str("binding_id"), "stale_binding_id");
    check(scope(update.str("memory_id")) == binding->str("target_scope"), "source_scope_mismatch");
    const auto component = binding->str("component"), primitive = binding->str("primitive_id");
    check((component == "action_rules" || component == "loop_rules")
        && prepared["components"][component].array(), "component_unavailable");
    budget = control_binding_budget(prepared, *binding);
    Json value, change{{"component", component}};
    std::size_t matched = 0, matched_index = 0;
    if(component == "action_rules" && primitive == "operation_budget") {
      check(update.has("max_attempts") && !update.has("max_iterations"), "budget_field_mismatch");
      check(!binding->str("operation").empty(), "binding_target_missing");
      const auto attempts = whole(update, "max_attempts");
      check(attempts >= budget.num("min", 1) && attempts <= budget.num("max", 0), "budget_out_of_range");
      for(std::size_t index = 0; index < prepared["components"][component].items().size(); ++index) {
        const auto& rule = prepared["components"][component].items()[index];
        if(rule.str("operation") == binding->str("operation") && rule.num("max_attempts", 0)
            && scope(rule.str("memory_id")) == binding->str("target_scope")) {
          ++matched; matched_index = index;
        }
      }
      check(matched != 1 || attempts <= prepared["components"][component].items()[matched_index].num("max_attempts", 0),
          "budget_out_of_range");
      value = matched == 1 ? prepared["components"][component].items()[matched_index] : Json::Object{};
      value["memory_id"] = update.str("memory_id"); value["primitive_id"] = primitive;
      value["operation"] = binding->str("operation"); value["max_attempts"] = attempts;
      if(!value["attempt_key_arguments"].array()) value["attempt_key_arguments"] = Json::Array{};
      value["reason"] = update.str("reason");
    } else if(component == "loop_rules" && primitive == "bounded_iteration_cap") {
      check(update.has("max_iterations") && !update.has("max_attempts"), "budget_field_mismatch");
      check(!binding->str("actor_id").empty() && binding->has("reviewed_waste_after"), "binding_target_missing");
      const auto iterations = whole(update, "max_iterations");
      check(iterations >= budget.num("min", 0) && iterations <= budget.num("max", 0)
          && iterations < binding->num("observed_iteration", 0), "budget_out_of_range");
      for(std::size_t index = 0; index < prepared["components"][component].items().size(); ++index) {
        const auto& rule = prepared["components"][component].items()[index];
        if(rule.str("actor_id") == binding->str("actor_id")
            && scope(rule.str("memory_id")) == binding->str("target_scope")) {
          ++matched; matched_index = index;
        }
      }
      const Json* base = nullptr;
      std::size_t exact = 0, generic = 0;
      if(prepared["controls"]["base_loop_rules"].array())
        for(const auto& rule : prepared["controls"]["base_loop_rules"].items()) {
          if(rule.str("target_scope") != binding->str("target_scope")
              || (!rule.str("actor_id").empty() && rule.str("actor_id") != binding->str("actor_id"))) continue;
          if(rule.str("actor_id").empty()) {
            ++generic;
            if(!exact) base = &rule;
          } else {
            ++exact; base = &rule;
          }
        }
      check(exact == 1 || (!exact && generic == 1), "base_loop_missing_or_ambiguous");
      check(matched != 1 || iterations <= prepared["components"][component].items()[matched_index].num("max_iterations", 0),
          "budget_out_of_range");
      value = matched == 1 ? prepared["components"][component].items()[matched_index] : Json::Object{};
      value["memory_id"] = update.str("memory_id"); value["actor_id"] = binding->str("actor_id");
      value["max_iterations"] = iterations; value["reason"] = update.str("reason");
      if(matched == 0) {
        value["max_no_progress"] = base->num("max_no_progress", 0);
        value["max_replans"] = base->num("max_replans", 0);
        value["replan_on_failure"] = base->flag("replan_on_failure", false);
        for(const auto* field : {"error_codes", "after_action_rejection", "require_finalization"})
          if(base->has(field)) value[field] = (*base)[field];
      }
      if(binding->flag("require_finalization", false)) value["require_finalization"] = true;
    } else check(false, "unsupported_primitive");
    check(matched <= 1, "existing_rules_ambiguous");
    change["op"] = matched == 1 ? "set" : "add";
    if(matched == 1) change["index"] = matched_index;
    change["value"] = std::move(value); result["control_updates"].push(std::move(change));
    ++update_index;
  }
  return result;
}

// 来源只表示已有批准关联，不把训练评审的新假设冒记为记忆正文中的结论。
Json control_probe_source(const Json& parent, const Json& prepared, const Json& binding, const Report& report) {
  const auto policy = proto<v1::RuntimePolicy>(parent["policy"]);
  std::map<std::string, std::string> approved;
  for(const auto& source : policy.snapshot().experiences()) {
    if(source.target_scope() != binding.str("target_scope")) continue;
    for(const auto& ref : policy.options().approved_experiences())
      if(ref.memory_id() == source.memory_id() && ref.content_revision() == source.content_revision())
        approved[source.memory_id()] = source.content_revision();
  }
  const auto eligible = [&](const std::string& id) {
    std::size_t matches = 0;
    for(const auto* sources : {&prepared["experiences"], &prepared["controls"]["control_sources"]})
      if(sources->array()) for(const auto& source : sources->items())
        matches += source.str("memory_id") == id && source.str("scope") == binding.str("target_scope");
    return approved.count(id) && matches == 1;
  };
  const auto& rules = prepared["components"][binding.str("component")];
  if(rules.array()) for(const auto& rule : rules.items()) {
    const auto field = binding.str("component") == "action_rules" ? "operation" : "actor_id";
    if(!binding.str(field).empty() && rule.str(field) == binding.str(field) && eligible(rule.str("memory_id")))
      return Json{{"memory_id", rule["memory_id"]}, {"association", "existing_control"}};
  }
  if(!prepared["feedback"].array()) return Json();
  for(std::size_t row = 0; row < prepared["feedback"].items().size(); ++row) {
    const auto& feedback = prepared["feedback"].items()[row];
    const auto& runs = feedback["trial"]["evidence"]["runs"];
    if(!runs.array()) continue;
    for(std::size_t run_index = 0; run_index < runs.items().size(); ++run_index) {
      const auto& run = runs.items()[run_index]; const auto& actions = run["actions"];
      if(!actions.array()) continue;
      for(std::size_t action_index = 0; action_index < actions.items().size(); ++action_index) {
        if(binding.str("evidence_ref") != "/feedback/" + std::to_string(row) + "/trial/evidence/runs/"
            + std::to_string(run_index) + "/actions/" + std::to_string(action_index)) continue;
        const auto& anchor = actions.items()[action_index];
        const auto iteration = iteration_identity(run, anchor);
        if(iteration.empty() || anchor.str("actor_id").empty() || anchor.str("event_id").empty()) return Json();
        for(const auto& trial : report.trials()) {
          if(trial.variant_id() != "candidate" || trial.task_id() != feedback.str("task_id")
              || trial.execution_status() != "completed") continue;
          const auto evidence = json(trial.evidence());
          const auto traces = evidence["traces"].array() ? evidence["traces"] : Json(Json::Array{evidence["trace"]});
          for(const auto& trace : traces.items()) {
            if(trace["identity"].str("run_id") != run.str("run_id") || !trace["events"].array()) continue;
            const auto same_round = [&](const Json& event) {
              return event.str("actor_id") == anchor.str("actor_id") && iteration_identity(run, event) == iteration;
            };
            bool found_anchor = false;
            std::map<std::string, Json> models, queries;
            for(const auto& event : trace["events"].items()) {
              if(!same_round(event)) continue;
              found_anchor |= event.str("event_id") == anchor.str("event_id") && event.str("kind") == anchor.str("kind");
              if(!event.str("step_id").empty() && event.str("kind") == "action.end" && event["attributes"].str("semantic") == "model"
                  && event["attributes"].flag("executed", false)) models[event.str("step_id")] = event;
              const auto& query = event["attributes"]["decision"]["query"];
              if(!event.str("step_id").empty() && !query["advice_query"].str("query_id").empty()
                  && event["attributes"].str("type") == "policy.query" && query.str("boundary") == "llm"
                  && query["advice_query"].str("target_scope") == binding.str("target_scope"))
                queries[event.str("step_id")] = query;
            }
            if(!found_anchor) continue;
            for(const auto& event : trace["events"].items()) {
              const auto step = event.str("step_id");
              if(!same_round(event) || event.str("kind") != "advice.applied"
                  || event["application"].str("status") != "applied" || !models.count(step) || !queries.count(step)
                  || event["application"].str("query_id") != queries.at(step)["advice_query"].str("query_id")) continue;
              const auto& receipt = event["attributes"]["receipt"];
              if(receipt.str("boundary") != "llm" || !receipt["experiences"].array()) continue;
              for(const auto& source : receipt["experiences"].items())
                if(eligible(source.str("memory_id")) && source.str("content_revision") == approved.at(source.str("memory_id")))
                  return Json{{"memory_id", source["memory_id"]}, {"association", "same_iteration_application"},
                      {"application_event_id", event["event_id"]}, {"model_event_id", models.at(step)["event_id"]}};
            }
          }
        }
      }
    }
  }
  return Json();
}

// 只保留固定错误与有界数字码；响应正文、模型标识和自由文本不能进入诊断树。
Json reflection_call_diagnostic(const legacy::RawModelResponse& response, std::size_t duration_ms) {
  static const std::set<std::string> fixed{"", "missing_record", "request_too_large", "response_too_large",
      "client_initialization_failed", "header_allocation_failed", "output_token_limit_reached",
      "invalid_api_response", "transport_exhausted"};
  const auto numeric_code = [&](const std::string& prefix, unsigned low, unsigned high) {
    const auto& code = response.error_code;
    if(code.compare(0, prefix.size(), prefix) != 0 || code.size() <= prefix.size()
        || code.size() > prefix.size() + 3 || code[prefix.size()] == '0') return false;
    unsigned value = 0;
    for(std::size_t i = prefix.size(); i < code.size(); ++i) {
      if(code[i] < '0' || code[i] > '9') return false;
      value = value * 10 + static_cast<unsigned>(code[i] - '0');
    }
    return value >= low && value <= high;
  };
  const bool known = fixed.count(response.error_code) || numeric_code("http_", 100, 599)
      || numeric_code("transport_", 1, 999);
  return Json{{"transport_ok", response.transport_ok},
      {"error_code", known ? response.error_code : "unknown_adapter_error"},
      {"attempts", std::size_t(response.attempts)}, {"duration_ms", duration_ms},
      {"decoded_body_bytes", response.body.size()}};
}

struct Search {
  fs::path dataset_path, evaluation_path, root;
  Json seed, baseline, evaluation, config, gate, controls, tree;
  std::string context, evaluation_context, candidate_evaluation_context;
  std::function<bool()> cancelled;
  std::shared_ptr<legacy::ModelAdapter> model;
  dataset::v1::EvaluationDataset dataset;
  std::vector<std::string> train, val, holdout;
  std::size_t reserve = 0;
  std::size_t final_holdout_top_k_override = 0;
  bool final_holdout_replay = false;
  std::size_t final_holdout_reserve = 0;

  void save() { const auto bytes = tree.dump(); require(bytes.size() <= 16777216, "gepa_tree_limit"); write(root / "tree.json", bytes); }
  void check() const {
    require(!cancelled || !cancelled(), "gepa_cancelled");
    require(now() < deadline(), "gepa_time_budget_exhausted");
  }
  std::size_t deadline() const {
    if(final_holdout_replay && tree["final_holdout"].num("replay_deadline_ms", 0))
      return tree["final_holdout"].num("replay_deadline_ms", 0);
    return tree.num("started_ms", 0) + config.num("timeout_ms", 1800000) + tree.num("paused_ms", 0);
  }
  void resume_clock(std::size_t resumed_at) {
    if(tree.str("status") != "cancelled" || tree.str("error") != "gepa_cancelled") return;
    const auto finished = tree.num("finished_ms", 0);
    // 仅扣除明确取消后的停机时间；保留原始时间戳，不能复活已耗尽的执行预算。
    if(!finished || finished < tree.num("started_ms", 0) || finished >= deadline() || resumed_at < finished) return;
    tree["paused_ms"] = tree.num("paused_ms", 0) + resumed_at - finished;
    tree.fields().erase("finished_ms");  // 同一取消检查点只能扣除一次。
  }
  std::size_t holdout_time() const {
    const auto budget = config.num("timeout_ms", 1800000);
    if(config.num("holdout_reserve_ms", 0)) return config.num("holdout_reserve_ms", 0);
    // 评测墙钟时间包含宿主执行、评分和持久化；其观测吞吐已包含评测器并行度，
    // 因而留出预算不能再按并行数重复折算。
    const auto trials = tree.num("timed_trials", 0);
    const auto estimate = trials ? (tree.num("evaluation_wall_ms", 0) * reserve * 3 / (trials * 2)) : 0;
    return std::min(budget - 1, std::max(budget / 5, estimate));
  }
  void search_check(std::size_t trials = 0) const {
    check();
    const auto measured = tree.num("timed_trials", 0);
    const auto estimate = measured ? tree.num("evaluation_wall_ms", 0) * trials / measured : 0;
    require(now() + estimate < deadline() - holdout_time(), "gepa_search_time_reserved");
  }
  Json artifact(std::size_t index) const {
    const auto value = Json::parse(read(root / "versions" / (std::to_string(index) + ".json")));
    require(agent_version_id(value.dump()) == tree["nodes"].items().at(index).str("version"), "gepa_version_changed"); return value;
  }
  HoldoutBaselineCache holdout_baselines() const {
    Json versions = Json::Object{};
    for(const auto& task : dataset.tasks()) if(task.split() == "holdout") versions[task.task_id()] = task.task_version();
    const Json identity{{"schema_version", 1}, {"evaluation_context", evaluation_context}, {"split", "holdout"},
        {"baseline_version", agent_version_id(baseline.dump())}, {"dataset_version", dataset.dataset_version()},
        {"task_versions", versions}, {"repetitions", evaluation.num("repetitions", 1)},
        {"loop_review", evaluation["judge"].has("loop_review_prompt")}};
    return {root.parent_path().parent_path(), identity, tree["baseline_cache"].str("key"),
        evaluation.num("max_artifact_bytes", 16777216), read, write, hash};
  }
  Json shared_evaluation_identity(const Json& settings, bool final) const {
    const auto& splits = settings["splits"];
    if(final || candidate_evaluation_context.empty() || !config.flag("cache_evaluations", true)
        || !splits.array() || splits.items().size() != 1
        || (splits.items()[0].text() != "train" && splits.items()[0].text() != "val")) return Json();
    return Json{{"schema_version", 1}, {"execution_context", candidate_evaluation_context},
        {"dataset_version", dataset.dataset_version()}, {"settings_hash", hash(settings.dump())}};
  }
  bool shared_report_complete(const Report& report, const Json& settings) const {
    if(!scored(report) || report.report_kind() != "task_evaluation.v1"
        || report.dataset_version() != dataset.dataset_version() || !settings["task_ids"].array()
        || !settings["variants"].array() || settings["variants"].items().size() != 1
        || scores(report).fields().size() != settings["task_ids"].items().size()) return false;
    const auto& variant = settings["variants"].items()[0];
    const auto repetitions = settings.num("repetitions", 1);
    std::set<std::pair<std::string, std::size_t>> seen;
    for(const auto& trial : report.trials()) {
      if(trial.variant_id() != variant.str("id") || trial.split() != settings["splits"].items()[0].text()
          || !trial.error_code().empty() || (trial.execution_status() != "completed" && trial.execution_status() != "failed")
          || !trial.repetition() || trial.repetition() > repetitions
          || !seen.emplace(trial.task_id(), trial.repetition()).second
          || std::none_of(settings["task_ids"].items().begin(), settings["task_ids"].items().end(),
              [&](const auto& id) { return id.text() == trial.task_id(); })) return false;
      const auto evidence = json(trial.evidence());
      if(evidence.str("loaded_version") != variant.str("version")
          || (settings["judge"].has("loop_review_prompt") && evidence["loop_review"].str("status") != "complete")) return false;
    }
    return seen.size() == settings["task_ids"].items().size() * repetitions;
  }
  fs::path shared_evaluation_report(const Json& settings, bool final) const {
    const auto identity = shared_evaluation_identity(settings, final);
    if(identity.null()) return {};
    // 与后台评测 worker 共用 evaluation.lock；此索引只保存摘要和根目录内的报告引用。
    const auto shared_root = root.parent_path().parent_path();
    try {
      const auto entry = Json::parse(read(shared_root / "candidate-cache" / (hash(identity.dump()) + ".json"), 8192));
      if(entry["identity"].dump() != identity.dump()) return {};
      const fs::path relative(entry.str("report"));
      if(relative.empty() || relative.is_absolute() || relative.filename() != "report.json") return {};
      for(const auto& part : relative) if(part == ".." || part == ".") return {};
      const auto path = shared_root / relative;
      const auto bytes = read(path, 134217728), manifest_bytes = read(path.parent_path() / "manifest.json", 16777216);
      if(hash(bytes) != entry.str("report_hash") || hash(manifest_bytes) != entry.str("manifest_hash")) return {};
      const auto report = proto<Report>(Json::parse(bytes)); const auto manifest = Json::parse(manifest_bytes);
      if(!manifest.flag("complete", false) || manifest.str("run_id") != report.run_id()
          || manifest["files"].str("report.json") != entry.str("report_hash") || !shared_report_complete(report, settings)) return {};
      return path;
    } catch(const std::exception&) { return {}; }  // 过期、清理或损坏的缓存按 miss，不终止搜索。
  }
  void remember_shared_evaluation(const Json& settings, bool final, const fs::path& path, const Report& report) const {
    const auto identity = shared_evaluation_identity(settings, final);
    if(identity.null() || !shared_report_complete(report, settings)) return;
    try {
      const auto bytes = read(path, 134217728), manifest_bytes = read(path.parent_path() / "manifest.json", 16777216);
      const auto manifest = Json::parse(manifest_bytes);
      if(!manifest.flag("complete", false) || manifest.str("run_id") != report.run_id()
          || manifest["files"].str("report.json") != hash(bytes)) return;
      const auto shared_root = root.parent_path().parent_path();
      const auto relative = fs::absolute(path).lexically_normal().lexically_relative(fs::absolute(shared_root).lexically_normal());
      if(relative.empty() || relative.is_absolute()) return;
      for(const auto& part : relative) if(part == ".." || part == ".") return;
      write(shared_root / "candidate-cache" / (hash(identity.dump()) + ".json"), Json{
          {"identity", identity}, {"report", relative.string()}, {"report_hash", hash(bytes)},
          {"manifest_hash", hash(manifest_bytes)}}.dump());
    } catch(const std::exception&) {}  // 可选加速失败不改变评测结果或发布结论。
  }
  Report evaluate(const Json& candidate, const std::vector<std::string>& ids, const std::string& split,
      bool final = false, bool shared_reuse = true, bool final_candidate_only = false,
      const std::string& final_campaign = {}) {
    Json settings = evaluation, selected = Json::Array{};
    for(const auto& id : ids) selected.push(id);
    settings["splits"] = Json::Array{split}; settings["task_ids"] = selected;
    auto host = evaluation["variants"].items().front(), a = host, b = host;
    a["id"] = "baseline"; a["artifact"] = baseline; a["version"] = agent_version_id(baseline.dump());
    b["id"] = "candidate"; b["artifact"] = candidate; b["version"] = agent_version_id(candidate.dump());
    settings["variants"] = final && !final_candidate_only ? Json(Json::Array{a, b}) : Json(Json::Array{b});
    const auto key = hash(context + settings.dump()
        + (final_campaign.empty() ? "" : "\nfinal_holdout_campaign=" + final_campaign));
    auto cache = static_cast<const Json&>(tree)["evaluations"][key];
    if(cache.str("status") == "complete" && (final || config.flag("cache_evaluations", true))) {
      const auto bytes = read(root / "evaluations" / key / ("attempt-" + std::to_string(cache.num("attempt", 0))) / "report.json", 134217728);
      require(hash(bytes) == cache.str("report_hash"), "gepa_report_changed");
      const auto report = proto<Report>(Json::parse(bytes));
      // 较早的 complete 条目可能缺少后来启用的目标指标，不能据此盲目复用。
      if(final || scores(report).fields().size() == ids.size()) {
        remember_shared_evaluation(settings, final, root / "evaluations" / key
            / ("attempt-" + std::to_string(cache.num("attempt", 0))) / "report.json", report);
        return report;
      }
    }
    if(final && !final_candidate_only && split == "holdout"
        && tree["holdout_baseline_cache"].str("status") == "hit") {
      const auto fixed = tree["holdout_baseline_cache"]["source"];
      const auto baseline_cache = holdout_baselines();
      // 搜索期间已锁定的源若损坏，不隐式更换另一份 L0，也不偷偷重跑基线。
      try { baseline_cache.load(fixed); }
      catch(const std::exception&) {
        tree["holdout_baseline_cache"]["status"] = "invalid";
        tree["holdout_baseline_cache"]["reason"] = "locked_source_changed"; save();
        throw Error("gepa_holdout_baseline_changed");
      }
      const auto fresh = evaluate(candidate, ids, split, true, false, true, final_campaign);
      auto candidate_settings = settings; candidate_settings["variants"] = Json::Array{b};
      const auto candidate_key = hash(context + candidate_settings.dump()
          + (final_campaign.empty() ? "" : "\nfinal_holdout_campaign=" + final_campaign));
      const auto& candidate_cache = static_cast<const Json&>(tree)["evaluations"][candidate_key];
      const auto directory = fs::path("evaluations") / key / ("attempt-" + std::to_string(cache.num("attempt", 0) + 1));
      const auto joined = baseline_cache.combine(fixed, fresh, root / candidate_cache.str("report"), root / directory, key,
          std::min<std::size_t>(evaluation.num("max_report_bytes", 134217728), 134217728));
      cache["attempt"] = cache.num("attempt", 0) + 1; cache["status"] = scored(joined) ? "complete" : "inconclusive";
      cache["version"] = b["version"]; cache["baseline_version"] = a["version"]; cache["split"] = split;
      cache["task_ids"] = selected; cache["trials_reserved"] = 0; cache["baseline_cache"] = "hit";
      cache["comparison_timing"] = "historical_baseline"; cache["baseline_source"] = fixed;
      cache["report"] = (directory / "report.json").string();
      cache["report_hash"] = hash(read(root / directory / "report.json", 134217728));
      tree["evaluations"][key] = cache; save(); return joined;
    }
    check();
    if(!final && config.flag("cache_evaluations", true)) {
      std::map<std::string, std::vector<dataset::v1::EvaluationTrial>> reused;
      Json sources = Json::Array{}, source_refs = Json::Array{}; Report joined;
      for(const auto& entry : static_cast<const Json&>(tree)["evaluations"].fields()) {
        const auto& previous = entry.second;
        if((previous.str("status") != "complete" && previous.str("status") != "inconclusive")
            || previous.str("split") != split
            || previous.str("version") != b.str("version") || previous.has("baseline_version")) continue;
        bool overlaps = false;
        for(const auto& id : previous["task_ids"].items())
          overlaps |= std::find(ids.begin(), ids.end(), id.text()) != ids.end() && !reused.count(id.text());
        if(!overlaps) continue;
        const auto source_report = fs::path("evaluations") / entry.first / ("attempt-" + std::to_string(previous.num("attempt", 0))) / "report.json";
        const auto bytes = read(root / source_report, 134217728);
        require(hash(bytes) == previous.str("report_hash"), "gepa_report_changed");
        const auto report = proto<Report>(Json::parse(bytes));
        if(previous.str("status") == "complete") require(scored(report), "gepa_report_changed");
        if(!report.complete()) continue;
        // 精确检查点可仅重评失败判定而无需重跑 Agent；拆成新子集会丢失该检查点。
        if(entry.first == key && std::any_of(report.trials().begin(), report.trials().end(), [](const auto& trial) {
          return trial.execution_status() == "completed"
              && (trial.error_code() == "evaluation_judge_failed"
                  || (trial.verdict() != "passed" && trial.verdict() != "failed"));
        })) { reused.clear(); break; }
        std::map<std::string, std::vector<dataset::v1::EvaluationTrial>> grouped;
        for(const auto& trial : report.trials()) if(trial.variant_id() == "candidate"
            && std::find(ids.begin(), ids.end(), trial.task_id()) != ids.end() && !reused.count(trial.task_id()))
          grouped[trial.task_id()].push_back(trial);
        bool used = false;
        for(auto& item : grouped) {
          std::set<unsigned> repetitions; for(const auto& trial : item.second) repetitions.insert(trial.repetition());
          if(item.second.size() == evaluation.num("repetitions", 1) && repetitions.size() == item.second.size()) {
            // 同组其他 trial 失败不使本任务失效；只复用其完整重复且全部目标指标仍可观测的结果。
            Report task_report; task_report.set_complete(true);
            for(const auto& trial : item.second) *task_report.add_trials() = trial;
            if(scores(task_report).fields().size() != 1) continue;
            reused.emplace(item.first, std::move(item.second)); used = true;
          }
        }
        if(used) {
          sources.push(entry.first);
          // 同 key 的重试必须引用更早 attempt，绝不能引用正在生成的自身报告。
          source_refs.push(Json{{"key", entry.first}, {"report", source_report.string()}, {"report_hash", previous["report_hash"]}});
          if(joined.schema_version() == 0) joined = report;
        }
      }
      if(!reused.empty()) {
        std::vector<std::string> missing; for(const auto& id : ids) if(!reused.count(id)) missing.push_back(id);
        Report fresh; bool complete = true;
        if(!missing.empty()) { fresh = evaluate(candidate, missing, split); complete = fresh.complete(); }
        const auto reused_count = reused.size() * evaluation.num("repetitions", 1);
        for(const auto& trial : fresh.trials()) reused[trial.task_id()].push_back(trial);
        if(!missing.empty()) {
          auto subset = settings; subset["task_ids"] = Json::Array{};
          for(const auto& id : missing) subset["task_ids"].push(id);
          const auto fresh_key = hash(context + subset.dump());
          const auto& added = static_cast<const Json&>(tree)["evaluations"][fresh_key];
          sources.push(fresh_key);
          source_refs.push(Json{{"key", fresh_key}, {"report", added["report"]}, {"report_hash", added["report_hash"]}});
        }
        joined.clear_trials();
        for(const auto& id : ids) for(const auto& trial : reused[id]) *joined.add_trials() = trial;
        complete &= static_cast<std::size_t>(joined.trials_size()) == ids.size() * evaluation.num("repetitions", 1);
        joined.set_report_kind("gepa_cached_evaluation.v1"); joined.set_run_id("gepa-cache-" + key);
        joined.set_config_fingerprint(key); joined.set_complete(complete); joined.clear_comparisons();
        auto configuration = json(joined.configuration()); configuration["task_ids"] = selected;
        *joined.mutable_configuration() = proto<google::protobuf::Struct>(configuration);
        *joined.mutable_summary() = proto<google::protobuf::Struct>(Json{{"trials", std::size_t(joined.trials_size())},
            {"reused_trials", reused_count}, {"source_reports", sources}, {"source_report_refs", source_refs}});
        cache["attempt"] = cache.num("attempt", 0) + 1;
        cache["status"] = scores(joined).fields().size() == ids.size() ? "complete" : "inconclusive";
        cache["version"] = b["version"]; cache["split"] = split; cache["task_ids"] = selected;
        cache["trials_reserved"] = 0; cache["source_reports"] = sources;
        cache["source_report_refs"] = source_refs;
        const auto directory = fs::path("evaluations") / key / ("attempt-" + std::to_string(cache.num("attempt", 0)));
        const auto bytes = json(joined).dump(); write(root / directory / "report.json", bytes);
        write(root / directory / "report.pb", joined.SerializeAsString());
        cache["report_hash"] = hash(bytes); cache["report"] = (directory / "report.json").string();
        tree["evaluations"][key] = cache; save(); return joined;
      }
    }
    TaskEvaluationOptions options; options.config_json = settings.dump();
    if(cache.num("attempt", 0) > 0 && (final || config.flag("cache_evaluations", true))) {
      const auto previous = root / "evaluations" / key / ("attempt-" + std::to_string(cache.num("attempt", 0)));
      // 恢复时校验上下文、清单和产物；保留已完成执行证据，只重试失败判定或未完成 trial。
      if(fs::exists(previous / "manifest.json")) options.resume_report = previous / "report.json";
    }
    fs::path shared_source;
    if(shared_reuse && options.resume_report.empty()) {
      shared_source = shared_evaluation_report(settings, final);
      if(!shared_source.empty()) {
        options.resume_report = shared_source;
        cache["shared_cache"] = "hit";
      }
    }
    auto cost = ids.size() * evaluation.num("repetitions", 1) * (final && !final_candidate_only ? 2 : 1);
    if(!options.resume_report.empty()) {
      const auto previous = proto<Report>(Json::parse(read(options.resume_report, 134217728)));
      std::size_t reusable = 0;
      for(const auto& trial : previous.trials()) {
        if(trial.execution_status() != "completed" && trial.execution_status() != "failed") continue;
        const auto evidence = json(trial.evidence());
        const bool reviewed = !evaluation["judge"].has("loop_review_prompt")
            || evidence["loop_review"].str("status") == "complete";
        const bool judge_error = trial.error_code() == "evaluation_judge_failed"
            && std::any_of(trial.checks().begin(), trial.checks().end(), [](const auto& check) {
              return check.check_id() == "model" && check.status() == "error";
            });
        reusable += judge_error || (reviewed && trial.error_code() != "evaluation_judge_failed"
            && (trial.verdict() == "passed" || trial.verdict() == "failed"));
      }
      cost -= std::min(cost, reusable);
    }
    if(!final) search_check(cost);
    require(tree.num("trials_used", 0) + cost + (final ? final_holdout_reserve : reserve) <= config.num("max_trials", 400), "gepa_trial_budget_exhausted");
    tree["trials_used"] = tree.num("trials_used", 0) + cost;
    cache["attempt"] = cache.num("attempt", 0) + 1; cache["status"] = "running"; cache["trials_reserved"] = cost;
    cache["version"] = b["version"]; cache["split"] = split; cache["task_ids"] = selected;
    if(final && !final_candidate_only) cache["baseline_version"] = a["version"];
    tree["evaluations"][key] = cache;
    save();
    const auto cutoff = final ? deadline() : deadline() - holdout_time();
    options.cancelled = [&] { return (cancelled && cancelled()) || now() >= cutoff; };
    const auto directory = root / "evaluations" / key / ("attempt-" + std::to_string(cache.num("attempt", 0)));
    const auto evaluation_started = now();
    Report report;
    try { report = run_task_evaluation(dataset_path, evaluation_path, directory, options); }
    catch(const std::exception& exc) {
      const std::string code = exc.what();
      const bool stale = code.rfind("evaluation_checkpoint_", 0) == 0 || code.rfind("evaluation_artifact_", 0) == 0
          || code == "evaluation_file_not_regular" || code == "evaluation_file_size_invalid" || code == "evaluation_read_failed";
      cache["status"] = "error";
      if(!shared_source.empty() && stale) cache["shared_cache"] = "invalid";
      tree["evaluations"][key] = cache; save();
      if(!shared_source.empty() && stale) {
        // 完整恢复校验发现已清理/变化的产物时，另开正常 attempt；不把坏缓存变成实验失败。
        const auto identity = shared_evaluation_identity(settings, final);
        try { write(root.parent_path().parent_path() / "candidate-cache" / (hash(identity.dump()) + ".json"), "{}"); }
        catch(const std::exception&) {}
        return evaluate(candidate, ids, split, final, false, final_candidate_only, final_campaign);
      }
      throw;
    }
    cache["status"] = scored(report) && (final || scores(report).fields().size() == ids.size()) ? "complete" : "inconclusive";
    cache["report_hash"] = hash(read(directory / "report.json", 134217728));
    cache["report"] = (fs::path("evaluations") / key / directory.filename() / "report.json").string();
    tree["evaluations"][key] = cache;
    if(report.complete() && cost) {
      tree["evaluation_wall_ms"] = tree.num("evaluation_wall_ms", 0) + now() - evaluation_started;
      tree["timed_trials"] = tree.num("timed_trials", 0) + cost;
    }
    save();
    remember_shared_evaluation(settings, final, directory / "report.json", report);
    if(final && !final_candidate_only && split == "holdout" && config.flag("cache_evaluations", true) && scored(report)) {
      try {
        holdout_baselines().remember(directory / "report.json", report);
        tree["holdout_baseline_cache"] = holdout_baselines().lookup();
        require(tree["holdout_baseline_cache"].str("status") == "hit", "gepa_holdout_baseline_changed");
        save();
      } catch(const std::exception&) {
        tree["holdout_baseline_cache"]["status"] = "invalid";
        tree["holdout_baseline_cache"]["reason"] = "source_validation_failed"; save();
      }
    }
    check();
    if(!final && now() >= cutoff) throw Error("gepa_search_time_reserved");
    for(const auto& trial : report.trials()) for(const auto& check : trial.checks())
      if(check.check_id() == "model" && check.status() == "error" && check.reason() == "evaluation_judge_http_429")
        throw Error("gepa_model_rate_limited");
    // 对暂态判定或 runner 失败最多恢复一次。检查点会保留成功 trial，故孤立 runner
    // 失败只会单独重放，不应扩大为整批重跑。
    if(!cache.flag("automatic_recovery_used", false) && (final || config.flag("cache_evaluations", true))
        && fs::exists(directory / "manifest.json")
        && tree.num("trials_used", 0) + cost + (final ? final_holdout_reserve : reserve) <= config.num("max_trials", 400)) {
      const std::set<std::string> transient{"evaluation_judge_transport_6", "evaluation_judge_transport_7",
          "evaluation_judge_transport_18", "evaluation_judge_transport_28", "evaluation_judge_transport_52",
          "evaluation_judge_transport_55", "evaluation_judge_transport_56", "evaluation_judge_http_408",
          "evaluation_judge_http_500", "evaluation_judge_http_502", "evaluation_judge_http_503",
          "evaluation_judge_http_504", "invalid_json"};
      bool regrade = false;
      for(const auto& trial : report.trials()) {
      const std::set<std::string> runner{"runner_nonzero_exit", "runner_wait_failed", "runner_poll_failed",
          "runner_read_failed", "runner_cleanup_timeout", "runner_timeout"};
        if(trial.execution_status() != "completed") {
          if(runner.count(trial.error_code())) { regrade = true; continue; }
          return report;
        }
        if(trial.error_code().empty()) continue;
        if(trial.error_code() != "evaluation_judge_failed") return report;
        const auto failed = std::find_if(trial.checks().begin(), trial.checks().end(), [](const auto& check) {
          return check.check_id() == "model" && check.status() == "error";
        });
        const auto invalid_loop_review = failed != trial.checks().end()
            && failed->reason().rfind("evaluation_loop_review_invalid:", 0) == 0;
        if(failed == trial.checks().end()
            || (!transient.count(failed->reason()) && !invalid_loop_review)) return report;
        regrade = true;
      }
      if(regrade) {
        // 人工取消/恢复不消耗这一次自动恢复额度。
        cache["automatic_recovery_used"] = true;
        tree["evaluations"][key] = cache; save();
        return evaluate(candidate, ids, split, final, shared_reuse, final_candidate_only, final_campaign);
      }
    }
    return report;
  }
  Json scores(const Report& report) const {
    Json result = Json::Object{};
    if(!scored(report)) return result;
    for(const auto& trial : report.trials()) {
      if(trial.variant_id() != "candidate") continue;
      const auto metrics = json(trial.metrics()), evidence = json(trial.evidence());
      bool passed = trial.verdict() == "passed";
      // 可复用已测量的基线执行，但比较候选质量前必须将当前确定性契约应用到保存的轨迹上。
      for(const auto& task : dataset.tasks()) if(task.task_id() == trial.task_id()) {
        passed &= schedule_contract_check(json(task.attributes()), evidence).str("status") != "failed";
        break;
      }
      const auto loops = evidence.has("loop_review")
          ? (evidence["loop_review"].str("status") == "complete" ? evidence["loop_review"]["count"] : Json())
          : evidence["trajectory"]["summary"]["invalid_loop_lower_bound"];
      const auto& objective = config["objective"];
      const auto lw = number(objective["invalid_loop_weight"], 0.2);
      const auto tw = number(objective["token_weight"], 0.05);
      const auto dw = number(objective["latency_weight"], 0);
      const bool need_loops = lw || gate.has("min_invalid_loop_reduction");
      const bool unknown_tokens = metrics["tokens"].null();
      if((need_loops && loops.null()) || (tw && unknown_tokens
          && config.str("unknown_token_policy", "reject") != "worst_case")) return Json(Json::Object{});
      auto penalty = [](double x, double scale) { return x / (scale + x); };
      // 此有界代价的上确界为 1。显式保守策略据此排序，绝不伪造未测得的 token 数。
      const auto token_cost = unknown_tokens ? 1.0
          : penalty(number(metrics["tokens"]), number(objective["token_scale"], 10000));
      const double fitness = (passed ? 1.0 : 0.0)
          - lw * penalty(number(loops), number(objective["invalid_loop_scale"], 1))
          - tw * token_cost
          - dw * penalty(trial.duration_ms(), number(objective["latency_scale_ms"], 60000));
      auto& row = result[trial.task_id()];
      row["fitness"] = number(row["fitness"]) + fitness;
      row["quality"] = number(row["quality"]) + (passed ? 1 : 0);
      row["duration_ms"] = number(row["duration_ms"]) + trial.duration_ms();
      if(!unknown_tokens) {
        row["tokens"] = number(row["tokens"]) + number(metrics["tokens"]);
        row["token_observations"] = row.num("token_observations", 0) + 1;
      }
      if(need_loops) {
        const auto method = evidence.has("loop_review") ? "model_review" : "rule_lower_bound";
        if(row.has("loop_method") && row.str("loop_method") != method) return Json(Json::Object{});
        row["loop_method"] = method;
        row["invalid_loops"] = number(row["invalid_loops"]) + number(loops);
      }
      if(lw) row["loop_efficiency"] = number(row["loop_efficiency"]) + 1 - penalty(number(loops), number(objective["invalid_loop_scale"], 1));
      if(tw) row["token_efficiency"] = number(row["token_efficiency"]) + 1 - token_cost;
      if(tw && unknown_tokens) row["unknown_token_trials"] = row.num("unknown_token_trials", 0) + 1;
      if(dw) row["latency_efficiency"] = number(row["latency_efficiency"]) + 1 - penalty(trial.duration_ms(), number(objective["latency_scale_ms"], 60000));
      row["repetitions"] = row.num("repetitions", 0) + 1;
    }
    for(auto& item : result.fields()) {
      item.second["fitness"] = number(item.second["fitness"]) / item.second.num("repetitions", 1);
      item.second["quality"] = number(item.second["quality"]) / item.second.num("repetitions", 1);
      for(const auto* field : {"invalid_loops", "loop_efficiency", "token_efficiency", "latency_efficiency", "duration_ms"})
        if(item.second.has(field)) item.second[field] = number(item.second[field]) / item.second.num("repetitions", 1);
      if(item.second.has("tokens"))
        item.second["tokens"] = number(item.second["tokens"]) / item.second.num("token_observations", 1);
    }
    return result;
  }
  static double mean(const Json& values, const char* field) {
    double sum = 0; for(const auto& item : values.fields()) sum += number(item.second[field]);
    return values.fields().empty() ? -1e9 : sum / values.fields().size();
  }
  Json optimization_status(const Json& candidate) const {
    const auto& base = static_cast<const Json&>(tree)["baseline_validation"];
    Json reasons = Json::Array{}, hard = Json::Array{}, criteria = Json::Object{};
    double gap = 0;
    auto reject = [&](bool condition, const char* code, double distance, bool hard_constraint) {
      if(!condition) return;
      reasons.push(code); if(hard_constraint) hard.push(code);
      gap += std::max(0.0, distance);
    };
    bool paired = candidate.object() && base.object() && !candidate.fields().empty()
        && candidate.fields().size() == base.fields().size();
    if(paired) for(const auto& item : base.fields()) paired &= candidate.has(item.first);
    reject(!paired, "validation_pair_incomplete", 1, true);
    const auto quality = paired ? mean(candidate, "quality") : -1;
    const auto base_quality = paired ? mean(base, "quality") : -1;
    const auto minimum_quality = number(gate["min_pass_rate"], 0.8);
    const auto allowed_drop = number(gate["max_pass_rate_drop"], 0);
    criteria["quality"] = quality; criteria["baseline_quality"] = base_quality;
    criteria["minimum_quality"] = minimum_quality; criteria["maximum_quality_drop"] = allowed_drop;
    reject(paired && quality < minimum_quality - 1e-12, "quality_below_threshold",
        (minimum_quality - quality) / std::max(0.1, minimum_quality), true);
    reject(paired && quality + allowed_drop < base_quality - 1e-12, "quality_regression",
        (base_quality - allowed_drop - quality) / std::max(0.1, base_quality), true);
    if(gate.has("max_latency_ratio") && paired) {
      const auto latency = mean(candidate, "duration_ms"), base_latency = mean(base, "duration_ms");
      const auto ratio = number(gate["max_latency_ratio"], 1);
      criteria["duration_ms"] = latency; criteria["baseline_duration_ms"] = base_latency;
      criteria["maximum_latency_ratio"] = ratio;
      reject(base_latency <= 0 || latency < 0 || latency > base_latency * ratio,
          "max_latency_ratio", base_latency > 0 && ratio > 0 ? latency / (base_latency * ratio) - 1 : 1, true);
    }
    if(gate.has("max_token_ratio") && paired) {
      bool observed = true;
      for(const auto& item : base.fields()) observed &= item.second.has("tokens")
          && item.second.num("token_observations", 0) == item.second.num("repetitions", 0)
          && candidate[item.first].has("tokens")
          && candidate[item.first].num("token_observations", 0) == candidate[item.first].num("repetitions", 0);
      const auto tokens = observed ? mean(candidate, "tokens") : -1;
      const auto base_tokens = observed ? mean(base, "tokens") : -1;
      const auto ratio = number(gate["max_token_ratio"], 1);
      criteria["tokens"] = observed ? Json(tokens) : Json();
      criteria["baseline_tokens"] = observed ? Json(base_tokens) : Json();
      criteria["maximum_token_ratio"] = ratio;
      reject(!observed || base_tokens <= 0 || tokens > base_tokens * ratio,
          !observed ? "token_metric_unobserved" : "max_token_ratio",
          observed && base_tokens > 0 && ratio > 0 ? tokens / (base_tokens * ratio) - 1 : 1, true);
    }
    if(gate.has("min_invalid_loop_reduction") && paired) {
      bool observed = true; std::string method;
      for(const auto& item : base.fields()) {
        const auto& value = candidate[item.first];
        observed &= item.second.has("invalid_loops") && value.has("invalid_loops")
            && !item.second["invalid_loops"].null() && !value["invalid_loops"].null()
            && item.second.str("loop_method") == value.str("loop_method");
        if(method.empty()) method = item.second.str("loop_method");
        observed &= item.second.str("loop_method") == method;
      }
      const auto loops = observed ? mean(candidate, "invalid_loops") : -1;
      const auto base_loops = observed ? mean(base, "invalid_loops") : -1;
      const auto reduction = number(gate["min_invalid_loop_reduction"], 0);
      const auto limit = base_loops * (1 - reduction);
      criteria["invalid_loops"] = observed ? Json(loops) : Json();
      criteria["baseline_invalid_loops"] = observed ? Json(base_loops) : Json();
      criteria["minimum_loop_reduction"] = reduction;
      reject(!observed, "loop_metric_unobserved", 1, false);
      reject(observed && reduction > 0 && base_loops <= 0,
          "loop_baseline_has_no_observed_waste", 1, false);
      reject(observed && base_loops > 0 && loops > limit + 1e-12,
          "loop_target_not_met", (loops - limit) / std::max(1.0, base_loops), false);
    }
    std::size_t supported = 0;
    if(paired) for(const auto& item : base.fields()) {
      const auto& value = candidate[item.first]; bool stable = number(value["quality"]) + 1e-12 >= number(item.second["quality"]);
      if(gate.has("max_latency_ratio")) stable &= number(value["duration_ms"])
          <= number(item.second["duration_ms"]) * number(gate["max_latency_ratio"], 1) + 1e-12;
      if(gate.has("max_token_ratio")) stable &= value.has("tokens") && item.second.has("tokens")
          && number(value["tokens"]) <= number(item.second["tokens"]) * number(gate["max_token_ratio"], 1) + 1e-12;
      if(gate.has("min_invalid_loop_reduction")) stable &= value.has("invalid_loops") && item.second.has("invalid_loops")
          && number(value["invalid_loops"]) <= number(item.second["invalid_loops"]) + 1e-12;
      supported += stable;
    }
    const auto robustness = paired ? double(supported) / base.fields().size() : 0;
    criteria["paired_nonregression"] = robustness;
    const bool feasible = paired && hard.items().empty();
    const bool passed = feasible && reasons.items().empty();
    return Json{{"passed", passed}, {"feasible", feasible}, {"score", 1.0 / (1.0 + gap)},
        {"robustness", robustness}, {"distance", gap}, {"reasons", reasons}, {"hard_reasons", hard}, {"criteria", criteria}};
  }
  Json annotate_validation(Json& values) const {
    const auto status = optimization_status(values);
    for(auto& item : values.fields()) {
      item.second["goal_score"] = status["score"];
      item.second["feasible_score"] = status.flag("feasible", false) ? 1 : 0;
      item.second["robustness_score"] = status["robustness"];
    }
    return status;
  }
  bool improves(const Json& child, const Json& parent) const {
    // 发布门禁比较原始循环均值，而非有界效率；更多正确答案不能掩盖配置循环目标的回退。
    if(!gepa::loops_no_worse(child, parent)) return false;
    const auto difference = mean(child, "quality") - mean(parent, "quality");
    return difference > 1e-12 || (difference >= -1e-12
        && mean(child, "fitness") > mean(parent, "fitness") + number(config["min_improvement"], 0.000001));
  }
  std::vector<std::size_t> frontier() const {
    return gepa::pool(gepa::frontier_map(tree["nodes"], val, config));
  }
  std::vector<std::string> minibatch(std::size_t round) const {
    const auto count = std::min(config.num("minibatch_size", 4), train.size());
    const auto batches = (train.size() + count - 1) / count;
    const bool epochs = config.str("minibatch_strategy", "epoch_shuffled") == "epoch_shuffled";
    auto shuffled = train; std::mt19937 random(static_cast<unsigned>(config.num("seed", 42) + (epochs ? round / batches : 0)));
    std::shuffle(shuffled.begin(), shuffled.end(), random);
    std::vector<std::string> selected;
    if(epochs) {
      const auto start = (round % batches) * count;
      for(std::size_t i = start; i < std::min(start + count, shuffled.size()); ++i) selected.push_back(shuffled[i]);
    } else for(std::size_t i = 0; i < count; ++i) selected.push_back(shuffled[(round * count + i) % shuffled.size()]);
    std::sort(selected.begin(), selected.end()); return selected;
  }
  Json components(const Json& candidate) const {
    Json result = Json::Object{};
    // 候选停用经验时仍保留种子策略已授权的搜索轴，才能把“停用”表示为明确变异。
    const auto authorized = source_experiences(seed);
    for(const auto& item : authorized.items()) result["experience:" + item.str("memory_id")] = nullptr;
    const auto experiences = source_experiences(candidate);
    for(const auto& item : experiences.items()) result["experience:" + item.str("memory_id")] = experience_component(item);
    const auto policy = proto<v1::RuntimePolicy>(candidate["policy"]);
    if(controls.flag("enabled", false) && policy.options().requested_level() >= v1::L2) {
      // 通用 schema 规则使用当前请求的工具契约，不依赖预配置的框架或工具白名单。
      result["action_rules"] = Json::Array{};
      for(const auto& rule : policy.options().action_rules()) if(rule.has_source_experience()) {
        Json arguments = Json::Array{}; for(const auto& key : rule.required_arguments()) arguments.push(key);
        Json item{{"memory_id", rule.source_experience().memory_id()}, {"operation", rule.operation()},
            {"required_arguments", arguments}, {"max_failed_attempts", std::size_t(rule.max_failed_attempts())},
            {"reason", rule.reason()}, {"match_mode", rule.match_mode()}};
        if(rule.failure_key_arguments_size()) {
          item["failure_key_arguments"] = Json::Array{};
          for(const auto& key : rule.failure_key_arguments()) item["failure_key_arguments"].push(key);
        }
        if(rule.terminal_error_codes_size()) {
          item["terminal_error_codes"] = Json::Array{}; item["error_scope_arguments"] = Json::Array{};
          for(const auto& code : rule.terminal_error_codes()) item["terminal_error_codes"].push(code);
          for(const auto& key : rule.error_scope_arguments()) item["error_scope_arguments"].push(key);
        }
        if(rule.max_attempts()) {
          item["max_attempts"] = std::size_t(rule.max_attempts()); item["attempt_key_arguments"] = Json::Array{};
          for(const auto& key : rule.attempt_key_arguments()) item["attempt_key_arguments"].push(key);
        }
        result["action_rules"].push(std::move(item));
      }
      if(std::find(policy.options().host_capabilities().begin(), policy.options().host_capabilities().end(), "schedule.dispatch")
          != policy.options().host_capabilities().end()) {
        result["schedule_rules"] = Json::Array{};
        for(const auto& rule : policy.options().schedule_rules()) if(rule.has_source_experience()) {
          Json task_terms = Json::Array{}, exclude_terms = Json::Array{};
          for(const auto& term : rule.task_terms()) task_terms.push(term);
          for(const auto& term : rule.exclude_terms()) exclude_terms.push(term);
          result["schedule_rules"].push(Json{{"memory_id", rule.source_experience().memory_id()},
              {"before_operation", rule.before_operation()}, {"after_operation", rule.after_operation()},
              {"require_success", rule.require_success()}, {"reason", rule.reason()},
              {"task_terms", task_terms}, {"exclude_terms", exclude_terms},
              {"before_argument", rule.before_argument()}, {"after_argument", rule.after_argument()}});
        }
      }
      if(policy.options().requested_level() >= v1::L3 && without_generated_controls(policy.options()).loop_rules_size()) {
        result["loop_rules"] = Json::Array{};
        for(const auto& rule : policy.options().loop_rules()) if(rule.has_source_experience()) {
          result["loop_rules"].push(Json{{"memory_id", rule.source_experience().memory_id()}, {"actor_id", rule.actor_id()}, {"max_iterations", std::size_t(rule.max_iterations())},
              {"max_no_progress", std::size_t(rule.max_no_progress())}, {"max_replans", std::size_t(rule.max_replans())}, {"replan_on_failure", rule.replan_on_failure()},
              {"reason", rule.reason()}, {"match_mode", rule.match_mode()}});
          auto& value = result["loop_rules"].items().back();
          if(rule.after_action_rejection()) value["after_action_rejection"] = true;
          if(rule.require_finalization()) value["require_finalization"] = true;
          if(!rule.error_codes().empty()) {
            value["error_codes"] = Json::Array{};
            for(const auto& code : rule.error_codes()) value["error_codes"].push(code);
          }
        }
      }
    }
    return result;
  }
  Json compose(const Json& parent, const Json& values) const {
    const auto original = components(parent);
    require(values.object() && original.fields().size() == values.fields().size(), "gepa_components_changed");
    for(const auto& item : original.fields()) require(values.has(item.first), "gepa_components_changed");
    validate_local_data(values);
    auto result = parent; const auto policy = proto<v1::RuntimePolicy>(parent["policy"]); auto options = policy.options();
    options.clear_experience_overrides();
    options.clear_approved_experiences();
    options.clear_disabled_injections();
    const auto experiences = source_experiences(seed);
    for(const auto& item : experiences.items()) {
      const auto id = item.str("memory_id");
      const auto& value = values["experience:" + id];
      const auto content = experience_content(value);
      require(value.null() || (!content.empty() && content.size() <= config.num("max_experience_bytes", 2048)), "gepa_experience_limit");
      for(const auto& entry : policy.snapshot().experiences()) if(entry.memory_id() == id) {
        auto* approved = options.add_approved_experiences();
        approved->set_memory_id(id); approved->set_content_revision(entry.content_revision());
        if(value.null()) {
          *options.add_disabled_injections() = *approved;
          continue;
        }
        const auto pattern = value.str("trigger_pattern");
        if(entry.content() != content || !pattern.empty() || value["observed_operations"].array()) {
          auto* rewrite = options.add_experience_overrides(); rewrite->set_content(content);
          *rewrite->mutable_source_experience() = *approved;
          if(pattern != entry.trigger_pattern()) rewrite->set_trigger_pattern(pattern);
          if(value["observed_operations"].array())
            for(const auto& operation : value["observed_operations"].items()) rewrite->add_observed_operations(operation.text());
        }
      }
    }
    // 私有/公共重复经验是一个可搜索组件；消融时保留被遮蔽公共经验的批准状态，
    // 并同时停用两侧注入，避免只消融半个语义组件。
    const auto seed_policy = proto<v1::RuntimePolicy>(seed["policy"]);
    // 控制证据不可变，不是可注入的经验组件；在变异与多父合并中都保留其种子批准。
    for(const auto& source : seed_policy.snapshot().experiences()) {
      if(source.control_target_type().empty()) continue;
      for(const auto& approved : seed_policy.options().approved_experiences())
        if(approved.memory_id() == source.memory_id()
            && approved.content_revision() == source.content_revision())
          *options.add_approved_experiences() = approved;
    }
    for(const auto& alias : canonical_experience_aliases(seed_policy)) {
      const auto key = "experience:" + alias.second;
      if(!values.has(key)) continue;
      for(const auto& approved : seed_policy.options().approved_experiences())
        if(approved.memory_id() == alias.first.first && approved.content_revision() == alias.first.second) {
          *options.add_approved_experiences() = approved;
          if(values[key].null()) *options.add_disabled_injections() = approved;
        }
    }
    std::sort(options.mutable_experience_overrides()->begin(), options.mutable_experience_overrides()->end(),
        [](const auto& a, const auto& b) { return a.source_experience().memory_id() < b.source_experience().memory_id(); });
    if(values.has("action_rules") || values.has("loop_rules") || values.has("schedule_rules")) {
      Json draft{{"action_rules", values.has("action_rules") ? values["action_rules"] : Json(Json::Array{})},
          {"loop_rules", values.has("loop_rules") ? values["loop_rules"] : Json(Json::Array{})},
          {"schedule_rules", values.has("schedule_rules") ? values["schedule_rules"] : Json(Json::Array{})}};
      options = apply_control_draft(policy.snapshot(), without_generated_controls(options), controls, draft);
    }
    result["policy"] = json(compile_runtime_policy(policy.snapshot(), options));
    require(published_policy_compatible(proto<v1::RuntimePolicy>(result["policy"]),
        proto<v1::RuntimePolicy>(seed["policy"]), controls.dump()), "gepa_candidate_not_authorized");
    agent_version_id(result.dump()); return result;
  }
  int schedule_training_priority(const std::string& task_id) const {
    for(const auto& task : dataset.tasks()) if(task.task_id() == task_id && task.split() == "train") {
      const auto attributes = json(task.attributes());
      const auto dependency = attributes["dependency"].string() ? attributes.str("dependency") : std::string{};
      if(dependency == "independent" || dependency == "parallel" || dependency == "none") return 0;
      if(!dependency.empty()) return std::holds_alternative<bool>(attributes["stop_on_failure"].value)
          && attributes["stop_on_failure"].boolean() ? 3 : 2;
      // 这只是训练任务的采样提示；边是否有效仍由真实批次、差分预检和配对评测决定。
      auto text = task.task_input();
      std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) {
        return static_cast<char>(c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c);
      });
      auto has = [&](const char* phrase) { return text.find(phrase) != std::string::npos; };
      if((has("先") && has("再")) || has("完成后") || has(" before ")
          || has(" after ") || has(" then ")) return 1;
      if(has("互不依赖") || has("无需依赖") || has("no dependency")
          || has("independent") || has("in parallel")) return 0;
      return has("依赖") || has("基于") ? 1 : 0;
    }
    return 0;
  }
  Json packet(const Json& parent, const Report& feedback, const Json& selected) const {
    const auto available = components(parent), sources = source_experiences(seed);
    Json selected_values = Json::Object{};
    bool control_selected = false;
    for(const auto& item : selected.items()) {
      require(available.has(item.text()), "gepa_component_not_authorized");
      selected_values[item.text()] = available[item.text()];
      control_selected |= item.text() == "action_rules" || item.text() == "loop_rules" || item.text() == "schedule_rules";
    }
    const bool experience_only = !selected.items().empty() && !control_selected;
    Json contract = Json::Object{};
    for(const auto* key : {"min_pass_rate", "max_pass_rate_drop", "min_invalid_loop_reduction",
             "max_latency_ratio", "max_token_ratio"})
      if(gate.has(key)) contract[key] = gate[key];
    Json result{{"parent_version", agent_version_id(parent.dump())}, {"experiences", Json::Array{}},
        {"components", selected_values}, {"selected_components", selected}, {"feedback", Json::Array{}},
        {"acceptance", config.str("acceptance", "strict_improvement")},
        {"objective", config["objective"]}, {"optimization_contract", contract},
        {"limits", Json{{"max_experience_bytes", config.num("max_experience_bytes", 2048)}}}};
    const auto policy = proto<v1::RuntimePolicy>(parent["policy"]);
    for(const auto& source : sources.items()) {
      const auto key = "experience:" + source.str("memory_id");
      if(!control_selected && !selected_values.has(key)) continue;
      auto experience = source;
      // 控制规则生成需要源经验语义，因为经验组件在该路径被有意省略；经验改写的
      // 当前文本已包含在 components 中，不能重复发送。
      if(!control_selected || selected_values.has(key)) experience.fields().erase("content");
      result["experiences"].push(experience);
    }
    for(auto& experience : result["experiences"].items()) {
      for(const auto& source : policy.snapshot().experiences())
        if(source.memory_id() == experience.str("memory_id")
            && selected_values.has("experience:" + source.memory_id())
            && experience_content(selected_values["experience:" + source.memory_id()]) != source.content())
          experience["original_content"] = source.content();
    }
    if(controls.flag("enabled", false) && control_selected) {
      result["controls"] = control_generation_input(policy.snapshot(), without_generated_controls(policy.options()), controls);
      result["controls"].fields().erase("memories");  // Sources already appear above; do not duplicate the snapshot.
    }
    restrict_reflection_components(result, selected);
    // 只有训练内容能进入反思；验证/留出分数只用于选择候选，其任务、评分标准和轨迹
    // 永远不会进入该请求包，防止验证集泄漏。
    std::vector<const dataset::v1::EvaluationTrial*> trials;
    for(const auto& trial : feedback.trials()) trials.push_back(&trial);
    const auto feedback_scores = scores(feedback);
    std::map<std::string, bool> relevant;
    std::map<std::string, Json> applications;
    bool has_applied_experience = false;
    std::map<const dataset::v1::EvaluationTrial*, Json> reviewed_views;
    for(const auto* trial : trials) {
      auto view = feedback_trial(*trial, controls);
      bind_iteration_assessments(view, json(trial->evidence()));
      if(selected_values.has("loop_rules"))
        merge_loop_requirements(result["controls"]["loop_requirements"], view["evidence"]["loop_requirements"]);
      if(selected_values.has("action_rules"))
        merge_action_requirements(result["controls"]["action_requirements"], view["evidence"]["action_requirements"]);
      reviewed_views[trial] = std::move(view);
    }
    for(const auto* trial : trials) {
      const auto& view = reviewed_views.at(trial);
      Json single{{"feedback", Json::Array{Json{{"trial", view}}}}};
      if(result.has("controls")) single["controls"] = static_cast<const Json&>(result)["controls"];
      // L1 的应用回执与 L2/L3 的可执行条件不同；无关失败不能挤掉用过本经验的样本。
      Report observed; *observed.add_trials() = *trial;
      const auto used = applied_experiences(observed);
      relevant[trial->trial_id()] = std::any_of(selected.items().begin(), selected.items().end(),
          [&](const auto& component) {
            return used.count(component.text()) || control_evidence(single, component.text());
          });
      if(experience_only) {
        applications[trial->trial_id()] = selected_experience_applications(*trial, selected, seed);
        relevant[trial->trial_id()] = applications.at(trial->trial_id()).num("count", 0) != 0;
        has_applied_experience |= relevant[trial->trial_id()];
      }
    }
    if(selected.items().size() == 1 && selected.items().front().text() == "schedule_rules"
        && std::any_of(trials.begin(), trials.end(), [&](const auto* trial) {
          return relevant[trial->trial_id()] && schedule_training_priority(trial->task_id()) > 0;
        }))
      trials.erase(std::remove_if(trials.begin(), trials.end(), [&](const auto* trial) {
        return !relevant[trial->trial_id()] || schedule_training_priority(trial->task_id()) == 0;
      }), trials.end());
    std::stable_sort(trials.begin(), trials.end(), [&](const auto* a, const auto* b) {
      if(relevant[a->trial_id()] != relevant[b->trial_id()]) return relevant[a->trial_id()];
      if(selected.items().size() == 1 && selected.items().front().text() == "schedule_rules"
          && schedule_training_priority(a->task_id()) != schedule_training_priority(b->task_id()))
        return schedule_training_priority(a->task_id()) > schedule_training_priority(b->task_id());
      if((a->verdict() == "failed") != (b->verdict() == "failed")) return a->verdict() == "failed";
      // 将受限反思上下文优先分配给观测最弱的任务，包括总体成功执行中的浪费循环。
      return number(feedback_scores[a->task_id()]["fitness"]) < number(feedback_scores[b->task_id()]["fitness"]);
    });
    for(const auto* trial : trials) {
      if(result["feedback"].items().size() >= config.num("max_feedback_trials", 4)) break;
      const dataset::v1::EvaluationTask* task = nullptr;
      for(const auto& value : dataset.tasks()) if(value.task_id() == trial->task_id()) task = &value;
      require(task && task->split() == "train", "gepa_reflection_split_violation");
      Json turns = Json::Array{}, constraints = Json::Array{};
      for(const auto& turn : task->followup_inputs()) turns.push(clip(turn, 4096));
      for(const auto& constraint : task->constraints()) constraints.push(clip(constraint, 4096));
      // 迭代上限由运行时策略授权，而不是由 control_learning 配置单独授权。
      auto feedback_controls = controls;
      feedback_controls["aggressive_controls"] = policy.options().aggressive_controls();
      auto trial_view = control_selected ? control_feedback_trial(*trial, selected, feedback_controls) : feedback_trial(*trial, controls);
      const bool counterexample = experience_only && has_applied_experience && !relevant[trial->trial_id()];
      // 未使用获选经验的失败只作对照；全未命中或停用待恢复时仍保留原执行反馈。
      if(counterexample) trial_view = Json{{"verdict", trial_view["verdict"]},
          {"execution_status", trial_view["execution_status"]}, {"metrics", trial_view["metrics"]},
          {"checks", compact_excerpt(trial_view["checks"])}, {"evidence_truncated", true}};
      Json row{{"task_id", task->task_id()}, {"task_input", clip(task->task_input(), 4096)},
          {"followup_inputs", turns}, {"constraints", constraints},
          {"expected_behavior", clip(task->expected_behavior(), 4096)}, {"trial", trial_view}};
      if(counterexample) { row = compact_excerpt(row); row["evidence_role"] = "unapplied_counterexample"; }
      if(experience_only) row["selected_experience_applications"] = applications.at(trial->trial_id());
      auto trial_packet = result; trial_packet["feedback"].push(row);
      if(trial_packet.dump().size() > config.num("max_input_bytes", 65536)) {
        if(control_selected) {
          row["task_input"] = clip(task->task_input(), 1024);
          row["followup_inputs"] = Json::Array{};
          for(const auto& turn : task->followup_inputs()) {
            if(row["followup_inputs"].items().size() == 4) break;
            row["followup_inputs"].push(clip(turn, 1024));
          }
          row["constraints"] = Json::Array{};
          for(const auto& constraint : task->constraints()) {
            if(row["constraints"].items().size() == 8) break;
            row["constraints"].push(clip(constraint, 1024));
          }
          row["expected_behavior"] = clip(task->expected_behavior(), 2048);
        } else if(!counterexample) {
          row["trial"] = Json::Object{};
          auto overhead = result; overhead["feedback"].push(row);
          const auto limit = config.num("max_input_bytes", 65536), used = overhead.dump().size();
          row["trial"] = compact_experience_feedback_trial(*trial, trial_view, selected,
              used < limit ? limit - used + 2 : 0, seed);
        }
        trial_packet = result; trial_packet["feedback"].push(row);
      }
      if(trial_packet.dump().size() <= config.num("max_input_bytes", 65536)) result = std::move(trial_packet);
    }
    require(!result["feedback"].items().empty() && result.dump().size() <= config.num("max_input_bytes", 65536), "gepa_feedback_budget_too_small");
    validate_local_data(result); return result;
  }
  bool control_evidence(const Json& input, const std::string& component) const {
    for(const auto& feedback : input["feedback"].items()) {
      // 受预算限制的行只保留分数和输出，不保留可执行证据，不能据此生成控制规则。
      const auto& runs = feedback["trial"]["evidence"]["runs"];
      if(!runs.array()) continue;
      for(const auto& run : runs.items()) {
        if(component == "schedule_rules" && run["schedule_plans"].array())
          for(const auto& plan : run["schedule_plans"].items()) {
            std::set<std::string> instances;
            if(plan["actions"].array()) for(const auto& action : plan["actions"].items())
              if(!action.str("operation").empty() && !action.str("action_id").empty()) instances.insert(action.str("action_id"));
            if(instances.size() > 1) return true;
          }
        if(!run["actions"].array()) continue;
        for(const auto& action : run["actions"].items()) {
          if(component == "action_rules" && action_control_evidence(action)) return true;
          if(component == "loop_rules" && action.str("kind") == "loop.decision"
              && action["loop_control"].object()
              && !action["loop_control"].flag("pending_actions", false)
              && (action["loop_control"].str("proposed_decision", "continue") == "continue"
                  || action["loop_control"].str("proposed_decision") == "replan")) {
            const auto reviewed_cap = std::max({controls.num("min_iterations", 2),
                action["loop_control"].num("reviewed_waste_after", 0),
                required_iteration_floor(input, action["loop_control"], action.str("actor_id"))});
            if(input["controls"].flag("aggressive_controls", false)
                && action["loop_control"].has("reviewed_waste_after")
                && action["loop_control"].num("iteration", 0) > reviewed_cap) return true;
            if(!action["loop_control"]["observed_no_progress"].null()
                && action["loop_control"].num("observed_no_progress", 0) >= controls.num("min_no_progress", 1)) return true;
            if(action["loop_control"].num("repeated_failure_steps", 0)) {
              const Json* base = nullptr;
              const auto& rules = input["controls"]["base_loop_rules"];
              if(rules.array()) for(const auto& rule : rules.items())
                  if(!rule.has("source_experience") && rule.str("target_scope") == action["loop_control"].str("target_scope")
                      && (rule.str("actor_id").empty() || rule.str("actor_id") == action.str("actor_id"))
                      && (!base || !rule.str("actor_id").empty())) base = &rule;
              if(base && base->str("no_progress_decision") == "replan" && base->num("max_replans", 0)) return true;
            }
          }
        }
      }
    }
    return false;
  }
  bool route_control_opportunity(const Json& parent, const Report& feedback, const Json& available,
      std::size_t slot, const std::string& current, Json& selected, Json& input) const {
    const char* candidates[] = {"schedule_rules", "action_rules", "loop_rules"};
    std::size_t best_count = SIZE_MAX;
    Json best = Json::Array{}, best_input;
    for(std::size_t offset = 0; offset < 3; ++offset) {
      const auto candidate = std::string(candidates[(slot + offset) % 3]);
      if(candidate == current || !available.has(candidate)) continue;
      const Json option = Json::Array{candidate};
      Json probe;
      try { probe = packet(parent, feedback, option); }
      catch(const Error& error) {
        if(std::string(error.what()) == "gepa_feedback_budget_too_small") continue;
        throw;
      }
      if(!control_evidence(probe, candidate)) continue;
      const auto count = tree["component_selections"].num(candidate, 0);
      if(count >= best_count) continue;
      best_count = count; best = option; best_input = std::move(probe);
    }
    if(best.items().empty()) return false;
    selected = std::move(best); input = std::move(best_input); return true;
  }
  const Json* repair_feedback_row(const Json& input, const Json& selected) const {
    const bool actions = std::any_of(selected.items().begin(), selected.items().end(), [](const auto& item) {
      return item.text() == "action_rules";
    });
    // 单条修复证据优先保留已放行的重复派发；模型计划和已拒绝动作没有新增拦截机会。
    if(actions) for(const auto& row : input["feedback"].items()) {
      const auto& runs = row["trial"]["evidence"]["runs"]; if(!runs.array()) continue;
      for(const auto& run : runs.items()) {
        if(!run["actions"].array()) continue;
        for(const auto& action : run["actions"].items())
          if(action.str("semantic") != "model" && action.flag("guard_only", false)
              && !action.flag("executed", true) && action_control_evidence(action)
              && action["action_control"].num("prior_operation_rounds", 0) > 0) return &row;
      }
    }
    const Json* evidence = nullptr;
    for(const auto& row : input["feedback"].items()) {
      if(!evidence) evidence = &row;
      Json single{{"feedback", Json::Array{row}}};
      if(input.has("controls")) single["controls"] = static_cast<const Json&>(input)["controls"];
      if(std::any_of(selected.items().begin(), selected.items().end(),
          [&](const auto& component) { return control_evidence(single, component.text()); })) return &row;
    }
    return evidence;
  }
  std::set<std::string> applied_experiences(const Report& report) const {
    const auto policy = proto<v1::RuntimePolicy>(seed["policy"]);
    const auto canonical = canonical_experience_aliases(policy);
    std::set<std::string> result;
    for(const auto& trial : report.trials()) {
      if(trial.variant_id() != "candidate") continue;
      const auto evidence = json(trial.evidence());
      const auto traces = evidence["traces"].array() ? evidence["traces"] : Json(Json::Array{evidence["trace"]});
      for(const auto& trace : traces.items()) for(const auto& event : trace["events"].items()) {
        if(event.str("kind") != "advice.applied" || event["application"].str("status") != "applied") continue;
        if(event["attributes"]["receipt"].str("boundary") != "llm") continue;
        const auto& experiences = event["attributes"]["receipt"]["experiences"];
        // 无经验的模型请求也可能确认应用；Protobuf 会省略空列表，不能据此中断反思。
        if(experiences.null()) continue;
        for(const auto& source : experiences.items()) {
          auto id = source.str("memory_id");
          const auto alias = canonical.find({id, source.str("content_revision")});
          if(alias != canonical.end()) id = alias->second;
          if(!id.empty()) result.insert("experience:" + id);
        }
      }
    }
    return result;
  }
  Json experience_ablation(const Json& parent, const Json& selected, const Report& report) const {
    if(selected.items().size() != 1) return Json();
    const auto key = selected.items().front().text();
    if(key.rfind("experience:", 0) != 0 || components(parent)[key].null()) return Json();
    for(const auto& trial : report.trials()) {
      if(trial.variant_id() != "candidate" || trial.execution_status() != "completed") continue;
      const auto evidence = json(trial.evidence());
      const bool waste = evidence["loop_review"].str("status") == "complete"
          && evidence["loop_review"].num("count", 0) > 0;
      if(trial.verdict() != "failed" && !(trial.verdict() == "passed" && waste)) continue;
      Report one; *one.add_trials() = trial;
      if(!applied_experiences(one).count(key)) continue;
      // 共现仅用于安排消融实验，不把任务失败或浪费直接归因于该经验。
      return Json{{"reason", "Compare disabling an injected experience on an unsuccessful or wasteful training trial; evaluation determines its effect."},
          {"experience", Json{{"memory_id", key.substr(11)}, {"content", nullptr}}}};
    }
    return Json();
  }
  bool experience_ablation_seen(const std::string& parent_version, const Json& selected, std::size_t before) const {
    const auto& nodes = tree["nodes"].items();
    for(std::size_t previous = 0; previous < std::min(before, nodes.size()); ++previous) {
      const auto& prior = nodes[previous];
      if(prior.str("generation_method") != "experience_ablation") continue;
      const auto parent_id = prior.num("parent", SIZE_MAX);
      if(parent_id < nodes.size()
          && nodes[parent_id].str("version") == parent_version && prior["components"].dump() == selected.dump()) return true;
    }
    return false;
  }
  Json reviewed_control_probe(const Json& parent, const Json& candidate, const Json& input,
      const Report& report, std::size_t index) const {
    const auto behavior = [&](const Json& artifact) {
      auto values = components(artifact);
      for(const auto* category : {"action_rules", "loop_rules", "schedule_rules"})
        if(values[category].array()) for(auto& rule : values[category].items()) rule.fields().erase("reason");
      return values.dump();
    };
    if(behavior(parent) != behavior(candidate)) return Json();
    const auto prepared = observed_operation_bindings(input, config.num("max_input_bytes", 65536));
    const auto& bindings = prepared["controls"]["editable_bindings"];
    if(!bindings.array()) return Json();
    for(const auto& binding : bindings.items()) {
      const bool action = binding.str("component") == "action_rules"
          && binding.str("primitive_id") == "operation_budget" && binding.flag("reviewed_redundant_suffix", false);
      const bool loop = binding.str("component") == "loop_rules"
          && binding.str("primitive_id") == "bounded_iteration_cap" && binding.has("reviewed_waste_after");
      if(!action && !loop) continue;
      const auto amount = action ? binding.num("prior_operation_rounds", 0)
          : control_binding_budget(prepared, binding).num("min", 0);
      if((action && (!amount || amount > 16)) || (loop && amount >= binding.num("observed_iteration", 0))) continue;
      Json signature{{"parent_version", agent_version_id(parent.dump())}, {"component", binding["component"]},
          {"scope", binding["target_scope"]}, {"operation", binding.str("operation")},
          {"actor_id", binding.str("actor_id")}, {"budget", amount},
          {"require_finalization", binding.flag("require_finalization", false)}};
      const auto key = "reviewed-control:" + hash(signature.dump());
      bool seen = false;
      if(tree["nodes"].array()) for(std::size_t previous = 0; previous < std::min(index, tree["nodes"].items().size()); ++previous)
        seen |= tree["nodes"].items()[previous]["control_probe"].str("key") == key;
      if(seen) continue;
      auto source = control_probe_source(parent, prepared, binding, report);
      if(source.null()) continue;
      const std::string reason = "训练评审提出的受限控制假设，经验仅关联已有批准来源；效果仍由真实对照评测确认。";
      Json update{{"binding_id", binding["binding_id"]}, {"memory_id", source["memory_id"]}, {"reason", reason}};
      update[action ? "max_attempts" : "max_iterations"] = amount;
      try {
        const auto draft = resolve_control_binding_updates(Json{{"reason", reason},
            {"control_binding_updates", Json::Array{update}}}, prepared);
        return Json{{"draft", draft}, {"detail", Json{{"key", key}, {"component", binding["component"]},
            {"evidence_ref", binding["evidence_ref"]}, {"source", source}, {"status", "pending"},
            {"hypothesis_origin", "training_review"}}}};
      } catch(const Error& error) {
        if(std::string(error.what()) != "control_binding_invalid") throw;
      }
    }
    return Json();
  }
  Json observed_experience(const Json& available, const std::set<std::string>& observed) const {
    std::string best; std::size_t uses = SIZE_MAX;
    for(const auto& key : observed) if(available.has(key)) {
      const auto count = tree["component_selections"].num(key, 0);
      if(count < uses || (count == uses && key < best)) { best = key; uses = count; }
    }
    return best.empty() ? Json(Json::Array{}) : Json(Json::Array{best});
  }
  Json applied_changes(const Json& parent, const Report& report) const {
    Json changed = Json::Object{};
    auto effect = [](const Json& decision, const std::string& boundary) {
      if(boundary == "llm") {
        auto context = decision.str("context");
        const auto prefix = "\n<trajectory-experience policy=\"" + decision.str("policy_id") + "\">\n";
        // 版本编号用于追溯，不是经验内容；仅归一化宿主生成的外层标记。
        if(!decision.str("policy_id").empty() && context.rfind(prefix, 0) == 0)
          context.replace(0, prefix.size(), "\n<trajectory-experience>\n");
        return Json(context);
      }
      if(boundary == "loop") return Json(decision["progress"].str("decision"));
      Json result = Json::Object{};
      if(boundary == "actions" && decision["actions"].array())
        for(const auto& action : decision["actions"].items()) result[action.str("action_id")] = action["decision"];
      if(boundary == "schedule" && decision["schedule"]["actions"].array())
        for(const auto& action : decision["schedule"]["actions"].items()) {
          result[action.str("action_id")] = Json::Object{};
          if(action["after"].array()) for(const auto& edge : action["after"].items())
            result[action.str("action_id")][edge.str("action_id")] = edge.flag("require_success", false);
        }
      return result;
    };
    for(const auto& trial : report.trials()) {
      if(trial.variant_id() != "candidate") continue;
      const auto evidence = json(trial.evidence());
      const auto traces = evidence["traces"].array() ? evidence["traces"] : Json(Json::Array{evidence["trace"]});
      for(const auto& trace : traces.items()) {
        if(!trace["events"].array()) continue;
        const auto bound = bind_runtime_policy(proto<v1::RuntimePolicy>(parent["policy"]), proto<v1::Identity>(trace["identity"]));
        const PolicyRuntime runtime(bound);
        std::map<std::string, std::string> observed;
        std::set<std::string> executed, completed_schedules, committed;
        for(const auto& event : trace["events"].items()) {
          if(event.str("kind") == "advice.applied")
            observed[event["application"].str("query_id")] = event["application"].str("status");
          if(event.str("kind") == "action.end" && event["attributes"].flag("executed", false)) executed.insert(event.str("step_id"));
          if(event.str("kind") == "step.end" && event.str("status") == "completed") completed_schedules.insert(event.str("step_id"));
          if(event.str("kind") == "loop.decision") committed.insert(event.str("step_id"));
        }
        const auto replay_options = json(bound.options());
        Json ledger = Json::Object{}, attempts = Json::Object{};
        std::map<std::string, Json> schedule_inputs;
        for(const auto& event : trace["events"].items()) {
          if(event.str("kind") == "step.start" && event["input"]["actions"].array())
            schedule_inputs[event.str("step_id")] = event["input"];
          snapshot_scheduled_target(attempts, ledger, replay_options, event);
          if(event.str("kind") == "action.end")
            settle_target_event(ledger, static_cast<const Json&>(attempts)[event.str("action_id")], replay_options, event);
          if(event["attributes"].str("type") != "policy.query") continue;
          const auto& actual = event["attributes"]["decision"];
          auto q = proto<v1::RuntimeQuery>(actual["query"]); const auto boundary = q.boundary();
          if(schedule_inputs.count(event.str("step_id"))) project_schedule_evidence(q, schedule_inputs.at(event.str("step_id")));
          if(boundary != "llm" && boundary != "actions" && boundary != "loop" && boundary != "schedule") continue;
          if(q.actor_id().empty()) q.set_actor_id(event.str("actor_id"));
          if(q.previous().iteration()) q.mutable_previous()->set_policy_id(bound.policy_id());
          for(auto& action : *q.mutable_actions()) if(boundary == "actions") {
            const auto snapshot = target_failure_snapshot(replay_options, q.advice_query().target_scope(),
                action.operation(), json(action.arguments()), ledger, q.execution_id());
            attempts[action.action_id()] = Json{{"entries", snapshot}, {"scope", q.advice_query().target_scope()},
                {"operation", action.operation()}, {"execution_id", q.execution_id()}};
            action.clear_target_failures();
            for(const auto& entry : snapshot.fields()) (*action.mutable_target_failures())[entry.first] = entry.second.num("count", 0);
          }
          if(effect(actual, boundary).dump() == effect(json(runtime.decide(q)), boundary).dump()) continue;
          const auto status = observed[q.advice_query().query_id()];
          // 移除注入或约束时出现 no-op 回执是预期行为；只有真实边界完成才计入，
          // 失败回执绝不能算作已应用。
          bool applied = status == "applied";
          if(actual.str("status") == "no_change" && status == "rejected") {
            if(boundary == "llm" || boundary == "actions") applied = executed.count(event.str("step_id"));
            if(boundary == "loop") applied = committed.count(event.str("step_id"));
            if(boundary == "schedule") applied = completed_schedules.count(event.str("step_id"));
          }
          if(applied) changed[trial.task_id()][boundary] = changed[trial.task_id()].num(boundary, 0) + 1;
        }
      }
    }
    return changed;
  }
  void refresh_change_evidence() {
    if(tree.num("change_evidence_revision", 0) >= 1 || !tree["active"].null() || !tree["batch"].null()) return;
    // 仅重算派生归因，原始试次与评分不变；批次边界更新，保留已排定的父子关系。
    auto updated = tree;
    for(std::size_t index = 1; index < updated["nodes"].items().size(); ++index) {
      auto& node = updated["nodes"].items()[index];
      if(node.str("version").empty() || !node["screening"].object()) continue;
      auto cached_report = [&](const std::string& split, const Json& tasks) {
        for(const auto& item : static_cast<const Json&>(tree)["evaluations"].fields()) {
          const auto& cached = item.second;
          if(cached.str("version") != node.str("version") || cached.str("split") != split
              || cached["task_ids"].dump() != tasks.dump() || cached.str("report_hash").empty()) continue;
          const auto bytes = read(root / "evaluations" / item.first
              / ("attempt-" + std::to_string(cached.num("attempt", 0))) / "report.json", 134217728);
          require(hash(bytes) == cached.str("report_hash"), "gepa_report_changed");
          return proto<Report>(Json::parse(bytes));
        }
        throw Error("gepa_attribution_report_unavailable");
      };
      const auto parents = gepa::parents(node);
      if(node["screening"]["applied_changes"].object()) {
        const auto report = cached_report(node["screening"].str("split", "train"), node["tasks"]);
        node["screening"]["applied_changes"] = applied_changes(artifact(parents.front()), report);
      }
      if(node["screening"]["parent_applied_changes"].array()) {
        const auto report = cached_report("val", node["tasks"]);
        node["screening"]["parent_applied_changes"] = Json::Array{};
        for(const auto parent : parents)
          node["screening"]["parent_applied_changes"].push(applied_changes(artifact(parent), report));
      }
      bool changed = node["screening"]["applied_changes"].object()
          && !node["screening"]["applied_changes"].fields().empty();
      if(node["validation_applied_changes"].object()) {
        Json tasks = Json::Array{}; for(const auto& id : val) tasks.push(id);
        const auto report = cached_report("val", tasks);
        for(const auto parent : parents) {
          auto changes = applied_changes(artifact(parent), report);
          changed |= !changes.fields().empty();
          node["validation_applied_changes"][std::to_string(parent)] = std::move(changes);
        }
      }
      if(node.str("status") == "accepted" && !changed) {
        node["attribution_previous_status"] = "accepted";
        node["status"] = "unobserved_changes"; node["reason"] = "no_observed_decision_change";
      }
    }
    // 已进入留出集的旧检查点不能沿用失去因果证据的赢家。
    if(updated.flag("holdout_started", false))
      require(updated["nodes"].items().at(updated.num("winner", 0)).str("status") == "accepted",
          "gepa_attribution_winner_invalid");
    tree = std::move(updated); tree["change_evidence_revision"] = 1;
    tree["frontier"] = Json::Array{}; for(const auto id : frontier()) tree["frontier"].push(id);
    tree["frontier_mapping"] = gepa::frontier_map(tree["nodes"], val, config); save();
  }
  std::pair<Json, Json> observed_score_subset(const Json& candidate, const Json& parent, const Json& changes) const {
    Json selected = Json::Object{}, previous = Json::Object{};
    if(changes.object()) for(const auto& task : changes.fields()) {
      bool applied = false;
      if(task.second.object()) for(const auto& boundary : task.second.fields()) applied |= number(boundary.second, 0) > 0;
      if(applied && candidate.has(task.first) && parent.has(task.first)) {
        selected[task.first] = candidate[task.first]; previous[task.first] = parent[task.first];
      }
    }
    return {std::move(selected), std::move(previous)};
  }
  bool observed_local_gain(const Json& candidate, const Json& parent, const Json& changes) const {
    const auto [selected, previous] = observed_score_subset(candidate, parent, changes);
    return gepa::local_gain(selected, previous, number(config["min_improvement"], 0.000001));
  }
  bool observed_training_gain(const Json& candidate, const Json& parent, const Json& changes) const {
    const auto [selected, previous] = observed_score_subset(candidate, parent, changes);
    const auto acceptance = config.str("acceptance", "strict_improvement");
    if(acceptance == "pareto") return gepa::no_worse(selected, previous)
        || gepa::local_gain(selected, previous, number(config["min_improvement"], 0.000001));
    return acceptance == "improvement_or_equal" ? gepa::no_worse(selected, previous) : improves(selected, previous);
  }
  void remember_opportunities(const Json& parent, const Report& report) {
    const auto policy = proto<v1::RuntimePolicy>(parent["policy"]);
    if(!controls.flag("enabled", false) || policy.options().requested_level() < v1::L2) return;
    auto packet_controls = control_generation_input(policy.snapshot(),
        without_generated_controls(policy.options()), controls);
    packet_controls.fields().erase("memories");
    for(const auto& trial : report.trials()) {
      if(trial.variant_id() != "candidate" || std::find(train.begin(), train.end(), trial.task_id()) == train.end()) continue;
      auto view = feedback_trial(trial, controls);
      bind_iteration_assessments(view, json(trial.evidence()));
      Json input{{"controls", packet_controls}, {"feedback", Json::Array{Json{{"trial", view}}}}};
      for(const auto* component : {"schedule_rules", "action_rules", "loop_rules"})
        if(control_evidence(input, component)) {
          auto& opportunity = tree["training_opportunities"][component][trial.task_id()];
          const auto previous = number(opportunity, opportunity.null() ? 0 : 1);
          opportunity = std::max(previous, trial.verdict() == "failed" ? 2.0 : 1.0);
        }
    }
  }
  std::vector<std::string> component_batch(const Json& selected, std::size_t sequence) const {
    auto result = minibatch(sequence);
    if(selected.items().size() != 1) return result;
    const auto& available = tree["training_opportunities"][selected.items().front().text()];
    std::vector<std::string> candidates;
    if(available.object()) for(const auto& item : available.fields())
      if(std::find(train.begin(), train.end(), item.first) != train.end()) candidates.push_back(item.first);
    if(selected.items().front().text() == "schedule_rules") for(const auto& id : train)
      if(schedule_training_priority(id) && std::find(candidates.begin(), candidates.end(), id) == candidates.end())
        candidates.push_back(id);
    if(candidates.empty()) return result;
    std::stable_sort(candidates.begin(), candidates.end(), [&](const auto& a, const auto& b) {
      if(selected.items().front().text() == "schedule_rules"
          && schedule_training_priority(a) != schedule_training_priority(b))
        return schedule_training_priority(a) > schedule_training_priority(b);
      return number(available[a], available[a].null() ? 0 : 1)
          > number(available[b], available[b].null() ? 0 : 1);
    });
    if(selected.items().front().text() == "schedule_rules" && schedule_training_priority(candidates.front()) >= 2)
      candidates.erase(std::remove_if(candidates.begin(), candidates.end(), [&](const auto& id) {
        return schedule_training_priority(id) == 0;
      }), candidates.end());
    // 在保持 epoch 覆盖的同时，为当前选中的能力预留一个有对应证据的样本。
    const auto& task = candidates[sequence % candidates.size()];
    if(std::find(result.begin(), result.end(), task) == result.end()) result.front() = task;
    std::sort(result.begin(), result.end()); return result;
  }
  Json control_preflight(const Json& parent, const Json& candidate, const Report& report) const {
    Json result{{"rules", Json::Object{}}, {"missing", Json::Array{}}};
    std::set<std::string> existing, retained;
    std::map<std::string, const v1::LoopRule*> changed_caps;
    std::set<std::string> changed_action_budgets;
    const auto old = proto<v1::RuntimePolicy>(parent["policy"]), policy = proto<v1::RuntimePolicy>(candidate["policy"]);
    for(const auto& rule : old.options().action_rules()) existing.insert(rule.rule_id());
    for(const auto& rule : old.options().loop_rules()) existing.insert(rule.rule_id());
    for(const auto& rule : old.options().schedule_rules()) existing.insert(rule.rule_id());
    for(const auto& rule : policy.options().action_rules()) retained.insert(rule.rule_id());
    for(const auto& rule : policy.options().loop_rules()) retained.insert(rule.rule_id());
    for(const auto& rule : policy.options().schedule_rules()) retained.insert(rule.rule_id());
    Json removed = Json::Array{};
    for(const auto& rule : old.options().action_rules())
      if(rule.has_source_experience() && !retained.count(rule.rule_id())) removed.push(rule.rule_id());
    for(const auto& rule : old.options().loop_rules())
      if(rule.has_source_experience() && !retained.count(rule.rule_id())) removed.push(rule.rule_id());
    for(const auto& rule : old.options().schedule_rules())
      if(rule.has_source_experience() && !retained.count(rule.rule_id())) removed.push(rule.rule_id());
    if(!removed.items().empty() && std::includes(existing.begin(), existing.end(), retained.begin(), retained.end())
        && source_experiences(parent).dump() == source_experiences(candidate).dump()) {
      // 纯删除也需训练差分：在父轨迹上回放候选，复用真实边界回执和分支账本。
      // 混合经验改写不走此捷径；没有新增规则不等于已证明移除约束有效。
      const auto changes = applied_changes(candidate, report);
      result["removed_rule_ids"] = removed;
      result["removal_changes"] = changes;
      result["pure_removal_no_effect"] = changes.fields().empty();
    }
    for(const auto& rule : policy.options().action_rules()) if(rule.has_source_experience() && !existing.count(rule.rule_id())) {
      result["rules"][rule.rule_id()] = 0;
      if(rule.max_attempts()) changed_action_budgets.insert(rule.rule_id());
    }
    for(const auto& rule : policy.options().loop_rules()) if(rule.has_source_experience() && !existing.count(rule.rule_id())) {
      result["rules"][rule.rule_id()] = 0;
      if(rule.max_iterations()) changed_caps[rule.rule_id()] = &rule;
      const v1::LoopRule* previous = nullptr;
      for(const auto& before : old.options().loop_rules())
        if(before.target_scope() == rule.target_scope()
            && (before.actor_id().empty() || before.actor_id() == rule.actor_id())
            && (!previous || (previous->actor_id().empty() && !before.actor_id().empty())
                || (previous->actor_id() == before.actor_id() && before.has_source_experience()))) previous = &before;
      // reason 是解释，不是执行条件。新削减恢复预算必须指向结构化事实，
      // 既有未修改规则仍可加载，不借本次变更迁移或重写旧策略。
      if(previous && rule.max_replans() < previous->max_replans()
          && !rule.after_action_rejection() && rule.error_codes().empty()) {
        if(!result.has("unconditioned_recovery")) result["unconditioned_recovery"] = Json::Array{};
        result["unconditioned_recovery"].push(rule.rule_id());
      }
    }
    for(const auto& rule : policy.options().schedule_rules()) if(rule.has_source_experience() && !existing.count(rule.rule_id()))
      result["rules"][rule.rule_id()] = 0;
    if(!result["rules"].fields().empty()) for(const auto& trial : report.trials()) {
      if(trial.variant_id() != "candidate") continue;
      const auto evidence = json(trial.evidence());
      auto reviewed = feedback_trial(trial, controls);
      bind_iteration_assessments(reviewed, evidence);
      const auto traces = evidence["traces"].array() ? evidence["traces"] : Json(Json::Array{evidence["trace"]});
      for(const auto& trace : traces.items()) {
        if(!trace["events"].array()) continue;
        const auto bound = bind_runtime_policy(policy, proto<v1::Identity>(trace["identity"]));
        const PolicyRuntime runtime(bound);
        const auto replay_options = json(bound.options());
        Json ledger = Json::Object{}, attempts = Json::Object{};
        std::map<std::string, Json> schedule_inputs;
        for(const auto& event : trace["events"].items()) {
          if(event.str("kind") == "step.start" && event["input"]["actions"].array())
            schedule_inputs[event.str("step_id")] = event["input"];
          snapshot_scheduled_target(attempts, ledger, replay_options, event);
          if(event.str("kind") == "action.end")
            settle_target_event(ledger, static_cast<const Json&>(attempts)[event.str("action_id")], replay_options, event);
          if(event["attributes"].str("type") != "policy.query") continue;
          const auto& before = event["attributes"]["decision"];
          auto query = proto<v1::RuntimeQuery>(before["query"]);
          if(schedule_inputs.count(event.str("step_id"))) project_schedule_evidence(query, schedule_inputs.at(event.str("step_id")));
          if(query.actor_id().empty()) query.set_actor_id(event.str("actor_id"));
          if(query.boundary() != "llm" && query.boundary() != "loop"
              && query.boundary() != "actions" && query.boundary() != "schedule") continue;
          if(query.previous().iteration()) query.mutable_previous()->set_policy_id(bound.policy_id());
          for(auto& action : *query.mutable_actions()) if(query.boundary() == "actions") {
            const auto snapshot = target_failure_snapshot(replay_options, query.advice_query().target_scope(),
                action.operation(), json(action.arguments()), ledger, query.execution_id());
            attempts[action.action_id()] = Json{{"entries", snapshot}, {"scope", query.advice_query().target_scope()},
                {"operation", action.operation()}, {"execution_id", query.execution_id()}};
            action.clear_target_failures();
            for(const auto& entry : snapshot.fields()) (*action.mutable_target_failures())[entry.first] = entry.second.num("count", 0);
          }
          const auto after = runtime.decide(query);
          if(query.boundary() == "llm" && query.observed_operations().empty()
              && after.context() != before.str("context")) {
            auto lower = [](std::string text) {
              std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) {
                return static_cast<char>(c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c);
              });
              return text;
            };
            const auto task = lower(query.advice_query().task());
            for(const auto& rule : bound.options().schedule_rules()) {
              if(!result["rules"].has(rule.rule_id()) || rule.task_terms().empty()
                  || rule.target_scope() != query.advice_query().target_scope()) continue;
              bool matched = true;
              for(const auto& term : rule.task_terms()) matched &= task.find(lower(term)) != std::string::npos;
              for(const auto& term : rule.exclude_terms()) matched &= task.find(lower(term)) == std::string::npos;
              if(matched && after.context().find(rule.before_operation()) != std::string::npos
                  && after.context().find(rule.after_operation()) != std::string::npos)
                result["rules"][rule.rule_id()] = result["rules"].num(rule.rule_id(), 0) + 1;
            }
          }
          if(query.boundary() == "schedule" && before["schedule"]["actions"].array()) {
            auto edges = [](const v1::SchedulePlan& plan) {
              Json result = Json::Object{};
              for(const auto& action : plan.actions()) {
                result[action.action_id()] = Json::Object{};
                for(const auto& dependency : action.after())
                  result[action.action_id()][dependency.action_id()] = dependency.require_success();
              }
              return result.dump();
            };
            if(edges(after.schedule()) != edges(proto<v1::SchedulePlan>(before["schedule"])))
              for(const auto& id : after.schedule().rule_ids()) if(result["rules"].has(id))
                result["rules"][id] = result["rules"].num(id, 0) + 1;
          }
          for(const auto& action : after.actions()) if(action.decision() == "reject" && result["rules"].has(action.rule_id())) {
            bool allowed = true;
            for(const auto& previous : before["actions"].items())
              if(previous.str("action_id") == action.action_id()) allowed = previous.str("decision") == "allow";
            if(allowed) {
              result["rules"][action.rule_id()] = result["rules"].num(action.rule_id(), 0) + 1;
              // 差分命中只证明会拦截，不证明拦截正确；完整训练评审中的必要动作是反例。
              if(changed_action_budgets.count(action.rule_id()))
                for(const auto& run : reviewed["evidence"]["runs"].items()) {
                  if(run.str("run_id") != trace["identity"].str("run_id")) continue;
                  for(const auto& reviewed_action : run["actions"].items()) {
                    if(reviewed_action.str("kind") != "action.end"
                        || !reviewed_action.flag("reviewed_required_action", false)
                        || reviewed_action["action_control"].str("query_event_id") != event.str("event_id")) continue;
                    const auto source = std::find_if(trace["events"].items().begin(), trace["events"].items().end(),
                        [&](const auto& value) { return value.str("event_id") == reviewed_action.str("event_id")
                            && value.str("kind") == "action.end" && value.str("action_id") == action.action_id(); });
                    if(source == trace["events"].items().end()) continue;
                    if(!result.has("known_keep_truncation")) result["known_keep_truncation"] = Json::Array{};
                    result["known_keep_truncation"].push(Json{{"rule_id", action.rule_id()}, {"task_id", trial.task_id()},
                        {"event_id", event["event_id"]}, {"action_id", action.action_id()},
                        {"execution_id", event["execution_id"]}});
                  }
                }
            }
          }
          const auto& progress = after.progress();
          if(query.boundary() == "loop" && progress.decision() == "stop"
              && before["progress"].str("decision") != "stop" && changed_caps.count(progress.rule_id())
              && query.iteration() > changed_caps.at(progress.rule_id())->max_iterations()
              && reviewed["evidence"]["runs"].array()) {
            const bool finish = changed_caps.at(progress.rule_id())->require_finalization() && query.can_finalize();
            bool cuts_work = false;
            for(const auto& run : reviewed["evidence"]["runs"].items()) {
              if(run.str("run_id") != trace["identity"].str("run_id")) continue;
              for(const auto& action : run["actions"].items())
                if((action.str("kind") == "loop.decision" || action.str("kind") == "action.end")
                    && action.str("execution_id") == event.str("execution_id")
                    && action.str("loop_id") == event.str("loop_id")
                    && proto<v1::TraceEvent>(Json{{"iteration", action["iteration"]}}).iteration() >= query.iteration()
                    && action["iteration_assessment"].str("decision") == "keep"
                    && !(finish && action.flag("reviewed_final_answer", false))) cuts_work = true;
            }
            if(cuts_work) {
              if(!result.has("known_keep_truncation")) result["known_keep_truncation"] = Json::Array{};
              result["known_keep_truncation"].push(Json{{"rule_id", progress.rule_id()}, {"task_id", trial.task_id()},
                  {"event_id", event["event_id"]}, {"execution_id", event["execution_id"]}});
            }
          }
          if(query.boundary() == "loop" && before["progress"].str("decision") != progress.decision()
              && progress.decision() != "continue" && result["rules"].has(progress.rule_id()))
            result["rules"][progress.rule_id()] = result["rules"].num(progress.rule_id(), 0) + 1;
        }
      }
    }
    for(const auto& rule : result["rules"].fields()) if(!rule.second.integer()) result["missing"].push(rule.first);
    result["passed"] = result["missing"].items().empty() && !result.flag("pure_removal_no_effect", false)
        && !result.has("unconditioned_recovery") && !result.has("known_keep_truncation"); return result;
  }
  Json repair_schedule_contracts(const Json& draft, const Report& report, Json& repairs) const {
    repairs = Json::Array{};
    if(!draft["controls"]["schedule_rules"].array()) return draft;
    auto result = draft;
    auto lower = [](std::string value) {
      std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c);
      });
      return value;
    };
    for(auto& rule : result["controls"]["schedule_rules"].items()) {
      if(rule.flag("require_success", false) || !rule["task_terms"].array()
          || rule["task_terms"].items().empty()) continue;
      for(const auto& trial : report.trials()) {
        if(trial.variant_id() != "candidate" || trial.split() != "train") continue;
        const dataset::v1::EvaluationTask* task = nullptr;
        for(const auto& candidate : dataset.tasks()) if(candidate.task_id() == trial.task_id()) { task = &candidate; break; }
        if(!task) continue;
        const auto attributes = json(task->attributes());
        const auto dependency = attributes["dependency"].string() ? attributes.str("dependency") : std::string{};
        if(!std::holds_alternative<bool>(attributes["stop_on_failure"].value)
            || !attributes["stop_on_failure"].boolean() || dependency.empty()
            || dependency == "independent" || dependency == "parallel" || dependency == "none") continue;
        const auto evidence = json(trial.evidence());
        const auto traces = evidence["traces"].array() ? evidence["traces"] : Json(Json::Array{evidence["trace"]});
        bool witnessed = false;
        for(const auto& trace : traces.items()) for(const auto& event : trace["events"].items()) {
          if(event["attributes"].str("type") != "policy.query") continue;
          const auto& q = event["attributes"]["decision"]["query"];
          if(q.str("boundary") != "schedule") continue;
          bool before = false, after = false, matches = true;
          for(const auto& action : q["actions"].items()) {
            before |= action.str("operation") == rule.str("before_operation");
            after |= action.str("operation") == rule.str("after_operation");
          }
          const auto text = lower(q["advice_query"].str("task"));
          for(const auto& term : rule["task_terms"].items()) matches &= text.find(lower(term.text())) != std::string::npos;
          if(rule["exclude_terms"].array()) for(const auto& term : rule["exclude_terms"].items())
            matches &= text.find(lower(term.text())) == std::string::npos;
          witnessed |= before && after && matches;
        }
        if(!witnessed) continue;
        // 已声明的训练约束具有权威性；不能从失败的供应商响应推断，也不能改写独立分支。
        rule["require_success"] = true;
        rule["reason"] = "前置操作成功后才启动依赖操作；前置失败时仅跳过依赖分支。";
        repairs.push(Json{{"task_id", trial.task_id()}, {"before_operation", rule["before_operation"]},
            {"after_operation", rule["after_operation"]}, {"constraint", "stop_on_failure"}});
        break;
      }
    }
    return result;
  }
  Json repair_schedule_guards(const Json& draft, const Report& report, Json& repairs) const {
    // 只根据带标签的训练批次校准；验证和留出仅作为选择证据，绝不能改写候选的任务守卫。
    auto result = draft;
    repairs = Json::Array{};
    if(!result["controls"]["schedule_rules"].array()) return result;
    for(auto& rule : result["controls"]["schedule_rules"].items()) {
      if(!rule["task_terms"].array() || rule["task_terms"].items().size() < 3) continue;
      const auto before_operation = rule.str("before_operation"), after_operation = rule.str("after_operation");
      std::vector<std::string> positive, negative;
      for(const auto& trial : report.trials()) {
        if(trial.variant_id() != "candidate") continue;
        const dataset::v1::EvaluationTask* task = nullptr;
        for(const auto& item : dataset.tasks()) if(item.task_id() == trial.task_id()) { task = &item; break; }
        if(!task) continue;
        const auto attributes = json(task->attributes());
        const auto dependency = attributes["dependency"].string() ? attributes.str("dependency") : std::string{};
        if(dependency.empty()) continue;
        const bool independent = dependency == "independent" || dependency == "parallel" || dependency == "none";
        const auto evidence = json(trial.evidence());
        const auto traces = evidence["traces"].array() ? evidence["traces"] : Json(Json::Array{evidence["trace"]});
        for(const auto& trace : traces.items()) {
          if(!trace["events"].array()) continue;
          for(const auto& event : trace["events"].items()) {
            if(event["attributes"].str("type") != "policy.query") continue;
            const auto& query = event["attributes"]["decision"]["query"];
            if(query.str("boundary") != "schedule" || !query["actions"].array()) continue;
            int before = 0, after = 0;
            for(const auto& action : query["actions"].items()) {
              before += action.str("operation") == before_operation;
              after += action.str("operation") == after_operation;
            }
            if(before && after && (before_operation != after_operation || before >= 2))
              (independent ? negative : positive).push_back(query["advice_query"].str("task"));
          }
        }
      }
      if(positive.empty()) continue;
      auto lower = [](std::string text) {
        std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) {
          return static_cast<char>(c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c);
        });
        return text;
      };
      std::vector<std::string> retained;
      for(const auto& task : positive) {
        std::vector<std::string> matched;
        const auto text = lower(task);
        for(const auto& term : rule["task_terms"].items())
          if(text.find(lower(term.text())) != std::string::npos) matched.push_back(term.text());
        if(matched.size() > retained.size()) retained = std::move(matched);
      }
      if(retained.size() < 2 || retained.size() == rule["task_terms"].items().size()) continue;
      bool unsafe = false;
      for(const auto& task : negative) {
        const auto text = lower(task);
        bool matches = true;
        for(const auto& term : retained) matches &= text.find(lower(term)) != std::string::npos;
        if(rule["exclude_terms"].array()) for(const auto& term : rule["exclude_terms"].items())
          if(text.find(lower(term.text())) != std::string::npos) matches = false;
        unsafe |= matches;
      }
      if(unsafe) continue;
      const auto removed = rule["task_terms"].items().size() - retained.size();
      rule["task_terms"] = Json::Array{};
      for(const auto& term : retained) rule["task_terms"].push(term);
      repairs.push(Json{{"before_operation", before_operation}, {"after_operation", after_operation},
          {"removed_terms", removed}, {"matched_terms", retained.size()}});
    }
    return result;
  }
  Json mutate(const Json& parent, const Json& proposal, const Json& selected) const {
    const auto before = components(parent);
    const auto draft = expand_control_updates(proposal, before, selected);
    keys(draft, {"reason", "experience", "experiences", "controls"}); validate_local_data(draft);
    require(!draft.str("reason").empty() && draft.str("reason").size() <= 2048
        && !(draft.has("experience") && draft.has("experiences"))
        && (draft.has("experience") || draft.has("experiences") || draft.has("controls")), "gepa_draft_invalid");
    auto values = before;
    Json changes = draft.has("experiences") ? draft["experiences"] : Json(Json::Array{});
    if(draft.has("experience")) changes.push(draft["experience"]);
    require(changes.array() && changes.items().size() <= before.fields().size(), "gepa_draft_invalid");
    std::set<std::string> unique;
    for(const auto& change : changes.items()) {
      keys(change, {"memory_id", "content", "trigger_pattern", "observed_operations"});
      require(change.object() && change.has("memory_id") && change.has("content")
          && !change.str("memory_id").empty()
          && (change["content"].null() || (change["content"].string()
              && !change["content"].text().empty()
              && change["content"].text().size() <= config.num("max_experience_bytes", 2048))),
          "gepa_draft_invalid");
      const auto key = "experience:" + change.str("memory_id");
      require(values.has(key), "gepa_experience_not_authorized");
      require(unique.insert(key).second, "gepa_draft_invalid");
      if(change["content"].null()) {
        require(!change.has("trigger_pattern") && !change.has("observed_operations"), "gepa_draft_invalid");
        values[key] = nullptr; continue;
      }
      auto value = values[key];
      if(value.null()) {
        const auto sources = source_experiences(seed);
        for(const auto& source : sources.items())
          if(source.str("memory_id") == change.str("memory_id")) value = experience_component(source);
      }
      if(value.object()) value["content"] = change["content"];
      else value = change["content"];
      for(const auto* field : {"trigger_pattern", "observed_operations"}) if(change.has(field)) {
        if(!value.object()) value = Json{{"content", value}};
        value[field] = change[field];
      }
      if(value.has("trigger_pattern")) require(value["trigger_pattern"].string()
          && value.str("trigger_pattern").size() <= 2048, "gepa_draft_invalid");
      if(value.has("observed_operations")) {
        require(value["observed_operations"].array() && value["observed_operations"].items().size() <= 16, "gepa_draft_invalid");
        std::set<std::string> operations;
        for(const auto& operation : value["observed_operations"].items())
          require(operation.string() && identifier(operation.text()) && operations.insert(operation.text()).second, "gepa_draft_invalid");
        value["observed_operations"] = Json::Array{};
        for(const auto& operation : operations) value["observed_operations"].push(operation);
      }
      values[key] = value;
    }
    if(draft.has("controls")) {
      keys(draft["controls"], {"action_rules", "loop_rules", "schedule_rules"});
      require(draft["controls"].object() && !draft["controls"].fields().empty(), "gepa_draft_invalid");
      for(const auto& item : draft["controls"].fields()) {
        require(item.second.array() && (values.has(item.first) || item.second.items().empty()), "gepa_component_not_authorized");
        if(values.has(item.first)) values[item.first] = item.second;
      }
    }
    std::set<std::string> allowed; for(const auto& item : selected.items()) allowed.insert(item.text());
    for(const auto& item : values.fields()) if(item.second.dump() != before[item.first].dump())
      require(allowed.count(item.first), "gepa_component_not_authorized");
    // 空草稿是合法弃权，不是一个新 Agent 版本；必须保留原始封装及其公开版本标识。
    if(values.dump() == before.dump()) return parent;
    const auto candidate = compose(parent, values);
    return components(candidate).dump() == before.dump() ? parent : candidate;
  }
  Json reflect(const Json& input, std::size_t node, const std::string& suffix = {}, bool aggregate = false) {
    const auto prepared = observed_operation_bindings(input, config.num("max_input_bytes", 65536));
    require(prepared.dump().size() <= config.num("max_input_bytes", 65536), "gepa_feedback_budget_too_small");
    search_check(); require(tree.num("model_calls", 0) < config.num("max_model_calls", 8), "gepa_model_budget_exhausted");
    tree["model_calls"] = tree.num("model_calls", 0) + 1;
    tree["nodes"].items()[node]["status"] = "reflecting"; save();
    auto adapter = model;
    if(!adapter) {
      auto runtime = legacy::load_runtime_configuration(config.str("runtime_config")); auto http = runtime.model;
      if(http.endpoint.empty()) if(const auto* base = std::getenv("OPENAI_BASE_URL")) {
        http.endpoint = base; while(!http.endpoint.empty() && http.endpoint.back() == '/') http.endpoint.pop_back(); http.endpoint += "/chat/completions";
      }
      if(http.model_id.empty()) if(const auto* name = std::getenv("OPENAI_MODEL")) http.model_id = name;
      require(!http.endpoint.empty() && !http.model_id.empty(), "gepa_model_not_configured");
      if(!runtime.no_api_key) {
        const auto* key = std::getenv((runtime.api_key_env.empty() ? "OPENAI_API_KEY" : runtime.api_key_env).c_str());
        require(key && *key, "gepa_model_key_missing"); http.api_key = key;
      }
      http.max_retries = 0; // One reserved reflection call is one transport attempt.
      search_check();
      http.timeout_ms = std::max<std::size_t>(1, std::min<std::size_t>(http.timeout_ms,
          deadline() - holdout_time() - now()));
      const auto cutoff = deadline() - holdout_time();
      http.cancelled = [&, cutoff] { return (cancelled && cancelled()) || now() >= cutoff; };
      http.max_request_bytes = std::max<std::size_t>(http.max_request_bytes, config.num("max_input_bytes", 65536) * 2);
      adapter = legacy::make_openai_compatible_model_adapter(http);
    }
    legacy::ModelRequest request;
    const auto name = std::to_string(node) + suffix;
    request.packet_id = "gepa-" + context.substr(0, 16) + "-" + name;
    request.system_prompt = read(config.str(aggregate ? "aggregation_prompt_file" : "prompt_file"), 65536); request.user_prompt = prepared.dump();
    validate_local_data(Json(request.system_prompt));
    write(root / "reflections" / (name + ".request.json"), Json{{"system", request.system_prompt}, {"input", prepared}}.dump());
    const auto call_started = std::chrono::steady_clock::now();
    const auto response = adapter->evaluate(request);
    const auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - call_started).count();
    tree["reflection_calls"][std::to_string(tree.num("model_calls", 0))] =
        reflection_call_diagnostic(response, static_cast<std::size_t>(duration_ms));
    save(); search_check();
    if(response.error_code == "http_429") {
      // 供应商拒绝本次调用时，保留已预留预算和完成的执行检查点，重试同一代生成。
      tree["nodes"].items()[node]["status"] = "pending";
      save(); throw Error("gepa_model_rate_limited");
    }
    require(response.transport_ok && response.error_code.empty() && response.body.size() <= 262144, "gepa_reflection_transport_failed");
    bool response_recovered = false, content_recovered = false;
    auto body = parse_model_object(response.body, &response_recovered);
    if(body.has("choices")) {
      require(body["choices"].array() && body["choices"].items().size() == 1, "gepa_reflection_choice_invalid");
      const auto& choice = body["choices"].items().front();
      require(choice.str("finish_reason") == "stop", "gepa_reflection_incomplete");
      body = parse_model_object(choice["message"].str("content"), &content_recovered);
    }
    if(response_recovered || content_recovered) {
      tree["nodes"].items()[node]["response_recovery"] = Json{{"method", "single_json_object"},
          {"provider_envelope", response_recovered}, {"message_content", content_recovered}};
      save();
    }
    validate_local_data(body);
    Json binding_error;
    try { return resolve_control_binding_updates(body, prepared, &binding_error); }
    catch(const Error&) {
      // 固定码与可信范围足以纠正绑定；不落盘模型原文、来源 ID 或自由文本。
      if(binding_error.object()) {
        tree["reflection_calls"][std::to_string(tree.num("model_calls", 0))]["control_binding_error"] = binding_error;
        save();
      }
      throw;
    }
  }
  Json propose(const Json& parent, const Json& input, std::size_t node, Json& pending) {
    const auto n = input["feedback"].items().size(), k = static_cast<std::size_t>(std::sqrt(double(n)));
    if(config.str("reflection_strategy", "single") != "combee" || k < 2) return reflect(input, node);
    const auto& selected = input["selected_components"];
    const auto needed = selected.items().size() * (k + 1);
    require(tree.num("model_calls", 0) + needed <= config.num("max_model_calls", 8), "gepa_model_budget_exhausted");
    auto values = components(parent);
    Json drafts{{"reason", "分组反思并汇总训练证据，保留各组件的受限改进。"}, {"experiences", Json::Array{}}};
    Json stages = Json::Array{};
    std::mt19937 random(static_cast<unsigned>(config.num("seed", 42) + node));
    std::size_t component_index = 0;
    for(const auto& component : selected.items()) {
      const auto key = component.text(), prefix = ".combee-" + std::to_string(component_index++);
      std::vector<std::size_t> shuffled;
      for(std::size_t repeat = 0; repeat < config.num("duplication_factor", 2); ++repeat)
        for(std::size_t i = 0; i < n; ++i) shuffled.push_back(i);
      std::shuffle(shuffled.begin(), shuffled.end(), random);
      Json intermediates = Json::Array{};
      const Json one = Json::Array{key};
      for(std::size_t group = 0; group < k; ++group) {
        auto packet = input; restrict_reflection_components(packet, one); packet["feedback"] = Json::Array{};
        packet["phase"] = "combee_map";
        for(std::size_t i = group; i < shuffled.size(); i += k) packet["feedback"].push(input["feedback"].items()[shuffled[i]]);
        // 打乱重复记录后，某个分组可能比原始包更大；仍保留具体反馈，但必须遵守相同字节预算。
        if(packet.dump().size() > config.num("max_input_bytes", 65536)) {
          for(auto& row : packet["feedback"].items()) {
            const auto trial = row["trial"];
            row["trial"] = Json{{"verdict", trial["verdict"]}, {"checks", trial["checks"]},
                {"output", trial.has("output") ? trial["output"] : trial["evidence"]["output"]},
                {"metrics", trial["metrics"]}, {"evidence_truncated", true}};
          }
          while(packet["feedback"].items().size() > 1 && packet.dump().size() > config.num("max_input_bytes", 65536)) {
            packet["feedback"].items().pop_back(); packet["feedback_truncated"] = true;
          }
        }
        const auto suffix = prefix + "-map-" + std::to_string(group);
        const auto draft = reflect(packet, node, suffix);
        // 分组校验失败时保留真实草稿供一次受限修复，不重跑已完成的分组。
        pending = draft;
        write(root / "reflections" / (std::to_string(node) + suffix + ".draft.json"), draft.dump());
        const auto candidate = mutate(parent, draft, one);
        intermediates.push(Json{{"reason", draft["reason"]}, {"component", key}, {"value", components(candidate)[key]}});
        stages.push(Json{{"component", key}, {"phase", "map"}, {"file", std::to_string(node) + suffix}, {"feedback_records", packet["feedback"].items().size()}});
        tree["nodes"].items()[node]["reflection_stages"] = stages; save();
      }
      auto packet = input; packet.fields().erase("feedback");
      packet["phase"] = "combee_reduce"; restrict_reflection_components(packet, one); packet["intermediate_proposals"] = intermediates;
      const auto draft = reflect(packet, node, prefix + "-reduce", true);
      pending = draft;
      write(root / "reflections" / (std::to_string(node) + prefix + "-reduce.draft.json"), draft.dump());
      const auto candidate = mutate(parent, draft, one); values[key] = components(candidate)[key];
      stages.push(Json{{"component", key}, {"phase", "reduce"}, {"file", std::to_string(node) + prefix + "-reduce"}, {"groups", k}});
      tree["nodes"].items()[node]["reflection_stages"] = stages; save();
      if(key.compare(0, 11, "experience:") == 0) {
        auto change = values[key].object() ? values[key] : Json{{"content", values[key]}};
        change["memory_id"] = key.substr(11); drafts["experiences"].push(change);
      } else drafts["controls"][key] = values[key];
    }
    return drafts;
  }
  bool attributed_validation_gain(std::size_t index) const {
    if(index == 0) return true;
    const auto& node = tree["nodes"].items().at(index);
    // 训练应用证据允许保留分支；选优时必须在实际改变行为的验证样本上有收益。
    // 未触发样本仍计入完整验证分数，但不能替新增规则证明效果。
    for(const auto parent : gepa::parents(node)) {
      if(parent >= tree["nodes"].items().size()) continue;
      Json candidate = Json::Object{}, previous = Json::Object{};
      const auto& prior = tree["nodes"].items()[parent]["validation"];
      const auto& changes = node["validation_applied_changes"][std::to_string(parent)];
      if(!changes.object()) continue;
      for(const auto& task : changes.fields()) {
        bool applied = false;
        for(const auto& boundary : task.second.fields()) applied |= number(boundary.second, 0) > 0;
        if(applied && prior.has(task.first) && node["validation"].has(task.first)) {
          candidate[task.first] = node["validation"][task.first]; previous[task.first] = prior[task.first];
        }
      }
      if(candidate.fields().empty() || !gepa::no_worse(candidate, previous)) continue;
      if(gate.has("max_latency_ratio")) {
        const auto duration = gepa::mean(candidate, "duration_ms"), baseline = gepa::mean(previous, "duration_ms");
        if(duration < 0 || baseline < 0 || duration > baseline * number(gate["max_latency_ratio"], 1) + 1e-12) continue;
      }
      if(gepa::local_gain(candidate, previous, number(config["min_improvement"], 0.000001))) return true;
    }
    return false;
  }
  std::size_t winner() const {
    std::size_t best = 0;
    const auto& nodes = tree["nodes"].items();
    auto preferable = [&](const Json& candidate, const Json& current) {
      const auto& a = candidate["optimization"]; const auto& b = current["optimization"];
      if(a.flag("feasible", false) != b.flag("feasible", false)) return a.flag("feasible", false);
      if(a.flag("passed", false) != b.flag("passed", false)) return a.flag("passed", false);
      const auto goal = number(a["score"]) - number(b["score"]);
      if(std::abs(goal) > 1e-12) return goal > 0;
      const auto robustness = number(a["robustness"]) - number(b["robustness"]);
      if(std::abs(robustness) > 1e-12) return robustness > 0;
      return improves(candidate["validation"], current["validation"]);
    };
    // 默认按验证效果选优；覆盖记录用于诊断，只有显式严格验收才要求全规则、全能力命中。
    const bool strict_coverage = gate.flag("require_level_coverage", false);
    for(std::size_t i = 1; i < nodes.size(); ++i) if(nodes[i].str("status") == "accepted"
        && attributed_validation_gain(i)
        && (!strict_coverage || (nodes[i]["control_coverage"].flag("complete", false)
            && nodes[i]["control_coverage"].flag("level_complete", false)))
        && preferable(nodes[i], nodes[best])) best = i;
    return best;
  }
  bool final_holdout_eligible(std::size_t index) const {
    const auto& nodes = tree["nodes"].items();
    if(index >= nodes.size()) return false;
    const auto& node = nodes[index];
    if(node.str("status") != "accepted" || !node["optimization"].flag("passed", false)) return false;
    if(index && !attributed_validation_gain(index)) return false;
    return !gate.flag("require_level_coverage", false)
        || (node["control_coverage"].flag("complete", false)
            && node["control_coverage"].flag("level_complete", false));
  }
  // 留出候选只按冻结时的验证排序确定；后续留出得分只负责门禁，绝不能重新排序。
  std::vector<std::size_t> final_holdout_candidates() const {
    const auto& nodes = tree["nodes"].items();
    auto preferable = [&](const Json& candidate, const Json& current) {
      const auto& a = candidate["optimization"]; const auto& b = current["optimization"];
      if(a.flag("feasible", false) != b.flag("feasible", false)) return a.flag("feasible", false);
      if(a.flag("passed", false) != b.flag("passed", false)) return a.flag("passed", false);
      const auto goal = number(a["score"]) - number(b["score"]);
      if(std::abs(goal) > 1e-12) return goal > 0;
      const auto robustness = number(a["robustness"]) - number(b["robustness"]);
      if(std::abs(robustness) > 1e-12) return robustness > 0;
      return improves(candidate["validation"], current["validation"]);
    };
    std::vector<std::size_t> candidates;
    std::set<std::string> versions;
    for(std::size_t index = 0; index < nodes.size(); ++index) {
      if(!final_holdout_eligible(index) || nodes[index].str("version").empty()) continue;
      if(versions.insert(nodes[index].str("version")).second) candidates.push_back(index);
    }
    std::sort(candidates.begin(), candidates.end(), [&](const auto left, const auto right) {
      if(preferable(nodes[left], nodes[right])) return true;
      if(preferable(nodes[right], nodes[left])) return false;
      return left < right;
    });
    const auto requested = final_holdout_top_k_override ? final_holdout_top_k_override
        : config.num("holdout_top_k", 1);
    if(candidates.size() > requested) candidates.resize(requested);
    return candidates;
  }
  void proposal(std::size_t index, const Json& candidate) {
    auto& node = tree["nodes"].items()[index]; const auto version = agent_version_id(candidate.dump());
    node["version"] = version; node["status"] = "proposed";
    auto executable = [&](const Json& artifact) {
      auto values = components(artifact);
      for(const auto* category : {"action_rules", "loop_rules", "schedule_rules"})
        if(values[category].array()) for(auto& rule : values[category].items()) rule.fields().erase("reason");
      return values.dump();
    };
    const auto behavior = executable(candidate);
    for(std::size_t j = 0; j < index; ++j) {
      const auto& previous = tree["nodes"].items()[j];
      if(previous.str("version").empty()) continue;
      // 组件经 Protobuf 读取时，省略字段与显式默认值等价，不能使既有导出包失效。
      if(previous.str("version") == version || executable(artifact(j)) == behavior) {
        node["duplicate_of"] = j; node["status"] = "duplicate";
        node["skip_reason"] = components(artifact(j)).dump() == components(candidate).dump()
            ? "no_behavior_change" : "no_control_predicate_change"; break;
      }
    }
    write(root / "versions" / (std::to_string(index) + ".json"), candidate.dump()); save();
  }
  void error(std::size_t index, const std::exception& exception) {
    const std::string code = exception.what();
    if(code == "gepa_cancelled" || code == "gepa_time_budget_exhausted"
        || code == "gepa_model_rate_limited") { save(); throw Error(code.c_str()); }
    auto& node = tree["nodes"].items()[index]; node["error_stage"] = node["status"]; node["status"] = "error";
    // 只持久化固定错误码；模型/传输诊断可能包含密钥等敏感信息。
    const std::set<std::string> codes{"gepa_trial_budget_exhausted", "gepa_search_time_reserved", "gepa_parent_evaluation_inconclusive",
        "gepa_feedback_budget_too_small", "gepa_reflection_transport_failed", "gepa_reflection_incomplete",
        "gepa_experience_not_authorized", "gepa_experience_limit", "gepa_draft_invalid", "gepa_candidate_not_authorized",
        "gepa_model_not_configured", "gepa_model_key_missing",
          "gepa_final_holdout_legacy_replay_required", "gepa_final_holdout_plan_changed", "gepa_holdout_baseline_changed", "credential_value_rejected", "sensitive_field_rejected",
        "invalid_json", "unknown_field", "control_operation_not_authorized", "control_argument_not_authorized", "control_loop_relaxes_limit",
        "control_source_not_approved", "control_base_loop_required", "control_loop_semantic_scope_required", "control_failure_replan_not_authorized",
        "control_draft_limits_invalid", "control_duplicate_loop_scope", "control_binding_invalid",
        "control_loop_relaxes_condition", "control_loop_condition_invalid",
        "gepa_component_not_authorized", "gepa_components_changed",
        "gepa_model_budget_exhausted"};
    node["error"] = codes.count(code) ? code : "gepa_proposal_or_evaluation_failed";
    if(code == "gepa_trial_budget_exhausted" || code == "gepa_search_time_reserved") tree["stop_reason"] = code;
    if(code == "gepa_model_budget_exhausted") tree["reflection_exhausted"] = true;
    if(code == "gepa_model_not_configured" || code == "gepa_model_key_missing") {
      tree["status"] = "error"; tree["active"] = nullptr; save(); throw Error(code.c_str());
    }
    save();
  }
  void validate(std::size_t index) {
    try {
      auto& node = tree["nodes"].items()[index];
      const auto& screened = static_cast<const Json&>(node)["screening"];
      // 续跑旧 screened 节点也先核对真实差分子集，不用无改动题的随机收益消耗验证预算。
      // merge 使用多父验证筛选；缺少旧字段时不把未记录的证据推断成零。
      if(node.str("kind") != "merge" && screened.object() && screened.str("split", "train") == "train"
          && screened["parent"].object() && screened["candidate"].object() && screened["applied_changes"].object()
          && !observed_training_gain(screened["candidate"], screened["parent"], screened["applied_changes"])) {
        node["status"] = "rejected_minibatch"; node["reason"] = "no_observed_training_gain"; save(); return;
      }
      const auto candidate = artifact(index);
      // 未改变的规则继承任一父节点已建立的证据；改变后的规则拥有新的 rule_id，
      // 因而必须在当前节点重新观测。
      Json inherited{{"rules", Json::Object{}}};
      for(const auto parent : gepa::parents(node)) {
        const auto& parent_node = static_cast<const Json&>(tree)["nodes"].items()[parent];
        if(!parent_node["control_coverage"]["rules"].object()) continue;
        for(const auto& rule : parent_node["control_coverage"]["rules"].fields()) {
          auto& counts = inherited["rules"][rule.first];
          for(const auto* field : {"queries", "triggered", "applied"})
            counts[field] = std::max(counts.num(field, 0), rule.second.num(field, 0));
        }
      }
      const auto& screening = static_cast<const Json&>(node)["screening"]["control_coverage"];
      const auto training = merge_control_coverage(screening.object() ? screening : Json{{"rules", Json::Object{}}}, inherited);
      const auto probe_size = std::min(val.size(), config.num("minibatch_size", 4));
      if(!control_changes_observed(training, inherited) && probe_size < val.size() && config.flag("cache_evaluations", true)) {
        // 为训练阶段未命中的控制规则提供一次验证机会；若此处命中，完整验证可复用
        // 已缓存 trial，避免额外执行。
        const std::vector<std::string> probe(val.begin(), val.begin() + probe_size);
        const auto report = evaluate(candidate, probe, "val");
        const auto coverage = control_coverage(candidate, report);
        const auto values = scores(report);
        node["validation_probe"] = Json{{"scores", values}, {"control_coverage", coverage}};
        for(const auto& rule : coverage["rules"].fields()) {
          const auto& previous = training["rules"][rule.first];
          if(!inherited["rules"].has(rule.first) && !values.fields().empty()
              && previous.num("queries", 0) && !previous.num("applied", 0)
              && rule.second.num("queries", 0) && !rule.second.num("applied", 0)) {
            // 即使另一条规则被淘汰，已执行的训练证据也不能丢失。
            node["control_coverage"] = merge_control_coverage(coverage, training);
            node["status"] = "unobserved_controls";
            node["reason"] = "control_unobserved_in_train_and_validation"; save(); return;
          }
        }
        save();
      }
      const auto report = evaluate(candidate, val, "val"); auto values = scores(report);
      node["validation"] = values;
      node["validation_applied_changes"] = Json::Object{};
      for(const auto parent : gepa::parents(node))
        node["validation_applied_changes"][std::to_string(parent)] = applied_changes(artifact(parent), report);
      if(!values.fields().empty()) node["optimization"] = annotate_validation(node["validation"]);
      node["validation_control_coverage"] = control_coverage(candidate, report);
      node["control_coverage"] = merge_control_coverage(node["validation_control_coverage"], training);
      bool changed = node["screening"]["applied_changes"].object() && !node["screening"]["applied_changes"].fields().empty();
      for(const auto& parent : node["validation_applied_changes"].fields()) changed |= !parent.second.fields().empty();
      node["status"] = values.fields().empty() ? "inconclusive"
          : !control_changes_observed(node["control_coverage"], inherited) ? "unobserved_controls"
          : changed ? "accepted" : "unobserved_changes";
      node["validation_gain_attributed"] = attributed_validation_gain(index);
      if(node.str("status") == "unobserved_changes") node["reason"] = "no_observed_decision_change";
      save();
    } catch(const std::exception& exception) { error(index, exception); }
  }
  void update_frontier(std::size_t previous_best, const std::string& previous_frontier) {
    Json ids = Json::Array{}; for(auto i : frontier()) ids.push(i);
    tree["stagnation"] = winner() == previous_best && ids.dump() == previous_frontier ? tree.num("stagnation", 0) + 1 : 0;
    tree["frontier"] = ids;
    tree["frontier_mapping"] = gepa::frontier_map(tree["nodes"], val, config); save();
  }
  void finish_batch() {
    auto& batch = tree["batch"];
    if(!batch.has("selected")) {
      batch["selected"] = Json::Array{};
      const auto selected = gepa::select_proposals(tree["nodes"], batch["nodes"], config);
      std::set<std::size_t> kept(selected.begin(), selected.end());
      for(auto index : selected) batch["selected"].push(index);
      for(const auto& id : batch["nodes"].items()) {
        auto& node = tree["nodes"].items()[id.integer()];
        if(node.str("status") == "screened" && !kept.count(id.integer())) node["status"] = "not_selected";
      }
      save();
    }
    for(const auto& id : batch["selected"].items()) {
      auto& node = tree["nodes"].items()[id.integer()];
      if(node.str("status") != "screened") continue;
      if(tree.str("stop_reason") == "gepa_trial_budget_exhausted" || tree.str("stop_reason") == "gepa_search_time_reserved") {
        node["status"] = "skipped_budget"; continue;
      }
      validate(id.integer());
      if(node.str("status") == "accepted") tree["merge_due"] = true;
    }
    const auto previous = batch.num("previous_best", 0); const auto old = batch.str("previous_frontier");
    tree["batch"] = nullptr; update_frontier(previous, old);
  }
  std::vector<std::string> merge_batch(std::size_t first, std::size_t second) const {
    std::vector<std::string> buckets[3], result;
    const auto& a = tree["nodes"].items()[first]["validation"];
    const auto& b = tree["nodes"].items()[second]["validation"];
    for(const auto& task : val) {
      require(a.has(task) && b.has(task), "gepa_merge_validation_missing");
      const auto difference = number(a[task]["fitness"]) - number(b[task]["fitness"]);
      buckets[difference > 1e-12 ? 0 : difference < -1e-12 ? 1 : 2].push_back(task);
    }
    std::mt19937 random(static_cast<unsigned>(config.num("seed", 42) + tree.num("rounds", 0)));
    for(auto& bucket : buckets) std::shuffle(bucket.begin(), bucket.end(), random);
    const auto count = std::min(val.size(), config.num("merge_minibatch_size", 4));
    while(result.size() < count) for(auto& bucket : buckets)
      if(!bucket.empty() && result.size() < count) { result.push_back(bucket.back()); bucket.pop_back(); }
    std::sort(result.begin(), result.end()); return result;
  }
  void merge(std::size_t index) {
    auto& node = tree["nodes"].items()[index]; const auto ids = gepa::parents(node);
    if(node.str("status") == "pending") {
      const auto a = artifact(ids[0]), b = artifact(ids[1]), base = artifact(node.num("ancestor", 0));
      const auto combined = gepa::recombine(components(base), components(a), components(b), ids[0], ids[1],
          !improves(tree["nodes"].items()[ids[1]]["validation"], tree["nodes"].items()[ids[0]]["validation"]));
      node["component_sources"] = combined["sources"]; node["conflicts"] = combined["conflicts"];
      if(!combined.flag("useful", false)) { node["status"] = "skipped_merge"; node["reason"] = "no_complementary_components"; save(); return; }
      proposal(index, compose(a, combined["components"]));
    }
    if(node.str("status") == "proposed") {
      std::vector<std::string> tasks; for(const auto& id : node["tasks"].items()) tasks.push_back(id.text());
      const auto report = evaluate(artifact(index), tasks, "val"); const auto values = scores(report);
      Json first = Json::Object{}, second = Json::Object{};
      for(const auto& id : tasks) {
        first[id] = tree["nodes"].items()[ids[0]]["validation"][id];
        second[id] = tree["nodes"].items()[ids[1]]["validation"][id];
      }
      node["screening"] = Json{{"parents", Json::Array{first, second}}, {"candidate", values}, {"split", "val"}};
      const auto changes_a = applied_changes(artifact(ids[0]), report), changes_b = applied_changes(artifact(ids[1]), report);
      node["screening"]["parent_applied_changes"] = Json::Array{changes_a, changes_b};
      const auto retained = [&](const Json& parent, const Json& changes) {
        return gepa::no_worse(values, parent) || (config.str("acceptance") == "pareto"
            && observed_local_gain(values, parent, changes));
      };
      node["status"] = values.fields().empty() ? "inconclusive"
          : retained(first, changes_a) && retained(second, changes_b) ? "screened" : "rejected_merge";
      save();
    }
    if(node.str("status") == "screened") validate(index);
  }
  GepaResult run() {
    dataset = read_evaluation_dataset(dataset_path);
    for(const auto& issue : dataset.issues()) require(issue.severity() != "error", "gepa_dataset_invalid");
    for(const auto& task : dataset.tasks()) {
      require(task.review_status() == "reviewed", "gepa_dataset_review_required");
      if(task.split() == "train") train.push_back(task.task_id());
      else if(task.split() == "val") val.push_back(task.task_id());
      else if(task.split() == "holdout") holdout.push_back(task.task_id());
    }
    require(!train.empty() && !val.empty() && !holdout.empty(), "gepa_three_splits_required");
    require(!source_experiences(seed).items().empty(), "gepa_approved_experiences_required");
    const auto finalist_budget_count = final_holdout_top_k_override ? final_holdout_top_k_override
        : config.num("holdout_top_k", 1);
    const auto holdout_trials = holdout.size() * evaluation.num("repetitions", 1);
    // 先为所有最终候选及可能需要的 L0 对照预留 trial，搜索阶段不能透支这部分预算。
    reserve = holdout_trials * (config.flag("cache_evaluations", true) ? finalist_budget_count + 1 : finalist_budget_count * 2);
    require(config.num("max_trials", 400) >= reserve + val.size() * evaluation.num("repetitions", 1) * 2,
        "gepa_holdout_budget_required");
    if(fs::exists(root / "tree.json")) {
      tree = Json::parse(read(root / "tree.json", 16777216));
      require((tree.num("schema_version", 0) == 1 || tree.num("schema_version", 0) == 2) && tree.str("context") == context
          && tree.str("seed_version") == agent_version_id(seed.dump())
          && tree.str("baseline_version") == agent_version_id(baseline.dump()), "gepa_resume_context_changed");
      require(tree["nodes"].array() && !tree["nodes"].items().empty()
          && tree["nodes"].items().size() <= config.num("max_rounds", 8) + 1, "gepa_tree_invalid");
      gepa::validate_nodes(tree["nodes"]);
      require(tree.num("rounds", SIZE_MAX) == tree["nodes"].items().size() - 1
          && tree.num("model_calls", SIZE_MAX) <= config.num("max_model_calls", 8)
          && tree.num("trials_used", SIZE_MAX) <= config.num("max_trials", 400), "gepa_tree_invalid");
      if(!tree["active"].null()) require(tree["active"].integer() > 0
          && tree["active"].integer() < tree["nodes"].items().size(), "gepa_tree_invalid");
      if(tree.has("batch") && !tree["batch"].null()) {
        const auto& batch = tree["batch"]; std::set<std::size_t> ids;
        require(batch.object() && batch["nodes"].array() && batch.num("limit", 0) > 0
            && batch.num("limit", 0) <= 8 && batch["nodes"].items().size() <= batch.num("limit", 0), "gepa_tree_invalid");
        for(const auto& id : batch["nodes"].items()) require(id.integer() > 0
            && id.integer() < tree["nodes"].items().size() && ids.insert(id.integer()).second
            && tree["nodes"].items()[id.integer()].str("kind") == "reflection", "gepa_tree_invalid");
        if(batch.has("plans")) {
          require(batch["plans"].array() && batch["plans"].items().size() == batch.num("limit", 0), "gepa_tree_invalid");
          for(const auto& plan : batch["plans"].items()) {
            require(plan.num("parent", SIZE_MAX) < tree["nodes"].items().size()
                && tree["nodes"].items()[plan.num("parent", 0)].str("status") == "accepted"
                && plan["tasks"].array() && !plan["tasks"].items().empty() && plan["components"].array(), "gepa_tree_invalid");
            for(const auto& task : plan["tasks"].items()) require(std::find(train.begin(), train.end(), task.text()) != train.end(), "gepa_tree_invalid");
          }
        }
        if(batch.has("selected")) for(const auto& id : batch["selected"].items()) require(ids.count(id.integer()), "gepa_tree_invalid");
      }
      for(auto& node : tree["nodes"].items()) if(node.str("status") == "reflecting") {
        node["status"] = "interrupted"; node["reason"] = "reflection_outcome_unknown"; tree["active"] = nullptr;
      }
      resume_clock(now()); save();
    } else {
      tree = Json{{"schema_version", 2}, {"change_evidence_revision", 1},
          {"algorithm", "gepa-reflective-dag-v4-constrained-portfolio"}, {"context", context},
          {"seed_version", agent_version_id(seed.dump())}, {"baseline_version", agent_version_id(baseline.dump())},
          {"dataset_version", dataset.dataset_version()}, {"started_ms", now()}, {"rounds", 0}, {"model_calls", 0},
          {"trials_used", 0}, {"component_cursor", 0}, {"active", nullptr}, {"evaluations", Json::Object{}}, {"nodes", Json::Array{
            Json{{"id", 0}, {"parent", nullptr}, {"parents", Json::Array{}}, {"kind", "seed"}, {"depth", 0},
                {"version", agent_version_id(seed.dump())}, {"status", "seed"}, {"selections", 0}}}}};
      write(root / "versions/0.json", seed.dump()); save();
    }
    if(!tree.has("merge_attempts")) tree["merge_attempts"] = Json::Array{};
    if(!tree.has("batch")) tree["batch"] = nullptr;
    if(!tree.has("component_cursor")) tree["component_cursor"] = 0;
    if(!tree["component_selections"].object()) tree["component_selections"] = Json::Object{};
    if(!tree["training_opportunities"].object()) tree["training_opportunities"] = Json::Object{};
    tree["schema_version"] = 2; tree["algorithm"] = "gepa-reflective-dag-v4-constrained-portfolio";
    tree["optimization_contract"] = gate;
    tree["available_components"] = Json::Array{};
    const auto available = components(seed);
    for(const auto& item : available.fields()) tree["available_components"].push(item.first);
    tree["search_warnings"] = Json::Array{};
    tree["fixed_controls"] = Json::Array{};
    if(proto<v1::RuntimePolicy>(seed["policy"]).options().requested_level() >= v1::L2)
      tree["fixed_controls"].push("observed_tool_required_arguments");
    if(tree["nodes"].items()[0].str("status") == "seed") {
      auto& root_node = tree["nodes"].items()[0];
      if(!root_node["validation"].object() || root_node["validation"].fields().empty()) {
        const auto report = evaluate(seed, val, "val"); const auto values = scores(report);
        require(!values.fields().empty(), "gepa_seed_evaluation_inconclusive");
        root_node["validation"] = values;
        root_node["control_coverage"] = control_coverage(seed, report);
        save();
      }
    }
    // 在为 L0 参考额外花费固定评测前，先验证可部署种子；验证内容始终不进入任何模型包。
    if(!tree.has("baseline_validation")) {
      Json task_ids = Json::Array{};
      for(const auto& id : val) task_ids.push(id);
      const Json identity{{"schema_version", 1}, {"evaluation_context", evaluation_context},
          {"deterministic_contracts", "schedule-success-v1"},
          {"baseline_version", agent_version_id(baseline.dump())}, {"dataset_version", dataset.dataset_version()},
          {"task_ids", task_ids}, {"objective", config["objective"]},
          {"unknown_token_policy", config.str("unknown_token_policy", "reject")}, {"gate", gate}};
      const auto key = hash(identity.dump());
      const auto cache_path = root.parent_path().parent_path() / "baseline-cache" / (key + ".json");
      Json values; bool hit = false;
      bool imported = false;
      if(config.has("baseline_report_file")) {
        const auto bytes = read(config.str("baseline_report_file"), 134217728);
        const auto report = proto<Report>(Json::parse(bytes));
        const auto source = json(report.configuration());
        auto judge_path = fs::path(evaluation["judge"].str("runtime_config"));
        if(judge_path.is_relative()) judge_path = evaluation_path.parent_path() / judge_path;
        const auto judge = legacy::load_runtime_configuration(judge_path);
        require(report.complete() && report.dataset_version() == dataset.dataset_version()
            && report.evaluator_version() == "task-evaluator-v1"
            && source["variants"].array() && source["variants"].items().size() == 1
            && source["variants"].items().front().str("version") == agent_version_id(baseline.dump())
            && source["splits"].array() && source["splits"].items().size() == 1
            && source["splits"].items().front().text() == "val"
            && source.str("judge_model") == judge.model.model_id
            && source.flag("model_enabled", false) == evaluation["judge"].flag("enabled", false),
            "gepa_baseline_report_incompatible");
        std::set<std::string> observed;
        for(const auto& trial : report.trials())
          require(trial.variant_id() == "candidate" && trial.split() == "val"
              && trial.repetition() == 1 && observed.insert(trial.task_id()).second,
              "gepa_baseline_report_incompatible");
        values = scores(report);
        require(observed.size() == val.size() && values.fields().size() == val.size(),
            "gepa_baseline_report_incompatible");
        for(const auto& id : val) require(observed.count(id) && values.has(id), "gepa_baseline_report_incompatible");
        hit = imported = true;
      }
      if(!imported && fs::exists(cache_path)) try {
        const auto cached = Json::parse(read(cache_path, 1048576));
        const auto values_bytes = cached.str("values_json");
        values = Json::parse(values_bytes);
        hit = cached["identity"].dump() == identity.dump() && values.object()
            && values.fields().size() == val.size() && cached.str("values_hash") == hash(values_bytes);
        for(const auto& id : val) hit &= values.has(id) && values[id].object()
            && values[id].has("fitness") && values[id].has("quality") && values[id].has("duration_ms");
      } catch(const std::exception&) { hit = false; }
      tree["baseline_cache"] = Json{{"key", key}, {"status", imported ? "imported" : hit ? "hit" : "miss"}};
      if(!hit) {
        const auto report = evaluate(baseline, val, "val"); values = scores(report);
        require(values.fields().size() == val.size(), "gepa_baseline_evaluation_inconclusive");
      }
      tree["baseline_validation"] = values; save();
      if(!hit) try {
        const auto values_bytes = values.dump();
        write(cache_path, Json{{"identity", identity}, {"values_json", values_bytes}, {"values_hash", hash(values_bytes)},
            {"created_ms", now()}}.dump());
      } catch(const std::exception&) {
        tree["baseline_cache"]["status"] = "write_failed"; save();
      }
    }
    if(config.flag("cache_evaluations", true)) {
      const auto pinned = tree["holdout_baseline_cache"];
      const bool locked = pinned.has("source");
      auto cached = holdout_baselines().lookup(pinned);
      if(locked && cached.str("status") != "hit") cached["source"] = pinned["source"];
      tree["holdout_baseline_cache"] = cached;
      if(cached.str("status") == "hit") reserve = holdout_trials * finalist_budget_count;
      save();
      require(!locked || cached.str("status") == "hit", "gepa_holdout_baseline_changed");
    }
    if(tree["nodes"].items()[0].str("status") == "seed") {
      auto& root_node = tree["nodes"].items()[0];
      root_node["optimization"] = annotate_validation(root_node["validation"]);
      root_node["status"] = "accepted"; save();
    } else if(!tree["nodes"].items()[0]["optimization"].object()) {
      tree["nodes"].items()[0]["optimization"] = annotate_validation(tree["nodes"].items()[0]["validation"]);
      save();
    }
    if(!tree.has("frontier") || tree["frontier"].null()) {
      tree["frontier"] = Json::Array{}; for(auto id : frontier()) tree["frontier"].push(id);
      tree["frontier_mapping"] = gepa::frontier_map(tree["nodes"], val, config);
    }
    tree["status"] = "searching";
    tree.fields().erase("error"); tree.fields().erase("finished_ms"); save();
    // batch 将父节点、组件和训练任务一起持久化；中断恢复时必须执行原计划，
    // 不能因前面并行候选的结果改变后续候选的父子关系。
    while(!tree.flag("holdout_started", false) && (!tree["active"].null() || !tree["batch"].null()
        || tree.num("rounds", 0) < config.num("max_rounds", 8))) {
      refresh_change_evidence();
      check();
      if(now() >= deadline() - holdout_time() || tree.str("stop_reason") == "gepa_search_time_reserved") {
        tree["stop_reason"] = "gepa_search_time_reserved";
        if(!tree["active"].null()) tree["nodes"].items()[tree["active"].integer()]["status"] = "skipped_budget";
        tree["active"] = nullptr;
        if(!tree["batch"].null()) finish_batch();
        save(); break;
      }
      if(tree["active"].null() && !tree["batch"].null()
          && (tree["batch"].has("selected") || tree["batch"]["nodes"].items().size() >= tree["batch"].num("limit", 1)
              || tree.num("model_calls", 0) >= config.num("max_model_calls", 8)
              || tree.flag("reflection_exhausted", false)
              || tree.str("stop_reason") == "gepa_trial_budget_exhausted")) {
        finish_batch();
        if(tree.str("stop_reason") == "gepa_trial_budget_exhausted") break;
        continue;
      }
      if(tree["active"].null() && tree["batch"].null()) {
        if(config.has("target_fitness") && mean(tree["nodes"].items()[winner()]["validation"], "quality") >= 1
            && mean(tree["nodes"].items()[winner()]["validation"], "fitness") >= number(config["target_fitness"], 1)) {
          tree["stop_reason"] = "target_fitness"; break;
        }
        if(config.num("patience", 0) && tree.num("stagnation", 0) >= config.num("patience", 0)) { tree["stop_reason"] = "patience"; break; }
        if(config.flag("use_merge", true) && tree.flag("merge_due", false)
            && tree["merge_attempts"].items().size() < config.num("max_merge_invocations", 5)) {
          const auto plan = gepa::select_merge(tree["nodes"], gepa::frontier_map(tree["nodes"], val, config),
              tree["merge_attempts"], config, tree.num("rounds", 0));
          tree["merge_due"] = false;
          if(!plan.null()) {
            const auto a = plan.num("first", 0), b = plan.num("second", 0), index = tree["nodes"].items().size();
            Json node{{"id", index}, {"parent", a}, {"parents", Json::Array{a, b}}, {"ancestor", plan["ancestor"]},
                {"kind", "merge"}, {"status", "pending"}, {"selections", 0}, {"previous_best", winner()},
                {"component_cursor", std::max(tree["nodes"].items()[a].num("component_cursor", 0), tree["nodes"].items()[b].num("component_cursor", 0))},
                {"previous_frontier", tree["frontier"].dump()}, {"depth", std::max(tree["nodes"].items()[a].num("depth", 0), tree["nodes"].items()[b].num("depth", 0)) + 1},
                {"tasks", Json::Array{}}};
            for(const auto& id : merge_batch(a, b)) node["tasks"].push(id);
            for(auto p : {a, b}) tree["nodes"].items()[p]["selections"] = tree["nodes"].items()[p].num("selections", 0) + 1;
            auto attempt = plan; attempt["node"] = index; tree["merge_attempts"].push(attempt);
            tree["nodes"].push(node); tree["active"] = index; tree["rounds"] = tree.num("rounds", 0) + 1; save();
          }
        }
        if(tree["active"].null()) {
          if(tree.flag("reflection_exhausted", false) || tree.num("model_calls", 0) >= config.num("max_model_calls", 8)) { tree["stop_reason"] = "model_budget"; break; }
          const auto sequence = tree.num("training_batches", tree.num("reflection_batches", 0));
          const auto sampling = config.str("sampling_strategy", "same_parent");
          const auto n = sampling == "single" ? 1 : config.num("proposals_per_round", 1);
          const auto total = n * (sampling == "pxn" ? config.num("parents_per_round", 2) : 1);
          const auto count = std::min({total, config.num("max_rounds", 8) - tree.num("rounds", 0),
              config.num("max_model_calls", 8) - tree.num("model_calls", 0)});
          tree["batch"] = Json{{"id", tree.num("reflection_batches", 0)}, {"limit", count}, {"nodes", Json::Array{}}, {"plans", Json::Array{}},
              {"previous_best", winner()}, {"previous_frontier", tree["frontier"].dump()}};
          std::size_t groups = 0;
          while(tree["batch"]["plans"].items().size() < count) {
            const auto p = gepa::select_parent(tree["nodes"], gepa::frontier_map(tree["nodes"], val, config), config, tree.num("rounds", 0) + groups);
            const auto cursor = tree.num("component_cursor", 0);
            Json plan{{"parent", p}, {"tasks", Json::Array{}},
                {"components", gepa::select_components(components(artifact(p)), config.str("component_selector", "round_robin"), cursor)},
                {"component_slot", cursor}, {"component_cursor", cursor + 1}};
            for(const auto& id : component_batch(plan["components"], sequence + groups)) plan["tasks"].push(id);
            tree["component_cursor"] = cursor + 1; ++groups;
            const auto repeats = sampling == "independent" ? 1 : n;
            for(std::size_t i = 0; i < repeats && tree["batch"]["plans"].items().size() < count; ++i) {
              auto proposal = plan; proposal["proposal_ordinal"] = i;
              tree["batch"]["plans"].push(std::move(proposal));
            }
          }
          tree["training_batches"] = sequence + groups;
          tree["reflection_batches"] = tree.num("reflection_batches", 0) + 1; save();
        }
      }
      if(tree["active"].null()) {
        const auto& plan = tree["batch"].has("plans")
            ? tree["batch"]["plans"].items().at(tree["batch"]["nodes"].items().size()) : tree["batch"];
        const auto p = plan.num("parent", 0), index = tree["nodes"].items().size();
        tree["nodes"].items()[p]["selections"] = tree["nodes"].items()[p].num("selections", 0) + 1;
        tree["nodes"].push(Json{{"id", index}, {"parent", p}, {"parents", Json::Array{p}}, {"kind", "reflection"},
            {"depth", tree["nodes"].items()[p].num("depth", 0) + 1}, {"status", "pending"}, {"selections", 0},
            {"component_slot", plan.num("component_slot", 0)},
            {"proposal_ordinal", plan.num("proposal_ordinal", 0)},
            {"component_cursor", plan.num("component_cursor", tree["nodes"].items()[p].num("component_cursor", 0))},
            {"batch", tree["batch"]["id"]}, {"components", plan["components"]}, {"tasks", plan["tasks"]}});
        tree["batch"]["nodes"].push(index);
        tree["active"] = index; tree["rounds"] = tree.num("rounds", 0) + 1; save();
      }
      const auto index = tree["active"].integer(); auto& node = tree["nodes"].items()[index];
      try {
        if(node.str("kind") == "merge") {
          merge(index);
        } else {
          const auto p = node.num("parent", 0); const auto parent_artifact = artifact(p);
          std::vector<std::string> batch;
          if(node.has("tasks")) for(const auto& id : node["tasks"].items()) batch.push_back(id.text());
          else batch = minibatch(index - 1);
          const auto parent_report = evaluate(parent_artifact, batch, "train"); const auto parent_scores = scores(parent_report);
          require(!parent_scores.fields().empty(), "gepa_parent_evaluation_inconclusive");
          remember_opportunities(parent_artifact, parent_report);
          if(node.str("status") == "pending") {
            bool perfect = config.flag("skip_perfect_score", false);
            for(const auto& item : parent_scores.fields()) perfect &= number(item.second["quality"]) >= 1
                && number(item.second["fitness"]) >= number(config["perfect_score"], 1);
            if(perfect) { node["status"] = "skipped_perfect"; save(); }
            else {
              auto selected = node.has("components") ? node["components"] : gepa::select_components(components(parent_artifact), "all", 0);
              const auto available = components(parent_artifact);
              const auto observed = applied_experiences(parent_report);
              auto input = packet(parent_artifact, parent_report, selected);
              if(config.str("component_selector", "round_robin") == "round_robin" && selected.items().size() == 1) {
                // 已应用经验与停用经验的恢复探索保留轮次；其他未应用经验再让位。
                const auto current = selected.items().front().text();
                const bool current_control = current == "schedule_rules" || current == "action_rules" || current == "loop_rules";
                if(current_control ? !control_evidence(input, current)
                    : !observed.count(current) && !available[current].null()) {
                  for(const auto* component : {"schedule_rules", "action_rules", "loop_rules"}) {
                    if(!available.has(component) || tree["component_selections"].num(component, 0)) continue;
                    const Json option = Json::Array{component};
                    Json probe;
                    try { probe = packet(parent_artifact, parent_report, option); }
                    catch(const Error& error) {
                      if(std::string(error.what()) == "gepa_feedback_budget_too_small") continue;
                      throw;
                    }
                    if(!control_evidence(probe, component)) continue;
                    if(current != component)
                      node["component_fallback"] = Json{{"from", current}, {"to", component}, {"reason", "unexplored_control_opportunity"}};
                    selected = option; node["components"] = selected; input = std::move(probe); break;
                  }
                }
              }
              if(config.str("component_selector", "round_robin") == "round_robin"
                  && selected.items().size() == 1
                  && (selected.items().front().text() == "action_rules" || selected.items().front().text() == "loop_rules"
                      || selected.items().front().text() == "schedule_rules")
                  && !control_evidence(input, selected.items().front().text())) {
                const auto skipped = selected.items().front().text();
                tree["deferred_components"][skipped] = tree["deferred_components"].num(skipped, 0) + 1;
                const auto slot = node.num("component_slot", 0);
                if(route_control_opportunity(parent_artifact, parent_report, available, slot, skipped, selected, input)) {
                  node["components"] = selected;
                  node["component_fallback"] = Json{{"from", skipped}, {"to", selected.items().front()},
                      {"reason", "executable_control_opportunity"}};
                  save();
                } else {
                  auto fallback = observed_experience(available, observed);
                  if(fallback.items().empty()) for(std::size_t offset = 1; offset <= available.fields().size() * 3; ++offset) {
                    const auto candidate = gepa::select_components(available, "round_robin", slot + offset);
                    if(candidate.items().front().text() == "action_rules" || candidate.items().front().text() == "loop_rules"
                        || candidate.items().front().text() == "schedule_rules") continue;
                    fallback = candidate; break;
                  }
                  if(!fallback.items().empty()) {
                    selected = fallback; node["components"] = selected;
                    node["component_fallback"] = Json{{"from", skipped}, {"to", selected.items().front()},
                        {"reason", "no_executable_training_evidence"}};
                    input = packet(parent_artifact, parent_report, selected); save();
                  }
                }
              }
              if(config.str("component_selector", "round_robin") == "round_robin"
                  && selected.items().size() == 1 && selected.items().front().text().rfind("experience:", 0) == 0
                  && !available[selected.items().front().text()].null()
                  && !observed.count(selected.items().front().text())) {
                const auto skipped = selected.items().front().text();
                const auto slot = node.num("component_slot", 0);
                if(route_control_opportunity(parent_artifact, parent_report, available, slot, skipped, selected, input)) {
                  node["components"] = selected;
                  node["component_fallback"] = Json{{"from", skipped}, {"to", selected.items().front()},
                      {"reason", "executable_control_opportunity"}};
                  save();
                } else {
                  const auto fallback = observed_experience(available, observed);
                  if(!fallback.items().empty()) {
                    selected = fallback; node["components"] = selected;
                    node["component_fallback"] = Json{{"from", skipped}, {"to", selected.items().front()},
                        {"reason", "experience_not_applied_in_training"}};
                    input = packet(parent_artifact, parent_report, selected); save();
                  }
                }
              }
              // 使用同一训练批次已有的证据，不额外调用模型、不扩展可执行权限。
              if(selected.items().size() == 1 && available.has("action_rules") && available.has("loop_rules")
                  && (selected.items().front().text() == "action_rules" || selected.items().front().text() == "loop_rules")) {
                const auto coupled = Json(Json::Array{"action_rules", "loop_rules"});
                const auto joint_input = packet(parent_artifact, parent_report, coupled);
                const auto next = recovery_components(selected, joint_input, available);
                if(next.items().size() > selected.items().size()) {
                  selected = next; node["components"] = selected; input = joint_input;
                  node["component_coupling"] = "same_branch_failure_recovery";
                }
              }
              if(!node.flag("component_reserved", false)) {
                for(const auto& component : selected.items()) tree["component_selections"][component.text()]
                    = tree["component_selections"].num(component.text(), 0) + 1;
                node["component_reserved"] = true; save();
              }
              Json draft, candidate, accepted;
              Json contract_repairs;
              try {
                bool paired = false;
                if(node.num("proposal_ordinal", 0) == 1 && tree["batch"].object())
                  for(const auto& sibling_id : tree["batch"]["nodes"].items()) {
                    if(sibling_id.integer() >= index) continue;
                    const auto& sibling = tree["nodes"].items()[sibling_id.integer()];
                    paired |= sibling.num("parent", SIZE_MAX) == node.num("parent", SIZE_MAX)
                        && sibling["components"].dump() == selected.dump() && sibling["tasks"].dump() == node["tasks"].dump()
                        && sibling.flag("component_reserved", false) && sibling.str("generation_method") != "experience_ablation";
                  }
                bool independent_ablation = config.str("sampling_strategy", "same_parent") == "independent";
                if(independent_ablation) independent_ablation = !experience_ablation_seen(
                    tree["nodes"].items()[node.num("parent", 0)].str("version"), selected, index);
                // 独立采样没有同轮兄弟：以实测父版本作对照，每版本/经验只消融一次。
                if(paired || independent_ablation) draft = experience_ablation(parent_artifact, selected, parent_report);
                if(!draft.null()) node["generation_method"] = "experience_ablation";
                else draft = propose(parent_artifact, input, index, draft);
                write(root / "reflections" / (std::to_string(index) + ".draft.json"), draft.dump());
                // 普通草稿和 COMBEE 均先校验再进入同一修复入口，权限与预算不变。
                accepted = expand_control_updates(draft, components(parent_artifact), selected);
                accepted = repair_schedule_contracts(accepted, parent_report, contract_repairs);
                if(!contract_repairs.items().empty()) node["schedule_contract_repair"] = contract_repairs;
                candidate = mutate(parent_artifact, accepted, selected);
              }
              catch(const std::exception& exception) {
                const std::string code = exception.what();
                if((code != "control_source_not_approved" && code != "gepa_component_not_authorized"
                        && code != "control_failure_replan_not_authorized"
                        && code != "unknown_field" && code != "gepa_draft_invalid"
                        && code != "control_duplicate_loop_scope" && code != "control_binding_invalid"
                        && code != "control_loop_relaxes_condition" && code != "control_loop_condition_invalid")
                    || controls.num("max_attempts", 2) < 2
                    || tree.num("model_calls", 0) >= config.num("max_model_calls", 8)) throw;
                // 在相同模型与输入预算内最多修复一次。来源和格式修正无需轨迹；
                // 组件选择修正必须附带证据。
                Json repair{{"phase", code == "control_source_not_approved" ? "repair_control_source"
                        : (code == "unknown_field" || code == "gepa_draft_invalid" || code == "control_duplicate_loop_scope")
                            ? "repair_draft_format" : "repair_component_selection"},
                    {"error", code}, {"draft", draft}, {"experiences", input["experiences"]},
                    {"selected_components", selected}, {"components", input["components"]}};
                if(input.has("controls")) repair["controls"] = input["controls"];
                if(code == "control_binding_invalid") {
                  repair = input;
                  repair.fields().erase("parent_version");
                  repair["phase"] = "repair_control_binding";
                  repair["error"] = code;
                  const Json& saved = tree;
                  const auto& diagnostic = saved["reflection_calls"][std::to_string(tree.num("model_calls", 0))]["control_binding_error"];
                  if(diagnostic.object()) {
                    repair["binding_error"] = diagnostic;
                    if(repair.dump().size() > config.num("max_input_bytes", 65536))
                      repair["binding_error"].fields().erase("budget");
                  }
                } else if(code == "gepa_component_not_authorized" || code == "control_failure_replan_not_authorized"
                    || code == "control_loop_relaxes_condition" || code == "control_loop_condition_invalid") {
                  repair = input;
                  repair.fields().erase("parent_version");
                  repair["phase"] = code == "gepa_component_not_authorized"
                      ? "repair_component_selection" : "repair_control_capability";
                  repair["error"] = code;
                  repair["feedback"] = Json::Array{};
                  const auto* evidence = repair_feedback_row(input, selected);
                  // 只复用一条相关训练行，不复用未授权草稿或整个小批；保留其任务契约和轨迹。
                  if(evidence) repair["feedback"].push(*evidence);
                }
                node["repair_error"] = code; save();
                accepted = reflect(repair, index, "-repair");
                write(root / "reflections" / (std::to_string(index) + "-repair.draft.json"), accepted.dump());
                accepted = expand_control_updates(accepted, components(parent_artifact), selected);
                accepted = repair_schedule_contracts(accepted, parent_report, contract_repairs);
                if(!contract_repairs.items().empty()) node["schedule_contract_repair"] = contract_repairs;
                candidate = mutate(parent_artifact, accepted, selected);
              }
              // 合法弃权才补一个有完整评审依据的候选，不覆盖有效提案或绕开原有门禁。
              const auto probe = reviewed_control_probe(parent_artifact, candidate, input, parent_report, index);
              if(!probe.null()) {
                node["control_probe"] = probe["detail"];
                try {
                  const auto probe_draft = expand_control_updates(probe["draft"], components(parent_artifact), selected);
                  const auto probe_candidate = mutate(parent_artifact, probe_draft, selected);
                  const auto check = control_preflight(parent_artifact, probe_candidate, parent_report);
                  if(check.flag("passed", false) && components(probe_candidate).dump() != components(parent_artifact).dump()) {
                    accepted = probe_draft; candidate = probe_candidate;
                    node["generation_method"] = "reviewed_control_probe"; node["control_probe"]["status"] = "proposed";
                  } else node["control_probe"]["status"] = "untriggerable";
                } catch(const Error& error) {
                  const std::string code = error.what();
                  if(code.rfind("control_", 0) != 0 && code != "gepa_candidate_not_authorized") throw;
                  node["control_probe"]["status"] = "invalid";
                }
              }
              node["reason"] = accepted.str("reason");
              node["control_preflight"] = control_preflight(parent_artifact, candidate, parent_report);
              if(node["control_preflight"].has("unconditioned_recovery") && !node.has("repair_error")
                  && controls.num("max_attempts", 2) >= 2
                  && tree.num("model_calls", 0) < config.num("max_model_calls", 8)) {
                // 预检缺少恢复条件也复用同一次修复预算；条件由模型根据训练事实补充，不能代填。
                auto repair = input;
                repair.fields().erase("parent_version");
                repair["phase"] = "repair_control_capability";
                repair["error"] = "recovery_condition_required";
                repair["draft"] = draft;
                repair["preflight"] = node["control_preflight"];
                repair["feedback"] = Json::Array{};
                const auto proposed_policy = proto<v1::RuntimePolicy>(candidate["policy"]);
                const Json* evidence = nullptr;
                for(const auto& row : input["feedback"].items()) {
                  const auto& runs = row["trial"]["evidence"]["runs"];
                  if(!runs.array()) continue;
                  for(const auto& run : runs.items()) {
                    if(!run["actions"].array()) continue;
                    for(const auto& action : run["actions"].items()) {
                      const auto& control = action["loop_control"];
                      if(action.str("kind") != "loop.decision" || !control.object()
                          || (!control.num("rejected_actions", 0)
                              && (!control["error_codes"].array() || control["error_codes"].items().empty()))) continue;
                      for(const auto& rule : proposed_policy.options().loop_rules())
                        if(rule.target_scope() == control.str("target_scope")
                            && (rule.actor_id().empty() || rule.actor_id() == action.str("actor_id"))
                            && std::any_of(node["control_preflight"]["unconditioned_recovery"].items().begin(),
                                node["control_preflight"]["unconditioned_recovery"].items().end(),
                                [&](const auto& id) { return id.text() == rule.rule_id(); })) evidence = &row;
                    }
                  }
                  if(evidence) break;
                }
                // 没有该规则作用域/actor 的真实条件，或原草稿与证据放不下时，保留原预检拒绝。
                if(evidence) repair["feedback"].push(*evidence);
                if(evidence && repair.dump().size() <= config.num("max_input_bytes", 65536)) {
                  node["preflight_before_repair"] = node["control_preflight"];
                  node["repair_error"] = "recovery_condition_required"; save();
                  accepted = reflect(repair, index, "-repair");
                  write(root / "reflections" / (std::to_string(index) + "-repair.draft.json"), accepted.dump());
                  accepted = expand_control_updates(accepted, components(parent_artifact), selected);
                  accepted = repair_schedule_contracts(accepted, parent_report, contract_repairs);
                  if(!contract_repairs.items().empty()) node["schedule_contract_repair"] = contract_repairs;
                  candidate = mutate(parent_artifact, accepted, selected);
                  node["reason"] = accepted.str("reason");
                  node["control_preflight"] = control_preflight(parent_artifact, candidate, parent_report);
                }
              }
              if(!node["control_preflight"].flag("passed", false)
                  && std::any_of(selected.items().begin(), selected.items().end(), [](const auto& item) {
                    return item.text() == "schedule_rules";
                  })) {
                Json repairs;
                auto repaired_draft = repair_schedule_guards(accepted, parent_report, repairs);
                if(!repairs.items().empty()) {
                  node["schedule_guard_repair"] = repairs;
                  auto repaired = mutate(parent_artifact, repaired_draft, selected);
                  auto check = control_preflight(parent_artifact, repaired, parent_report);
                  if(check.flag("passed", false)) {
                    candidate = std::move(repaired); accepted = std::move(repaired_draft);
                    node["control_preflight"] = std::move(check);
                  }
                }
              }
              if(!node["control_preflight"].flag("passed", false)
                  && (config.str("component_selector", "round_robin") == "round_robin"
                      || node["control_preflight"].flag("pure_removal_no_effect", false)
                      || node["control_preflight"].has("unconditioned_recovery")
                      || node["control_preflight"].has("known_keep_truncation"))) {
                node["status"] = "untriggerable_controls";
                node["reason"] = node["control_preflight"].has("unconditioned_recovery")
                    ? "recovery_condition_required" : node["control_preflight"].has("known_keep_truncation")
                    ? "known_required_iterations_would_be_truncated" : "no_differential_training_trigger"; save();
              } else proposal(index, candidate);
            }
          }
          if(node.str("status") == "proposed") {
            const auto child = artifact(index); const auto screening = evaluate(child, batch, "train"); const auto values = scores(screening);
            node["screening"] = Json{{"parent", parent_scores}, {"candidate", values}};
            node["screening"]["control_coverage"] = control_coverage(child, screening);
            node["screening"]["applied_changes"] = applied_changes(parent_artifact, screening);
            const auto acceptance = config.str("acceptance", "strict_improvement");
            const bool pass = acceptance == "pareto"
                ? gepa::no_worse(values, parent_scores) || observed_local_gain(values, parent_scores,
                    node["screening"]["applied_changes"])
                : acceptance == "improvement_or_equal" ? gepa::no_worse(values, parent_scores) : improves(values, parent_scores);
            const bool changed = !node["screening"]["applied_changes"].fields().empty();
            const bool observed_gain = observed_training_gain(values, parent_scores, node["screening"]["applied_changes"]);
            node["status"] = values.fields().empty() ? "inconclusive" : !changed ? "unobserved_changes"
                : pass && observed_gain ? "screened" : "rejected_minibatch";
            if(!changed && !values.fields().empty()) node["reason"] = "no_observed_decision_change";
            else if(changed && !values.fields().empty() && !observed_gain) node["reason"] = "no_observed_training_gain";
            save();
            // 无行为差分不靠验证随机收益续命；真实消融由父策略回放识别。
          }
          if(node.str("status") == "screened" && tree["batch"].null()) validate(index);
        }
      } catch(const std::exception& exception) { error(index, exception); }
      tree["active"] = nullptr;
      if(node.str("kind") == "merge" || tree["batch"].null()) {
        tree["merge_due"] = node.str("status") == "accepted";
        update_frontier(node.num("previous_best", 0), node.str("previous_frontier"));
      }
      save();
      if((tree.str("stop_reason") == "gepa_trial_budget_exhausted" || tree.str("stop_reason") == "gepa_search_time_reserved")
          && tree["batch"].null()) break;
    }
    if(!final_holdout_replay) refresh_change_evidence();
    if(!tree.flag("holdout_started", false)) {
      tree["winner"] = winner();
      if(!tree.has("stop_reason")) tree["stop_reason"] = tree.flag("reflection_exhausted", false)
          || tree.num("model_calls", 0) >= config.num("max_model_calls", 8) ? "model_budget" : "max_rounds";
      save();
    }
    // 先把验证排名、版本和覆盖记录写入持久化计划，再依次执行留出。
    // 这样恢复会沿用同一批 Top-K；留出分数不会反馈到候选排序，避免数据泄漏。
    const auto validation_winner = tree.num("winner", 0);
    const auto selected = artifact(validation_winner);
    Report report;
    std::vector<GepaFinalist> finalists;
    auto& final_holdout = tree["final_holdout"];
    const bool prepare_finalists = !final_holdout.object()
        && (!tree.flag("holdout_started", false) || final_holdout_replay);
    if(!final_holdout.object() && tree.flag("holdout_started", false) && !final_holdout_replay)
      throw Error("gepa_final_holdout_legacy_replay_required");
    if(prepare_finalists) {
      if(final_holdout.object()) tree["previous_final_holdout"] = final_holdout;
      const auto candidates = final_holdout_candidates();
      if(candidates.empty()) {
        tree["status"] = "validation_rejected";
        fs::remove(root / "selected-version.json");
      } else {
        const auto epoch = tree.num("final_holdout_epoch", 0) + 1;
        Json plan{{"protocol", "top-k-v1"}, {"selection", "frozen_validation_rank"},
            {"requested_top_k", finalist_budget_count}, {"epoch", epoch}, {"status", "prepared"},
            {"candidates", Json::Array{}}};
        std::string campaign = "gepa-final-holdout-v1\n" + tree.str("context") + "\n" + std::to_string(epoch);
        for(std::size_t rank = 0; rank < candidates.size(); ++rank) {
          const auto index = candidates[rank];
          const auto candidate = artifact(index);
          plan["candidates"].push(Json{{"rank", rank + 1}, {"node", index},
              {"version", agent_version_id(candidate.dump())}, {"optimization", tree["nodes"].items()[index]["optimization"]},
              {"control_coverage", tree["nodes"].items()[index]["control_coverage"]}, {"status", "pending"}});
          campaign += "\n" + plan["candidates"].items().back().str("version");
        }
        plan["campaign_id"] = hash(campaign);
        if(final_holdout_replay) {
          plan["retrospective"] = true;
          plan["replay_deadline_ms"] = now() + config.num("timeout_ms", 1800000);
        }
        tree["final_holdout_epoch"] = epoch;
        final_holdout = std::move(plan);
        tree["holdout_started"] = true;
        fs::remove(root / "selected-version.json");
        save();
      }
    }
    if(final_holdout.object() && final_holdout["candidates"].array() && !final_holdout["candidates"].items().empty()) {
      final_holdout["status"] = "running"; save();
      for(std::size_t rank = 0; rank < final_holdout["candidates"].items().size(); ++rank) {
        auto& entry = final_holdout["candidates"].items()[rank];
        const auto index = entry.num("node", SIZE_MAX);
        require(index < tree["nodes"].items().size() && entry.num("rank", 0) == rank + 1,
            "gepa_final_holdout_plan_changed");
        const auto candidate = artifact(index);
        require(entry.str("version") == agent_version_id(candidate.dump()), "gepa_final_holdout_plan_changed");
        const auto remaining = final_holdout["candidates"].items().size() - rank - 1;
        // 缓存开启时，首个候选若尚无 L0 缓存会执行一次“基线+候选”，随后固定并
        // 复用该基线；后续候选只需各自一次 trial。这里不能把首个候选开始前的
        // cache miss 外推到所有剩余候选，否则 Top-K 会被按 K 份基线过度预留。
        const bool baseline_reusable = tree["holdout_baseline_cache"].str("status") == "hit"
            || config.flag("cache_evaluations", true);
        const auto per_remaining_candidate = holdout_trials * (baseline_reusable ? 1 : 2);
        final_holdout_reserve = remaining * per_remaining_candidate;
        entry["status"] = "running"; save();
        // 每个最终候选使用同一冻结 campaign，且不复用通用候选缓存，确保本次留出
        // 真正执行候选；L0 仅可复用已锁定且经校验的基线来源。
        const auto final_report = evaluate(candidate, holdout, "holdout", true, false, false,
            final_holdout.str("campaign_id"));
        entry["status"] = scored(final_report) ? "complete" : "inconclusive";
        entry["report_kind"] = final_report.report_kind();
        entry["report_fingerprint"] = hash(json(final_report).dump());
        entry["trials"] = std::size_t(final_report.trials_size());
        entry["baseline_source"] = tree["holdout_baseline_cache"]["source"];
        save();
        finalists.push_back(GepaFinalist{index, candidate, final_report, entry["control_coverage"]});
      }
      final_holdout_reserve = 0;
      bool complete = true;
      for(const auto& entry : final_holdout["candidates"].items()) complete &= entry.str("status") == "complete";
      final_holdout["status"] = complete ? "completed" : "inconclusive";
      tree["status"] = complete ? "completed" : "inconclusive";
      report = finalists.front().holdout;
    } else if(tree.str("status") != "validation_rejected") {
      tree["status"] = "validation_rejected";
    }
    tree["finished_ms"] = now();
    tree["selected_version"] = agent_version_id(selected.dump()); save();
    Json summary{{"algorithm", tree["algorithm"]}, {"status", tree["status"]}, {"rounds", tree["rounds"]},
        {"nodes", tree["nodes"].items().size()}, {"model_calls", tree["model_calls"]}, {"trials_used", tree["trials_used"]},
        {"winner", tree["winner"]}, {"selected_version", tree["selected_version"]}, {"stop_reason", tree["stop_reason"]}};
    summary["merge_attempts"] = tree["merge_attempts"].items().size();
    summary["reflection_batches"] = tree.num("reflection_batches", 0);
    summary["available_components"] = tree["available_components"];
    summary["search_warnings"] = tree["search_warnings"];
    summary["fixed_controls"] = tree["fixed_controls"];
    summary["control_coverage"] = tree["nodes"].items()[tree.num("winner", 0)]["control_coverage"];
    summary["optimization"] = tree["nodes"].items()[tree.num("winner", 0)]["optimization"];
    summary["component_selections"] = tree["component_selections"];
    summary["training_opportunities"] = tree["training_opportunities"];
    summary["deferred_components"] = tree["deferred_components"];
    summary["final_holdout"] = final_holdout;
    GepaResult result;
    result.artifact = selected; result.holdout = report; result.summary = std::move(summary);
    result.finalists = std::move(finalists);
    return result;
  }
};
}  // namespace

Json load_gepa_config(const fs::path& path) {
  auto config = Json::parse(read(path));
  keys(config, {"schema_version", "runtime_config", "prompt_file", "max_rounds", "max_model_calls", "max_trials", "timeout_ms", "holdout_reserve_ms", "holdout_top_k",
      "minibatch_size", "max_feedback_trials", "max_input_bytes", "max_experience_bytes", "patience", "seed", "min_improvement", "objective",
      "use_merge", "max_merge_invocations", "merge_minibatch_size", "candidate_selection_strategy", "frontier_type", "prune_dominated",
      "epsilon", "top_k", "component_selector", "minibatch_strategy", "skip_perfect_score", "perfect_score", "acceptance",
      "proposals_per_round", "proposal_selection", "proposal_top_k", "cache_evaluations",
      "sampling_strategy", "parents_per_round", "target_fitness",
      "reflection_strategy", "duplication_factor", "aggregation_prompt_file", "unknown_token_policy", "baseline_report_file"});
  require(config.num("schema_version", 0) == 1, "gepa_config_invalid");
  bound(config, "max_rounds", 8, 1, 256); bound(config, "max_model_calls", 8, 1, 256);
  bound(config, "max_trials", 400, 4, 10000); bound(config, "timeout_ms", 1800000, 1000, 86400000);
  bound(config, "holdout_reserve_ms", 0, 0, config.num("timeout_ms", 1800000) - 1);
  bound(config, "holdout_top_k", 1, 1, 3);
  bound(config, "minibatch_size", 4, 1, 128); bound(config, "max_feedback_trials", 4, 1, 32);
  bound(config, "max_input_bytes", 65536, 8192, 1048576); bound(config, "max_experience_bytes", 2048, 64, 2048);
  bound(config, "patience", 0, 0, 256); bound(config, "seed", 42, 0, 2147483000);
  bound(config, "max_merge_invocations", 5, 0, 256); bound(config, "merge_minibatch_size", 4, 1, 128);
  bound(config, "top_k", 5, 1, 256); bound(config, "proposals_per_round", 1, 1, 8);
  bound(config, "proposal_top_k", 2, 1, 8);
  bound(config, "parents_per_round", 2, 1, 8);
  bound(config, "duplication_factor", 2, 1, 4);
  require(config.str("sampling_strategy", "same_parent") != "pxn"
      || config.num("proposals_per_round", 1) * config.num("parents_per_round", 2) <= 8, "gepa_batch_limit");
  for(const auto* key : {"use_merge", "prune_dominated", "skip_perfect_score", "cache_evaluations"}) if(config.has(key)) config[key].boolean();
  auto choice = [&](const char* key, const char* fallback, std::initializer_list<const char*> allowed) {
    const auto value = config.str(key, fallback);
    require(std::any_of(allowed.begin(), allowed.end(), [&](auto item) { return value == item; }), "gepa_strategy_invalid");
  };
  choice("candidate_selection_strategy", "pareto", {"pareto", "current_best", "epsilon_greedy", "top_k_pareto", "fair"});
  choice("frontier_type", "instance", {"instance", "objective", "hybrid", "cartesian"});
  choice("component_selector", "round_robin", {"round_robin", "all", "model_choice"});
  choice("minibatch_strategy", "epoch_shuffled", {"epoch_shuffled", "fixed"});
  choice("acceptance", "strict_improvement", {"strict_improvement", "improvement_or_equal", "pareto"});
  choice("proposal_selection", "all_improvements", {"all_improvements", "best", "top_k"});
  choice("sampling_strategy", "same_parent", {"single", "same_parent", "independent", "pxn"});
  choice("reflection_strategy", "single", {"single", "combee"});
  choice("unknown_token_policy", "reject", {"reject", "worst_case"});
  for(const auto* key : {"epsilon", "perfect_score", "target_fitness"}) if(config.has(key))
    require(number(config[key], -1) >= 0 && number(config[key], -1) <= 1, "gepa_strategy_invalid");
  require(number(config["min_improvement"], 0.000001) > 0 && number(config["min_improvement"], 0.000001) <= 1, "gepa_improvement_invalid");
  if(!config.has("objective")) config["objective"] = Json::Object{};
  keys(config["objective"], {"invalid_loop_weight", "token_weight", "latency_weight", "invalid_loop_scale", "token_scale", "latency_scale_ms"});
  double sum = 0;
  for(const auto& item : config["objective"].fields()) {
    const auto value = number(item.second, -1); require(value >= 0, "gepa_objective_invalid");
    if(item.first.find("weight") != std::string::npos) require(value <= 0.49, "gepa_objective_invalid");
    else require(value > 0, "gepa_objective_invalid");
  }
  sum = number(config["objective"]["invalid_loop_weight"], 0.2) + number(config["objective"]["token_weight"], 0.05) + number(config["objective"]["latency_weight"], 0);
  require(sum < 0.5, "gepa_quality_weight_required");
  for(const auto* key : {"runtime_config", "prompt_file"}) {
    fs::path value(config.str(key)); require(!value.empty(), "gepa_configuration_path_required");
    if(value.is_relative()) value = fs::absolute(path).parent_path() / value;
    config[key] = value.lexically_normal().string();
  }
  if(config.has("aggregation_prompt_file") || config.str("reflection_strategy", "single") == "combee") {
    fs::path value = config.has("aggregation_prompt_file") ? fs::path(config.str("aggregation_prompt_file"))
        : fs::path(config.str("prompt_file")).parent_path() / "gepa-aggregate.txt";
    require(!value.empty(), "gepa_configuration_path_required");
    if(value.is_relative()) value = fs::absolute(path).parent_path() / value;
    config["aggregation_prompt_file"] = value.lexically_normal().string();
  }
  if(config.has("baseline_report_file")) {
    fs::path value(config.str("baseline_report_file"));
    require(!value.empty(), "gepa_configuration_path_required");
    if(value.is_relative()) value = fs::absolute(path).parent_path() / value;
    config["baseline_report_file"] = value.lexically_normal().string();
  }
  config["config_file"] = fs::absolute(path).lexically_normal().string();
  validate_local_data(config); return config;
}

std::string gepa_config_fingerprint(const Json& config) {
  if(config.null()) return {};
  auto runtime = legacy::load_runtime_configuration(config.str("runtime_config"));
  std::string bytes = "gepa-reflective-dag-v15-top-k-holdout:" + config.dump() + read(config.str("config_file")) + read(config.str("prompt_file"));
  if(config.has("aggregation_prompt_file")) bytes += read(config.str("aggregation_prompt_file"));
  for(const auto& path : runtime.source_files) bytes += read(path);
  if(runtime.model.endpoint.empty()) if(const auto* base = std::getenv("OPENAI_BASE_URL")) bytes += base;
  if(runtime.model.model_id.empty()) if(const auto* model = std::getenv("OPENAI_MODEL")) bytes += model;
  return hash(bytes);
}

GepaResult run_gepa_search(const Json& seed, const Json& baseline, const fs::path& dataset,
    const fs::path& evaluation_path, const Json& evaluation, const Json& config, const Json& gate,
    const Json& controls,
    const fs::path& output, const std::string& context, const std::string& evaluation_context,
    const std::string& candidate_evaluation_context,
    const std::function<bool()>& cancelled,
    std::shared_ptr<legacy::ModelAdapter> reflection_model,
    std::size_t final_holdout_top_k_override, bool final_holdout_replay) {
  Search search;
  search.seed = seed; search.baseline = baseline; search.dataset_path = dataset; search.evaluation_path = evaluation_path;
  search.evaluation = evaluation; search.config = config; search.gate = gate; search.controls = controls; search.root = output;
  search.context = context; search.evaluation_context = evaluation_context;
  search.candidate_evaluation_context = candidate_evaluation_context;
  search.cancelled = cancelled; search.model = std::move(reflection_model);
  search.final_holdout_top_k_override = final_holdout_top_k_override;
  search.final_holdout_replay = final_holdout_replay;
  try { return search.run(); }
  catch(const std::exception& exc) {
    if(search.tree.str("context") == context && search.tree["nodes"].array()) {
      const std::string code = exc.what();
      search.tree["status"] = code == "gepa_cancelled" ? "cancelled" : "error";
      const std::set<std::string> known{"gepa_cancelled", "gepa_time_budget_exhausted", "gepa_search_time_reserved",
          "gepa_model_rate_limited", "gepa_baseline_evaluation_inconclusive", "gepa_seed_evaluation_inconclusive",
          "gepa_model_not_configured", "gepa_model_key_missing"};
      search.tree["error"] = known.count(code) ? code : "gepa_search_failed";
      search.tree["finished_ms"] = now(); search.save();
    }
    throw;
  }
}
}  // namespace trajectory::evolution::integration
