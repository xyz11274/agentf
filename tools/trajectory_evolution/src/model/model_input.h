// 模型输入压缩：移除与决策无关的传输元数据，并共享重复负载。
// 只操作明确的轨迹/分析字段，不递归删除任意业务输入中的同名键。
// 执行身份、时间顺序与因果关系保留；共享相同文本不合并两次真实调用。

#pragma once

#include <cstdint>
#include <initializer_list>
#include <map>
#include <set>
#include <string>
#include <vector>
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

namespace agentflow::trajectory::model_input {

using Value = rapidjson::Value;
using Allocator = rapidjson::Document::AllocatorType;

// 将 JSON 值编码为紧凑字节表示，供有界模型输入与传输使用。
inline std::string bytes(const Value &value) {
  rapidjson::StringBuffer buffer;
  rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
  value.Accept(writer);
  return {buffer.GetString(), buffer.GetSize()};
}

// 移除明确列出的元信息字段，不递归删除用户业务中的同名内容。
inline void omit(Value &value, std::initializer_list<const char *> fields) {
  if (value.IsObject())
    for (const auto *field : fields)
      value.RemoveMember(field);
}

// 精简组件内部传输信息，保留身份、因果及应用扩展。
inline void runtime_metadata_view(Value &attributes) {
  omit(attributes, {"protocol", "agent_version_id", "evaluation_trial_id"});
  if (attributes.IsObject() && attributes.HasMember("extensions"))
    // SDK 引用键已经解析为执行身份和 caused_by；
    // 应用自定义扩展与策略作用域仍需保留。
    omit(attributes["extensions"], {"point_reference"});
}

// 这些字段用于存储和传输身份，不代表 Agent 决策。
// 时间戳、执行及循环身份、caused_by 仍保留，以支持并发证据归因。
inline void event_view(Value &event) {
  omit(event, {"producer_id", "sequence"});
  if (!event.IsObject())
    return;
  // Protobuf 标量默认值不构成观测，但不能删除应用
  // 输入输出、显式空状态、自定义属性或因果关系。
  for (const auto *key : {"action_id", "execution_id", "step_id", "loop_id", "operation", "status",
                          "decision", "reason"}) {
    if (event.HasMember(key) && event[key].IsString() && event[key].GetStringLength() == 0)
      event.RemoveMember(key);
  }
  if (event.HasMember("attributes"))
    runtime_metadata_view(event["attributes"]);
  if (event.HasMember("run_attributes"))
    runtime_metadata_view(event["run_attributes"]);
}

// 只处理分析包，不遍历任意工具输入或应用数据。
inline void analysis_view(Value &value) {
  if (value.IsArray())
    for (auto &item : value.GetArray())
      analysis_view(item);
  if (!value.IsObject())
    return;
  for (auto it = value.MemberBegin(); it != value.MemberEnd();) {
    const std::string key(it->name.GetString(), it->name.GetStringLength());
    if (key == "packet_id" || (key.size() >= 12 && key.substr(key.size() - 12) == "_fingerprint"))
      it = value.EraseMember(it);
    else {
      analysis_view(it->value);
      ++it;
    }
  }
}

// 建立模型可见的轨迹视图，去除无关存储字段而保留执行事实。
inline void trace_view(Value &trace) {
  if (!trace.IsObject())
    return;
  omit(trace, {"schema_version", "trace_kind"});
  if (trace.HasMember("identity"))
    omit(trace["identity"], {"application_id", "user_id", "conversation_id"});
  if (trace.HasMember("attributes"))
    runtime_metadata_view(trace["attributes"]);
  if (trace.HasMember("events") && trace["events"].IsArray())
    for (auto &event : trace["events"].GetArray())
      event_view(event);
}

// A request handoff is supported only by a real common agent-turn parent,
// an explicit target and either exact actual user input or a unique framework
// dispatch key. This is not a new DAG edge and makes no claim about the
// contribution or necessity of either iteration.
inline Value dispatch_handoffs(const Value &trace, Allocator &allocator) {
  Value result(rapidjson::kArrayType);
  if (!trace.IsObject() || !trace.HasMember("events") || !trace["events"].IsArray())
    return result;
  const Value empty;
  auto member = [&](const Value &value, const char *key) -> const Value & {
    return value.IsObject() && value.HasMember(key) ? value[key] : empty;
  };
  auto text = [&](const Value &value) -> std::string {
    return value.IsString() ? std::string(value.GetString(), value.GetStringLength()) : "";
  };
  auto stamp = [&](const Value &event) -> std::uint64_t {
    const auto &value = member(event, "timestamp_us");
    if (value.IsUint64()) return value.GetUint64();
    const auto raw = text(value);
    if (raw.empty()) return 0;
    for (const auto ch : raw) if (ch < '0' || ch > '9') return 0;
    try { return std::stoull(raw); } catch (...) { return 0; }
  };
  std::map<std::string, const Value *> events;
  for (const auto &event : trace["events"].GetArray()) {
    const auto id = text(member(event, "event_id"));
    if (id.empty() || id.size() > 256 || !events.emplace(id, &event).second)
      return result;
  }
  auto parent = [&](const Value &event) -> std::string {
    std::vector<std::string> pending{text(member(event, "event_id"))};
    std::set<std::string> seen, parents;
    while (!pending.empty()) {
      const auto id = pending.back(); pending.pop_back();
      if (!seen.insert(id).second) continue;
      const auto found = events.find(id);
      if (found == events.end()) return "";
      const auto &item = *found->second;
      const auto &attrs = member(item, "attributes");
      if (text(member(item, "kind")) == "step.start" &&
          text(member(attrs, "activity_kind")) == "agent-turn") {
        parents.insert(id); continue;
      }
      const auto &causes = member(item, "caused_by");
      if (causes.IsArray()) for (const auto &cause : causes.GetArray()) {
        if (!cause.IsString()) return "";
        pending.push_back(text(cause));
      }
    }
    return parents.size() == 1 ? *parents.begin() : "";
  };
  struct Dispatch {
    const Value *event;
    std::string parent, target, prompt, call_id;
    std::size_t ordinal;
  };
  struct Child {
    const Value *event;
    std::string parent, prompt, key, target;
  };
  std::vector<Dispatch> dispatches;
  std::vector<Child> children;
  for (const auto &event : trace["events"].GetArray()) {
    const auto &attrs = member(event, "attributes"), &ext = member(attrs, "extensions");
    if (text(member(event, "kind")) != "action.end" ||
        text(member(attrs, "semantic")) != "model" ||
        !member(attrs, "executed").IsTrue() ||
        text(member(ext, "source_event_name")) != "llm.request.prepared") continue;
    const auto owner = parent(event);
    if (owner.empty() || !stamp(event)) continue;
    const auto actor = text(member(event, "actor_id"));
    const auto &choices = member(member(event, "output"), "choices");
    if (actor == "coordinator_decide" && choices.IsArray())
      for (const auto &choice : choices.GetArray()) {
        const auto &message = member(choice, "message");
        const auto &calls = member(message.IsObject() ? message : choice, "tool_calls");
        if (!calls.IsArray()) continue;
        for (const auto &call : calls.GetArray()) {
          const auto &function = member(call, "function");
          const auto &fn = function.IsObject() ? function : call;
          auto name = text(member(fn, "name"));
          if (name.empty()) name = text(member(call, "tool_name"));
          if (name != "run_skills") continue;
          auto call_id = text(member(call, "tool_call_id"));
          if (call_id.empty()) call_id = text(member(call, "id"));
          if (call_id.empty() || call_id.size() > 256) continue;
          const auto &raw = member(fn, "arguments");
          rapidjson::Document parsed;
          if (raw.IsString() && raw.GetStringLength() <= 32768)
            parsed.Parse(raw.GetString(), raw.GetStringLength());
          const auto &args = raw.IsObject() ? raw : static_cast<const Value &>(parsed);
          const auto &skills = member(args, "skills");
          if (!skills.IsArray()) continue;
          std::size_t ordinal = 0;
          for (const auto &skill : skills.GetArray()) {
            const auto target = text(member(skill, "name")), prompt = text(member(skill, "prompt"));
            if (!target.empty() && target.size() <= 128 && !prompt.empty() && prompt.size() <= 4096)
              dispatches.push_back({&event, owner, target, prompt, call_id, ordinal});
            ++ordinal;
          }
        }
      }
    if (actor == "skill_executor") {
      const auto key = text(member(ext, "exec_skill_key"));
      if (key.empty() || key.size() > 512) continue;
      const auto &messages = member(member(event, "input"), "messages");
      if (!messages.IsArray()) continue;
      std::string prompt;
      std::string target;
      for (const auto &message : messages.GetArray()) {
        if (text(member(message, "role")) == "user") {
          if (prompt.empty())
            prompt = text(member(message, "content"));
          continue;
        }
        if (text(member(message, "role")) == "system") {
          const auto content = text(member(message, "content"));
          const std::string prefix = "## Skill: ";
          if (content.compare(0, prefix.size(), prefix) == 0) {
            const auto end = content.find('\n', prefix.size());
            target = content.substr(prefix.size(), end == std::string::npos
                                                        ? std::string::npos
                                                        : end - prefix.size());
          }
        }
      }
      if (!prompt.empty() && prompt.size() <= 4096 && target.size() <= 128)
        children.push_back({&event, owner, prompt, key, target});
    }
  }
  if (dispatches.size() > 128 || children.size() > 128) return result;
  struct Match {
    std::size_t dispatch, child;
    const char *basis;
  };
  std::vector<Match> matches;
  std::vector<std::size_t> dispatch_count(dispatches.size()), child_count(children.size());
  std::map<std::string, std::size_t> dispatch_target_counts, child_target_counts;
  auto pair_key = [](const std::string &parent, const std::string &target) {
    return parent + '\x1f' + target;
  };
  for (const auto &dispatch : dispatches)
    ++dispatch_target_counts[pair_key(dispatch.parent, dispatch.target)];
  for (const auto &child : children)
    if (!child.target.empty())
      ++child_target_counts[pair_key(child.parent, child.target)];
  for (std::size_t d = 0; d < dispatches.size(); ++d)
    for (std::size_t c = 0; c < children.size(); ++c) {
      const auto &dispatch = dispatches[d]; const auto &child = children[c];
      if (dispatch.parent != child.parent ||
          stamp(*dispatch.event) >= stamp(*child.event)) continue;
      if (child.target != dispatch.target) continue;
      if (dispatch.prompt == child.prompt) {
        matches.push_back({d, c, "unique_same_parent_exact_prompt"});
        ++dispatch_count[d]; ++child_count[c];
        continue;
      }
      // AppWorld's skill wrapper prepends the complete skill document and
      // normalizes the child prompt. A unique explicit dispatch key keeps
      // the causal handoff observable without asserting that the child was
      // useful or that either loop must be kept.
      if (child.key.rfind("dispatch:", 0) == 0 &&
          dispatch_target_counts[pair_key(dispatch.parent, dispatch.target)] == 1 &&
          child_target_counts[pair_key(child.parent, child.target)] == 1)
        matches.push_back({d, c, "unique_same_parent_dispatch_key"});
    }
  for (const auto &match : matches) {
    if (match.basis == std::string("unique_same_parent_exact_prompt") &&
        (dispatch_count[match.dispatch] != 1 || child_count[match.child] != 1)) continue;
    const auto &dispatch = dispatches[match.dispatch]; const auto &child = children[match.child];
    Value row(rapidjson::kObjectType);
    auto add = [&](const char *key, const std::string &value) {
      if (!value.empty()) row.AddMember(Value(key, allocator), Value(value.data(), value.size(), allocator), allocator);
    };
    add("dispatch_event_id", text(member(*dispatch.event, "event_id")));
    add("skill_event_id", text(member(*child.event, "event_id")));
    add("parent_event_id", dispatch.parent); add("target", dispatch.target);
    add("tool_call_id", dispatch.call_id); add("exec_skill_key", child.key);
    add("dispatch_source_call_id", text(member(member(member(*dispatch.event, "attributes"), "extensions"), "source_call_id")));
    add("skill_source_call_id", text(member(member(member(*child.event, "attributes"), "extensions"), "source_call_id")));
    row.AddMember("skill_ordinal", static_cast<unsigned>(dispatch.ordinal), allocator);
    row.AddMember("matching_basis", Value(match.basis, allocator), allocator);
    result.PushBack(row, allocator);
  }
  return result;
}

// Derive bounded identity metadata before any public payload is previewed.
// Reapplying this to a projected trace must not duplicate the real handoff.
inline void bind_dispatch_handoffs(Value &trace, Allocator &allocator) {
  if (!trace.IsObject()) return;
  auto handoffs = dispatch_handoffs(trace, allocator);
  std::set<std::string> recorded;
  auto identity = [](const Value &handoff) {
    if (!handoff.IsObject()) return bytes(handoff);
    std::string key;
    for (const auto *field : {"dispatch_event_id", "skill_event_id", "parent_event_id",
                             "target", "tool_call_id", "exec_skill_key", "matching_basis"}) {
      key += '\x1f';
      if (handoff.HasMember(field) && handoff[field].IsString())
        key.append(handoff[field].GetString(), handoff[field].GetStringLength());
    }
    return key;
  };
  for (const auto &handoff : handoffs.GetArray()) recorded.insert(identity(handoff));
  if (trace.HasMember("attributes") && trace["attributes"].IsObject() &&
      trace["attributes"].HasMember("dispatch_handoffs") &&
      trace["attributes"]["dispatch_handoffs"].IsArray()) {
    for (const auto &handoff : trace["attributes"]["dispatch_handoffs"].GetArray())
      if (recorded.insert(identity(handoff)).second) {
        Value copy(handoff, allocator);
        handoffs.PushBack(std::move(copy), allocator);
      }
  }
  if (trace.HasMember("attributes") && trace["attributes"].IsObject())
    trace["attributes"].RemoveMember("dispatch_handoffs");
  if (handoffs.Empty()) return;
  if (!trace.HasMember("attributes"))
    trace.AddMember("attributes", Value(rapidjson::kObjectType), allocator);
  if (trace["attributes"].IsObject())
    trace["attributes"].AddMember("dispatch_handoffs", handoffs, allocator);
}

inline void bind_dispatch_handoffs(rapidjson::Document &evidence) {
  if (!evidence.IsObject()) return;
  if (evidence.HasMember("trace"))
    bind_dispatch_handoffs(evidence["trace"], evidence.GetAllocator());
  if (evidence.HasMember("traces") && evidence["traces"].IsArray())
    for (auto &trace : evidence["traces"].GetArray())
      bind_dispatch_handoffs(trace, evidence.GetAllocator());
}

// A uniquely observed dispatch is a prerequisite of its actual child request.
// Reject contradictory reviews, rather than assigning either contribution here.
// Missing/ambiguous links and children already judged waste remain model-reviewed.
inline Value dispatch_dependency_conflicts(const Value &review, const Value &inventory,
                                          const Value &evidence, Allocator &allocator) {
  Value result(rapidjson::kArrayType);
  const Value empty;
  auto member = [&](const Value &value, const char *key) -> const Value & {
    return value.IsObject() && value.HasMember(key) ? value[key] : empty;
  };
  auto text = [&](const Value &value) -> std::string {
    return value.IsString() ? std::string(value.GetString(), value.GetStringLength()) : "";
  };
  const auto &rows = member(review, "iteration_reviews");
  if (!rows.IsArray() || !inventory.IsObject()) return result;
  std::map<std::string, std::string> decisions;
  for (const auto &row : rows.GetArray())
    decisions.emplace(text(member(row, "iteration_id")), text(member(row, "decision")));
  std::set<std::string> rejected;
  auto inspect = [&](const Value &trace) {
    const auto run = text(member(member(trace, "identity"), "run_id"));
    const auto &links = member(member(trace, "attributes"), "dispatch_handoffs");
    if (run.empty() || !links.IsArray()) return;
    auto iteration = [&](const std::string &event) -> std::string {
      std::string found;
      for (auto entry = inventory.MemberBegin(); entry != inventory.MemberEnd(); ++entry) {
        if (text(member(entry->value, "run_id")) != run) continue;
        const auto &ids = member(entry->value, "event_ids");
        if (!ids.IsArray()) continue;
        for (const auto &id : ids.GetArray()) if (text(id) == event) {
          if (!found.empty()) return "";
          found = text(entry->name);
        }
      }
      return found;
    };
    for (const auto &link : links.GetArray()) {
      // These records were derived from real same-parent requests by the
      // projection above. We do not infer links from a repeated skill name.
      if (text(member(link, "matching_basis")) != "unique_same_parent_exact_prompt" ||
          text(member(link, "parent_event_id")).empty() ||
          text(member(link, "target")).empty()) continue;
      const auto dispatch_event = text(member(link, "dispatch_event_id"));
      const auto child_event = text(member(link, "skill_event_id"));
      if (dispatch_event.empty() || child_event.empty() || dispatch_event == child_event) continue;
      const auto dispatch = iteration(dispatch_event), child = iteration(child_event);
      if (dispatch.empty() || child.empty() || dispatch == child ||
          decisions[dispatch] != "waste" || decisions[child] != "keep" ||
          !rejected.insert(dispatch).second) continue;
      Value conflict(rapidjson::kObjectType);
      auto add = [&](const char *key, const std::string &value) {
        conflict.AddMember(Value(key, allocator), Value(value.data(), value.size(), allocator), allocator);
      };
      add("iteration_id", dispatch);
      add("issue", "necessary_contribution_discarded");
      add("constraint", "actual_dispatch_required_by_retained_child");
      add("retained_iteration_id", child);
      add("dispatch_event_id", dispatch_event);
      add("skill_event_id", child_event);
      result.PushBack(conflict, allocator);
    }
  };
  const auto &traces = member(evidence, "traces");
  if (traces.IsArray()) for (const auto &trace : traces.GetArray()) inspect(trace);
  else inspect(member(evidence, "trace"));
  return result;
}

// 移除重复模型信封和组件诊断，保留实际动作、返回及并发因果。
inline void task_evidence_view(rapidjson::Document &evidence, bool include_model_requests = true,
                               bool keep_causal_anchors = false, bool keep_dispatch_bindings = false,
                               bool keep_outer_request_inputs = false) {
  // 确定性分析、来源信息和尚未得出的评测结论保留在本地。
  omit(evidence, {"trajectory", "execution_run_id", "loaded_version", "loop_review"});
  if (!evidence.IsObject())
    return;
  if (evidence.HasMember("artifacts") && evidence["artifacts"].IsObject())
    for (auto &file : evidence["artifacts"].GetObject())
      omit(file.value, {"stored_file"});
  auto execution_view = [&](Value &trace) {
    if (keep_dispatch_bindings)
      bind_dispatch_handoffs(trace, evidence.GetAllocator());
    trace_view(trace);
    if (include_model_requests || !trace.IsObject() || !trace.HasMember("events") ||
        !trace["events"].IsArray())
      return;
    auto &events = trace["events"];
    // 可选地保留真正被依赖的传输节点身份。正文仍省略，不能用压缩后的
    // 相邻位置补造前因，也不能将不同 Run 的相同 ID 串在一起。
    std::vector<Value> causal_metadata;
    std::map<std::string, std::size_t> causal_indices;
    std::set<std::string> ambiguous_causal_ids;
    std::vector<std::size_t> retained_indices;
    if (keep_causal_anchors) {
      causal_metadata.reserve(events.Size());
      for (const auto &event : events.GetArray()) {
        Value metadata(rapidjson::kObjectType);
        if (event.IsObject()) {
          for (const auto *field : {"event_id", "kind", "timestamp_us", "actor_id", "action_id",
                                   "execution_id", "step_id", "loop_id", "iteration", "operation",
                                   "status", "caused_by"}) {
            const auto found = event.FindMember(field);
            if (found != event.MemberEnd()) {
              Value key(field, evidence.GetAllocator());
              Value value(found->value, evidence.GetAllocator());
              metadata.AddMember(key, value, evidence.GetAllocator());
            }
          }
          const auto attributes = event.FindMember("attributes");
          if (attributes != event.MemberEnd() && attributes->value.IsObject()) {
            Value typed(rapidjson::kObjectType);
            for (const auto *field : {"semantic", "type", "executed"}) {
              const auto found = attributes->value.FindMember(field);
              if (found != attributes->value.MemberEnd()) {
                Value key(field, evidence.GetAllocator());
                Value value(found->value, evidence.GetAllocator());
                typed.AddMember(key, value, evidence.GetAllocator());
              }
            }
            if (!typed.ObjectEmpty())
              metadata.AddMember("attributes", typed, evidence.GetAllocator());
          }
          const auto id = event.FindMember("event_id");
          if (id != event.MemberEnd() && id->value.IsString()) {
            const std::string name(id->value.GetString(), id->value.GetStringLength());
            if (!name.empty() && !causal_indices.emplace(name, causal_metadata.size()).second)
              ambiguous_causal_ids.insert(name);
          }
        }
        causal_metadata.push_back(std::move(metadata));
      }
    }
    bool has_action_start = false, has_action_end = false;
    std::set<std::string> action_steps;
    for (const auto &event : events.GetArray()) {
      if (!event.IsObject() || !event.HasMember("kind") || !event["kind"].IsString())
        continue;
      const bool start = event["kind"] == "action.start", end = event["kind"] == "action.end";
      has_action_start |= start;
      has_action_end |= end;
      if ((start || end) && event.HasMember("step_id") && event["step_id"].IsString())
        action_steps.emplace(event["step_id"].GetString(), event["step_id"].GetStringLength());
    }
    const bool has_action_evidence = has_action_start && has_action_end;
    std::set<std::string> retained_step_inputs;
    std::set<std::pair<std::string, std::string>> retained_outer_requests;
    std::size_t original_index = 0;
    for (auto it = events.Begin(); it != events.End();) {
      const auto current_index = original_index++;
      auto &event = *it;
      if (!event.IsObject()) {
        if (keep_causal_anchors)
          retained_indices.push_back(current_index);
        ++it;
        continue;
      }
      auto attributes_it = event.FindMember("attributes");
      const bool has_attributes =
          attributes_it != event.MemberEnd() && attributes_it->value.IsObject();
      auto *attributes = has_attributes ? &attributes_it->value : nullptr;
      const bool model =
          attributes && attributes->HasMember("semantic") && (*attributes)["semantic"] == "model";
      const std::string kind =
          event.HasMember("kind") && event["kind"].IsString()
              ? std::string(event["kind"].GetString(), event["kind"].GetStringLength())
              : std::string{};
      const bool policy_query = kind == "custom" && attributes && attributes->HasMember("type") &&
                                (*attributes)["type"] == "policy.query";
      bool retain_step = false;
      if (has_action_evidence && (kind == "step.start" || kind == "step.end")) {
        const std::string step =
            event.HasMember("step_id") && event["step_id"].IsString()
                ? std::string(event["step_id"].GetString(), event["step_id"].GetStringLength())
                : std::string{};
        if (!action_steps.count(step)) {
          if (kind == "step.end")
            retain_step = event.HasMember("output") && !event["output"].IsNull();
          else if (event.HasMember("input") && !event["input"].IsNull()) {
            const std::string execution =
                event.HasMember("execution_id") && event["execution_id"].IsString()
                    ? std::string(event["execution_id"].GetString(),
                                  event["execution_id"].GetStringLength())
                : event.HasMember("actor_id") && event["actor_id"].IsString()
                    ? std::string(event["actor_id"].GetString(),
                                  event["actor_id"].GetStringLength())
                    : std::string{};
            const bool message = event["input"].IsObject() && event["input"].HasMember("role");
            const auto &input = event["input"];
            const bool outer_request = keep_outer_request_inputs && attributes &&
                attributes->HasMember("activity_kind") && (*attributes)["activity_kind"] == "agent-turn" &&
                input.IsObject() && input.HasMember("system") && input["system"].IsString() &&
                input.HasMember("messages") && input["messages"].IsArray() &&
                input.HasMember("turn") && input["turn"].IsNumber();
            // Each outer request may carry new observations while sharing the
            // same native execution. A preceding control-state input must not
            // consume its slot. Repeated identical requests remain compact;
            // the existing text sharing handles repeated history strings.
            retain_step = message || (outer_request
                ? retained_outer_requests.emplace(execution, bytes(input)).second
                : retained_step_inputs.insert(execution).second);
          }
        }
      }
      // 策略查询和应用遥测仍保存在持久证据中，用于归因。
      // 与动作配对的步骤只是传输封装；独立的首次输入、
      // 跨边界消息和终态 Agent 交接必须保留。
      if (policy_query || kind == "advice.applied" ||
          (has_action_evidence && (kind == "step.start" || kind == "step.end") && !retain_step) ||
          (has_action_evidence && kind == "run.start") ||
          (has_action_evidence && kind == "action.start" && model)) {
        it = events.Erase(it);
        continue;
      }
      // 只有显式标记的模型请求才可精简，不根据提供方字段名猜测，
      // 也不能删除外形相似的工具参数或自定义活动内容。
      if (model)
        event.RemoveMember("input");
      // 模型输入已在上面按 semantic 排除。工具结束事件的 input 可能记录了
      // 执行时修正的参数，或是缺少开始事件时仅有的参数证据，不能无条件删除。
      // 提供方信封可去掉重复元信息与隐藏推理，助手输出和工具调用仍然保留。
      if (kind == "action.end") {
        if (model && event.HasMember("output") && event["output"].IsObject()) {
          auto &output = event["output"];
          omit(output,
               {"_agentflow_call_id", "id", "object", "created", "model", "service_tier", "usage"});
          if (output.HasMember("choices") && output["choices"].IsArray())
            for (auto &choice : output["choices"].GetArray()) {
              if (!choice.IsObject())
                continue;
              omit(choice, {"index", "logprobs", "finish_reason"});
              if (!choice.HasMember("message") || !choice["message"].IsObject())
                continue;
              auto &message = choice["message"];
              omit(message, {"role", "reasoning", "reasoning_content", "analysis"});
              if (message.HasMember("tool_calls") && message["tool_calls"].IsArray() &&
                  !message["tool_calls"].Empty())
                message.RemoveMember("content");
              if (message.HasMember("tool_calls") && message["tool_calls"].IsArray())
                for (auto &call : message["tool_calls"].GetArray())
                  if (call.IsObject())
                    omit(call, {"id", "type", "index"});
            }
        }
      }
      auto cleaned_attributes = event.FindMember("attributes");
      if (cleaned_attributes != event.MemberEnd() && cleaned_attributes->value.IsObject()) {
        auto &value = cleaned_attributes->value;
        if (kind == "action.start")
          omit(value, {"activity_kind", "decision", "phase", "request_binding"});
        else if (kind == "action.end") {
          omit(value, {"extensions", "metrics", "tool_contracts"});
          if (value.HasMember("executed") && value["executed"].IsBool() &&
              value["executed"].GetBool())
            value.RemoveMember("executed");
        } else if (kind == "loop.decision") {
          value.RemoveMember("extensions");
          if (value.HasMember("interrupted") && value["interrupted"].IsBool() &&
              !value["interrupted"].GetBool())
            value.RemoveMember("interrupted");
        } else if (kind == "run.end")
          value.RemoveMember("extensions");
        if (value.ObjectEmpty())
          event.RemoveMember("attributes");
      }
      if (keep_causal_anchors)
        retained_indices.push_back(current_index);
      ++it;
    }
    if (keep_causal_anchors) {
      std::set<std::size_t> required(retained_indices.begin(), retained_indices.end());
      std::vector<std::size_t> pending(retained_indices.begin(), retained_indices.end());
      while (!pending.empty()) {
        const auto index = pending.back();
        pending.pop_back();
        const auto &metadata = causal_metadata[index];
        const auto causes = metadata.FindMember("caused_by");
        if (causes == metadata.MemberEnd() || !causes->value.IsArray())
          continue;
        for (const auto &cause : causes->value.GetArray()) {
          if (!cause.IsString())
            continue;
          const std::string id(cause.GetString(), cause.GetStringLength());
          const auto found = causal_indices.find(id);
          if (found != causal_indices.end() && !ambiguous_causal_ids.count(id) &&
              required.insert(found->second).second)
            pending.push_back(found->second);
        }
      }
      Value complete(rapidjson::kArrayType);
      complete.Reserve(static_cast<rapidjson::SizeType>(required.size()), evidence.GetAllocator());
      std::size_t retained = 0;
      for (std::size_t index = 0; index < causal_metadata.size(); ++index) {
        if (retained < retained_indices.size() && retained_indices[retained] == index)
          complete.PushBack(events[static_cast<rapidjson::SizeType>(retained++)], evidence.GetAllocator());
        else if (required.count(index))
          complete.PushBack(causal_metadata[index], evidence.GetAllocator());
      }
      events = std::move(complete);
    }
  };
  if (evidence.HasMember("trace"))
    execution_view(evidence["trace"]);
  if (evidence.HasMember("traces") && evidence["traces"].IsArray())
    for (auto &trace : evidence["traces"].GetArray())
      execution_view(trace);
}

// 精确共享不是摘要，每份原始值仍保留在首次出现的位置。
// 只有登记的路径才是引用，用户自带相似对象仍按普通数据处理。
class Sharing {
public:
  // 初始化 Sharing 的状态和资源；输入参数的校验与所有权在构造阶段确定。
  // 对象创建后复用既有配置，实际执行与收尾分别由后续接口负责。
  Sharing(Allocator &allocator, const char *reference_key, bool containers = true,
          std::size_t minimum_bytes = 512, std::size_t maximum_container_bytes = 1048576)
      : allocator_(allocator), reference_key_(reference_key), containers_(containers),
        minimum_bytes_(minimum_bytes), maximum_container_bytes_(maximum_container_bytes) {}

  // 收集当前缓冲区或候选片段，遵守容量上限和内存所有权要求。
  void collect(Value &value, const std::string &path, bool share_node = true) {
    if (replacements_ >= 4096 || path.size() > 4096)
      return;
    if (share_node &&
        (value.IsString() || (containers_ && (value.IsArray() || value.IsObject())))) {
      const auto text = bytes(value);
      if (text.size() >= minimum_bytes_) {
        const auto found = sources_.find(text);
        if (found != sources_.end()) {
          auto &reference = references_[found->second];
          reference["paths"].PushBack(Value(path.c_str(), allocator_), allocator_);
          Value source;
          source.CopyFrom(reference["source"], allocator_);
          value.SetObject();
          value.AddMember(Value(reference_key_, allocator_), source, allocator_);
          ++replacements_;
          return;
        }
        // 同时限制本地工作内存与模型输入。
        // 整段历史数组过大时，仍可按单条消息共享重复内容。
        if (text.size() <= 1048576 &&
            (value.IsString() || text.size() <= maximum_container_bytes_) &&
            stored_bytes_ + text.size() <= 4194304 &&
            sources_.size() < 4096) {
          sources_.emplace(text, references_.Size());
          stored_bytes_ += text.size();
          Value reference(rapidjson::kObjectType);
          reference.AddMember("source", Value(path.c_str(), allocator_), allocator_);
          reference.AddMember("paths", Value(rapidjson::kArrayType), allocator_);
          references_.PushBack(reference, allocator_);
        }
      }
    }
    if (value.IsArray()) {
      for (rapidjson::SizeType i = 0; i < value.Size(); ++i)
        collect(value[i], path + '/' + std::to_string(i));
    } else if (value.IsObject()) {
      for (auto it = value.MemberBegin(); it != value.MemberEnd(); ++it) {
        std::string token;
        for (char c : std::string(it->name.GetString(), it->name.GetStringLength()))
          token += c == '~' ? "~0" : c == '/' ? "~1" : std::string(1, c);
        collect(it->value, path + '/' + token);
      }
    }
  }

  // 返回已登记的精确共享引用表，只有这些位置才被解释为引用。
  Value references() {
    Value used(rapidjson::kArrayType);
    for (auto &item : references_.GetArray())
      if (!item["paths"].Empty())
        used.PushBack(item, allocator_);
    return used;
  }

private:
  Allocator &allocator_;
  const char *reference_key_;
  bool containers_;
  std::size_t minimum_bytes_, maximum_container_bytes_;
  std::map<std::string, rapidjson::SizeType> sources_;
  Value references_{rapidjson::kArrayType};
  std::size_t replacements_ = 0, stored_bytes_ = 0;
};

// 构造对话分析的精简视图，保留选定片段与必要相邻事实。
inline void dialogue_view(rapidjson::Document &payload, bool standard_trace) {
  omit(payload, {"schema_version", "evaluation_scope", "content_policy", "source"});
  std::map<std::string, const Value *> episodes;
  for (const auto &episode : payload["episodes"].GetArray())
    episodes.emplace(episode["episode_id"].GetString(), &episode);
  for (auto &episode : payload["episodes"].GetArray()) {
    omit(episode, {"selected"}); // selected_episode_ids 是唯一的片段白名单。
    omit(episode["evidence"], {"episode_id"});
    analysis_view(episode["evidence"]);
    if (episode.HasMember("rule_assessment")) {
      const auto &rule = episode["rule_assessment"];
      if (rule.IsObject() && rule.MemberCount() == 2 && rule.HasMember("verdict") &&
          rule["verdict"] == "SEMANTIC_VERDICT_UNASSESSED" && rule.HasMember("explanation") &&
          rule["explanation"] == "未命中确定性异常规则；这不等同于已证明该步骤有效。")
        episode.RemoveMember("rule_assessment");
    }
    auto &evidence = episode["evidence"];
    if (evidence.HasMember("dispatch") && evidence["dispatch"].IsObject() &&
        evidence["dispatch"].ObjectEmpty())
      evidence.RemoveMember("dispatch");
    // 邻接身份已经表达关系，不再重复该片段已有事实。
    // 缺失目标与不一致事实必须保留。
    for (const auto *key : {"previous_same_owner", "next_same_owner"}) {
      if (!evidence.HasMember(key) || !evidence[key].IsObject())
        continue;
      auto &neighbor = evidence[key];
      if (!neighbor.HasMember("episode_id") || !neighbor["episode_id"].IsString())
        continue;
      const auto found = episodes.find(neighbor["episode_id"].GetString());
      if (found == episodes.end())
        continue;
      const auto &target = (*found->second)["evidence"];
      for (auto it = neighbor.MemberBegin(); it != neighbor.MemberEnd();) {
        const auto other = target.FindMember(it->name);
        if (it->name != "episode_id" && other != target.MemberEnd() && it->value == other->value)
          it = neighbor.EraseMember(it);
        else
          ++it;
      }
    }
  }
  if (standard_trace)
    for (auto &event : payload["content_events"].GetArray()) {
      auto &data = event["data"];
      event_view(data);
      // 外层事件已经提供这些字段，不重复发送。
      omit(data, {"event_id", "kind", "timestamp_us", "iteration"});
      if (data.HasMember("identity"))
        omit(data["identity"], {"application_id", "user_id", "conversation_id"});
    }
  rapidjson::Document compact;
  compact.CopyFrom(payload, compact.GetAllocator());
  // Growing request histories must not exhaust the bounded exact-value
  // index before their individual messages and short failure results are seen.
  // Keep all unique values; limit only container indexing, not input content.
  Sharing sharing(compact.GetAllocator(), "$content_ref", true, 128, 8192);
  auto &events = compact["content_events"];
  for (rapidjson::SizeType i = 0; i < events.Size(); ++i)
    sharing.collect(events[i]["data"], "/content_events/" + std::to_string(i) + "/data", false);
  auto references = sharing.references();
  if (!references.Empty()) {
    compact.AddMember("content_value_refs", references, compact.GetAllocator());
    if (bytes(compact).size() < bytes(payload).size())
      payload.Swap(compact);
  }
}

} // 命名空间结束：agentflow::trajectory::model_input
