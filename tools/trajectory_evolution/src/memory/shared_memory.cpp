// 公共与私有经验管理：处理经验关系、跨用户支持、冲突、晋升与淘汰。
// 共享前检查来源与隐私，不能把带有用户专属条件的经验直接泛化给所有用户。
// 策略编译需同时考虑私有覆盖和公共可信度，不能把重复经验叠加为额外支持。

// 公共与私有经验在这里统一去重、判冲突、晋升和淘汰，运行时只读取发布视图。
#include "shared_memory.h"
#include "gepa_feedback.h"
#include "local_support.h"
#include "model_json.h"
#include "agentflow/trajectory/experience.h"

#include <chrono>
#include <future>
#include <regex>
#include <set>
#include <tuple>
#include <openssl/sha.h>

namespace trajectory::evolution::integration {
namespace {
namespace ep = agentflow::trajectory::evaluation;
using agentflow::trajectory::ModelAdapter;

// 计算稳定内容摘要，供请求标识、缓存或版本比较使用。
std::string hash(const std::string &value) {
  unsigned char bytes[SHA256_DIGEST_LENGTH];
  require(SHA256(reinterpret_cast<const unsigned char *>(value.data()), value.size(), bytes),
          "shared_hash_failed");
  std::string out;
  constexpr char hex[] = "0123456789abcdef";
  for (auto b : bytes) {
    out += hex[b >> 4];
    out += hex[b & 15];
  }
  return out;
}

// 读取当前墙上时钟并换算为毫秒；用于持久化时间标记，不代替单调时钟的超时计时。
std::size_t now_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

// 校验或编码单段身份字段，供当前模块构造隔离路径或稳定标识。
std::string component(const std::string &value) {
  std::string out;
  constexpr char hex[] = "0123456789ABCDEF";
  for (unsigned char c : value) {
    if ((c < 128 && std::isalnum(c)) || c == '-' || c == '_' || c == '.')
      out += c;
    else {
      out += '%';
      out += hex[c >> 4];
      out += hex[c & 15];
    }
  }
  return out;
}

// 提取记忆的有效内容用于比较，不把管理元数据当作可注入经验。
Json content(const v1::ShadowExperience &item) {
  return Json{{"target_scope", item.target_scope()},
              {"trigger_pattern", item.trigger_pattern()},
              {"content", item.content()}};
}

// 控制类型属于语义契约；支持票和别名不属于。运行时版本仍独立精确校验。
std::string private_semantic_revision(const v1::ShadowExperience &item) {
  return hash(content(item).dump() + '\0' + item.control_target_type());
}

// 共享前先做隐私与用户专属性检查；私有经验可用不代表可以公开。
bool public_text(const Json &value, const std::string &user) {
  validate_local_data(value);
  // 作用域必须是显式配置的应用命名空间，不使用经验正文代替。
  const auto bytes = value.str("trigger_pattern") + '\n' + value.str("content");
  // 公共经验不需要具体端点、账号或文件路径。
  // 这里只做保守语法检查，不代表能发现所有个人隐私。
  static const std::regex concrete(
      R"((https?://|www\.|[A-Za-z0-9._%+-]+@[A-Za-z0-9.-]+|[0-9]{6,}|([0-9]{1,3}\.){3}[0-9]{1,3}|[A-Za-z]:\\|/(home|root|Users|tmp)/))");
  return !std::regex_search(bytes, concrete) &&
         (user.empty() || bytes.find(user) == std::string::npos);
}

// 调用配置的模型完成当前关系审核，输入与输出经过统一约束。
Json ask(const std::shared_ptr<ModelAdapter> &model, const std::string &prompt, const Json &payload,
         Json &state, Json &audit, std::size_t &budget, const SharedCheckpoint &checkpoint) {
  require(model && budget > 0, "shared_model_unavailable");
  validate_local_data(payload);
  agentflow::trajectory::ModelRequest request;
  request.system_prompt = prompt;
  request.user_prompt = payload.dump();
  require(!prompt.empty() && prompt.size() + request.user_prompt.size() <= 1048576,
          "shared_request_limit");
  request.packet_id = "shared:" + hash(prompt + '\0' + request.user_prompt);
  audit["request_fingerprint"] = request.packet_id;
  audit["state"] = "running";
  audit["attempts"] = audit.num("attempts", 0) + 1;
  state["model_calls"] = state.num("model_calls", 0) + 1;
  --budget;
  checkpoint(state);
  try {
    const auto response = model->evaluate(request);
    audit["response_fingerprint"] = hash(response.body);
    audit["transport_attempts"] = audit.num("transport_attempts", 0) + response.attempts;
    require(response.transport_ok && response.error_code.empty() && response.body.size() <= 65536,
            "shared_model_response_failed");
    auto value = Json::parse(agentflow::trajectory::unwrap_model_json(response.body));
    validate_local_data(value);
    audit["state"] = "completed";
    return value;
  } catch (...) {
    audit["state"] = "failed";
    throw;
  }
}

// 检查经验进入公共管理流程的准入条件，证据不足时不自动晋升。
bool gate(const Json &review, const Json &options) {
  const auto &confidence = review["confidence"];
  return std::holds_alternative<double>(confidence.value) &&
         std::get<double>(confidence.value) >= std::get<double>(options["min_confidence"].value) &&
         std::get<double>(confidence.value) <= 1.0 && review.flag("privacy_safe", false) &&
         review.flag("generalizable", false) && review.flag("supported", false) &&
         review.flag("non_overriding", false);
}

// 根据当前支持和冲突关系刷新公共可信状态，不能只在第一次入库时审查。
void update_publication(Json &state, const Json &options) {
  std::map<std::string, std::set<std::string>> supporters, training, disputes;
  for (const auto &pair : state["sources"].fields()) {
    const auto &s = pair.second;
    if (s.str("state") == "accepted" && !s.str("entry_id").empty() &&
        s.num("last_evidence_ms", 0) + options.num("max_evidence_age_ms", 2592000000ULL) >
            now_ms()) {
      if (s.flag("online_verified", true))
        supporters[s.str("entry_id")].insert(s.str("user_key"));
      if (options.flag("allow_training_sources", true))
        for (const auto &group : s["training_groups"].items())
          training[s.str("entry_id")].insert(group.text());
    }
  }
  for (const auto &pair : state["relations"].fields()) {
    const auto &r = pair.second;
    if (r.str("state") == "accepted" && r["relation"].str("relation") == "conflict")
      disputes[r["relation"].str("shared_memory_id")].insert(r.str("user_key"));
  }
  // GEPA 只对原文启停的独立效果作公共可信度判断；组合失利不能污染成员。
  // 这些记录是私有证据的无正文投影。重复任务仍只是一票，正票也不会绕过晋升审查。
  if (state.has("evaluation_feedback") && state["evaluation_feedback"].object())
    for (const auto &pair : state["evaluation_feedback"].fields()) {
      const auto &f = pair.second;
      const auto id = f.str("memory_id");
      if (!state["entries"].has(id)) continue;
      auto value = state["entries"][id]["experience"];
      value["memory_id"] = id;
      if (f.str("content_revision") != shared_content_revision(proto<v1::PublishedExperience>(value)))
        continue;
      if (f.num("negative_trial_count", 0) >= f.num("minimum_pairs", 2))
        disputes[id].insert(f.str("user_key"));
    }
  for (auto &pair : state["entries"].fields()) {
    auto &e = pair.second;
    e["supporting_users"] = supporters[pair.first].size();
    e["supporting_training_groups"] = training[pair.first].size();
    e["disputing_users"] = disputes[pair.first].size();
    e["state"] = !disputes[pair.first].empty()
                     ? "disputed"
                     : (supporters[pair.first].size() >= options.num("min_supporting_users", 2) ||
                                training[pair.first].size() >= options.num("min_training_groups", 2)
                            ? "published"
                            : "pending_support");
  }
  std::vector<std::string> active;
  for (const auto &p : state["entries"].fields())
    if (p.second.str("state") == "published")
      active.push_back(p.first);
  std::sort(active.begin(), active.end(), [&](const auto &a, const auto &b) {
    const auto &x = state["entries"][a];
    const auto &y = state["entries"][b];
    return std::make_tuple(x.num("supporting_users", 0) + x.num("supporting_training_groups", 0),
                           x.num("last_evidence_ms", 0), a) >
           std::make_tuple(y.num("supporting_users", 0) + y.num("supporting_training_groups", 0),
                           y.num("last_evidence_ms", 0), b);
  });
  for (std::size_t i = options.num("max_entries", 64); i < active.size(); ++i)
    state["entries"][active[i]]["state"] = "reserve";
}

// 淘汰时同步移除关系引用并留下有界审计记录，避免失效经验仍参与策略编译。
void retire_entry(Json &state, std::string id, const std::string &reason) {
  state["entries"].fields().erase(id);
  for (auto &p : state["sources"].fields())
    if (p.second.str("entry_id") == id) {
      p.second["state"] = "retired";
      p.second["entry_id"] = "";
    }
  for (auto it = state["relations"].fields().begin(); it != state["relations"].fields().end();) {
    if (it->second["relation"].str("shared_memory_id") == id)
      it = state["relations"].fields().erase(it);
    else
      ++it;
  }
  if (state.has("evaluation_feedback") && state["evaluation_feedback"].object())
    for (auto it = state["evaluation_feedback"].fields().begin();
         it != state["evaluation_feedback"].fields().end();) {
      if (it->second.str("memory_id") == id)
        it = state["evaluation_feedback"].fields().erase(it);
      else ++it;
    }
  state["retired"].push(Json{{"memory_id", id}, {"reason", reason}, {"at_ms", now_ms()}});
  if (state["retired"].items().size() > 512)
    state["retired"].items().erase(state["retired"].items().begin());
  state["retired_count"] = state.num("retired_count", 0) + 1;
}

// 按配置清理过期条目；经验库容量与生命周期都需要约束。
void expire_entries(Json &state, const Json &options) {
  std::vector<std::string> expired;
  for (const auto &p : state["entries"].fields())
    if (p.second.num("last_evidence_ms", 0) + options.num("max_evidence_age_ms", 2592000000ULL) <=
        now_ms())
      expired.push_back(p.first);
  for (const auto &id : expired)
    retire_entry(state, id, "evidence_expired");
}

// 计算真正需要再次工作的时间；没有待处理需求时不做频繁空闲刷新。
std::size_t next_due(const Json &state, const Json &options, bool has_model) {
  std::size_t due = has_model && !state["pending_users"].items().empty()
                        ? now_ms() + options.num("min_interval_ms", 60000)
                        : 0;
  for (const auto &p : state["entries"].fields()) {
    const auto expiry =
        p.second.num("last_evidence_ms", 0) + options.num("max_evidence_age_ms", 2592000000ULL);
    if (!due || expiry < due)
      due = expiry;
  }
  for (const auto &p : state["sources"].fields())
    if (p.second.str("state") == "accepted") {
      const auto expiry =
          p.second.num("last_evidence_ms", 0) + options.num("max_evidence_age_ms", 2592000000ULL);
      if (expiry > now_ms() && (!due || expiry < due))
        due = expiry;
    }
  return due;
}

// 读取公共经验管理配置，统一晋升、冲突和淘汰参数。
Json public_options(const Json &state, const std::string &scope) {
  Json result = Json::Array{};
  for (const auto &pair : state["entries"].fields()) {
    if (pair.second["experience"].str("target_scope") == scope) {
      auto item = pair.second["experience"];
      item["memory_id"] = pair.first;
      result.push(std::move(item));
    }
  }
  return result;
}
}

// 解析公共与私有协同选项，保持默认值和范围校验一致。
Json shared_memory_options(Json options) {
  if (options.null())
    options = Json::Object{};
  keys(options,
       {"enabled", "min_supporting_users", "max_entries", "max_sources", "max_relations",
        "max_model_calls_per_cycle", "min_interval_ms", "max_attempts", "min_confidence",
        "distill_prompt_file", "review_prompt_file", "relation_prompt_file", "min_source_runs",
        "max_evidence_age_ms", "allow_training_sources", "min_training_groups",
        "model_parallelism", "relation_batch_size", "relation_batch_prompt_file"});
  const Json defaults{{"enabled", true},
                      {"min_supporting_users", 2},
                      {"max_entries", 64},
                      {"max_sources", 8192},
                      {"max_relations", 32768},
                      {"max_model_calls_per_cycle", 8},
                      {"min_interval_ms", 60000},
                      {"max_attempts", 2},
                      {"model_parallelism", 4},
                      {"relation_batch_size", 8},
                      {"min_confidence", 0.9},
                      {"min_source_runs", 2},
                      {"max_evidence_age_ms", std::size_t(2592000000ULL)},
                      {"allow_training_sources", true},
                      {"min_training_groups", 2}};
  for (const auto &p : defaults.fields())
    if (!options.has(p.first))
      options[p.first] = p.second;
  require(options.num("min_supporting_users", 0) >= 1 &&
              options.num("min_supporting_users", 0) <= 256 && options.num("max_entries", 0) >= 1 &&
              options.num("max_entries", 0) <= 128 && options.num("max_sources", 0) >= 1 &&
              options.num("max_sources", 0) <= 65536 && options.num("max_relations", 0) >= 1 &&
              options.num("max_relations", 0) <= 131072 &&
              options.num("max_model_calls_per_cycle", 0) >= 2 &&
              options.num("max_model_calls_per_cycle", 0) <= 64 &&
              options.num("min_interval_ms", 0) >= 10 &&
              options.num("min_interval_ms", 0) <= 86400000 &&
              options.num("max_attempts", 0) >= 1 && options.num("max_attempts", 0) <= 4 &&
              options.num("model_parallelism", 0) >= 1 && options.num("model_parallelism", 0) <= 32 &&
              options.num("relation_batch_size", 0) >= 1 && options.num("relation_batch_size", 0) <= 16 &&
              options.num("min_source_runs", 0) >= 1 && options.num("min_source_runs", 0) <= 256 &&
              options.num("min_training_groups", 0) >= 2 &&
              options.num("min_training_groups", 0) <= 256 &&
              options.num("max_evidence_age_ms", 0) >= 1000 &&
              options.num("max_evidence_age_ms", 0) <= 31536000000ULL &&
              std::holds_alternative<double>(options["min_confidence"].value) &&
              std::get<double>(options["min_confidence"].value) >= 0.5 &&
              std::get<double>(options["min_confidence"].value) <= 1.0,
          "sharing_configuration_invalid");
  options.flag("enabled", false);
  options.flag("allow_training_sources", true);
  return options;
}

// 初始化公共库状态和版本，不包含任何未审查的用户私有内容。
Json new_shared_memory(const std::string &app) {
  return Json{{"schema_version", 1},
              {"application_id", app},
              {"revision", 0},
              {"next_due_ms", 0},
              {"entries", Json::Object{}},
              {"sources", Json::Object{}},
              {"relations", Json::Object{}},
              {"users", Json::Object{}},
              {"pending_users", Json::Array{}},
              {"retired", Json::Array{}}};
}

// 验证公共库内容、来源和关系状态，损坏条目不能参与策略编译。
void validate_shared_memory(const Json &state, const std::string &app, const Json &options) {
  require(state.num("schema_version", 0) == 1 && state.str("application_id") == app &&
              state["entries"].fields().size() <= options.num("max_entries", 64) * 2 &&
              state["sources"].fields().size() <= options.num("max_sources", 8192) &&
              state["relations"].fields().size() <= options.num("max_relations", 32768) &&
              state["users"].fields().size() <= 256 && state["pending_users"].items().size() <= 256,
          "shared_state_invalid_or_limit");
  if (state.has("evaluation_feedback")) {
    require(state["evaluation_feedback"].object() &&
                state["evaluation_feedback"].fields().size() <= 256 * 256,
            "shared_feedback_limit");
    for (const auto &pair : state["evaluation_feedback"].fields())
      require(pair.first == hash(pair.second.str("user_key") + '\0' + pair.second.str("memory_id")) &&
                  pair.second.str("user_key").size() == 64 &&
                  pair.second.num("minimum_pairs", 0) >= 1,
              "shared_feedback_invalid");
  }
  for (const auto &p : state["entries"].fields()) {
    const auto &e = p.second["experience"];
    keys(e, {"target_scope", "trigger_pattern", "content"});
    require(p.first == "shared-" + hash(app + '\0' + e.dump()) && public_text(e, {}) &&
                agentflow::trajectory::experience_fields_valid(
                    e.str("target_scope"), e.str("trigger_pattern"), e.str("content")),
            "shared_entry_changed");
  }
  validate_local_data(state);
}

// 协调公共经验与各用户私有经验的支持、分歧和版本关系。
// 私有冲突会降低公共条目的可信状态；不能把不同用户的支持简单相加后覆盖反证。
void reconcile_shared_memory(Json &state, const Json &options) {
  const auto before = json(shared_memory_view(state)).dump();
  expire_entries(state, options);
  update_publication(state, options);
  if (json(shared_memory_view(state)).dump() != before)
    state["revision"] = state.num("revision", 0) + 1;
}

void reconcile_shared_feedback(Json &state, const Json &config, const std::string &user,
                               const ep::EvolutionMemoryStore &memory) {
  if (!config["sharing"].flag("enabled", false)) return;
  const auto before = json(shared_memory_view(state)).dump();
  const auto user_key = hash(user);
  std::map<std::string, const ep::EvolutionMemoryEntry *> by_key;
  for (const auto &entry : memory.entries())
    by_key.emplace(hash(user + '\0' + entry.memory_id()), &entry);
  for (auto &pair : state["sources"].fields()) {
    if (pair.second.str("user_key") != user_key) continue;
    const auto found = by_key.find(pair.first);
    bool eligible = false;
    if (found != by_key.end()) {
      const auto &entry = *found->second;
      const auto &c = entry.candidate();
      eligible = (entry.status() == ep::EVOLUTION_MEMORY_STATUS_CANDIDATE ||
                  entry.status() == ep::EVOLUTION_MEMORY_STATUS_VALIDATED) &&
          c.status() == ep::EVOLUTION_CANDIDATE_STATUS_PROPOSED &&
          c.contradiction_count() == 0 && c.contradiction_evidence().empty() &&
          entry.evaluation_feedback().status() != "regressed" &&
          entry.evaluation_feedback().status() != "conflicting_trial_results" &&
          pair.second.str("fingerprint") == hash(Json{{"target_scope", c.target_scope()},
              {"trigger_pattern", c.trigger_pattern()}, {"content", c.proposed_change()}}.dump());
    }
    // 只撤回当前用户的失效来源，其他用户或其他私有条目的支持保持原样。
    if (!eligible) {
      pair.second["state"] = "withdrawn";
      pair.second["entry_id"] = "";
    }
  }

  const auto feedback = json(memory.gepa_feedback());
  if (feedback["records"].object()) {
    require(feedback.str("user_id") == user, "gepa_shared_feedback_identity_invalid");
    v1::Identity identity;
    identity.set_agent_id(config.str("agent_id"));
    identity.set_user_id(user);
    identity.set_conversation_id("memory-management");
    identity.set_run_id("memory-management");
    auto options = proto<v1::ShadowOptions>(config["shadow_options"]);
    options.clear_target_scopes();
    for (const auto &actor : config["actors"].items())
      options.add_target_scopes("agent:" + component(config.str("agent_id")) +
                                "/actor:" + component(actor.text()));
    ep::EvolutionMemoryStore empty;
    empty.set_schema_version(1);
    empty.set_store_kind("trajectory_evolution_memory");
    empty.set_store_version("evolution-memory-store-v1");
    for (const auto &pair : state["entries"].fields()) {
      // 单独编译已有公共正文以核对运行时修订，避免容量筛选和 disputed 状态遮蔽反证。
      auto value = pair.second["experience"];
      value["memory_id"] = pair.first;
      const auto published = proto<v1::PublishedExperience>(value);
      v1::SharedExperienceStore single;
      single.set_application_id(config.str("agent_id"));
      *single.add_experiences() = published;
      const auto snapshot = compile_shadow_policy(empty, identity, options, single);
      if (snapshot.experiences().empty()) continue;
      const auto evidence = gepa_source_feedback(memory, snapshot.experiences(0));
      if (evidence.report_ids().empty()) continue;
      auto &projection = state["evaluation_feedback"][hash(user_key + '\0' + pair.first)];
      // 原证据裁剪不能清除已成立的反证；更换正文版本才使用新的条目身份。
      const bool same = projection.str("content_revision") == shared_content_revision(published);
      const auto positive = std::max<std::size_t>(evidence.positive_trial_count(),
          same ? projection.num("positive_trial_count", 0) : 0);
      const auto negative = std::max<std::size_t>(evidence.negative_trial_count(),
          same ? projection.num("negative_trial_count", 0) : 0);
      const auto minimum = std::max(feedback.num("minimum_pairs", 2),
          same ? projection.num("minimum_pairs", 1) : std::size_t(1));
      projection = Json{{"user_key", user_key}, {"memory_id", pair.first},
          {"content_revision", shared_content_revision(published)},
          {"status", positive && negative ? "conflicting_trial_results" :
              negative >= minimum ? "regressed" : positive >= minimum ? "validated" : "review_needed"},
          {"positive_trial_count", positive}, {"negative_trial_count", negative},
          {"minimum_pairs", minimum}};
    }
  }
  expire_entries(state, config["sharing"]);
  update_publication(state, config["sharing"]);
  if (json(shared_memory_view(state)).dump() != before)
    state["revision"] = state.num("revision", 0) + 1;
}

// 把待管理的用户经验变化加入公共库后台队列，避免热路径同步扫描全库。
void enqueue_shared_memory(Json &state, const std::string &user) {
  require(identifier(user), "shared_user_invalid");
  const auto key = hash(user);
  require(state["users"].has(key) || state["users"].fields().size() < 256, "shared_user_limit");
  state["users"][key] = user;
  for (const auto &item : state["pending_users"].items())
    if (item.text() == key)
      return;
  state["pending_users"].push(key);
}

// 计算公共库有效内容的版本，供策略编译判断是否需要更新。
std::string shared_content_revision(const v1::PublishedExperience &item) {
  return hash(json(item).dump());
}

// 生成指定用户可见的公共视图，结合用户与公共经验之间的关系进行筛选。
v1::SharedExperienceStore shared_memory_view(const Json &state, const std::string &user) {
  v1::SharedExperienceStore result;
  result.set_application_id(state.str("application_id"));
  result.set_revision(state.num("revision", 0));
  for (const auto &pair : state["entries"].fields())
    if (pair.second.str("state") == "published") {
      auto value = pair.second["experience"];
      value["memory_id"] = pair.first;
      *result.add_experiences() = proto<v1::PublishedExperience>(value);
    }
  if (!user.empty()) {
    result.set_user_id(user);
    for (const auto &pair : state["relations"].fields()) {
      if (pair.second.str("user_key") == hash(user) && pair.second.str("state") == "accepted")
        *result.add_relations() = proto<v1::CrossMemoryRelation>(pair.second["relation"]);
    }
  }
  return result;
}

// 批量执行共享管理，模型预算与检查点贯穿处理中断、重试和恢复。
void process_shared_memory(Json &state, const Json &config, const PrivateMemoryLoader &load,
                           const std::shared_ptr<ModelAdapter> &model,
                           const SharedCheckpoint &checkpoint,
                           const PublicEvidenceCheck &evidence) {
  const auto &options = config["sharing"];
  if (!options.flag("enabled", false))
    return;
  if (state.num("next_due_ms", 0) > now_ms())
    return;
  const auto public_before = json(shared_memory_view(state)).dump();
  expire_entries(state, options);
  update_publication(state, options);
  if (state["pending_users"].items().empty()) {
    if (json(shared_memory_view(state)).dump() != public_before)
      state["revision"] = state.num("revision", 0) + 1;
    state["next_due_ms"] = next_due(state, options, model != nullptr);
    checkpoint(state);
    return;
  }
  const auto user_key = state["pending_users"].items().front().text(),
             user = state["users"].str(user_key);
  ep::EvolutionMemoryStore memory;
  auto shadow = proto<v1::ShadowOptions>(config["shadow_options"]);
  shadow.clear_target_scopes();
  for (const auto &name : config["actors"].items())
    shadow.add_target_scopes("agent:" + component(config.str("agent_id")) +
                             "/actor:" + component(name.text()));
  v1::Identity identity;
  identity.set_agent_id(config.str("agent_id"));
  identity.set_user_id(user);
  identity.set_conversation_id("memory-management");
  identity.set_run_id("memory-management");
  v1::ShadowPolicySnapshot snapshot;
  try {
    memory = load(user);
    snapshot = compile_shadow_policy(memory, identity, shadow);
    if (state["user_errors"].object())
      state["user_errors"].fields().erase(user_key);
  } catch (...) {
    // 损坏的用户库不能阻塞整个队列；撤回其正向支持，
    // 但不能把无法读取的反证理解为已经被推翻。
    for (auto &p : state["sources"].fields())
      if (p.second.str("user_key") == user_key) {
        p.second["state"] = "withdrawn";
        p.second["entry_id"] = "";
      }
    state["user_errors"][user_key] = "private_memory_unavailable";
    state["pending_users"].items().erase(state["pending_users"].items().begin());
    update_publication(state, options);
    if (json(shared_memory_view(state)).dump() != public_before)
      state["revision"] = state.num("revision", 0) + 1;
    state["last_status"] = "user_quarantined";
    state["last_cycle_ms"] = now_ms();
    state["next_due_ms"] = next_due(state, options, model != nullptr);
    checkpoint(state);
    return;
  }
  // 后续训练或在线管理也读取同一份反馈，不能把 GEPA 已撤回的支持重新算回来。
  reconcile_shared_feedback(state, config, user, memory);
  std::map<std::string, v1::ShadowExperience> eligible;
  std::map<std::string, Json> proofs;
  for (const auto &item : snapshot.experiences()) {
    const auto source = std::find_if(memory.entries().begin(), memory.entries().end(),
        [&](const auto &entry) { return entry.memory_id() == item.memory_id(); });
    if (source != memory.entries().end() &&
        (source->evaluation_feedback().content_revision() != item.content_revision() ||
         (source->evaluation_feedback().status() != "regressed" &&
          source->evaluation_feedback().status() != "conflicting_trial_results")))
      eligible.emplace(hash(user + '\0' + item.memory_id()), item);
  }
  for (const auto &item : memory.entries()) {
    const auto key = hash(user + '\0' + item.memory_id());
    if (eligible.count(key)) {
      try {
        auto proof = evidence(user, item);
        // 字符串回调保留原有已验证真实用户契约。
        if (proof.string())
          proof = Json{{"fingerprint", proof},
                       {"online_verified", true},
                       {"training_groups", Json::Array{}}};
        keys(proof, {"fingerprint", "online_verified", "training_groups", "review_context"});
        require(!proof.has("review_context") ||
                    (proof["review_context"].array() && proof["review_context"].dump().size() <= 16384),
                "shared_evidence_invalid");
        require(proof["fingerprint"].string() && proof.str("fingerprint").size() <= 256 &&
                    proof["training_groups"].array() &&
                    proof["training_groups"].items().size() <= 256,
                "shared_evidence_invalid");
        proof.flag("online_verified", false);
        for (const auto &group : proof["training_groups"].items())
          require(group.string() && group.text().size() == 64 &&
                      group.text().find_first_not_of("0123456789abcdef") == std::string::npos,
                  "shared_evidence_invalid");
        proofs[key] = std::move(proof);
      } catch (...) {
        proofs[key] = Json::Object{};
      }
    }
  }
  // 被归档、反驳、删除或改写的来源，
  // 不再向公共库提供原支持；其他用户的支持不受影响。
  for (auto &pair : state["sources"].fields())
    if (pair.second.str("user_key") == user_key) {
      const auto found = eligible.find(pair.first);
      if (found == eligible.end() || proofs[pair.first].str("fingerprint").empty() ||
          pair.second.str("fingerprint") != hash(content(found->second).dump())) {
        pair.second["state"] = "withdrawn";
        pair.second["entry_id"] = "";
      }
    }
  for (auto it = state["sources"].fields().begin(); it != state["sources"].fields().end();) {
    if (it->second.str("user_key") == user_key && !eligible.count(it->first))
      it = state["sources"].fields().erase(it);
    else
      ++it;
  }
  // 运行版本仍精确绑定来源别名，但关系判断另用纯内容指纹。
  // 只增加证据不重新问模型；内容、条件、作用域改变才使该项失效。
  std::map<std::string, v1::ShadowExperience> current_by_id;
  for (const auto &pair : eligible) current_by_id.emplace(pair.second.memory_id(), pair.second);
  std::vector<std::pair<std::string, Json>> rebound;
  std::vector<std::string> obsolete;
  for (const auto &pair : state["relations"].fields()) {
    if (pair.second.str("user_key") != user_key) continue;
    const auto &old = pair.second["relation"];
    const auto found = current_by_id.find(old.str("private_memory_id"));
    if (found == current_by_id.end()) continue;
    const auto semantic = private_semantic_revision(found->second);
    if (old.str("private_content_revision") != found->second.content_revision() &&
        pair.second.str("private_semantic") != semantic) continue;
    auto record = pair.second;
    record["private_semantic"] = semantic;
    record["relation"]["private_content_revision"] = found->second.content_revision();
    // 旧协议把 uncertain 当错误重试后标 rejected；只迁移这些协议遗留项一次。
    if (!record.has("protocol_version") && record.str("error") == "shared_relation_unresolved") {
      record["state"] = "pending";
      record["attempts"] = 0;
    }
    record["protocol_version"] = 2;
    // 缓存键不包含判断结果，否则每次绑定都生成一个新键。
    auto binding = record["relation"];
    binding.fields().erase("relation");
    const auto binding_key = hash(user_key + '\0' + binding.dump());
    rebound.emplace_back(binding_key, std::move(record));
    if (binding_key != pair.first) obsolete.push_back(pair.first);
  }
  for (const auto &key : obsolete) state["relations"].fields().erase(key);
  for (auto &pair : rebound) state["relations"][pair.first] = std::move(pair.second);
  // 带版本的关系只是缓存，不是永久事实，过期关系必须清理。
  std::set<std::string> private_revisions;
  for (const auto &pair : eligible)
    private_revisions.insert(pair.second.memory_id() + '\0' + pair.second.content_revision());
  for (auto it = state["relations"].fields().begin(); it != state["relations"].fields().end();) {
    const auto &r = it->second["relation"];
    if (!state["entries"].has(r.str("shared_memory_id")) ||
        (it->second.str("user_key") == user_key &&
         !private_revisions.count(r.str("private_memory_id") + '\0' +
                                  r.str("private_content_revision"))))
      it = state["relations"].fields().erase(it);
    else
      ++it;
  }
  update_publication(state, options);
  auto budget = options.num("max_model_calls_per_cycle", 8);
  bool pending = false;
  for (const auto &pair : eligible) {
    const auto input = content(pair.second);
    const auto fingerprint = hash(input.dump());
    auto &sources = state["sources"];
    if (!sources.has(pair.first)) {
      if (sources.fields().size() >= options.num("max_sources", 8192)) {
        state["last_error"] = "shared_source_capacity";
        continue;
      }
      sources[pair.first] = Json::Object{};
    }
    auto &source = sources[pair.first];
    if (source.str("fingerprint") != fingerprint || source.str("state") == "withdrawn")
      source = Json{{"fingerprint", fingerprint},
                    {"user_key", user_key},
                    {"state", "pending"},
                    {"attempts", 0}};
    const auto &proof = proofs[pair.first];
    if (proof.str("fingerprint").empty()) {
      source["state"] = "private_only";
      source["reason"] = "independent_evidence_missing";
      continue;
    }
    source["online_verified"] = proof.flag("online_verified", false);
    source["training_groups"] = proof["training_groups"];
    if (source.str("evidence_fingerprint") != proof.str("fingerprint")) {
      source["evidence_fingerprint"] = proof["fingerprint"];
      source["last_evidence_ms"] = now_ms();
      if (source.str("state") == "retired" || source.str("state") == "private_only") {
        source["state"] = "pending";
        source["attempts"] = 0;
      }
      const auto id = source.str("entry_id");
      if (state["entries"].has(id))
        state["entries"][id]["last_evidence_ms"] = now_ms();
    }
    if (source.str("state") == "accepted" || source.str("state") == "rejected" ||
        source.str("state") == "retired")
      continue;
    if (source.num("attempts", 0) >= options.num("max_attempts", 2)) {
      source["state"] = "unresolved";
      continue;
    }
    if (!model || budget < (source["validated_draft"].object() ? 1 : 2)) {
      pending = true;
      continue;
    }
    source["attempts"] = source.num("attempts", 0) + 1;
    try {
      auto draft = source["validated_draft"];
      if (!draft.object()) {
        draft = ask(model, config.str("shared_distill_prompt"), Json{{"source", input}}, state,
                    source["distill"], budget, checkpoint);
        keys(draft, {"shareable", "target_scope", "trigger_pattern", "content"});
        if (!draft.flag("shareable", false)) {
          source["state"] = "rejected";
          source["reason"] = "not_generalizable";
          continue;
        }
        draft.fields().erase("shareable");
      } else {
        state["draft_cache_hits"] = state.num("draft_cache_hits", 0) + 1;
      }
      require(
          draft.str("target_scope") == pair.second.target_scope() &&
              draft.str("content").size() <= 4096 && draft.str("trigger_pattern").size() <= 4096 &&
              agentflow::trajectory::experience_fields_valid(
                  draft.str("target_scope"), draft.str("trigger_pattern"), draft.str("content")) &&
              public_text(draft, user),
          "shared_draft_rejected");
      source["validated_draft"] = draft;
      checkpoint(state);
      auto review_input = Json{{"source", input}, {"draft", draft},
                               {"existing", public_options(state, draft.str("target_scope"))}};
      if (proof.has("review_context"))
        review_input["evidence_context"] = proof["review_context"];
      const auto review = ask(model, config.str("shared_review_prompt"), review_input,
                              state, source["review"], budget, checkpoint);
      keys(review, {"decision", "match_id", "privacy_safe", "generalizable", "supported",
                    "non_overriding", "confidence"});
      const auto decision = review.str("decision");
      if (!gate(review, options) || (decision != "new" && decision != "duplicate")) {
        source["state"] = "rejected";
        source["reason"] = "shared_review_rejected";
        source.fields().erase("validated_draft");
        continue;
      }
      const auto exact_id = "shared-" + hash(state.str("application_id") + '\0' + draft.dump());
      auto id = decision == "duplicate" ? review.str("match_id") : exact_id;
      if (decision == "duplicate")
        require(state["entries"].has(id) && state["entries"][id]["experience"].str(
                                                "target_scope") == draft.str("target_scope"),
                "shared_match_invalid");
      else {
        require(review.str("match_id").empty(), "shared_match_invalid");
        if (!state["entries"].has(id)) {
          if (state["entries"].fields().size() >= options.num("max_entries", 64) * 2) {
            // 淘汰支持最少或最旧的条目，优先保留已发布知识，
            // 而非存在争议或支持不足的候选。
            auto victim = state["entries"].fields().begin();
            for (auto it = state["entries"].fields().begin(); it != state["entries"].fields().end();
                 ++it) {
              const auto score = [](const Json &e) {
                return std::make_tuple(e.str("state") == "published",
                                       e.num("supporting_users", 0) +
                                           e.num("supporting_training_groups", 0),
                                       e.num("last_evidence_ms", 0));
              };
              if (score(it->second) < score(victim->second))
                victim = it;
            }
            if (victim->second.str("state") == "published") {
              source["state"] = "rejected";
              source["reason"] = "shared_entry_capacity";
              continue;
            }
            retire_entry(state, victim->first, "capacity");
          }
          state["entries"][id] = Json{
              {"experience", draft}, {"state", "pending_support"}, {"last_evidence_ms", now_ms()}};
        }
      }
      source["entry_id"] = id;
      source["state"] = "accepted";
      source.fields().erase("validated_draft");
      source["reason"] = "automatic_model_review";
      state["entries"][id]["last_evidence_ms"] = now_ms();
    } catch (...) {
      source["state"] = "failed";
      source["reason"] = "shared_model_or_privacy_failure";
      pending |= source.num("attempts", 0) < options.num("max_attempts", 2);
    }
    checkpoint(state);
  }
  update_publication(state, options);
  auto public_view = shared_memory_view(state);
  std::vector<std::pair<std::string, Json>> todo;
  // 公共与私有库比较必须绑定精确内容版本。
  // 关系判定失败或缺失时，不能据此授权合并两条建议。
  for (const auto &pair : eligible)
    for (const auto &shared : public_view.experiences()) {
      const auto &own = pair.second;
      if (own.target_scope() != shared.target_scope())
        continue;
      Json relation{{"private_memory_id", own.memory_id()},
                    {"private_content_revision", own.content_revision()},
                    {"shared_memory_id", shared.memory_id()},
                    {"shared_content_revision", shared_content_revision(shared)}};
      const auto key = hash(user_key + '\0' + relation.dump());
      auto &relations = state["relations"];
      if (!relations.has(key)) {
        if (relations.fields().size() >= options.num("max_relations", 32768)) {
          state["last_error"] = "shared_relation_capacity";
          continue;
        }
        relations[key] = Json{{"user_key", user_key}, {"relation", relation}, {"state", "pending"},
                              {"private_semantic", private_semantic_revision(own)}, {"protocol_version", 2}};
      }
      auto &record = relations[key];
      if (record.str("state") == "accepted" || record.str("state") == "rejected" || record.str("state") == "unresolved")
        continue;
      if (own.trigger_pattern() == shared.trigger_pattern() && own.content() == shared.content()) {
        record["relation"]["relation"] = "duplicate";
        record["state"] = "accepted";
        record["method"] = "exact";
        record.fields().erase("error");
        continue;
      }
      if (record.num("attempts", 0) >= options.num("max_attempts", 2)) {
        record["state"] = "unresolved";
        continue;
      }
      if (!model || budget == 0 || record.num("next_due_ms", 0) > now_ms()) {
        pending = true;
        continue;
      }
      const Json public_content{{"target_scope", shared.target_scope()},
                                {"trigger_pattern", shared.trigger_pattern()}, {"content", shared.content()}};
      todo.emplace_back(key, Json{{"id", key}, {"private", content(own)}, {"public", public_content}});
    }
  // 只对缺失或失败的关系生成有限工作。分组模型请求不写共享状态；提交仍保持固定顺序。
  const auto batch_size = config.str("shared_relation_batch_prompt").empty() ? 1 : options.num("relation_batch_size", 8);
  std::size_t cursor = 0;
  while (model && budget && cursor < todo.size()) {
    struct Work { std::vector<std::string> keys; agentflow::trajectory::ModelRequest request;
                  std::future<agentflow::trajectory::RawModelResponse> future; };
    std::vector<Work> work;
    while (budget && cursor < todo.size() && work.size() < options.num("model_parallelism", 4)) {
      Work item;
      Json pairs = Json::Array{};
      const auto count = state["relations"][todo[cursor].first].flag("single_retry", false) ? 1 : batch_size;
      while (cursor < todo.size() && item.keys.size() < count) {
        // 防止分组本身放大上下文；单条已有经验受各自 4 KiB 内容限额保护。
        if (!item.keys.empty() && pairs.dump().size() + todo[cursor].second.dump().size() > 32768) break;
        item.keys.push_back(todo[cursor].first);
        pairs.push(todo[cursor].second);
        ++cursor;
      }
      item.request.system_prompt = config.str("shared_relation_prompt");
      if (item.keys.size() == 1) {
        auto payload = pairs.items().front();
        payload.fields().erase("id");
        item.request.user_prompt = payload.dump();
      } else {
        item.request.system_prompt += '\n' + config.str("shared_relation_batch_prompt");
        item.request.user_prompt = Json{{"pairs", pairs}}.dump();
      }
      validate_local_data(Json::parse(item.request.user_prompt));
      item.request.packet_id = "shared:" + hash(item.request.system_prompt + '\0' + item.request.user_prompt);
      for (const auto &key : item.keys) {
        auto &record = state["relations"][key];
        record["state"] = "running";
        record["attempts"] = record.num("attempts", 0) + 1;
        record["request_fingerprint"] = item.request.packet_id;
        record["next_due_ms"] = now_ms() + options.num("min_interval_ms", 60000);
      }
      --budget;
      state["model_calls"] = state.num("model_calls", 0) + 1;
      state["relation_requests"] = state.num("relation_requests", 0) + 1;
      checkpoint(state);
      item.future = std::async(std::launch::async, [model, request = item.request]() {
        try { return model->evaluate(request); }
        catch (...) { agentflow::trajectory::RawModelResponse response; response.error_code = "adapter_exception"; return response; }
      });
      work.push_back(std::move(item));
    }
    for (auto &item : work) {
      const auto response = item.future.get();
      Json answers = Json::Object{};
      try {
        require(response.transport_ok && response.error_code.empty() && response.body.size() <= 65536,
                "shared_model_response_failed");
        auto answer = Json::parse(agentflow::trajectory::unwrap_model_json(response.body));
        validate_local_data(answer);
        if (item.keys.size() == 1) answers[item.keys.front()] = answer;
        else {
          keys(answer, {"relations"});
          std::set<std::string> duplicate;
          for (auto entry : answer["relations"].items()) {
            const auto id = entry.str("id");
            if (std::find(item.keys.begin(), item.keys.end(), id) == item.keys.end()) continue;
            if (answers.has(id)) duplicate.insert(id);
            entry.fields().erase("id");
            answers[id] = std::move(entry);
          }
          for (const auto &id : duplicate) answers.fields().erase(id);
        }
      } catch (...) { /* 协议失败只影响该请求内未得到有效判断的项，原文不落盘。 */ }
      // 对当前库重取快照；外部并发改写只重新排队变化项，不能提交旧内容的判断。
      std::map<std::string, std::string> fresh;
      try {
        const auto latest = compile_shadow_policy(load(user), identity, shadow);
        for (const auto &own : latest.experiences()) fresh[own.memory_id()] = private_semantic_revision(own);
      } catch (...) { /* 无法确认快照仍有效时，本组不提交授权关系。 */ }
      for (const auto &key : item.keys) {
        auto &record = state["relations"][key];
        record["response_fingerprint"] = hash(response.body);
        record["transport_attempts"] = record.num("transport_attempts", 0) + response.attempts;
        if (fresh[record["relation"].str("private_memory_id")] != record.str("private_semantic")) {
          record["state"] = "stale";
          pending = true;
          continue;
        }
        try {
          const auto &answer = answers[key];
          keys(answer, {"relation", "confidence"});
          const auto relation = answer.str("relation");
          require(std::holds_alternative<double>(answer["confidence"].value), "shared_relation_protocol");
          const auto confidence = std::get<double>(answer["confidence"].value);
          require(std::isfinite(confidence) && confidence >= 0 && confidence <= 1 &&
                      (relation == "duplicate" || relation == "related" || relation == "conflict" ||
                       relation == "independent" || relation == "uncertain"), "shared_relation_protocol");
          record["confidence"] = confidence;
          if (relation == "uncertain" || confidence < std::get<double>(options["min_confidence"].value)) {
            record["state"] = "unresolved";
            record["error"] = "shared_relation_uncertain";
          } else {
            record["relation"]["relation"] = relation;
            record["state"] = "accepted";
            record["method"] = "model";
            record.fields().erase("error");
          }
          record["next_due_ms"] = 0;
        } catch (...) {
          record["state"] = record.num("attempts", 0) >= options.num("max_attempts", 2) ? "unresolved" : "failed";
          record["error"] = "shared_relation_protocol_or_transport";
          record["single_retry"] = true;
          pending |= record.str("state") == "failed";
        }
      }
      checkpoint(state);
    }
  }
  pending |= cursor < todo.size();
  update_publication(state, options);
  state["pending_users"].items().erase(state["pending_users"].items().begin());
  if (pending)
    state["pending_users"].push(user_key);
  if (json(shared_memory_view(state)).dump() != public_before) {
    state["revision"] = state.num("revision", 0) + 1;
    for (const auto &p : state["users"].fields())
      enqueue_shared_memory(state, p.second.text());
  }
  std::size_t unresolved = 0;
  for (const auto &p : state["relations"].fields())
    unresolved += p.second.str("state") == "unresolved" || p.second.str("state") == "rejected";
  state["unresolved_relations"] = unresolved;
  state["last_status"] = !model ? "waiting_for_model" : pending ? "pending" :
                         unresolved ? "completed_with_unresolved" : "completed";
  state["last_cycle_ms"] = now_ms();
  state["next_due_ms"] = next_due(state, options, model != nullptr);
  validate_shared_memory(state, config.str("agent_id"), options);
  checkpoint(state);
}

} // 命名空间结束：trajectory::evolution::integration
