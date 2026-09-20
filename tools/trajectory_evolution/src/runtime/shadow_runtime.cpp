#include "trajectory_evolution/shadow_runtime.h"
#include "trajectory_evolution/trace.h"
#include "agentflow/trajectory/evolution_pipeline.h"
#include "agentflow/trajectory/experience.h"
#include "shared_memory.h"
#include "retrieval_text.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <fstream>
#include <map>
#include <set>
#include <utility>
#include <vector>
#include <google/protobuf/io/coded_stream.h>
#include <google/protobuf/io/zero_copy_stream_impl_lite.h>
#include <google/protobuf/util/json_util.h>
#include <openssl/sha.h>

namespace trajectory::evolution {
namespace {
namespace ep = agentflow::trajectory::evaluation;
using agentflow::trajectory::EvaluationError;
using agentflow::trajectory::ensure_safe_evolution_data;

void require(bool ok, const char* code) {
  if(!ok) throw EvaluationError(code);
}

bool identifier(const std::string& value) {
  return !value.empty() && value.size() <= 256U
      && std::none_of(value.begin(), value.end(), [](unsigned char c) {
           return c < 32 || c == 127;
         });
}

void validate_identity(const v1::Identity& identity) {
  require(identifier(identity.agent_id()) && identifier(identity.user_id())
              && identifier(identity.run_id())
              && (identity.conversation_id().empty()
                  || identifier(identity.conversation_id())),
          "shadow_identity_invalid");
}

void validate_options(const v1::ShadowOptions& options) {
  require(options.target_scopes_size() > 0 && options.target_scopes_size() <= 64,
          "shadow_scopes_invalid");
  for(const auto& scope : options.target_scopes()) {
    require(identifier(scope), "shadow_scopes_invalid");
  }
  require(options.max_policy_entries() > 0 && options.max_policy_entries() <= 512
              && options.max_advice_items() > 0 && options.max_advice_items() <= 32
              && options.max_advice_bytes() >= 512 && options.max_advice_bytes() <= 65536
              && options.max_query_bytes() >= 512 && options.max_query_bytes() <= 65536
              && options.max_memory_bytes() > 0 && options.max_memory_bytes() <= 64U * 1024U * 1024U
              && options.max_snapshot_bytes() >= 4096 && options.max_snapshot_bytes() <= 512U * 1024U
              && options.max_match_terms() > 0 && options.max_match_terms() <= 512
              && options.min_matched_terms() > 0
              && options.min_matched_terms() <= options.max_match_terms()
              && options.stop_terms_size() <= 512,
          "shadow_limits_invalid");
  for(const auto& term : options.stop_terms()) {
    require(term.size() <= 128, "shadow_stop_term_invalid");
  }
}

std::string digest(const std::string& input) {
  std::array<unsigned char, SHA256_DIGEST_LENGTH> bytes{};
  require(SHA256(reinterpret_cast<const unsigned char*>(input.data()),
                 input.size(), bytes.data()) != nullptr, "shadow_hash_failed");
  const char* hex = "0123456789abcdef";
  std::string result;
  for(const auto byte : bytes) {
    result += hex[byte >> 4U];
    result += hex[byte & 15U];
  }
  return result;
}

std::string stable_bytes(const google::protobuf::Message& value) {
  std::string bytes;
  {
    google::protobuf::io::StringOutputStream stream(&bytes);
    google::protobuf::io::CodedOutputStream coded(&stream);
    coded.SetSerializationDeterministic(true);
    require(value.SerializeToCodedStream(&coded), "shadow_serialization_failed");
  }
  return bytes;
}

std::string snapshot_digest(v1::ShadowPolicySnapshot snapshot) {
  snapshot.clear_snapshot_id();
  return "shadow-v1:" + digest(stable_bytes(snapshot));
}

bool same_identity(const v1::Identity& a, const v1::Identity& b) {
  return a.agent_id() == b.agent_id() && a.user_id() == b.user_id()
      && a.run_id() == b.run_id() && a.conversation_id() == b.conversation_id();
}

// Deterministic lexical candidates; independent matches are counted below.
std::set<std::string> fallback_terms(const std::string& text) {
  std::set<std::string> result;
  std::string word;
  std::vector<std::string> han;
  auto flush_word = [&] {
    if(word.size() >= 2) result.insert(word);
    word.clear();
  };
  auto flush_han = [&] {
    for(std::size_t width = 2; width <= 4; ++width)
      for(std::size_t start = 0; start + width <= han.size(); ++start) {
        std::string token;
        for(std::size_t i = start; i < start + width; ++i) token += han[i];
        result.insert(std::move(token));
      }
    han.clear();
  };
  for(std::size_t i = 0; i < text.size();) {
    const auto first = static_cast<unsigned char>(text[i]);
    if(first < 0x80) {
      flush_han();
      if(std::isalnum(first) || first == '_') {
        word += static_cast<char>(std::tolower(first));
      } else {
        flush_word();
      }
      ++i;
      continue;
    }
    flush_word();
    const std::size_t width = first >= 0xf0 ? 4 : (first >= 0xe0 ? 3 : 2);
    require(first >= 0xc2 && first <= 0xf4 && i + width <= text.size(),
            "shadow_utf8_invalid");
    std::uint32_t cp = first & (width == 4 ? 7U : (width == 3 ? 15U : 31U));
    for(std::size_t j = 1; j < width; ++j) {
      const auto next = static_cast<unsigned char>(text[i + j]);
      require((next & 0xc0U) == 0x80U, "shadow_utf8_invalid");
      cp = (cp << 6U) | (next & 0x3fU);
    }
    require(cp <= 0x10ffffU && !(cp >= 0xd800U && cp <= 0xdfffU)
                && cp >= (width == 2 ? 0x80U : (width == 3 ? 0x800U : 0x10000U)),
            "shadow_utf8_invalid");
    const bool is_han = (cp >= 0x3400 && cp <= 0x9fff)
        || (cp >= 0x20000 && cp <= 0x2fa1f);
    if(is_han) {
      han.push_back(text.substr(i, width));
    } else {
      flush_han();
    }
    i += width;
  }
  flush_word();
  flush_han();
  return result;
}

void add_english_variants(std::set<std::string>& result, const std::string& token) {
  if(token.size() < 4 || !std::all_of(token.begin(), token.end(), [](unsigned char c) {
       return std::islower(c);
     })) return;
  std::string normalized = token;
  if(token.size() > 5 && token.compare(token.size() - 3, 3, "ies") == 0)
    normalized = token.substr(0, token.size() - 3) + "y";
  else if(token.size() > 5 && token.compare(token.size() - 3, 3, "ing") == 0)
    normalized = token.substr(0, token.size() - 3);
  else if(token.size() > 4 && token.compare(token.size() - 2, 2, "ed") == 0)
    normalized = token.substr(0, token.size() - 2);
  else if(token.size() > 4 && token.back() == 's'
      && token.compare(token.size() - 2, 2, "ss") != 0
      && token.compare(token.size() - 2, 2, "us") != 0
      && token.compare(token.size() - 2, 2, "is") != 0)
    normalized = token.substr(0, token.size() - 1);
  // A word and its normalization provide one piece of evidence, not two.
  result.erase(token); result.insert(std::move(normalized));
}

bool ascii_term(const std::string& term);

std::set<std::string> terms(const std::string& text) {
  auto result = fallback_terms(text);
  const auto base = result;
  for(const auto& token : base) if(ascii_term(token)) {
    add_english_variants(result, token);
  }
  return result;
}

std::size_t token_units(const std::string& term) {
  std::size_t result = 0;
  for(std::size_t i = 0; i < term.size(); ++result) {
    const auto first = static_cast<unsigned char>(term[i]);
    i += first < 0x80 ? 1 : first >= 0xf0 ? 4 : first >= 0xe0 ? 3 : 2;
  }
  return result;
}

bool ascii_term(const std::string& term) {
  return std::all_of(term.begin(), term.end(), [](unsigned char c) { return c < 0x80; });
}

bool strong_term(const std::string& term) {
  if(!ascii_term(term)) {
    if(token_units(term) < 3) return false;
    // Windows crossing function words are not independent phrases.
    for(const std::string edge : {"的", "了", "且", "或", "当", "时", "与", "把"})
      if(term.compare(0, edge.size(), edge) == 0
          || term.compare(term.size() - edge.size(), edge.size(), edge) == 0) return false;
    return true;
  }
  return term.size() >= 4 || (term.size() >= 3
      && std::any_of(term.begin(), term.end(), [](unsigned char c) { return std::isdigit(c); }));
}

double information_weight(const std::string& term) {
  return ascii_term(term) ? 2.0 + std::min<std::size_t>(term.size(), 16) / 8.0
                          : static_cast<double>(token_units(term) - 1);
}

std::vector<std::string> applicability_terms(const std::string& text,
                                             const v1::ShadowOptions& options) {
  std::vector<std::string> result;
  for(const auto& term : terms(text))
    if(std::find(options.stop_terms().begin(), options.stop_terms().end(), term)
        == options.stop_terms().end()) result.push_back(term);
  std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) {
    const auto aw = information_weight(a), bw = information_weight(b);
    return aw != bw ? aw > bw : a < b;
  });
  if(result.size() > options.max_match_terms()) result.resize(options.max_match_terms());
  return result;
}

bool authorized_scope(const v1::ShadowOptions& options, const std::string& scope) {
  return std::find(options.target_scopes().begin(), options.target_scopes().end(), scope)
      != options.target_scopes().end();
}

bool control_target(const std::string& value) {
  return value == "CANDIDATE_TARGET_TYPE_SKILL_ORCHESTRATION"
      || value == "CANDIDATE_TARGET_TYPE_LOOP_STOP_CONDITION"
      || value == "CANDIDATE_TARGET_TYPE_RETRY_POLICY"
      || value == "CANDIDATE_TARGET_TYPE_TOOL_RESULT_CONSUMPTION";
}

bool injectable(const v1::ShadowExperience& experience) {
  return experience.control_target_type().empty();
}

v1::ShadowPolicySnapshot checked_snapshot(v1::ShadowPolicySnapshot snapshot) {
  // Version 1 remains readable for offline replay, but cannot become an active policy.
  require(snapshot.schema_version() == 1 || snapshot.schema_version() == 2, "shadow_schema_invalid");
  validate_identity(snapshot.identity());
  validate_options(snapshot.options());
  require(snapshot.ByteSizeLong() <= snapshot.options().max_snapshot_bytes()
              && to_json(snapshot, false).size() <= snapshot.options().max_snapshot_bytes()
              && snapshot.experiences_size()
                     <= static_cast<int>(snapshot.options().max_policy_entries()),
          "shadow_snapshot_limit");
  require(snapshot.snapshot_id() == snapshot_digest(snapshot), "shadow_snapshot_changed");
  std::set<std::string> ids;
  std::map<std::string, const v1::ShadowExperience*> by_id;
  for(const auto& item : snapshot.experiences()) {
    require(identifier(item.memory_id()) && ids.insert(item.memory_id()).second
                && !item.content().empty()
                && (snapshot.schema_version() == 1
                    || (injectable(item) && agentflow::trajectory::experience_fields_valid(
                        item.target_scope(), item.trigger_pattern(), item.content()))
                    || (!injectable(item) && control_target(item.control_target_type())
                        && !item.application_shared()))
                && authorized_scope(snapshot.options(), item.target_scope())
                && item.match_terms_size() <= static_cast<int>(snapshot.options().max_match_terms()),
            "shadow_experience_invalid");
    auto canonical = item;
    canonical.clear_content_revision();
    require(item.content_revision() == digest(stable_bytes(canonical)),
            "shadow_experience_changed");
    by_id[item.memory_id()] = &item;
  }
  std::set<std::pair<std::string, std::string>> pairs;
  for(const auto& r : snapshot.cross_relations()) {
    const auto own = by_id.find(r.private_memory_id()), shared = by_id.find(r.shared_memory_id());
    require(own != by_id.end() && shared != by_id.end() && !own->second->application_shared()
        && shared->second->application_shared() && r.relation() == "duplicate"
        && own->second->target_scope() == shared->second->target_scope()
        && own->second->content_revision() == r.private_content_revision()
        && pairs.emplace(r.private_memory_id(), r.shared_memory_id()).second, "shadow_relation_invalid");
    v1::PublishedExperience published;
    published.set_memory_id(shared->second->memory_id()); published.set_target_scope(shared->second->target_scope());
    published.set_trigger_pattern(shared->second->trigger_pattern()); published.set_content(shared->second->content());
    require(integration::shared_content_revision(published) == r.shared_content_revision(), "shadow_relation_changed");
  }
  ensure_safe_evolution_data(snapshot);
  return snapshot;
}
}  // namespace

v1::ShadowPolicySnapshot compile_shadow_policy(
    const ep::EvolutionMemoryStore& memory, const v1::Identity& identity,
    const v1::ShadowOptions& options) {
  return compile_shadow_policy(memory, identity, options, {});
}

v1::ShadowPolicySnapshot compile_shadow_policy(
    const ep::EvolutionMemoryStore& memory, const v1::Identity& identity,
    const v1::ShadowOptions& options, const v1::SharedExperienceStore& shared) {
  validate_identity(identity);
  validate_options(options);
  require(memory.schema_version() == 1
              && memory.store_kind() == "trajectory_evolution_memory"
              && memory.store_version() == "evolution-memory-store-v1",
          "shadow_memory_schema_invalid");
  require(memory.ByteSizeLong() <= options.max_memory_bytes()
              && memory.entries_size() <= 10000,
          "shadow_memory_limit");
  v1::ShadowPolicySnapshot snapshot;
  snapshot.set_schema_version(2);
  *snapshot.mutable_identity() = identity;
  *snapshot.mutable_options() = options;
  snapshot.set_memory_revision(memory.revision());
  require((shared.application_id().empty() && shared.experiences().empty() && shared.revision() == 0)
              || shared.application_id() == identity.agent_id(), "shared_application_mismatch");
  require(shared.experiences_size() <= 256 && shared.ByteSizeLong() <= options.max_memory_bytes(),
          "shared_memory_limit");
  require((shared.user_id().empty() && shared.relations().empty()) || shared.user_id() == identity.user_id(),
          "shared_view_user_mismatch");
  snapshot.set_shared_revision(shared.revision());
  std::sort(snapshot.mutable_options()->mutable_target_scopes()->begin(),
            snapshot.mutable_options()->mutable_target_scopes()->end());
  std::sort(snapshot.mutable_options()->mutable_stop_terms()->begin(),
            snapshot.mutable_options()->mutable_stop_terms()->end());
  std::map<std::string, const ep::EvolutionMemoryEntry*> entries;
  std::set<std::string> conflicted;
  for(const auto& entry : memory.entries()) {
    require(identifier(entry.memory_id())
                && entries.emplace(entry.memory_id(), &entry).second,
            "shadow_memory_identity_invalid");
    for(const auto& relation : entry.relations()) {
      if(relation.relation() == ep::MEMORY_MATCH_RELATION_CONTRADICTS) {
        conflicted.insert(entry.memory_id());
        conflicted.insert(relation.target_memory_id());
      }
    }
  }
  for(const auto& pair : entries) {
    const auto& entry = *pair.second;
    const auto& candidate = entry.candidate();
    auto skip = [&](const char* reason) { ++(*snapshot.mutable_excluded_counts())[reason]; };
    if(!authorized_scope(options, candidate.target_scope())) {
      skip("scope"); continue;
    }
    if(candidate.support_evidence_size() == 0 || candidate.support_user_count() != 1
       || std::any_of(candidate.support_evidence().begin(), candidate.support_evidence().end(),
                      [&](const auto& evidence) {
                        return evidence.source().user_id() != identity.user_id();
                      })) {
      skip("ownership"); continue;
    }
    const bool prompt_experience = candidate.target_type()
        == ep::CANDIDATE_TARGET_TYPE_EXPERIENCE_INJECTION;
    const bool control_source = candidate.target_type()
        == ep::CANDIDATE_TARGET_TYPE_SKILL_ORCHESTRATION
        || candidate.target_type() == ep::CANDIDATE_TARGET_TYPE_LOOP_STOP_CONDITION
        || candidate.target_type() == ep::CANDIDATE_TARGET_TYPE_RETRY_POLICY
        || candidate.target_type() == ep::CANDIDATE_TARGET_TYPE_TOOL_RESULT_CONSUMPTION;
    if(!prompt_experience && !control_source) {
      skip("target_type"); continue;
    }
    if(prompt_experience && (!agentflow::trajectory::is_scoped_experience(candidate)
       || !agentflow::trajectory::experience_fields_valid(
           candidate.target_scope(), candidate.trigger_pattern(), candidate.proposed_change()))) {
      skip("experience_contract"); continue;
    }
    if(conflicted.count(entry.memory_id()) || candidate.contradiction_count() != 0
       || candidate.contradiction_evidence_size() != 0) {
      skip("conflict"); continue;
    }
    if((entry.status() != ep::EVOLUTION_MEMORY_STATUS_CANDIDATE
        && entry.status() != ep::EVOLUTION_MEMORY_STATUS_VALIDATED)
       || candidate.status() != ep::EVOLUTION_CANDIDATE_STATUS_PROPOSED) {
      skip("status"); continue;
    }
    if(!identifier(candidate.candidate_id()) || candidate.proposed_change().empty()
       || candidate.proposed_change().size() > 8192
       || candidate.trigger_pattern().size() > 8192
       || candidate.observed_problem().size() > 8192) {
      skip("content"); continue;
    }
    v1::ShadowExperience experience;
    experience.set_memory_id(entry.memory_id());
    experience.set_target_scope(candidate.target_scope());
    experience.set_trigger_pattern(candidate.trigger_pattern());
    experience.set_observed_problem(candidate.observed_problem());
    experience.set_content(candidate.proposed_change());
    if(control_source) experience.set_control_target_type(
        ep::CandidateTargetType_Name(candidate.target_type()));
    std::set<std::string> source_ids(entry.source_candidate_ids().begin(),
                                     entry.source_candidate_ids().end());
    source_ids.insert(candidate.candidate_id());
    for(const auto& source : source_ids) experience.add_source_candidate_ids(source);
    try {
      ensure_safe_evolution_data(experience);
      // Only L1 experiences are matched against user context. Control sources
      // are revision-pinned evidence for generated rules, never prompt advice.
      if(prompt_experience)
        for(const auto& term : applicability_terms(candidate.trigger_pattern(), options))
          experience.add_match_terms(term);
    } catch(const EvaluationError&) {
      skip("unsafe_content"); continue;
    }
    experience.set_content_revision(digest(stable_bytes(experience)));
    if(snapshot.experiences_size() >= static_cast<int>(options.max_policy_entries())) {
      skip("capacity"); continue;
    }
    *snapshot.add_experiences() = std::move(experience);
    // Reserve room for the digest and exclusion counters before publishing.
    if(snapshot.ByteSizeLong() + 2048U > options.max_snapshot_bytes()) {
      snapshot.mutable_experiences()->RemoveLast();
      skip("capacity");
    }
  }
  std::set<std::string> shared_ids;
  std::map<std::string, const v1::CrossMemoryRelation*> relations;
  for(const auto& r : shared.relations()) {
    const auto key = r.private_memory_id() + '\0' + r.private_content_revision() + '\0'
        + r.shared_memory_id() + '\0' + r.shared_content_revision();
    require(relations.emplace(key, &r).second, "shared_relation_duplicate");
  }
  std::vector<v1::ShadowExperience> private_experiences;
  for(const auto& item : snapshot.experiences())
    if(injectable(item)) private_experiences.push_back(item);
  for(const auto& item : shared.experiences()) {
    require(item.memory_id().rfind("shared-", 0) == 0 && identifier(item.memory_id())
                && shared_ids.insert(item.memory_id()).second && !entries.count(item.memory_id())
                && item.content().size() <= 8192 && item.trigger_pattern().size() <= 8192
                && agentflow::trajectory::experience_fields_valid(
                    item.target_scope(), item.trigger_pattern(), item.content()), "shared_experience_invalid");
    if(!authorized_scope(options, item.target_scope())) {
      ++(*snapshot.mutable_excluded_counts())["shared_scope"]; continue;
    }
    bool conflict = false, unresolved = false;
    std::vector<v1::CrossMemoryRelation> duplicates;
    const auto public_revision = integration::shared_content_revision(item);
    for(const auto& own : private_experiences) {
      if(own.target_scope() != item.target_scope()) continue;
      const auto found = relations.find(own.memory_id() + '\0' + own.content_revision() + '\0'
          + item.memory_id() + '\0' + public_revision);
      if(found == relations.end()) { unresolved = true; continue; }
      if(found->second->relation() == "conflict") conflict = true;
      else if(found->second->relation() == "duplicate") duplicates.push_back(*found->second);
      else if(found->second->relation() != "related" && found->second->relation() != "independent") unresolved = true;
    }
    if(conflict || unresolved) {
      ++(*snapshot.mutable_excluded_counts())[conflict ? "cross_memory_conflict" : "cross_memory_unreviewed"]; continue;
    }
    v1::ShadowExperience experience;
    experience.set_memory_id(item.memory_id()); experience.set_target_scope(item.target_scope());
    experience.set_trigger_pattern(item.trigger_pattern()); experience.set_content(item.content());
    experience.set_application_shared(true);
    for(const auto& term : applicability_terms(item.trigger_pattern(), options))
      experience.add_match_terms(term);
    ensure_safe_evolution_data(experience);
    experience.set_content_revision(digest(stable_bytes(experience)));
    std::size_t relation_bytes = 0;
    for(const auto& r : duplicates) relation_bytes += r.ByteSizeLong() + 8;
    if(snapshot.experiences_size() >= static_cast<int>(options.max_policy_entries())
        || snapshot.ByteSizeLong() + experience.ByteSizeLong() + relation_bytes + 2048U > options.max_snapshot_bytes()) {
      ++(*snapshot.mutable_excluded_counts())["shared_capacity"]; continue;
    }
    *snapshot.add_experiences() = std::move(experience);
    for(const auto& r : duplicates) *snapshot.add_cross_relations() = r;
  }
  // Enforce the JSON/DataBus wire budget too, including escaping overhead.
  snapshot.set_snapshot_id(snapshot_digest(snapshot));
  while(to_json(snapshot, false).size() > options.max_snapshot_bytes()
        && !snapshot.experiences().empty()) {
    const auto removed = snapshot.experiences().rbegin()->memory_id();
    snapshot.mutable_experiences()->RemoveLast();
    auto* relations = snapshot.mutable_cross_relations();
    for(int i = relations->size() - 1; i >= 0; --i)
      if(relations->Get(i).shared_memory_id() == removed || relations->Get(i).private_memory_id() == removed)
        relations->DeleteSubrange(i, 1);
    ++(*snapshot.mutable_excluded_counts())["capacity"];
    snapshot.set_snapshot_id(snapshot_digest(snapshot));
  }
  return checked_snapshot(std::move(snapshot));
}

v1::ShadowPolicySnapshot compile_shadow_policy_file(
    const std::filesystem::path& path, const v1::Identity& identity,
    const v1::ShadowOptions& options) {
  validate_options(options);
  std::ifstream input(path, std::ios::binary | std::ios::ate);
  require(static_cast<bool>(input), "shadow_memory_unavailable");
  const auto size = input.tellg();
  require(size >= 0 && static_cast<std::uint64_t>(size) <= options.max_memory_bytes(),
          "shadow_memory_limit");
  input.seekg(0);
  std::string bytes(static_cast<std::size_t>(size), '\0');
  require(bytes.empty() || static_cast<bool>(input.read(bytes.data(), size)),
          "shadow_memory_read_failed");
  ep::EvolutionMemoryStore memory;
  require(path.extension() == ".pb"
              ? memory.ParseFromString(bytes)
              : google::protobuf::util::JsonStringToMessage(bytes, &memory).ok(),
          "shadow_memory_invalid");
  return compile_shadow_policy(memory, identity, options);
}

ShadowRuntime::ShadowRuntime(v1::ShadowPolicySnapshot snapshot)
    : snapshot_(checked_snapshot(std::move(snapshot))) {}

v1::ShadowPolicySnapshot bind_shadow_policy(
    v1::ShadowPolicySnapshot snapshot, const v1::Identity& identity) {
  snapshot = checked_snapshot(std::move(snapshot));
  require(snapshot.identity().agent_id() == identity.agent_id()
      && snapshot.identity().user_id() == identity.user_id(), "policy_owner_mismatch");
  *snapshot.mutable_identity() = identity;
  snapshot.set_snapshot_id(snapshot_digest(snapshot));
  return checked_snapshot(std::move(snapshot));
}

v1::AdviceResult ShadowRuntime::query_advice(const v1::AdviceQuery& query) const {
  return this->query(query, nullptr);
}
v1::AdviceResult ShadowRuntime::query_advice(const v1::AdviceQuery& query,
                                          const std::set<std::string>& approved_ids) const {
  return this->query(query, &approved_ids);
}
v1::AdviceResult ShadowRuntime::query_advice(const v1::AdviceQuery& query,
                                          const std::set<std::string>& approved_ids,
                                          const std::map<std::string, std::string>& triggers) const {
  return this->query(query, &approved_ids, &triggers);
}
v1::AdviceResult ShadowRuntime::query(const v1::AdviceQuery& query,
                                    const std::set<std::string>* approved_ids,
                                    const std::map<std::string, std::string>* triggers) const {
  v1::AdviceResult result;
  result.set_schema_version(1);
  *result.mutable_identity() = query.identity();
  result.set_query_id(query.query_id());
  result.set_decision_id(query.decision_id());
  result.set_mode("shadow");
  result.set_application_status("not_applied");
  result.set_status("error");
  auto finish = [&] {
    ensure_safe_evolution_data(result);
    return result;
  };
  if(!same_identity(query.identity(), snapshot_.identity())
     || !authorized_scope(snapshot_.options(), query.target_scope())) {
    result.set_reason("scope_mismatch");
    return finish();
  }
  if(!identifier(query.query_id()) || !identifier(query.decision_id())
     || query.ByteSizeLong() > snapshot_.options().max_query_bytes()) {
    result.set_reason("query_invalid_or_too_large");
    return finish();
  }
  result.set_snapshot_id(snapshot_.snapshot_id());
  auto input = query;
  input.clear_query_id();
  input.clear_decision_id();
  result.set_input_fingerprint(digest(stable_bytes(input)));
  auto text = integration::current_task_retrieval_text(integration::Json(query.task()))
      + "\n" + integration::retrieval_text(integration::Json(query.state()));
  const auto all_keywords = terms(text);
  std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return std::tolower(c); });
  auto condition = [&](const auto& experience) -> const std::string& {
    if(triggers) {
      const auto it = triggers->find(experience.memory_id());
      if(it != triggers->end()) return it->second;
    }
    return experience.trigger_pattern();
  };
  std::map<std::string, std::size_t> document_frequency;
  std::set<std::string> patterns;
  std::size_t documents = 0;
  for(const auto& experience : snapshot_.experiences()) {
    if(!injectable(experience)) continue;
    if(experience.target_scope() != query.target_scope()) continue;
    if(approved_ids && !approved_ids->count(experience.memory_id())) continue;
    if(!patterns.insert(condition(experience)).second) continue;
    ++documents;
    for(const auto& term : applicability_terms(condition(experience), snapshot_.options()))
      ++document_frequency[term];
  }
  struct Match { v1::ShadowAdvice advice; double score = 0; };
  std::vector<Match> matches;
  for(const auto& experience : snapshot_.experiences()) {
    if(!injectable(experience)) continue;
    if(experience.target_scope() != query.target_scope()) continue;
    if(approved_ids && !approved_ids->count(experience.memory_id())) continue;
    Match match;
    const auto& pattern = condition(experience);
    const auto candidates = applicability_terms(pattern, snapshot_.options());
    auto keywords = all_keywords;
    // Words from an explicit error/message literal only count when that whole
    // literal is observed. "file" alone is not "No such file or directory".
    std::set<std::string> literal_terms, observed_literal_terms;
    auto unquoted = pattern;
    for(std::size_t pos = 0; pos < unquoted.size(); ++pos) {
      if(unquoted[pos] != '\'' && unquoted[pos] != '"') continue;
      if(pos && std::isalnum(static_cast<unsigned char>(unquoted[pos - 1]))) continue;
      const auto end = unquoted.find(unquoted[pos], pos + 1);
      if(end == std::string::npos) continue;
      auto literal = unquoted.substr(pos + 1, end - pos - 1);
      const auto words = ascii_term(literal) ? terms(literal) : std::set<std::string>{};
      if(words.size() >= 2 && literal.find(' ') != std::string::npos) {
        literal_terms.insert(words.begin(), words.end());
        std::transform(literal.begin(), literal.end(), literal.begin(), [](unsigned char c) { return std::tolower(c); });
        if(text.find(literal) != std::string::npos) observed_literal_terms.insert(words.begin(), words.end());
        unquoted.replace(pos, end - pos + 1, end - pos + 1, ' ');
      }
      pos = end;
    }
    const auto unquoted_terms = literal_terms.empty() ? std::set<std::string>{} : terms(unquoted);
    bool named_anchor = false, anchor_matched = false;
    std::string anchored_text;
    std::set<std::pair<std::size_t, std::size_t>> paragraphs;
    std::size_t strong_matches = 0;
    for(const auto& term : candidates) {
      if(term.find('_') == std::string::npos) continue;
      // Outcome labels are supporting evidence, not the named operation or
      // object a condition refers to (for example, a generic tool_error).
      bool outcome = false;
      for(const std::string suffix : {"_error", "_status", "_result"})
        outcome |= term.size() >= suffix.size() && term.compare(term.size() - suffix.size(), suffix.size(), suffix) == 0;
      if(outcome) continue;
      named_anchor = true; anchor_matched |= keywords.count(term) != 0;
      if(!keywords.count(term)) continue;
      // Do not borrow success/error words from unrelated report paragraphs.
      // Ordinary newlines still join structured operation/result values.
      for(std::size_t at = 0; (at = text.find(term, at)) != std::string::npos; at += term.size()) {
        std::size_t begin = 0, end = text.size();
        for(const std::string separator : {"\n\n", "\\n\\n", "。", "；"}) {
          const auto left = text.rfind(separator, at), right = text.find(separator, at + term.size());
          if(left != std::string::npos) begin = std::max(begin, left + separator.size());
          if(right != std::string::npos) end = std::min(end, right);
        }
        if(paragraphs.emplace(begin, end).second) anchored_text += text.substr(begin, end - begin) + "\n";
        if(end > at + term.size()) at = end - term.size();
      }
    }
    if(named_anchor && !anchor_matched) continue;
    if(named_anchor) keywords = terms(anchored_text);
    std::vector<std::pair<std::size_t, std::size_t>> spans;
    std::size_t han_units = 0;
    bool ascii_match = false;
    for(const auto& term : candidates) {
      if(!keywords.count(term)) continue;
      if(literal_terms.count(term) && !observed_literal_terms.count(term) && !unquoted_terms.count(term)) continue;
      if(!ascii_term(term)) {
        // Longer Han matches win. Their substrings and overlapping windows
        // cannot satisfy the evidence threshold again.
        if(std::any_of(match.advice.matched_terms().begin(), match.advice.matched_terms().end(),
            [&](const auto& prior) { return prior.find(term) != std::string::npos; })) continue;
        auto pos = pattern.find(term);
        while(pos != std::string::npos) {
          const bool overlaps = std::any_of(spans.begin(), spans.end(), [&](const auto& span) {
            return pos < span.second && pos + term.size() > span.first;
          });
          // A short Han fragment can extend a specific phrase, but cannot
          // independently combine distant generic words into a match.
          const bool adjacent = std::any_of(spans.begin(), spans.end(), [&](const auto& span) {
            return pos == span.second || pos + term.size() == span.first;
          });
          if(!overlaps && (strong_term(term) || adjacent)) break;
          pos = pattern.find(term, pos + 1);
        }
        if(pos == std::string::npos) continue;
        spans.emplace_back(pos, pos + term.size());
        han_units += token_units(term);
      } else if(!strong_term(term)) continue;
      else ascii_match = true;
      match.advice.add_matched_terms(term);
      strong_matches += strong_term(term);
      match.score += information_weight(term)
          * (1.0 + std::log((documents + 1.0) / (document_frequency[term] + 0.5)));
    }
    const auto minimum = snapshot_.options().min_matched_terms();
    if(!strong_matches || match.advice.matched_terms_size() < static_cast<int>(minimum)) continue;
    // Short fragments can complete a longer phrase, not make a single partial
    // phrase sufficient. Two full Han windows provide eight observed characters.
    if(!ascii_match && strong_matches < minimum && han_units < 4U * minimum) continue;
    *match.advice.mutable_experience() = experience;
    match.advice.mutable_experience()->set_trigger_pattern(pattern);
    match.advice.mutable_experience()->clear_match_terms();
    match.advice.set_reason("作用域一致，当前任务或观察命中了独立词项；已排除字段名、提醒和重复词元，仍需评测验证适用性。");
    matches.push_back(std::move(match));
  }
  // Private precedence resolves duplicates, not unrelated retrieval results.
  // A canonical private copy inherits its matching public copy's relevance so
  // that deduplication still works after ranking by task applicability.
  std::map<std::string, Match*> by_id;
  for(auto& match : matches) by_id[match.advice.experience().memory_id()] = &match;
  for(const auto& relation : snapshot_.cross_relations()) if(relation.relation() == "duplicate") {
    const auto own = by_id.find(relation.private_memory_id()), shared = by_id.find(relation.shared_memory_id());
    if(own != by_id.end() && shared != by_id.end()) own->second->score = std::max(own->second->score, shared->second->score);
  }
  std::sort(matches.begin(), matches.end(), [](const auto& a, const auto& b) {
    if(a.score != b.score) return a.score > b.score;
    if(a.advice.experience().application_shared() != b.advice.experience().application_shared())
      return !a.advice.experience().application_shared();
    if(a.advice.matched_terms_size() != b.advice.matched_terms_size())
      return a.advice.matched_terms_size() > b.advice.matched_terms_size();
    return a.advice.experience().memory_id() < b.advice.experience().memory_id();
  });
  std::size_t bytes_used = 0;
  std::set<std::string> selected;
  for(const auto& match : matches) {
    const auto& advice = match.advice;
    if(advice.experience().application_shared() && std::any_of(snapshot_.cross_relations().begin(),
        snapshot_.cross_relations().end(), [&](const auto& r) {
          return r.shared_memory_id() == advice.experience().memory_id() && selected.count(r.private_memory_id());
        })) continue;
    const auto bytes = to_json(advice, false).size();
    if(bytes_used + bytes > snapshot_.options().max_advice_bytes()) continue;
    *result.add_advice() = advice;
    bytes_used += bytes;
    selected.insert(advice.experience().memory_id());
    if(result.advice_size() >= static_cast<int>(snapshot_.options().max_advice_items())) break;
  }
  result.set_status(result.advice_size() ? "matched" : "no_match");
  result.set_reason(result.advice_size() ? "shadow_only"
      : (matches.empty() ? "no_applicable_experience" : "advice_budget_exceeded"));
  return finish();
}

v1::TraceEvent shadow_trace_event(const v1::AdviceResult& result,
                                 const std::string& producer_id,
                                 std::uint64_t sequence) {
  require(identifier(producer_id) && sequence > 0
              && result.mode() == "shadow" && result.application_status() == "not_applied",
          "shadow_event_invalid");
  ensure_safe_evolution_data(result);
  v1::TraceEvent event;
  event.set_event_id(producer_id + ":" + std::to_string(sequence));
  event.set_kind("custom");
  event.set_producer_id(producer_id);
  event.set_sequence(sequence);
  event.set_actor_id("evolution.shadow");
  event.set_operation("evolution.shadow.query");
  require(google::protobuf::util::JsonStringToMessage(
              to_json(result, false), event.mutable_output()).ok(),
          "shadow_event_serialization_failed");
  return event;
}

}  // namespace trajectory::evolution
