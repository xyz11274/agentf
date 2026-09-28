// 学习证据只取真实事件，不把模型的完成声明升级成业务成功。
// 历史事件始终保留原 run/event 身份；限长只影响分析视图，不修改原轨迹。
#pragma once

#include <algorithm>
#include <set>

#include "integration_json.h"
#include "judge_evidence.h"

namespace trajectory::evolution::integration {

// 保留小型结构化结果；较大正文明确标记为预览，不能据其缺项断言未执行。
inline Json learning_preview(const Json &value, std::size_t limit) {
  // 工具经常返回 JSON 字符串。只解一层对象/数组，避免重复转义消耗证据预算；
  // 普通字符串及 JSON 标量保持原类型，不猜测业务含义。
  auto content = value;
  if (value.string()) {
    try {
      auto decoded = Json::parse(value.text());
      if (decoded.object() || decoded.array())
        content = std::move(decoded);
    } catch (...) { /* 普通文本走有界预览。 */
    }
  }
  const auto bytes = content.dump();
  if (bytes.size() <= limit)
    return content;
  // 大对象的短标量字段独立保留，防止 status 等回执信息排在长正文后面而消失。
  // 不按业务字段名做特判；仍明确标记为不完整视图。
  Json fields = Json::Object{};
  std::size_t field_bytes = 0;
  std::vector<std::pair<std::size_t, std::string>> scalar_fields;
  if (content.object())
    for (const auto &[key, item] : content.fields()) {
      if (!item.object() && !item.array()) {
        const auto size = Json{{key, item}}.dump().size();
        if (size <= 128)
          scalar_fields.emplace_back(size, key);
      }
    }
  // 优先短事实，不让字典序靠前的说明文本、长标识挤掉靠后的布尔值和状态。
  std::sort(scalar_fields.begin(), scalar_fields.end());
  for (const auto &[size, key] : scalar_fields)
    if (field_bytes + size <= limit / 2) {
      fields[key] = content[key];
      field_bytes += size;
    }
  return Json{{"preview_only", true},
              {"original_bytes", bytes.size()},
              {"fields", fields},
              {"preview", evidence_preview(bytes, limit - field_bytes)}};
}

// input 可能是消息数组的 JSON 字符串。只取本轮最后一条用户输入，不携带重复历史。
inline Json learning_user_input(const Json &trace) {
  for (const auto &event : trace["events"].items()) {
    if (event.str("kind") != "run.start")
      continue;
    auto value = event["input"];
    if (value.string()) {
      try {
        value = Json::parse(value.text());
      } catch (...) {
        return learning_preview(value, 1024);
      }
    }
    if (value.object() && value["messages"].array())
      for (auto it = value["messages"].items().rbegin(); it != value["messages"].items().rend();
           ++it)
        if (it->str("role") == "user")
          return learning_preview((*it)["content"], 1024);
    return learning_preview(value, 1024);
  }
  return nullptr;
}

// 事实与声明显式分开。action 的 completed 是执行终态，不能代替 output 中的业务状态。
inline Json learning_event_fact(const Json &trace, const Json &event) {
  const auto &attributes = event["attributes"];
  const bool report =
      attributes.str("semantic") == "model" || attributes.str("type") == "schedule.action" ||
      attributes.flag("delegates_schedule", false) || event.str("kind") == "run.end";
  const auto kind = report                              ? "agent_report"
                    : event.str("kind") == "action.end" ? "execution_observation"
                    : event.str("kind") == "run.start"  ? "task_context"
                                                        : "lifecycle_observation";
  return Json{{"run_id", trace["identity"]["run_id"]},
              {"event_id", event["event_id"]},
              {"actor", event["actor_id"]},
              {"kind", event["kind"]},
              {"operation", event["operation"]},
              {"execution_status", event["status"]},
              {"evidence_kind", kind},
              {"input", report ? Json(nullptr) : learning_preview(event["input"], 256)},
              {"output", learning_preview(event["output"], 768)},
              {"error", learning_preview(event["error"], 256)}};
}

// 只补充当前轮之前、同一会话的执行观察。缺省不跨轮假设任何任务尚未完成。
// 并发采集的数组不是时间线。按真实时间取各执行者最近事实，轮流选取，
// 避免单个检索分支占满预算而挤掉另一分支已完成的操作；绝不读取未来轮次。
inline Json learning_history(const Json &traces, std::size_t current, std::size_t budget = 8192) {
  Json facts = Json::Array{};
  std::size_t bytes = 0, eligible = 0, prior_turns = 0;

  struct Observation {
    const Json *trace;
    const Json *event;
    std::size_t turn;
    std::uint64_t time;
  };

  std::map<std::string, std::vector<Observation>> by_actor;
  const auto &identity = traces.items().at(current)["identity"];
  for (std::size_t i = current; i > 0; --i) {
    const auto &previous = traces.items().at(i - 1);
    const auto &other = previous["identity"];
    if (identity["user_id"].dump() != other["user_id"].dump() ||
        identity["conversation_id"].dump() != other["conversation_id"].dump() ||
        identity["agent_id"].dump() != other["agent_id"].dump())
      continue;
    ++prior_turns;
    for (const auto &event : previous["events"].items()) {
      const auto &attributes = event["attributes"];
      if (event.str("kind") != "action.end" || attributes.str("semantic") == "model" ||
          attributes.str("type") == "schedule.action" ||
          attributes.flag("delegates_schedule", false))
        continue;
      // Protobuf JSON 的 int64 是字符串，原生 JSON 入口也可以提供整数。
      std::uint64_t time = 0;
      const auto &stamp = event["timestamp_us"];
      if (stamp.string())
        time = std::stoull(stamp.text());
      else if (!stamp.null())
        time = stamp.integer();
      by_actor[event.str("actor_id")].push_back({&previous, &event, i - 1, time});
      ++eligible;
    }
  }
  std::size_t longest = 0;
  for (auto &[actor, observations] : by_actor) {
    std::stable_sort(observations.begin(), observations.end(), [](const auto &a, const auto &b) {
      return a.turn != b.turn ? a.turn > b.turn : a.time > b.time;
    });
    longest = std::max(longest, observations.size());
  }
  for (std::size_t index = 0; index < longest && facts.items().size() < 32; ++index)
    for (const auto &[actor, observations] : by_actor) {
      if (index >= observations.size())
        continue;
      const auto &observation = observations[index];
      auto fact = learning_event_fact(*observation.trace, *observation.event);
      const auto size = fact.dump().size();
      if (facts.items().size() >= 32 || size > budget - std::min(bytes, budget))
        continue;
      bytes += size;
      facts.push(std::move(fact));
    }
  return Json{{"prior_turns", prior_turns},
              {"observations", facts},
              {"omitted_observations", eligible - facts.items().size()},
              {"scope", "historical_context_not_current_actions"}};
}

// 公共审核携带被引用的原始证据片段，而不是只比较两段模型生成的摘要。
// 片段不足时应保持私有/待审；不能把历史事件 ID 当作当前轮可引用事件。
inline Json learning_review_context(const Json &trace, const std::set<std::string> &cited) {
  Json facts = Json::Array{};
  std::size_t bytes = 0, omitted = 0;
  for (const auto &event : trace["events"].items()) {
    if (!cited.count(event.str("event_id")))
      continue;
    auto fact = learning_event_fact(trace, event);
    const auto size = fact.dump().size();
    if (facts.items().size() >= 4 || bytes + size > 4096) {
      ++omitted;
      continue;
    }
    bytes += size;
    facts.push(std::move(fact));
  }
  const auto &context = trace["attributes"]["training_evaluation"];
  return Json{{"user_input", learning_user_input(trace)},
              {"expected_behavior", learning_preview(context["expected_behavior"], 768)},
              {"constraints", learning_preview(context["constraints"], 512)},
              {"evidence", facts},
              {"omitted_evidence", omitted}};
}

} // 命名空间结束：trajectory::evolution::integration
