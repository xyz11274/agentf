#pragma once

#include <initializer_list>
#include <map>
#include <string>
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

namespace agentflow::trajectory::model_input {

using Value = rapidjson::Value;
using Allocator = rapidjson::Document::AllocatorType;

inline std::string bytes(const Value& value) {
  rapidjson::StringBuffer buffer;
  rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
  value.Accept(writer);
  return {buffer.GetString(), buffer.GetSize()};
}

inline void omit(Value& value, std::initializer_list<const char*> fields) {
  if(value.IsObject()) for(const auto* field : fields) value.RemoveMember(field);
}

inline void runtime_metadata_view(Value& attributes) {
  omit(attributes, {"protocol", "agent_version_id", "evaluation_trial_id"});
  if(attributes.IsObject() && attributes.HasMember("extensions"))
    // SDK reference keys have already been resolved into execution identities
    // and caused_by. Keep arbitrary application extensions and policy scope.
    omit(attributes["extensions"], {"point_reference"});
}

// These fields identify storage/transport, not an Agent's decisions. Keep
// timestamps, execution/loop identities and caused_by for concurrent evidence.
inline void event_view(Value& event) {
  omit(event, {"producer_id", "sequence"});
  if(!event.IsObject()) return;
  // Protobuf scalar defaults convey no observation. Never strip application
  // input/output, explicit empty states, custom attributes or causal links.
  for(const auto* key : {"action_id", "execution_id", "step_id", "loop_id",
                        "operation", "status", "decision", "reason"}) {
    if(event.HasMember(key) && event[key].IsString() && event[key].GetStringLength() == 0)
      event.RemoveMember(key);
  }
  if(event.HasMember("attributes"))
    runtime_metadata_view(event["attributes"]);
  if(event.HasMember("run_attributes")) runtime_metadata_view(event["run_attributes"]);
}

// Only visit analysis packets, never arbitrary tool input or application data.
inline void analysis_view(Value& value) {
  if(value.IsArray()) for(auto& item : value.GetArray()) analysis_view(item);
  if(!value.IsObject()) return;
  for(auto it = value.MemberBegin(); it != value.MemberEnd();) {
    const std::string key(it->name.GetString(), it->name.GetStringLength());
    if(key == "packet_id" || (key.size() >= 12 && key.substr(key.size() - 12) == "_fingerprint"))
      it = value.EraseMember(it);
    else { analysis_view(it->value); ++it; }
  }
}

inline void trace_view(Value& trace) {
  if(!trace.IsObject()) return;
  omit(trace, {"schema_version", "trace_kind"});
  if(trace.HasMember("identity"))
    omit(trace["identity"], {"application_id", "user_id", "conversation_id"});
  if(trace.HasMember("attributes")) runtime_metadata_view(trace["attributes"]);
  if(trace.HasMember("events") && trace["events"].IsArray())
    for(auto& event : trace["events"].GetArray()) event_view(event);
}

inline void task_evidence_view(Value& evidence, bool include_model_requests = true) {
  // Deterministic analysis, provenance and the pending judgment stay local.
  omit(evidence, {"trajectory", "execution_run_id", "loaded_version", "loop_review"});
  if(!evidence.IsObject()) return;
  if(evidence.HasMember("artifacts") && evidence["artifacts"].IsObject())
    for(auto& file : evidence["artifacts"].GetObject()) omit(file.value, {"stored_file"});
  auto execution_view = [&](Value& trace) {
    trace_view(trace);
    if(include_model_requests || !trace.IsObject() || !trace.HasMember("events") || !trace["events"].IsArray()) return;
    for(auto& event : trace["events"].GetArray()) {
      if(!event.IsObject() || !event.HasMember("attributes")) continue;
      auto& attributes = event["attributes"];
      // Only explicitly typed model requests: never guess from provider field
      // names or strip similarly shaped tool arguments/custom activity data.
      if(attributes.IsObject() && attributes.HasMember("semantic") && attributes["semantic"] == "model")
        event.RemoveMember("input");
      // Retrieval diagnostics repeat the query and injected context, not an
      // execution observation. Keep action/progress decisions and receipts.
      if(event.HasMember("kind") && event["kind"] == "custom" && attributes.IsObject()
         && attributes.HasMember("type") && attributes["type"] == "policy.query"
         && attributes.HasMember("decision"))
        omit(attributes["decision"], {"query", "context", "experiences", "policy_id"});
    }
  };
  if(evidence.HasMember("trace")) execution_view(evidence["trace"]);
  if(evidence.HasMember("traces") && evidence["traces"].IsArray())
    for(auto& trace : evidence["traces"].GetArray()) execution_view(trace);
}

// Exact sharing is not summarization: each original value remains at its first
// location. Only registered paths are references; lookalike user objects are data.
class Sharing {
 public:
  Sharing(Allocator& allocator, const char* reference_key, bool containers = true)
      : allocator_(allocator), reference_key_(reference_key), containers_(containers) {}

  void collect(Value& value, const std::string& path, bool share_node = true) {
    if(replacements_ >= 4096 || path.size() > 4096) return;
    if(share_node && (value.IsString() || (containers_ && (value.IsArray() || value.IsObject())))) {
      const auto text = bytes(value);
      if(text.size() >= 512) {
        const auto found = sources_.find(text);
        if(found != sources_.end()) {
          auto& reference = references_[found->second];
          reference["paths"].PushBack(Value(path.c_str(), allocator_), allocator_);
          Value source;
          source.CopyFrom(reference["source"], allocator_);
          value.SetObject();
          value.AddMember(Value(reference_key_, allocator_), source, allocator_);
          ++replacements_;
          return;
        }
        // Bound local working memory as well as model input. Large histories
        // still share their individual messages when the whole array is large.
        if(text.size() <= 1048576 && stored_bytes_ + text.size() <= 4194304 && sources_.size() < 4096) {
          sources_.emplace(text, references_.Size());
          stored_bytes_ += text.size();
          Value reference(rapidjson::kObjectType);
          reference.AddMember("source", Value(path.c_str(), allocator_), allocator_);
          reference.AddMember("paths", Value(rapidjson::kArrayType), allocator_);
          references_.PushBack(reference, allocator_);
        }
      }
    }
    if(value.IsArray()) {
      for(rapidjson::SizeType i = 0; i < value.Size(); ++i)
        collect(value[i], path + '/' + std::to_string(i));
    } else if(value.IsObject()) {
      for(auto it = value.MemberBegin(); it != value.MemberEnd(); ++it) {
        std::string token;
        for(char c : std::string(it->name.GetString(), it->name.GetStringLength()))
          token += c == '~' ? "~0" : c == '/' ? "~1" : std::string(1, c);
        collect(it->value, path + '/' + token);
      }
    }
  }

  Value references() {
    Value used(rapidjson::kArrayType);
    for(auto& item : references_.GetArray())
      if(!item["paths"].Empty()) used.PushBack(item, allocator_);
    return used;
  }

 private:
  Allocator& allocator_;
  const char* reference_key_;
  bool containers_;
  std::map<std::string, rapidjson::SizeType> sources_;
  Value references_{rapidjson::kArrayType};
  std::size_t replacements_ = 0, stored_bytes_ = 0;
};

inline void dialogue_view(rapidjson::Document& payload, bool standard_trace) {
  omit(payload, {"schema_version", "evaluation_scope", "content_policy", "source"});
  std::map<std::string, const Value*> episodes;
  for(const auto& episode : payload["episodes"].GetArray())
    episodes.emplace(episode["episode_id"].GetString(), &episode);
  for(auto& episode : payload["episodes"].GetArray()) {
    omit(episode, {"selected"}); // selected_episode_ids is the single whitelist.
    omit(episode["evidence"], {"episode_id"});
    analysis_view(episode["evidence"]);
    if(episode.HasMember("rule_assessment")) {
      const auto& rule = episode["rule_assessment"];
      if(rule.IsObject() && rule.MemberCount() == 2 && rule.HasMember("verdict")
         && rule["verdict"] == "SEMANTIC_VERDICT_UNASSESSED" && rule.HasMember("explanation")
         && rule["explanation"] == "未命中确定性异常规则；这不等同于已证明该步骤有效。")
        episode.RemoveMember("rule_assessment");
    }
    auto& evidence = episode["evidence"];
    if(evidence.HasMember("dispatch") && evidence["dispatch"].IsObject()
       && evidence["dispatch"].ObjectEmpty()) evidence.RemoveMember("dispatch");
    // Neighbor identity carries the relationship; repeat no facts already
    // present on that episode. Keep missing targets and inconsistent facts.
    for(const auto* key : {"previous_same_owner", "next_same_owner"}) {
      if(!evidence.HasMember(key) || !evidence[key].IsObject()) continue;
      auto& neighbor = evidence[key];
      if(!neighbor.HasMember("episode_id") || !neighbor["episode_id"].IsString()) continue;
      const auto found = episodes.find(neighbor["episode_id"].GetString());
      if(found == episodes.end()) continue;
      const auto& target = (*found->second)["evidence"];
      for(auto it = neighbor.MemberBegin(); it != neighbor.MemberEnd();) {
        const auto other = target.FindMember(it->name);
        if(it->name != "episode_id" && other != target.MemberEnd() && it->value == other->value)
          it = neighbor.EraseMember(it);
        else ++it;
      }
    }
  }
  if(standard_trace) for(auto& event : payload["content_events"].GetArray()) {
    auto& data = event["data"];
    event_view(data);
    // The outer event already supplies these fields exactly once.
    omit(data, {"event_id", "kind", "timestamp_us", "iteration"});
    if(data.HasMember("identity")) omit(data["identity"], {"application_id", "user_id", "conversation_id"});
  }
  rapidjson::Document compact;
  compact.CopyFrom(payload, compact.GetAllocator());
  Sharing sharing(compact.GetAllocator(), "$content_ref");
  auto& events = compact["content_events"];
  for(rapidjson::SizeType i = 0; i < events.Size(); ++i)
    sharing.collect(events[i]["data"], "/content_events/" + std::to_string(i) + "/data", false);
  auto references = sharing.references();
  if(!references.Empty()) {
    compact.AddMember("content_value_refs", references, compact.GetAllocator());
    if(bytes(compact).size() < bytes(payload).size()) payload.Swap(compact);
  }
}

}  // namespace agentflow::trajectory::model_input
