// 裁判证据视图：限制请求体积，同时保留原始字段的补查路径。
// 预览不是完整证据；原始轨迹仍完整保存，不能依据被省略的字段判定未执行。
// 历史补查仅访问已经发生的轮次，避免把未来信息用于解释过去决策。

#pragma once

#include "integration_json.h"
#include <set>

namespace trajectory::evolution::integration {

// 仅用于裁判视图。原始日志不截断；预览显式携带原始路径，裁判可申请读取缺失字段。
// 按 UTF-8 字节边界截取，避免中文预览产生非法编码。
inline std::string evidence_preview(const std::string &text, std::size_t limit) {
  if (text.size() <= limit)
    return text;
  while (limit && (static_cast<unsigned char>(text[limit]) & 0xc0) == 0x80)
    --limit;
  return text.substr(0, limit);
}

// 按 JSON Pointer 规则转义字段名，防止包含斜杠或波浪号的键指向错误位置。
inline std::string evidence_path_key(const std::string &key) {
  std::string result;
  for (char c : key)
    result += c == '~' ? "~0" : c == '/' ? "~1" : std::string(1, c);
  return result;
}

// 递归压缩裁判视图中的大文本和业务数组，保留原始字段路径用于限次补查。
// 事件身份与因果骨架不折叠，原始落盘轨迹不受此裁剪影响。
inline void bound_judge_payload(Json &value, const std::string &path, std::size_t field_limit,
                                std::size_t answer_field_limit = 0) {
  // 只提高聚合的最终答复预算；事件中的工具输出、状态和同名业务字段不随之膨胀。
  // 旧调用不传第四个参数时保持原限额，超长答复仍走同一预览与补查机制。
  if (path == "/output" && answer_field_limit)
    field_limit = answer_field_limit;
  // 工具返回的大数组也需要限长；事件骨架本身不能折叠，否则循环归因会丢失。
  const bool payload = path.find("/input/") != std::string::npos ||
                       path.find("/output/") != std::string::npos || path.rfind("/state/", 0) == 0;
  if (payload && value.array() && value.items().size() > 16 &&
      value.dump().size() > field_limit * 2) {
    const auto bytes = value.dump();
    value = Json{{"preview_only", true},
                 {"preview", evidence_preview(bytes, field_limit)},
                 {"original_bytes", bytes.size()},
                 {"source", "current"},
                 {"path", path}};
    return;
  }
  if (value.string() && value.text().size() > field_limit) {
    const auto text = value.text();
    auto preview = evidence_preview(text, field_limit);
    // 回复常把结论放在末尾。固定预算内保留头尾，避免只看到前置分析而漏掉答案。
    // 仍明确标为预览，不能把中段省略当作未执行；需要时继续按原路径补查。
    if (path.find("/output/") != std::string::npos && field_limit >= 64) {
      const std::string omitted = "\n[...中段省略...]\n";
      const auto head = evidence_preview(text, (field_limit - omitted.size()) / 2);
      auto tail = text.size() - (field_limit - omitted.size() - head.size());
      while (tail < text.size() &&
             (static_cast<unsigned char>(text[tail]) & 0xc0) == 0x80)
        ++tail;
      preview = head + omitted + text.substr(tail);
    }
    value = Json{{"preview_only", true},
                 {"preview", preview},
                 {"original_bytes", text.size()},
                 {"source", "current"},
                 {"path", path}};
  } else if (value.object()) {
    for (auto &item : value.fields())
      bound_judge_payload(item.second, path + "/" + evidence_path_key(item.first), field_limit,
                          answer_field_limit);
  } else if (value.array()) {
    for (std::size_t i = 0; i < value.items().size(); ++i)
      bound_judge_payload(value.items()[i], path + "/" + std::to_string(i), field_limit,
                          answer_field_limit);
  }
}

// 为当前批次目标寻找被裁剪的答案字段；include_definite 同时检查已判通过或失败的目标。
// 不读取整段轨迹、输入或前后轮历史；原文仍由调用处按原路径、输入预算限量加载。
inline Json answer_previews_for_unknown_goals(const Json &evidence, const Json &scope,
                                            const Json &rows, bool include_definite = false) {
  Json references = Json::Array{};
  std::set<std::string> paths;
  if (evidence["context_evidence"].array())
    for (const auto &ref : evidence["context_evidence"].items())
      if (ref.str("source") == "current")
        paths.insert(ref.str("path"));
  const auto collect = [&](const auto &self, const Json &value,
                           const std::string &expected_path) -> void {
    if (references.items().size() >= 4)
      return;
    // 业务数据可以恰好使用这些字段名；类型或路径不匹配时不把它当作裁剪标记。
    if (value.object() && std::holds_alternative<bool>(value["preview_only"].value) &&
        value["preview_only"].boolean() && value["path"].string() && value["source"].string()) {
      const auto path = value.str("path");
      if (value.str("source") == "current" && path == expected_path &&
          path.rfind("/output/", 0) == 0 &&
          paths.insert(path).second)
        references.push(Json{{"source", "current"}, {"path", path}});
      return;
    }
    if (value.object())
      for (const auto &item : value.fields())
        self(self, item.second, expected_path + "/" + evidence_path_key(item.first));
    else if (value.array())
      for (std::size_t i = 0; i < value.items().size(); ++i)
        self(self, value.items()[i], expected_path + "/" + std::to_string(i));
  };
  const auto begin = scope.num("turn_begin", 1), end = scope.num("turn_end", begin);
  for (std::size_t i = 0; i < rows.items().size() && i < scope["goals"].items().size(); ++i) {
    const auto status = rows.items()[i].str("status");
    if (status != "uncertain" &&
        !(include_definite && (status == "passed" || status == "failed")))
      continue;
    const auto turn = scope["goals"].items()[i].num("turn", 0);
    if (turn < begin || turn > end)
      continue;
    const auto &output = evidence["output"];
    if (output["turns"].array() && turn - begin < output["turns"].items().size())
      collect(collect, output["turns"].items()[static_cast<std::size_t>(turn - begin)],
              "/output/turns/" + std::to_string(static_cast<std::size_t>(turn - begin)));
    else if (turn == end && output.has("final"))
      collect(collect, output["final"], "/output/final");
    else if (begin == end)
      collect(collect, output, "/output");
  }
  return references;
}

// 仅为当前未知目标追加工具结果原文，不读取模型请求、输入或其他轮次。
// 裁剪标记在事件视图压缩前生成，必须按 event_id 对齐原始事件，不能使用视图下标。
// 复用答案补查的四字段上限；已补查字段不再占位，后续仍可读取其他必要字段。
inline Json result_previews_for_unknown_goals(const Json &view, const Json &original,
                                             const Json &scope, const Json &rows) {
  auto references = answer_previews_for_unknown_goals(view, scope, rows);
  std::set<std::string> paths;
  if (view["context_evidence"].array())
    for (const auto &ref : view["context_evidence"].items())
      if (ref.str("source") == "current")
        paths.insert(ref.str("path"));
  for (const auto &ref : references.items())
    paths.insert(ref.str("path"));
  const auto collect = [&](const auto &self, const Json &value,
                           const std::string &expected_path) -> void {
    if (references.items().size() >= 4)
      return;
    if (value.object() && std::holds_alternative<bool>(value["preview_only"].value) &&
        value["preview_only"].boolean() && value["path"].string() && value["source"].string()) {
      if (value.str("source") == "current" && value.str("path") == expected_path &&
          paths.insert(expected_path).second)
        references.push(Json{{"source", "current"}, {"path", expected_path}});
      return;
    }
    if (value.object())
      for (const auto &item : value.fields())
        self(self, item.second, expected_path + "/" + evidence_path_key(item.first));
    else if (value.array())
      for (std::size_t i = 0; i < value.items().size(); ++i)
        self(self, value.items()[i], expected_path + "/" + std::to_string(i));
  };
  const auto begin = scope.num("turn_begin", 1), end = scope.num("turn_end", begin);
  std::set<std::size_t> visited;
  for (std::size_t i = 0; i < rows.items().size() && i < scope["goals"].items().size(); ++i) {
    if (rows.items()[i].str("status") != "uncertain" || references.items().size() >= 4)
      continue;
    const auto turn = scope["goals"].items()[i].num("turn", 0);
    if (turn < begin || turn > end || !visited.insert(turn - begin).second)
      continue;
    const bool multiple = view["traces"].array();
    if (multiple ? (!original["traces"].array() ||
                    turn - begin >= view["traces"].items().size() ||
                    turn - begin >= original["traces"].items().size())
                 : (!view.has("trace") || !original.has("trace") || begin != end))
      continue;
    const auto &trace = multiple ? view["traces"].items()[turn - begin] : view["trace"];
    const auto &raw = multiple ? original["traces"].items()[turn - begin] : original["trace"];
    if (!trace["events"].array() || !raw["events"].array())
      continue;
    std::map<std::string, std::size_t> indices;
    std::set<std::string> ambiguous;
    for (std::size_t j = 0; j < raw["events"].items().size(); ++j) {
      const auto id = raw["events"].items()[j].str("event_id");
      if (!indices.emplace(id, j).second)
        ambiguous.insert(id);
    }
    for (const auto &event : trace["events"].items()) {
      const auto id = event.str("event_id");
      const auto found = indices.find(id);
      if (id.empty() || found == indices.end() || ambiguous.count(id) ||
          event.str("kind") != "action.end" || event["attributes"].str("semantic") == "model")
        continue;
      const auto &source = raw["events"].items()[found->second];
      if (source.str("kind") != "action.end" || source["attributes"].str("semantic") == "model")
        continue;
      const auto prefix = multiple ? "/traces/" + std::to_string(turn - begin) : "/trace";
      collect(collect, event["output"],
              prefix + "/events/" + std::to_string(found->second) + "/output");
    }
  }
  return references;
}

// 为裁判把实际请求与返回放在相邻字段，避免并发交错时拿错另一调用的空结果。
// 只在同一 Run 中按唯一 action_id 绑定，不能按工具名、位置或返回顺序猜测。
// request 是可见视图的派生字段；事件顺序、原始日志和结束事件自身的 input 均不改变。
inline void bind_judge_action_requests(Json &evidence) {
  auto bind = [](Json &trace) {
    if (!static_cast<const Json &>(trace)["events"].array())
      return;
    auto &events = trace["events"].items();
    std::map<std::string, std::size_t> starts;
    std::set<std::string> ambiguous;
    for (std::size_t i = 0; i < events.size(); ++i) {
      const auto &event = events[i];
      if (event.str("kind") != "action.start" || event.str("action_id").empty())
        continue;
      const auto id = event.str("action_id");
      if (!starts.emplace(id, i).second)
        ambiguous.insert(id);
    }
    for (auto &event : events) {
      const auto &end = static_cast<const Json &>(event);
      if (end.str("kind") != "action.end" || end.has("request") ||
          end["attributes"].str("semantic") == "model")
        continue;
      const auto id = end.str("action_id");
      const auto found = starts.find(id);
      if (found == starts.end() || ambiguous.count(id))
        continue;
      const auto &start = static_cast<const Json &>(events[found->second]);
      if (!start.has("input") || start.str("event_id").empty() ||
          start["attributes"].str("semantic") == "model")
        continue;
      // action_id 唯一也不能覆盖生产者给出的矛盾身份。信息不足时保留原视图，
      // 不把另一 actor/execution 的参数补到当前结果上。
      bool same_owner = true;
      for (const auto *field : {"actor_id", "execution_id"})
        if (!start.str(field).empty() && !end.str(field).empty() &&
            start.str(field) != end.str(field))
          same_owner = false;
      if (!same_owner)
        continue;
      Json request{{"event_id", start["event_id"]}, {"input", start["input"]}};
      if (start.has("operation"))
        request["operation"] = start["operation"];
      event["request"] = std::move(request);
    }
  };
  if (evidence.has("trace"))
    bind(evidence["trace"]);
  if (static_cast<const Json &>(evidence)["traces"].array())
    for (auto &trace : evidence["traces"].items())
      bind(trace);
}

// 输入数组顺序不一定等于事件时间顺序；终点按 kind 查找，不能盲取最后一项。
inline std::size_t terminal_event_index(const Json &trace) {
  const auto &events = trace["events"].items();
  for (std::size_t i = 0; i < events.size(); ++i)
    if (events[i].str("kind") == "run.end")
      return i;
  throw Error("evaluation_terminal_missing");
}

// 模型看到的事件数组可能删除了重复事件；引用先通过 event_id 找回原始下标。
// 普通字段保持原路径。缺失事件身份时拒绝判定，不能把同一下标的另一事件当证据。
inline std::string original_judge_path(const std::string &path, const Json &view,
                                       const Json &original) {
  const bool multiple = view["traces"].array();
  const auto traces = multiple ? view["traces"]
                     : view.has("trace") ? Json(Json::Array{view["trace"]}) : Json();
  if (!traces.array())
    return path;
  for (std::size_t run = 0; run < traces.items().size(); ++run) {
    const auto prefix = multiple ? "/traces/" + std::to_string(run) + "/events/"
                                 : std::string("/trace/events/");
    if (path.compare(0, prefix.size(), prefix) != 0)
      continue;
    const auto &events = traces.items()[run]["events"].items();
    for (std::size_t i = 0; i < events.size(); ++i) {
      const auto local = prefix + std::to_string(i);
      if (path != local && path.compare(0, local.size() + 1, local + '/') != 0)
        continue;
      const auto id = events[i].str("event_id");
      const auto &raw = multiple ? original["traces"].items().at(run)["events"].items()
                                 : original["trace"]["events"].items();
      for (std::size_t j = 0; j < raw.size(); ++j) {
        if (id.empty() || raw[j].str("event_id") != id)
          continue;
        auto suffix = path.substr(local.size());
        // 派生 request 必须引用真正的开始事件；不能把它作为原始日志新增字段保存。
        // 原数据自己已有 request 时按业务原值处理，不擅自解释成派生引用。
        if (!raw[j].has("request") &&
            (suffix == "/request" || suffix.compare(0, 9, "/request/") == 0)) {
          const auto &request = events[i]["request"];
          for (std::size_t k = 0; k < raw.size(); ++k) {
            const auto &start = raw[k];
            if (!request.object() || request.str("event_id").empty() ||
                start.str("event_id") != request.str("event_id") ||
                start.str("kind") != "action.start" || raw[j].str("kind") != "action.end" ||
                start.str("action_id").empty() ||
                start.str("action_id") != raw[j].str("action_id"))
              continue;
            bool same_owner = true;
            for (const auto *field : {"actor_id", "execution_id"})
              if (!start.str(field).empty() && !raw[j].str(field).empty() &&
                  start.str(field) != raw[j].str(field))
                same_owner = false;
            require(same_owner && start["attributes"].str("semantic") != "model",
                    "evaluation_goal_evidence_invalid");
            return prefix + std::to_string(k) + suffix.substr(8);
          }
          throw Error("evaluation_goal_evidence_invalid");
        }
        return prefix + std::to_string(j) + suffix;
      }
      throw Error("evaluation_goal_evidence_invalid");
    }
    throw Error("evaluation_goal_evidence_invalid");
  }
  return path;
}

// 将批次内部的路径恢复为整题原始证据路径。补查历史保留真实来源，不能归到当前轮。
// 输出/状态使用产生它的 run.end 作锚点；根级聚合引用只锚定整个轨迹集合。
inline std::string global_batch_path(std::string path, const Json &part, const Json &traces,
                                     std::size_t begin, std::size_t end) {
  if (part["context_evidence"].array())
    for (std::size_t i = 0; i < part["context_evidence"].items().size(); ++i) {
      const auto prefix = "/context_evidence/" + std::to_string(i) + "/value";
      if (path != prefix && path.compare(0, prefix.size() + 1, prefix + '/') != 0)
        continue;
      const auto &reference = part["context_evidence"].items()[i];
      path = reference.str("path") + path.substr(prefix.size());
      if (reference.str("source") == "history")
        return path;
      break;
    }
  for (std::size_t i = 0; i < end - begin; ++i) {
    const auto local = "/traces/" + std::to_string(i);
    if (path == local || path.compare(0, local.size() + 1, local + '/') == 0)
      return "/traces/" + std::to_string(begin + i) + path.substr(local.size());
  }
  for (std::size_t i = begin; i < end; ++i) {
    const auto terminal = terminal_event_index(traces.items()[i]);
    const auto raw =
        "/traces/" + std::to_string(i) + "/events/" + std::to_string(terminal) + "/output";
    for (const auto &local :
         {"/output/turns/" + std::to_string(i - begin), std::string("/output/final")})
      if ((local != "/output/final" || i + 1 == end) &&
          (path == local || path.compare(0, local.size() + 1, local + '/') == 0))
        return raw + path.substr(local.size());
    if (i + 1 == end && (path == "/state" || path.compare(0, 7, "/state/") == 0))
      return raw +
             (traces.items()[i]["events"].items()[terminal]["output"].has("state") ? path : "");
  }
  if (path == "/artifacts" || path.compare(0, 11, "/artifacts/") == 0)
    return path;
  return path == "/execution_status" ? path : "/traces";
}

// 提供历史定位索引，不复制历史模型请求或整段工具日志。只有前缀历史可访问，
// 防止用后轮才知道的事实倒推前轮 loop 没有必要。
inline Json prior_turn_catalog(const Json &traces, const std::vector<std::string> &inputs,
                               std::size_t end) {
  Json result = Json::Array{};
  for (std::size_t i = 0; i < end; ++i) {
    const auto &trace = traces.items().at(i);
    const auto path = "/traces/" + std::to_string(i);
    const auto &events = trace["events"].items();
    const auto terminal = terminal_event_index(trace);
    const auto output_path = path + "/events/" + std::to_string(terminal) + "/output";
    result.push(Json{{"turn", i + 1},
                     {"run_id", trace["identity"]["run_id"]},
                     {"input_preview", evidence_preview(inputs.at(i), 256)},
                     {"preview_only", true},
                     {"output_preview", evidence_preview(events[terminal]["output"].dump(), 512)},
                     {"source", "history"},
                     {"path", output_path}});
  }
  return result;
}

} // 命名空间结束：trajectory::evolution::integration
