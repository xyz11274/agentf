// 标准轨迹契约：检查身份、事件结构、起止配对和因果引用。
// 格式合法与业务成功是两个独立判断；此处只建立后续分析所依赖的可信结构。
// 校验问题必须显式返回，不能默默补造缺失的执行事件。

#include "trajectory_evolution/trace.h"

#include "agentflow/trajectory/hybrid_evaluator.h"
#include "agentflow/trajectory/loop_episode_analyzer.h"

#include <algorithm>
#include <fstream>
#include <limits>
#include <map>
#include <queue>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <google/protobuf/util/json_util.h>
#include <google/protobuf/util/message_differencer.h>

namespace trajectory::evolution {
namespace {
using agentflow::trajectory::EvaluationError;

// 检查标识符是否符合协议允许的字符和长度，拒绝把任意路径当作身份使用。
bool identifier(const std::string &value) {
  return !value.empty() && value.size() <= 512U &&
         std::none_of(value.begin(), value.end(),
                      [](unsigned char c) { return c < 32 || c == 127; });
}

// 检查是否为协议允许的终态，不把任意状态名称作为已完成。
bool terminal_status(const std::string &value) {
  return value == "completed" || value == "failed" || value == "cancelled" || value == "unknown";
}

// 创建结构化检查问题，使用固定类别和定位字段，避免回显敏感正文。
void issue(v1::CapabilityReport *report, const std::string &code, std::size_t index = 0,
           bool warning = false) {
  auto *item = report->add_issues();
  item->set_code(code);
  item->set_event_index(index);
  item->set_severity(warning ? "warning" : "error");
}

// 返回各能力等级所需的轨迹与控制条件，供接入检查复用。
const std::vector<std::pair<std::string, v1::Level>> &requirements() {
  static const std::vector<std::pair<std::string, v1::Level>> values = {
      {"run_observation", v1::L0}, {"experience_injection", v1::L1},  {"action_control", v1::L2},
      {"loop_control", v1::L3},    {"controlled_experiment", v1::L4},
  };
  return values;
}

// 读取原始字节并检查输入上限，防止日志或配置无限占用内存。
std::string read_bytes(const std::filesystem::path &path, std::size_t limit) {
  std::ifstream input(path, std::ios::binary | std::ios::ate);
  if (!input) {
    throw EvaluationError("cannot read trajectory input");
  }
  const auto size = input.tellg();
  if (size < 0 || static_cast<std::uint64_t>(size) > limit ||
      size > std::numeric_limits<int>::max()) {
    throw EvaluationError("trajectory input exceeds byte limit");
  }
  input.seekg(0);
  std::string content(static_cast<std::size_t>(size), '\0');
  if (!content.empty() && !input.read(content.data(), size)) {
    throw EvaluationError("failed to read trajectory input");
  }
  return content;
}

// 检查事件集合是否包含指定类型，用于判断已有证据而非推断业务成功。
bool has_kind(const v1::TraceRun &trace, const std::string &kind) {
  return std::any_of(trace.events().begin(), trace.events().end(),
                     [&](const auto &event) { return event.kind() == kind; });
}

} // 结束当前匿名命名空间。

// 将当前结果转换为公开 JSON 表示，内部临时状态不随意暴露。
std::string to_json(const google::protobuf::Message &message, bool pretty) {
  google::protobuf::util::JsonPrintOptions options;
  options.add_whitespace = pretty;
  options.preserve_proto_field_names = true;
#if GOOGLE_PROTOBUF_VERSION >= 5026000
  options.always_print_fields_with_no_presence = true;
#else
  options.always_print_primitive_fields = true;
#endif
  std::string output;
  if (!google::protobuf::util::MessageToJsonString(message, &output, options).ok()) {
    throw EvaluationError("cannot serialize trajectory protocol JSON");
  }
  return output;
}

// 判断路径是否属于标准轨迹输入，避免把其他产物误当日志。
bool is_trace_path(const std::filesystem::path &path) {
  const auto name = path.filename().string();
  return (name.size() >= 11U && name.compare(name.size() - 11, 11, ".trace.json") == 0) ||
         (name.size() >= 9U && name.compare(name.size() - 9, 9, ".trace.pb") == 0);
}

// 解析标准轨迹 JSON 并按协议校验，不泄露解析器携带的原文片段。
v1::TraceRun parse_trace_json(const std::string &json, TraceLimits limits) {
  if (json.size() > limits.max_bytes) {
    throw EvaluationError("trajectory input exceeds byte limit");
  }
  v1::TraceRun trace;
  if (!google::protobuf::util::JsonStringToMessage(json, &trace).ok()) {
    // 解析器详细错误可能包含任意输入，包括凭据。
    throw EvaluationError("invalid standard trajectory JSON");
  }
  if (trace.schema_version() != 1 || trace.trace_kind() != "trajectory.v1") {
    throw EvaluationError("unsupported standard trajectory schema");
  }
  return trace;
}

// 读取并解析标准轨迹文件，保留输入大小与格式约束。
v1::TraceRun read_trace(const std::filesystem::path &path, TraceLimits limits) {
  const std::string bytes = read_bytes(path, limits.max_bytes);
  if (path.extension() != ".pb") {
    return parse_trace_json(bytes, limits);
  }
  v1::TraceRun trace;
  if (!trace.ParseFromString(bytes) || trace.schema_version() != 1 ||
      trace.trace_kind() != "trajectory.v1") {
    throw EvaluationError("invalid standard trajectory Protobuf");
  }
  return trace;
}

// 检查事件身份、父子关系、因果链接和生命周期完整性，发现缺口明确报告。
ValidatedTrace validate_trace(const v1::TraceRun &input, TraceLimits limits) {
  if (input.schema_version() != 1 || input.trace_kind() != "trajectory.v1") {
    throw EvaluationError("unsupported standard trajectory schema");
  }
  if (limits.max_bytes == 0 || limits.max_events == 0 || limits.max_events > 100000U ||
      limits.max_bytes > 256U * 1024U * 1024U || input.ByteSizeLong() > limits.max_bytes ||
      static_cast<std::size_t>(input.events_size()) > limits.max_events) {
    throw EvaluationError("standard trajectory exceeds resource limits");
  }
  ValidatedTrace output;
  output.trace = input;
  output.trace.clear_events();
  auto &report = output.report;
  report.set_schema_version(1);
  report.set_source_format("trajectory.v1");
  report.set_component_level(v1::L3);
  const auto capture = input.attributes().fields().find("capture_complete");
  if (capture != input.attributes().fields().end() &&
      (capture->second.kind_case() != google::protobuf::Value::kBoolValue ||
       !capture->second.bool_value())) {
    issue(&report, "incomplete_capture");
  }
  if (!identifier(input.identity().run_id()) || !identifier(input.identity().agent_id())) {
    issue(&report, "invalid_run_identity");
  }
  std::unordered_map<std::string, int> ids;
  for (int i = 0; i < input.events_size(); ++i) {
    const auto &event = input.events(i);
    const auto found = ids.find(event.event_id());
    if (found != ids.end()) {
      if (google::protobuf::util::MessageDifferencer::Equals(event,
                                                             output.trace.events(found->second))) {
        report.set_duplicate_event_count(report.duplicate_event_count() + 1U);
        issue(&report, "duplicate_event", i, true);
      } else {
        issue(&report, "conflicting_duplicate_event", i);
      }
      continue;
    }
    ids.emplace(event.event_id(), output.trace.events_size());
    *output.trace.add_events() = event;
  }
  const auto &trace = output.trace;
  const int count = trace.events_size();
  report.set_event_count(count);
  std::vector<std::vector<int>> edges(count);
  std::vector<int> indegree(count, 0);
  std::unordered_set<std::uint64_t> edge_ids;
  auto edge = [&](int from, int to) {
    if (edge_ids.insert(static_cast<std::uint64_t>(from) * count + to).second) {
      if (edge_ids.size() > 1000000U) {
        throw EvaluationError("trajectory exceeds total causal link limit");
      }
      edges[from].push_back(to);
      ++indegree[to];
    }
  };
  std::map<std::string, std::map<std::uint64_t, int>> producers;
  std::map<std::string, int> step_starts, step_ends, action_starts, action_ends;
  std::vector<int> starts, ends;
  const std::set<std::string> kinds = {
      "run.start",     "run.end",    "step.start",    "step.end",
      "action.start",  "action.end", "loop.decision", "advice.applied",
      "state.changed", "evaluation", "custom",
  };
  for (int i = 0; i < count; ++i) {
    const auto &event = trace.events(i);
    if (!identifier(event.event_id()) || !identifier(event.producer_id()) ||
        !identifier(event.actor_id()) || event.sequence() == 0) {
      issue(&report, "invalid_event_identity", i);
    }
    if (!producers[event.producer_id()].emplace(event.sequence(), i).second) {
      issue(&report, "duplicate_producer_sequence", i);
    }
    if (kinds.count(event.kind()) == 0) {
      issue(&report, "unknown_event_kind", i);
    }
    if (event.caused_by_size() > 64) {
      throw EvaluationError("trajectory event exceeds causal link limit");
    }
    for (const auto &cause : event.caused_by()) {
      const auto found = ids.find(cause);
      if (found == ids.end()) {
        issue(&report, "missing_causal_event", i);
      } else {
        edge(found->second, i);
      }
    }
    if (event.kind() == "run.start") {
      starts.push_back(i);
      if (!event.has_input())
        issue(&report, "missing_run_input", i);
    } else if (event.kind() == "run.end") {
      ends.push_back(i);
      if (!event.has_output())
        issue(&report, "missing_run_output", i);
      if (!terminal_status(event.outcome().execution_status())) {
        issue(&report, "missing_execution_outcome", i);
      }
      const auto &business = event.outcome().business_status();
      if (business != "succeeded" && business != "failed" && business != "partial" &&
          business != "unknown") {
        issue(&report, "missing_business_outcome", i);
      }
    }
    auto register_boundary = [&](std::map<std::string, int> &index, const std::string &key) {
      if (!identifier(key) || !identifier(event.execution_id())) {
        issue(&report, "missing_execution_or_pair_id", i);
      }
      if (!index.emplace(key, i).second)
        issue(&report, "duplicate_pair_boundary", i);
    };
    if (event.kind() == "step.start")
      register_boundary(step_starts, event.step_id());
    if (event.kind() == "step.end")
      register_boundary(step_ends, event.step_id());
    if (event.kind() == "action.start") {
      register_boundary(action_starts, event.action_id());
      if (!identifier(event.operation()))
        issue(&report, "missing_operation", i);
    }
    if (event.kind() == "action.end")
      register_boundary(action_ends, event.action_id());
    if ((event.kind().rfind("action.", 0) == 0 || event.kind() == "loop.decision") &&
        !identifier(event.step_id())) {
      issue(&report, "missing_step_reference", i);
    }
    if (event.loop_id().empty() != (event.iteration() == 0)) {
      issue(&report, "incomplete_loop_identity", i);
    }
    if (event.has_error() && event.status() == "completed") {
      issue(&report, "success_with_error", i);
    }
    if ((event.kind() == "step.end" || event.kind() == "action.end") &&
        !terminal_status(event.status())) {
      issue(&report, "missing_terminal_status", i);
    }
    if (event.has_error() && event.error().retryability() != "retryable" &&
        event.error().retryability() != "non_retryable" &&
        event.error().retryability() != "unknown") {
      issue(&report, "invalid_retryability", i);
    }
    if (event.kind() == "loop.decision") {
      if (!identifier(event.execution_id()) || !identifier(event.loop_id()) ||
          event.iteration() == 0 ||
          (event.decision() != "continue" && event.decision() != "replan" &&
           event.decision() != "stop")) {
        issue(&report, "invalid_loop_decision", i);
      }
    }
    if (event.kind() == "advice.applied") {
      const auto &application = event.application();
      if (!identifier(application.query_id()) || !identifier(application.policy_id()) ||
          !identifier(application.policy_revision()) || !identifier(application.decision_id()) ||
          (application.status() != "applied" && application.status() != "rejected" &&
           application.status() != "failed")) {
        issue(&report, "invalid_application_receipt", i);
      }
    }
  }
  if (starts.size() != 1U)
    issue(&report, "run_start_count");
  if (ends.size() != 1U)
    issue(&report, "run_end_count");
  if (starts.size() == 1U && ends.size() == 1U) {
    for (int i = 0; i < count; ++i) {
      if (i != starts[0])
        edge(starts[0], i);
      if (i != ends[0])
        edge(i, ends[0]);
    }
  }
  for (const auto &producer : producers) {
    int previous = -1;
    for (const auto &record : producer.second) {
      if (previous >= 0)
        edge(previous, record.second);
      previous = record.second;
    }
  }
  auto pair = [&](const std::map<std::string, int> &begin, const std::map<std::string, int> &end) {
    for (const auto &item : begin) {
      const auto match = end.find(item.first);
      if (match == end.end()) {
        issue(&report, "missing_pair_end", item.second);
        continue;
      }
      const auto &left = trace.events(item.second);
      const auto &right = trace.events(match->second);
      if (left.actor_id() != right.actor_id() || left.execution_id() != right.execution_id() ||
          left.step_id() != right.step_id() || left.loop_id() != right.loop_id() ||
          left.iteration() != right.iteration()) {
        issue(&report, "pair_context_mismatch", match->second);
      }
      edge(item.second, match->second);
    }
    for (const auto &item : end) {
      if (begin.count(item.first) == 0)
        issue(&report, "missing_pair_start", item.second);
    }
  };
  pair(step_starts, step_ends);
  pair(action_starts, action_ends);
  for (int i = 0; i < count; ++i) {
    const auto &event = trace.events(i);
    if (event.step_id().empty() || event.kind().rfind("step.", 0) == 0)
      continue;
    const auto begin = step_starts.find(event.step_id());
    const auto end = step_ends.find(event.step_id());
    if (begin == step_starts.end() || end == step_ends.end()) {
      issue(&report, "unknown_step_reference", i);
    } else {
      const auto &parent = trace.events(begin->second);
      if (parent.actor_id() != event.actor_id() || parent.execution_id() != event.execution_id() ||
          parent.loop_id() != event.loop_id() || parent.iteration() != event.iteration()) {
        issue(&report, "step_context_mismatch", i);
      }
      edge(begin->second, i);
      // 循环决定可以晚于步骤结果，但动作必须在步骤结果之前结束。
      if (event.kind().rfind("action.", 0) == 0)
        edge(i, end->second);
    }
  }
  std::priority_queue<std::pair<std::string, int>, std::vector<std::pair<std::string, int>>,
                      std::greater<std::pair<std::string, int>>>
      ready;
  for (int i = 0; i < count; ++i) {
    if (indegree[i] == 0)
      ready.emplace(trace.events(i).event_id(), i);
  }
  std::vector<int> order;
  while (!ready.empty()) {
    const int i = ready.top().second;
    ready.pop();
    order.push_back(i);
    for (int next : edges[i]) {
      if (--indegree[next] == 0)
        ready.emplace(trace.events(next).event_id(), next);
    }
  }
  if (order.size() != static_cast<std::size_t>(count)) {
    issue(&report, "causal_cycle_or_invalid_order");
  } else {
    auto sorted = trace;
    sorted.clear_events();
    for (int i : order)
      *sorted.add_events() = trace.events(i);
    output.trace = std::move(sorted);
  }
  report.set_trace_valid(std::none_of(report.issues().begin(), report.issues().end(),
                                      [](const auto &item) { return item.severity() == "error"; }));
  return output;
}

// 解析轨迹清单及其能力声明，拒绝不符合契约的字段。
v1::CapabilityManifest parse_manifest_json(const std::string &json) {
  v1::CapabilityManifest manifest;
  if (json.size() > 65536U || !google::protobuf::util::JsonStringToMessage(json, &manifest).ok()) {
    throw EvaluationError("invalid capability manifest JSON");
  }
  return manifest;
}

// 根据真实采集与控制条件判断可支持等级，不凭名称宣称能力完整。
v1::CapabilityReport inspect_capabilities(const ValidatedTrace &validated,
                                          const v1::CapabilityManifest &manifest) {
  if (manifest.schema_version() != 1 || manifest.requested_level() < v1::L0 ||
      manifest.requested_level() > v1::L4 ||
      (manifest.fallback() != "strict" && manifest.fallback() != "downgrade")) {
    throw EvaluationError("invalid capability manifest contract");
  }
  std::set<std::string> known;
  for (const auto &item : requirements())
    known.insert(item.first);
  std::set<std::string> declared, not_applicable;
  for (const auto &item : manifest.declared_capabilities()) {
    if (!known.count(item) || !declared.insert(item).second) {
      throw EvaluationError("unknown or duplicate declared capability");
    }
  }
  for (const auto &item : manifest.not_applicable()) {
    if (!known.count(item) || !not_applicable.insert(item).second || declared.count(item) ||
        item == "run_observation") {
      throw EvaluationError("invalid not-applicable capability");
    }
  }
  v1::CapabilityReport report = validated.report;
  report.clear_checks();
  report.set_component_level(v1::L3);
  report.set_requested_level(manifest.requested_level());
  report.set_data_level(v1::LEVEL_NONE);
  const auto &trace = validated.trace;
  bool data_prefix = report.trace_valid();
  for (const auto &item : requirements()) {
    auto *check = report.add_checks();
    check->set_capability(item.first);
    check->set_required_level(item.second);
    check->set_declared(declared.count(item.first) != 0);
    bool data = report.trace_valid();
    if (item.second == v1::L1) {
      data = data && has_kind(trace, "step.start") && has_kind(trace, "advice.applied");
    } else if (item.second == v1::L2) {
      data = data && has_kind(trace, "action.start") && has_kind(trace, "action.end");
    } else if (item.second == v1::L3) {
      data = data && has_kind(trace, "loop.decision");
      for (const auto &event : trace.events()) {
        if (event.kind() == "loop.decision" &&
            (event.state_before().empty() || event.state_after().empty() || event.reason().empty()))
          data = false;
      }
    } else if (item.second == v1::L4) {
      data = data && has_kind(trace, "evaluation");
      bool attributed = false;
      for (const auto &event : trace.events()) {
        if (event.kind() == "advice.applied" && !event.application().experiment_id().empty() &&
            !event.application().cohort().empty())
          attributed = true;
      }
      data = data && attributed;
    }
    if (not_applicable.count(item.first)) {
      check->set_data_status(v1::NOT_APPLICABLE);
      check->set_runtime_status(v1::NOT_APPLICABLE);
      check->add_reasons("not_applicable_is_not_verified");
      data_prefix = false;
      continue;
    }
    check->set_data_status(data ? v1::PASS : v1::FAIL);
    if (report.source_format() == "agentflow.sessionlog" && item.second > v1::L0) {
      check->set_data_status(v1::NOT_VERIFIED);
      check->add_reasons("legacy_higher_level_projection_not_implemented");
    }
    data_prefix = data_prefix && data;
    if (data_prefix)
      report.set_data_level(item.second);
    if (item.second == v1::L0) {
      check->set_runtime_status(data ? v1::PASS : v1::FAIL);
    } else {
      check->set_runtime_status(v1::NOT_VERIFIED);
      check->add_reasons("host_execution_not_verified_offline");
      if (!check->declared())
        check->add_reasons("capability_not_declared");
    }
    if (!data)
      check->add_reasons("required_trace_evidence_missing_or_invalid");
  }
  const auto verified = report.trace_valid() ? v1::L0 : v1::LEVEL_NONE;
  report.set_verified_level(verified);
  report.set_requested_level_satisfied(verified >= manifest.requested_level());
  report.set_effective_level(manifest.fallback() == "strict" && !report.requested_level_satisfied()
                                 ? v1::LEVEL_NONE
                                 : verified);
  return report;
}

// 检查指定文件的格式和接入条件，不触发宿主执行或模型评测。
v1::CapabilityReport inspect_file(const std::filesystem::path &path,
                                  const v1::CapabilityManifest &manifest, TraceLimits limits) {
  if (path.extension() != ".jsonl") {
    return inspect_capabilities(validate_trace(read_trace(path, limits), limits), manifest);
  }
  // 旧版完整性检查仍然有效，不能根据 SessionLog 字段
  // 拼造在线能力已经具备的证据。
  agentflow::trajectory::AnalyzerOptions options;
  options.max_file_bytes = limits.max_bytes;
  const auto analysis = agentflow::trajectory::LoopEpisodeAnalyzer().analyze_file(path, options);
  ValidatedTrace legacy;
  legacy.report.set_schema_version(1);
  legacy.report.set_source_format("agentflow.sessionlog");
  legacy.report.set_trace_valid(analysis.trace_integrity_valid);
  legacy.report.set_event_count(analysis.summary.event_count);
  legacy.report.set_duplicate_event_count(analysis.summary.duplicate_event_count);
  for (const auto &item : analysis.trace_issues) {
    issue(&legacy.report, item.code, 0, item.severity != "error");
  }
  return inspect_capabilities(legacy, manifest);
}

} // 命名空间结束：trajectory::evolution
