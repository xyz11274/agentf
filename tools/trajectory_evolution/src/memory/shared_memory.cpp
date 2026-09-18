// 公共与私有经验在这里统一去重、判冲突、晋升和淘汰，运行时只读取发布视图。
#include "shared_memory.h"
#include "local_support.h"
#include "model_json.h"
#include "agentflow/trajectory/experience.h"

#include <chrono>
#include <regex>
#include <set>
#include <tuple>
#include <openssl/sha.h>

namespace trajectory::evolution::integration {
namespace {
namespace ep = agentflow::trajectory::evaluation;
using agentflow::trajectory::ModelAdapter;
std::string hash(const std::string& value) {
  unsigned char bytes[SHA256_DIGEST_LENGTH];
  require(SHA256(reinterpret_cast<const unsigned char*>(value.data()), value.size(), bytes), "shared_hash_failed");
  std::string out; constexpr char hex[] = "0123456789abcdef";
  for(auto b : bytes) { out += hex[b >> 4]; out += hex[b & 15]; } return out;
}
std::size_t now_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}
std::string component(const std::string& value) {
  std::string out; constexpr char hex[] = "0123456789ABCDEF";
  for(unsigned char c : value) {
    if((c < 128 && std::isalnum(c)) || c == '-' || c == '_' || c == '.') out += c;
    else { out += '%'; out += hex[c >> 4]; out += hex[c & 15]; }
  } return out;
}
Json content(const v1::ShadowExperience& item) {
  return Json{{"target_scope", item.target_scope()}, {"trigger_pattern", item.trigger_pattern()}, {"content", item.content()}};
}
bool public_text(const Json& value, const std::string& user) {
  validate_local_data(value);
  // Scope is an explicitly configured application namespace, not advice text.
  const auto bytes = value.str("trigger_pattern") + '\n' + value.str("content");
  // Public advice does not need concrete endpoints, account numbers or paths.
  // This is a conservative syntax gate, not a complete personal-data detector.
  static const std::regex concrete(R"((https?://|www\.|[A-Za-z0-9._%+-]+@[A-Za-z0-9.-]+|[0-9]{6,}|([0-9]{1,3}\.){3}[0-9]{1,3}|[A-Za-z]:\\|/(home|root|Users|tmp)/))");
  return !std::regex_search(bytes, concrete) && (user.empty() || bytes.find(user) == std::string::npos);
}
Json ask(const std::shared_ptr<ModelAdapter>& model, const std::string& prompt, const Json& payload,
         Json& state, Json& audit, std::size_t& budget, const SharedCheckpoint& checkpoint) {
  require(model && budget > 0, "shared_model_unavailable");
  validate_local_data(payload);
  agentflow::trajectory::ModelRequest request;
  request.system_prompt = prompt; request.user_prompt = payload.dump();
  require(!prompt.empty() && prompt.size() + request.user_prompt.size() <= 1048576, "shared_request_limit");
  request.packet_id = "shared:" + hash(prompt + '\0' + request.user_prompt);
  audit["request_fingerprint"] = request.packet_id;
  audit["state"] = "running"; audit["attempts"] = audit.num("attempts", 0) + 1;
  state["model_calls"] = state.num("model_calls", 0) + 1; --budget;
  checkpoint(state);
  try {
    const auto response = model->evaluate(request);
    audit["response_fingerprint"] = hash(response.body);
    audit["transport_attempts"] = audit.num("transport_attempts", 0) + response.attempts;
    require(response.transport_ok && response.error_code.empty()
        && response.body.size() <= 65536, "shared_model_response_failed");
    auto value = Json::parse(agentflow::trajectory::unwrap_model_json(response.body)); validate_local_data(value);
    audit["state"] = "completed"; return value;
  } catch(...) { audit["state"] = "failed"; throw; }
}
bool gate(const Json& review, const Json& options) {
  const auto& confidence = review["confidence"];
  return std::holds_alternative<double>(confidence.value)
      && std::get<double>(confidence.value) >= std::get<double>(options["min_confidence"].value)
      && std::get<double>(confidence.value) <= 1.0
      && review.flag("privacy_safe", false) && review.flag("generalizable", false)
      && review.flag("supported", false) && review.flag("non_overriding", false);
}
void update_publication(Json& state, const Json& options) {
  std::map<std::string, std::set<std::string>> supporters, training, disputes;
  for(const auto& pair : state["sources"].fields()) {
    const auto& s = pair.second;
    if(s.str("state") == "accepted" && !s.str("entry_id").empty()
        && s.num("last_evidence_ms", 0) + options.num("max_evidence_age_ms", 2592000000ULL) > now_ms()) {
      if(s.flag("online_verified", true)) supporters[s.str("entry_id")].insert(s.str("user_key"));
      if(options.flag("allow_training_sources", true)) for(const auto& group : s["training_groups"].items())
        training[s.str("entry_id")].insert(group.text());
    }
  }
  for(const auto& pair : state["relations"].fields()) {
    const auto& r = pair.second;
    if(r.str("state") == "accepted" && r["relation"].str("relation") == "conflict")
      disputes[r["relation"].str("shared_memory_id")].insert(r.str("user_key"));
  }
  for(auto& pair : state["entries"].fields()) {
    auto& e = pair.second;
    e["supporting_users"] = supporters[pair.first].size();
    e["supporting_training_groups"] = training[pair.first].size();
    e["disputing_users"] = disputes[pair.first].size();
    e["state"] = !disputes[pair.first].empty() ? "disputed"
        : (supporters[pair.first].size() >= options.num("min_supporting_users", 2)
            || training[pair.first].size() >= options.num("min_training_groups", 2) ? "published" : "pending_support");
  }
  std::vector<std::string> active;
  for(const auto& p : state["entries"].fields()) if(p.second.str("state") == "published") active.push_back(p.first);
  std::sort(active.begin(), active.end(), [&](const auto& a, const auto& b) {
    const auto& x = state["entries"][a]; const auto& y = state["entries"][b];
    return std::make_tuple(x.num("supporting_users", 0) + x.num("supporting_training_groups", 0), x.num("last_evidence_ms", 0), a)
        > std::make_tuple(y.num("supporting_users", 0) + y.num("supporting_training_groups", 0), y.num("last_evidence_ms", 0), b);
  });
  for(std::size_t i = options.num("max_entries", 64); i < active.size(); ++i) state["entries"][active[i]]["state"] = "reserve";
}
void retire_entry(Json& state, std::string id, const std::string& reason) {
  state["entries"].fields().erase(id);
  for(auto& p : state["sources"].fields()) if(p.second.str("entry_id") == id) {
    p.second["state"] = "retired"; p.second["entry_id"] = "";
  }
  for(auto it = state["relations"].fields().begin(); it != state["relations"].fields().end();) {
    if(it->second["relation"].str("shared_memory_id") == id) it = state["relations"].fields().erase(it);
    else ++it;
  }
  state["retired"].push(Json{{"memory_id", id}, {"reason", reason}, {"at_ms", now_ms()}});
  if(state["retired"].items().size() > 512) state["retired"].items().erase(state["retired"].items().begin());
  state["retired_count"] = state.num("retired_count", 0) + 1;
}
void expire_entries(Json& state, const Json& options) {
  std::vector<std::string> expired;
  for(const auto& p : state["entries"].fields())
    if(p.second.num("last_evidence_ms", 0) + options.num("max_evidence_age_ms", 2592000000ULL) <= now_ms()) expired.push_back(p.first);
  for(const auto& id : expired) retire_entry(state, id, "evidence_expired");
}
std::size_t next_due(const Json& state, const Json& options, bool has_model) {
  std::size_t due = has_model && !state["pending_users"].items().empty() ? now_ms() + options.num("min_interval_ms", 60000) : 0;
  for(const auto& p : state["entries"].fields()) {
    const auto expiry = p.second.num("last_evidence_ms", 0) + options.num("max_evidence_age_ms", 2592000000ULL);
    if(!due || expiry < due) due = expiry;
  }
  for(const auto& p : state["sources"].fields()) if(p.second.str("state") == "accepted") {
    const auto expiry = p.second.num("last_evidence_ms", 0) + options.num("max_evidence_age_ms", 2592000000ULL);
    if(expiry > now_ms() && (!due || expiry < due)) due = expiry;
  }
  return due;
}
Json public_options(const Json& state, const std::string& scope) {
  Json result = Json::Array{};
  for(const auto& pair : state["entries"].fields()) {
    if(pair.second["experience"].str("target_scope") == scope) {
      auto item = pair.second["experience"]; item["memory_id"] = pair.first; result.push(std::move(item));
    }
  } return result;
}
}

Json shared_memory_options(Json options) {
  if(options.null()) options = Json::Object{};
  keys(options, {"enabled", "min_supporting_users", "max_entries", "max_sources", "max_relations",
    "max_model_calls_per_cycle", "min_interval_ms", "max_attempts", "min_confidence",
    "distill_prompt_file", "review_prompt_file", "relation_prompt_file", "min_source_runs", "max_evidence_age_ms",
    "allow_training_sources", "min_training_groups"});
  const Json defaults{{"enabled", true}, {"min_supporting_users", 2}, {"max_entries", 64},
    {"max_sources", 8192}, {"max_relations", 32768}, {"max_model_calls_per_cycle", 8},
    {"min_interval_ms", 60000}, {"max_attempts", 2}, {"min_confidence", 0.9},
    {"min_source_runs", 2}, {"max_evidence_age_ms", std::size_t(2592000000ULL)},
    {"allow_training_sources", true}, {"min_training_groups", 2}};
  for(const auto& p : defaults.fields()) if(!options.has(p.first)) options[p.first] = p.second;
  require(options.num("min_supporting_users", 0) >= 1 && options.num("min_supporting_users", 0) <= 256
      && options.num("max_entries", 0) >= 1 && options.num("max_entries", 0) <= 128
      && options.num("max_sources", 0) >= 1 && options.num("max_sources", 0) <= 65536
      && options.num("max_relations", 0) >= 1 && options.num("max_relations", 0) <= 131072
      && options.num("max_model_calls_per_cycle", 0) >= 2 && options.num("max_model_calls_per_cycle", 0) <= 64
      && options.num("min_interval_ms", 0) >= 10 && options.num("min_interval_ms", 0) <= 86400000
      && options.num("max_attempts", 0) >= 1 && options.num("max_attempts", 0) <= 4
      && options.num("min_source_runs", 0) >= 1 && options.num("min_source_runs", 0) <= 256
      && options.num("min_training_groups", 0) >= 2 && options.num("min_training_groups", 0) <= 256
      && options.num("max_evidence_age_ms", 0) >= 1000 && options.num("max_evidence_age_ms", 0) <= 31536000000ULL
      && std::holds_alternative<double>(options["min_confidence"].value)
      && std::get<double>(options["min_confidence"].value) >= 0.5
      && std::get<double>(options["min_confidence"].value) <= 1.0, "sharing_configuration_invalid");
  options.flag("enabled", false); options.flag("allow_training_sources", true); return options;
}
Json new_shared_memory(const std::string& app) {
  return Json{{"schema_version", 1}, {"application_id", app}, {"revision", 0}, {"next_due_ms", 0},
    {"entries", Json::Object{}}, {"sources", Json::Object{}}, {"relations", Json::Object{}},
    {"users", Json::Object{}}, {"pending_users", Json::Array{}}, {"retired", Json::Array{}}};
}
void validate_shared_memory(const Json& state, const std::string& app, const Json& options) {
  require(state.num("schema_version", 0) == 1 && state.str("application_id") == app
      && state["entries"].fields().size() <= options.num("max_entries", 64) * 2
      && state["sources"].fields().size() <= options.num("max_sources", 8192)
      && state["relations"].fields().size() <= options.num("max_relations", 32768)
      && state["users"].fields().size() <= 256 && state["pending_users"].items().size() <= 256,
      "shared_state_invalid_or_limit");
  for(const auto& p : state["entries"].fields()) {
    const auto& e = p.second["experience"];
    keys(e, {"target_scope", "trigger_pattern", "content"});
    require(p.first == "shared-" + hash(app + '\0' + e.dump()) && public_text(e, {})
        && agentflow::trajectory::experience_fields_valid(e.str("target_scope"), e.str("trigger_pattern"), e.str("content")),
        "shared_entry_changed");
  }
  validate_local_data(state);
}
void reconcile_shared_memory(Json& state, const Json& options) {
  const auto before = json(shared_memory_view(state)).dump();
  expire_entries(state, options); update_publication(state, options);
  if(json(shared_memory_view(state)).dump() != before) state["revision"] = state.num("revision", 0) + 1;
}
void enqueue_shared_memory(Json& state, const std::string& user) {
  require(identifier(user), "shared_user_invalid");
  const auto key = hash(user);
  require(state["users"].has(key) || state["users"].fields().size() < 256, "shared_user_limit");
  state["users"][key] = user;
  for(const auto& item : state["pending_users"].items()) if(item.text() == key) return;
  state["pending_users"].push(key);
}
std::string shared_content_revision(const v1::PublishedExperience& item) {
  return hash(json(item).dump());
}
v1::SharedExperienceStore shared_memory_view(const Json& state, const std::string& user) {
  v1::SharedExperienceStore result;
  result.set_application_id(state.str("application_id")); result.set_revision(state.num("revision", 0));
  for(const auto& pair : state["entries"].fields()) if(pair.second.str("state") == "published") {
    auto value = pair.second["experience"]; value["memory_id"] = pair.first;
    *result.add_experiences() = proto<v1::PublishedExperience>(value);
  }
  if(!user.empty()) {
    result.set_user_id(user);
    for(const auto& pair : state["relations"].fields()) {
      if(pair.second.str("user_key") == hash(user) && pair.second.str("state") == "accepted")
        *result.add_relations() = proto<v1::CrossMemoryRelation>(pair.second["relation"]);
    }
  }
  return result;
}

void process_shared_memory(Json& state, const Json& config, const PrivateMemoryLoader& load,
                          const std::shared_ptr<ModelAdapter>& model, const SharedCheckpoint& checkpoint,
                          const PublicEvidenceCheck& evidence) {
  const auto& options = config["sharing"];
  if(!options.flag("enabled", false)) return;
  if(state.num("next_due_ms", 0) > now_ms()) return;
  const auto public_before = json(shared_memory_view(state)).dump();
  expire_entries(state, options);
  update_publication(state, options);
  if(state["pending_users"].items().empty()) {
    if(json(shared_memory_view(state)).dump() != public_before) state["revision"] = state.num("revision", 0) + 1;
    state["next_due_ms"] = next_due(state, options, model != nullptr); checkpoint(state); return;
  }
  const auto user_key = state["pending_users"].items().front().text(), user = state["users"].str(user_key);
  ep::EvolutionMemoryStore memory;
  auto shadow = proto<v1::ShadowOptions>(config["shadow_options"]);
  shadow.clear_target_scopes();
  for(const auto& name : config["actors"].items())
    shadow.add_target_scopes("agent:" + component(config.str("agent_id")) + "/actor:" + component(name.text()));
  v1::Identity identity; identity.set_agent_id(config.str("agent_id")); identity.set_user_id(user);
  identity.set_conversation_id("memory-management"); identity.set_run_id("memory-management");
  v1::ShadowPolicySnapshot snapshot;
  try {
    memory = load(user); snapshot = compile_shadow_policy(memory, identity, shadow);
    if(state["user_errors"].object()) state["user_errors"].fields().erase(user_key);
  } catch(...) {
    // A damaged tenant must not hold the queue hostage. Withdraw its positive
    // votes, but never interpret an unreadable counterexample as disproved.
    for(auto& p : state["sources"].fields()) if(p.second.str("user_key") == user_key) {
      p.second["state"] = "withdrawn"; p.second["entry_id"] = "";
    }
    state["user_errors"][user_key] = "private_memory_unavailable";
    state["pending_users"].items().erase(state["pending_users"].items().begin());
    update_publication(state, options);
    if(json(shared_memory_view(state)).dump() != public_before) state["revision"] = state.num("revision", 0) + 1;
    state["last_status"] = "user_quarantined"; state["last_cycle_ms"] = now_ms();
    state["next_due_ms"] = next_due(state, options, model != nullptr); checkpoint(state); return;
  }
  std::map<std::string, v1::ShadowExperience> eligible;
  std::map<std::string, Json> proofs;
  for(const auto& item : snapshot.experiences()) eligible.emplace(hash(user + '\0' + item.memory_id()), item);
  for(const auto& item : memory.entries()) {
    const auto key = hash(user + '\0' + item.memory_id());
    if(eligible.count(key)) {
      try {
        auto proof = evidence(user, item);
        // String callbacks retain the existing verified real-user contract.
        if(proof.string()) proof = Json{{"fingerprint", proof}, {"online_verified", true}, {"training_groups", Json::Array{}}};
        keys(proof, {"fingerprint", "online_verified", "training_groups"});
        require(proof["fingerprint"].string() && proof.str("fingerprint").size() <= 256
            && proof["training_groups"].array() && proof["training_groups"].items().size() <= 256, "shared_evidence_invalid");
        proof.flag("online_verified", false);
        for(const auto& group : proof["training_groups"].items()) require(group.string() && group.text().size() == 64
            && group.text().find_first_not_of("0123456789abcdef") == std::string::npos, "shared_evidence_invalid");
        proofs[key] = std::move(proof);
      } catch(...) { proofs[key] = Json::Object{}; }
    }
  }
  // A source that was archived, contradicted, removed or rewritten no longer
  // supplies evidence to the public pool. Other users' support is untouched.
  for(auto& pair : state["sources"].fields()) if(pair.second.str("user_key") == user_key) {
    const auto found = eligible.find(pair.first);
    if(found == eligible.end() || proofs[pair.first].str("fingerprint").empty() || pair.second.str("fingerprint") != hash(content(found->second).dump())) {
      pair.second["state"] = "withdrawn"; pair.second["entry_id"] = "";
    }
  }
  for(auto it = state["sources"].fields().begin(); it != state["sources"].fields().end();) {
    if(it->second.str("user_key") == user_key && !eligible.count(it->first)) it = state["sources"].fields().erase(it);
    else ++it;
  }
  // Versioned relations are a cache, not permanent facts. Remove stale pairs.
  std::set<std::string> private_revisions;
  for(const auto& pair : eligible) private_revisions.insert(pair.second.memory_id() + '\0' + pair.second.content_revision());
  for(auto it = state["relations"].fields().begin(); it != state["relations"].fields().end();) {
    const auto& r = it->second["relation"];
    if(!state["entries"].has(r.str("shared_memory_id")) || (it->second.str("user_key") == user_key
        && !private_revisions.count(r.str("private_memory_id") + '\0' + r.str("private_content_revision"))))
      it = state["relations"].fields().erase(it);
    else ++it;
  }
  update_publication(state, options);
  auto budget = options.num("max_model_calls_per_cycle", 8);
  bool pending = false;
  for(const auto& pair : eligible) {
    const auto input = content(pair.second);
    const auto fingerprint = hash(input.dump());
    auto& sources = state["sources"];
    if(!sources.has(pair.first)) {
      if(sources.fields().size() >= options.num("max_sources", 8192)) { state["last_error"] = "shared_source_capacity"; continue; }
      sources[pair.first] = Json::Object{};
    }
    auto& source = sources[pair.first];
    if(source.str("fingerprint") != fingerprint || source.str("state") == "withdrawn")
      source = Json{{"fingerprint", fingerprint}, {"user_key", user_key}, {"state", "pending"}, {"attempts", 0}};
    const auto& proof = proofs[pair.first];
    if(proof.str("fingerprint").empty()) { source["state"] = "private_only"; source["reason"] = "independent_evidence_missing"; continue; }
    source["online_verified"] = proof.flag("online_verified", false);
    source["training_groups"] = proof["training_groups"];
    if(source.str("evidence_fingerprint") != proof.str("fingerprint")) {
      source["evidence_fingerprint"] = proof["fingerprint"]; source["last_evidence_ms"] = now_ms();
      if(source.str("state") == "retired" || source.str("state") == "private_only") { source["state"] = "pending"; source["attempts"] = 0; }
      const auto id = source.str("entry_id");
      if(state["entries"].has(id)) state["entries"][id]["last_evidence_ms"] = now_ms();
    }
    if(source.str("state") == "accepted" || source.str("state") == "rejected" || source.str("state") == "retired") continue;
    if(source.num("attempts", 0) >= options.num("max_attempts", 2)) { source["state"] = "rejected"; continue; }
    if(!model || budget < 2) { pending = true; continue; }
    source["attempts"] = source.num("attempts", 0) + 1;
    try {
      auto draft = ask(model, config.str("shared_distill_prompt"), Json{{"source", input}}, state, source["distill"], budget, checkpoint);
      keys(draft, {"shareable", "target_scope", "trigger_pattern", "content"});
      if(!draft.flag("shareable", false)) { source["state"] = "rejected"; source["reason"] = "not_generalizable"; continue; }
      draft.fields().erase("shareable");
      require(draft.str("target_scope") == pair.second.target_scope()
          && draft.str("content").size() <= 4096 && draft.str("trigger_pattern").size() <= 4096
          && agentflow::trajectory::experience_fields_valid(draft.str("target_scope"), draft.str("trigger_pattern"), draft.str("content"))
          && public_text(draft, user), "shared_draft_rejected");
      const auto review = ask(model, config.str("shared_review_prompt"), Json{{"source", input}, {"draft", draft},
          {"existing", public_options(state, draft.str("target_scope"))}}, state, source["review"], budget, checkpoint);
      keys(review, {"decision", "match_id", "privacy_safe", "generalizable", "supported", "non_overriding", "confidence"});
      const auto decision = review.str("decision");
      if(!gate(review, options) || (decision != "new" && decision != "duplicate")) {
        source["state"] = "rejected"; source["reason"] = "shared_review_rejected"; continue;
      }
      const auto exact_id = "shared-" + hash(state.str("application_id") + '\0' + draft.dump());
      auto id = decision == "duplicate" ? review.str("match_id") : exact_id;
      if(decision == "duplicate") require(state["entries"].has(id)
          && state["entries"][id]["experience"].str("target_scope") == draft.str("target_scope"), "shared_match_invalid");
      else {
        require(review.str("match_id").empty(), "shared_match_invalid");
        if(!state["entries"].has(id)) {
          if(state["entries"].fields().size() >= options.num("max_entries", 64) * 2) {
            // Retire the least-supported/oldest entry. Prefer keeping published
            // knowledge over disputed or under-supported candidates.
            auto victim = state["entries"].fields().begin();
            for(auto it = state["entries"].fields().begin(); it != state["entries"].fields().end(); ++it) {
              const auto score = [](const Json& e) { return std::make_tuple(e.str("state") == "published",
                  e.num("supporting_users", 0) + e.num("supporting_training_groups", 0), e.num("last_evidence_ms", 0)); };
              if(score(it->second) < score(victim->second)) victim = it;
            }
            if(victim->second.str("state") == "published") {
              source["state"] = "rejected"; source["reason"] = "shared_entry_capacity"; continue;
            }
            retire_entry(state, victim->first, "capacity");
          }
          state["entries"][id] = Json{{"experience", draft}, {"state", "pending_support"}, {"last_evidence_ms", now_ms()}};
        }
      }
      source["entry_id"] = id; source["state"] = "accepted"; source["reason"] = "automatic_model_review";
      state["entries"][id]["last_evidence_ms"] = now_ms();
    } catch(...) { source["state"] = "failed"; source["reason"] = "shared_model_or_privacy_failure"; pending |= source.num("attempts", 0) < options.num("max_attempts", 2); }
    checkpoint(state);
  }
  update_publication(state, options);
  auto public_view = shared_memory_view(state);
  // Cross-pool comparisons use exact content versions. A failed or missing
  // relation never grants permission to combine the two recommendations.
  for(const auto& pair : eligible) for(const auto& shared : public_view.experiences()) {
    const auto& own = pair.second;
    if(own.target_scope() != shared.target_scope()) continue;
    Json relation{{"private_memory_id", own.memory_id()}, {"private_content_revision", own.content_revision()},
      {"shared_memory_id", shared.memory_id()}, {"shared_content_revision", shared_content_revision(shared)}};
    const auto key = hash(user_key + '\0' + relation.dump());
    auto& relations = state["relations"];
    if(!relations.has(key)) {
      if(relations.fields().size() >= options.num("max_relations", 32768)) { state["last_error"] = "shared_relation_capacity"; continue; }
      relations[key] = Json{{"user_key", user_key}, {"relation", relation}, {"state", "pending"}};
    }
    auto& record = relations[key];
    if(record.str("state") == "accepted" || record.str("state") == "rejected") continue;
    if(own.trigger_pattern() == shared.trigger_pattern() && own.content() == shared.content()) {
      record["relation"]["relation"] = "duplicate"; record["state"] = "accepted"; record["method"] = "exact";
      record.fields().erase("error"); continue;
    }
    if(record.num("attempts", 0) >= options.num("max_attempts", 2)) { record["state"] = "rejected"; continue; }
    if(!model || budget == 0) { pending = true; continue; }
    try {
      const Json public_content{{"target_scope", shared.target_scope()},
          {"trigger_pattern", shared.trigger_pattern()}, {"content", shared.content()}};
      const auto result = ask(model, config.str("shared_relation_prompt"), Json{{"private", content(own)}, {"public", public_content}}, state, record, budget, checkpoint);
      keys(result, {"relation", "confidence"});
      const auto r = result.str("relation");
      require(std::holds_alternative<double>(result["confidence"].value)
          && std::get<double>(result["confidence"].value) >= std::get<double>(options["min_confidence"].value)
          && std::get<double>(result["confidence"].value) <= 1.0
          && (r == "duplicate" || r == "related" || r == "conflict" || r == "independent"), "shared_relation_uncertain");
      record["relation"]["relation"] = r; record["state"] = "accepted"; record["method"] = "model";
      record.fields().erase("error");
    } catch(...) { record["state"] = "failed"; record["error"] = "shared_relation_unresolved"; pending |= record.num("attempts", 0) < options.num("max_attempts", 2); }
    checkpoint(state);
  }
  update_publication(state, options);
  state["pending_users"].items().erase(state["pending_users"].items().begin());
  if(pending) state["pending_users"].push(user_key);
  if(json(shared_memory_view(state)).dump() != public_before) {
    state["revision"] = state.num("revision", 0) + 1;
    for(const auto& p : state["users"].fields()) enqueue_shared_memory(state, p.second.text());
  }
  state["last_status"] = model ? "processed" : "waiting_for_model";
  state["last_cycle_ms"] = now_ms();
  state["next_due_ms"] = next_due(state, options, model != nullptr);
  validate_shared_memory(state, config.str("agent_id"), options); checkpoint(state);
}

}  // namespace trajectory::evolution::integration
