// 将标准轨迹归约为确定性指标；模型评测在其上补充语义判断，而不覆盖原始事实。
#include "trace_internal.h"

#include <algorithm>
#include <iomanip>
#include <map>
#include <set>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

#include <google/protobuf/io/coded_stream.h>
#include <google/protobuf/io/zero_copy_stream_impl_lite.h>
#include <openssl/evp.h>
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include "agentflow/trajectory/hybrid_evaluator.h"

namespace trajectory::evolution {
namespace {
namespace legacy = agentflow::trajectory;

std::string digest(const std::string& value) {
  unsigned char bytes[EVP_MAX_MD_SIZE];
  unsigned int size = 0;
  if(EVP_Digest(value.data(), value.size(), bytes, &size, EVP_sha256(), nullptr) != 1) {
    throw legacy::EvaluationError("cannot fingerprint standard trajectory evidence");
  }
  std::ostringstream text;
  for(unsigned int i = 0; i < size; ++i) {
    text << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(bytes[i]);
  }
  return text.str();
}

std::string fingerprint(const google::protobuf::Message& value) {
  std::string bytes;
  {
    google::protobuf::io::StringOutputStream stream(&bytes);
    google::protobuf::io::CodedOutputStream coded(&stream);
    coded.SetSerializationDeterministic(true);
    if(!value.SerializeToCodedStream(&coded)) {
      throw legacy::EvaluationError("cannot serialize trajectory evidence");
    }
  }
  return digest(bytes);
}

std::string scope_component(const std::string& value) {
  const char* hex = "0123456789ABCDEF";
  std::string result;
  for(unsigned char c : value) {
    if((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
       || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.') {
      result.push_back(c);
    } else {
      result.push_back('%');
      result.push_back(hex[c >> 4]);
      result.push_back(hex[c & 15]);
    }
  }
  return result;
}

void write_canonical(const rapidjson::Value& value,
                     rapidjson::Writer<rapidjson::StringBuffer>& writer) {
  if(value.IsObject()) {
    std::vector<const rapidjson::Value::Member*> members;
    for(auto it = value.MemberBegin(); it != value.MemberEnd(); ++it) members.push_back(&*it);
    std::sort(members.begin(), members.end(), [](const auto* left, const auto* right) {
      return std::string(left->name.GetString(), left->name.GetStringLength())
          < std::string(right->name.GetString(), right->name.GetStringLength());
    });
    writer.StartObject();
    for(const auto* item : members) {
      writer.Key(item->name.GetString(), item->name.GetStringLength());
      write_canonical(item->value, writer);
    }
    writer.EndObject();
  } else if(value.IsArray()) {
    writer.StartArray();
    for(const auto& item : value.GetArray()) write_canonical(item, writer);
    writer.EndArray();
  } else {
    value.Accept(writer);
  }
}
}  // namespace

legacy::AnalysisReport analyze_trace(const ValidatedTrace& validated) {
  const auto& trace = validated.trace;
  legacy::AnalysisReport report;
  report.analyzer_version = "standard-trace-analyzer-v2";
  report.rule_set_version = "standard-trace-rules-v1";
  report.dialogue_id = trace.identity().run_id();
  report.user_id = trace.identity().user_id();
  report.conversation_id = trace.identity().conversation_id();
  report.trace_integrity_valid = validated.report.trace_valid();
  report.summary.event_count = validated.report.event_count();
  report.summary.duplicate_event_count = validated.report.duplicate_event_count();
  for(const auto& issue : validated.report.issues()) {
    legacy::TraceIssue target;
    target.code = issue.code();
    target.severity = issue.severity();
    target.message = "Standard trace validation: " + issue.code();
    target.issue_id = "trace-issue:" + issue.code() + ":" + std::to_string(issue.event_index());
    report.trace_issues.push_back(std::move(target));
  }
  if(!report.trace_integrity_valid) return report;

  std::map<std::string, const v1::TraceEvent*> step_ends, action_ends;
  std::unordered_map<std::string, int> event_positions;
  const v1::TraceEvent* run_start = nullptr;
  const v1::TraceEvent* run_end = nullptr;
  for(int i = 0; i < trace.events_size(); ++i) {
    const auto& event = trace.events(i);
    event_positions.emplace(event.event_id(), i);
    if(event.kind() == "step.end") step_ends[event.step_id()] = &event;
    if(event.kind() == "action.end") action_ends[event.action_id()] = &event;
    if(event.kind() == "run.start") run_start = &event;
    if(event.kind() == "run.end") run_end = &event;
  }
  std::map<std::string, std::size_t> episode_for_step;
  std::map<std::string, std::string> iteration_for_episode;
  auto append_episode = [&](const v1::TraceEvent& begin, const v1::TraceEvent& end) {
    legacy::LoopEpisode episode;
    episode.episode_id = "step:" + begin.event_id();
    episode.dialogue_id = report.dialogue_id;
    episode.scope = legacy::LoopScope::agent;
    const auto policy_actor = begin.attributes().fields().find("policy_scope");
    const auto actor_scope = policy_actor != begin.attributes().fields().end()
        && policy_actor->second.kind_case() == google::protobuf::Value::kStringValue
        && !policy_actor->second.string_value().empty() ? policy_actor->second.string_value() : begin.actor_id();
    episode.actor_id = scope_component(trace.identity().agent_id())
        + "/actor:" + scope_component(actor_scope);
    episode.owner_id = digest(begin.actor_id() + '\0' + begin.execution_id()
                              + '\0' + begin.loop_id());
    if(!begin.loop_id().empty() && begin.iteration() > 0) {
      const auto owner = policy_actor != begin.attributes().fields().end() ? begin.loop_id() : episode.owner_id;
      iteration_for_episode[episode.episode_id] = owner + '\0' + std::to_string(begin.iteration());
    }
    episode.round = begin.iteration();
    episode.step_index = report.episodes.size() + 1U;
    episode.request_event_id = begin.event_id();
    episode.response_event_id = end.event_id();
    episode.started_at_us = begin.timestamp_us();
    episode.ended_at_us = end.timestamp_us();
    episode.action = begin.operation();
    episode.input_fingerprint = begin.has_input() ? fingerprint(begin.input()) : "";
    episode.outcome_fingerprint = end.has_output() ? fingerprint(end.output()) : "";
    episode.terminal_status = end.status();
    episode.outcome_failed = end.status() == "failed";
    episode.source_event_ids = {begin.event_id(), end.event_id()};
    if(begin.kind() == "run.start") {
      episode.terminal = true;
      episode.terminal_status = end.outcome().execution_status();
      episode.outcome_failed = end.outcome().business_status() == "failed";
    }
    episode_for_step[begin.step_id()] = report.episodes.size();
    report.episodes.push_back(std::move(episode));
  };
  for(const auto& event : trace.events()) {
    if(event.kind() == "step.start") append_episode(event, *step_ends.at(event.step_id()));
  }
  // A black-box run still has an assessable episode with its actual input/output.
  if(report.episodes.empty()) append_episode(*run_start, *run_end);

  struct PreviousAction {
    const v1::TraceEvent* begin;
    const v1::TraceEvent* end;
  };
  std::map<std::string, PreviousAction> previous;
  auto caused_after = [&](const v1::TraceEvent& current, const v1::TraceEvent& earlier) {
    // The validated trace is topologically ordered. Follow explicit causes through
    // activity/control envelopes without mistaking arrival order for causality.
    const auto target = event_positions.at(earlier.event_id());
    std::vector<int> pending;
    std::unordered_set<int> visited;
    for(const auto& id : current.caused_by()) pending.push_back(event_positions.at(id));
    while(!pending.empty()) {
      const auto index = pending.back(); pending.pop_back();
      if(index == target) return true;
      if(index < target || !visited.insert(index).second) continue;
      for(const auto& id : trace.events(index).caused_by()) pending.push_back(event_positions.at(id));
    }
    return false;
  };
  for(const auto& event : trace.events()) {
    const auto found = episode_for_step.find(event.step_id());
    if(found == episode_for_step.end()) continue;
    auto& episode = report.episodes[found->second];
    if(std::find(episode.source_event_ids.begin(), episode.source_event_ids.end(),
                 event.event_id()) == episode.source_event_ids.end()) {
      episode.source_event_ids.push_back(event.event_id());
    }
    if(event.kind() == "loop.decision") {
      episode.control_event_id = event.event_id();
      episode.continued = event.decision() == "continue" || event.decision() == "replan";
      episode.terminal = event.decision() == "stop";
      episode.reason = event.reason();
    }
    if(event.kind() != "action.start") continue;
    const auto& end = *action_ends.at(event.action_id());
    legacy::ToolActionSummary action;
    action.tool_call_id = event.action_id();
    action.tool_name = event.operation();
    action.status = end.status();
    action.arguments_fingerprint = event.has_input() ? fingerprint(event.input()) : "";
    action.result_fingerprint = end.has_output() ? fingerprint(end.output()) : "";
    action.error_class = end.error().code();
    action.retryability = end.error().retryability();
    action.call_event_id = event.event_id();
    action.result_event_id = end.event_id();
    const auto semantic = event.attributes().fields().find("semantic");
    const auto kind = semantic == event.attributes().fields().end() ? "action" : semantic->second.string_value();
    const auto executed = end.attributes().fields().find("executed");
    const bool ran = executed == end.attributes().fields().end() || executed->second.bool_value();
    // Older traces without semantic tags use action for a tool. Model/custom
    // activities retain their evidence and retry diagnostics, not tool counts.
    if(kind == "action" && ran)
      episode.tool_actions.push_back(action);
    episode.has_non_retryable_failure = episode.has_non_retryable_failure
        || (ran && end.status() == "failed" && end.error().retryability() == "non_retryable");
    if(!ran || event.loop_id().empty() || !event.has_input()) continue;
    const std::string group = fingerprint(event.input()) + "\n" + event.actor_id()
        + "\n" + event.execution_id() + "\n" + event.loop_id() + "\n" + event.operation() + "\n" + kind;
    const auto prior = previous.find(group);
    // Concurrent actions and distinct execution instances are never retries.
    if(prior != previous.end()
       && ((prior->second.end->producer_id() == event.producer_id()
            && prior->second.end->sequence() < event.sequence())
           || caused_after(event, *prior->second.end))
       && prior->second.begin->iteration() < event.iteration()
       && end.status() == "failed" && prior->second.end->status() == "failed"
       && end.error().retryability() == "non_retryable"
       && prior->second.end->error().retryability() == "non_retryable"
       && !end.error().code().empty()
       && end.error().code() == prior->second.end->error().code()) {
      legacy::LoopDiagnostic diagnostic;
      diagnostic.diagnostic_id = "diagnostic:" + digest(event.event_id());
      diagnostic.episode_id = episode.episode_id;
      diagnostic.category = legacy::InvalidLoopCategory::non_retryable_repeat;
      diagnostic.counts_as_invalid = true;
      diagnostic.confidence = 1.0;
      diagnostic.severity = "warning";
      diagnostic.summary = "同一执行实例在明确不可重试失败后再次以相同参数执行同一操作。";
      diagnostic.observable_cause = "输入、操作和错误一致，前次调用已结束且轮次递增。";
      diagnostic.tool_name = event.operation();
      diagnostic.error_class = end.error().code();
      diagnostic.evidence_event_ids = {
          prior->second.begin->event_id(), prior->second.end->event_id(),
          event.event_id(), end.event_id()};
      episode.diagnostic_ids.push_back(diagnostic.diagnostic_id);
      report.diagnostics.push_back(std::move(diagnostic));
    }
    previous.insert_or_assign(group, PreviousAction{&event, &end});
  }
  std::set<std::string> invalid_iterations;
  for(auto& episode : report.episodes) {
    episode.all_tool_results_failed = !episode.tool_actions.empty()
        && std::all_of(episode.tool_actions.begin(), episode.tool_actions.end(),
                       [](const auto& action) { return action.status == "failed"; });
    if(!episode.diagnostic_ids.empty()) {
      ++report.summary.invalid_episode_count;
      const auto found = iteration_for_episode.find(episode.episode_id);
      if(found != iteration_for_episode.end()) invalid_iterations.insert(found->second);
    }
  }
  report.summary.episode_count = report.episodes.size();
  report.summary.invalid_loop_lower_bound = invalid_iterations.size();
  report.summary.category_counts["non_retryable_repeat"] = report.diagnostics.size();
  return report;
}

std::string trace_content_jsonl(const v1::TraceRun& trace) {
  std::string result;
  for(const auto& event : trace.events()) {
    rapidjson::Document object(rapidjson::kObjectType);
    auto& allocator = object.GetAllocator();
    object.AddMember("event_id", rapidjson::Value(event.event_id().c_str(), allocator), allocator);
    object.AddMember("type", rapidjson::Value(event.kind().c_str(), allocator), allocator);
    object.AddMember("timestamp_us", event.timestamp_us(), allocator);
    object.AddMember("round", event.iteration(), allocator);
    // Actual structured content goes through the existing recursive sanitizer.
    rapidjson::Document data;
    const auto json = to_json(event, false);
    data.Parse(json.data(), json.size());
    if(data.HasParseError()) throw legacy::EvaluationError("cannot prepare trace evidence");
    rapidjson::Value value;
    value.CopyFrom(data, allocator);
    if(event.kind() == "run.start") {
      for(const auto& item : std::vector<std::pair<const char*, const google::protobuf::Message*>>{
               {"identity", &trace.identity()}, {"run_attributes", &trace.attributes()}}) {
        const auto context = to_json(*item.second, false);
        rapidjson::Document parsed;
        parsed.Parse(context.data(), context.size());
        if(parsed.HasParseError()) throw legacy::EvaluationError("cannot prepare trace context");
        rapidjson::Value copied;
        copied.CopyFrom(parsed, allocator);
        value.AddMember(rapidjson::Value(item.first, allocator), copied, allocator);
      }
    }
    object.AddMember("data", value, allocator);
    rapidjson::StringBuffer buffer;
    rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
    // Protobuf Struct map iteration order must not change evidence identity.
    write_canonical(object, writer);
    result.append(buffer.GetString(), buffer.GetSize()).push_back('\n');
  }
  return result;
}

}  // namespace trajectory::evolution
