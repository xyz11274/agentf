// 模型输入压缩：移除与决策无关的传输元数据，并共享重复负载。
// 只操作明确的轨迹/分析字段，不递归删除任意业务输入中的同名键。
// 执行身份、时间顺序与因果关系保留；共享相同文本不合并两次真实调用。

#pragma once

#include <initializer_list>
#include <map>
#include <set>
#include <string>
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

// 移除重复模型信封和组件诊断，保留实际动作、返回及并发因果。
inline void task_evidence_view(Value &evidence, bool include_model_requests = true) {
  // 确定性分析、来源信息和尚未得出的评测结论保留在本地。
  omit(evidence, {"trajectory", "execution_run_id", "loaded_version", "loop_review"});
  if (!evidence.IsObject())
    return;
  if (evidence.HasMember("artifacts") && evidence["artifacts"].IsObject())
    for (auto &file : evidence["artifacts"].GetObject())
      omit(file.value, {"stored_file"});
  auto execution_view = [&](Value &trace) {
    trace_view(trace);
    if (include_model_requests || !trace.IsObject() || !trace.HasMember("events") ||
        !trace["events"].IsArray())
      return;
    auto &events = trace["events"];
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
    for (auto it = events.Begin(); it != events.End();) {
      auto &event = *it;
      if (!event.IsObject()) {
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
            retain_step = message || retained_step_inputs.insert(execution).second;
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
      ++it;
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
  Sharing(Allocator &allocator, const char *reference_key, bool containers = true)
      : allocator_(allocator), reference_key_(reference_key), containers_(containers) {}

  // 收集当前缓冲区或候选片段，遵守容量上限和内存所有权要求。
  void collect(Value &value, const std::string &path, bool share_node = true) {
    if (replacements_ >= 4096 || path.size() > 4096)
      return;
    if (share_node &&
        (value.IsString() || (containers_ && (value.IsArray() || value.IsObject())))) {
      const auto text = bytes(value);
      if (text.size() >= 512) {
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
        if (text.size() <= 1048576 && stored_bytes_ + text.size() <= 4194304 &&
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
  Sharing sharing(compact.GetAllocator(), "$content_ref");
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
