// 逐轮验收契约：核对标准与真实 run 的对应关系，保存业务结论和意图观察。
// 意图指标基于实际开始的目标派发，不统计建议、跳过或同一目标的重复重试。
// 条件分支和缺失轨迹单独统计，不直接混入无条件匹配率。

#pragma once

#include "integration_json.h"
#include "evaluation_dataset.pb.h"
#include <set>
#include <regex>

namespace trajectory::evolution::integration {

// 可选的逐轮验收契约。目标名是宿主自己的操作/子任务标识，不解释任何框架的 skill。
inline Json turn_expectations(const dataset::v1::EvaluationTask &task) {
  const auto attributes = json(task.attributes());
  if (!attributes.has("turn_expectations"))
    return nullptr;
  auto turns = attributes["turn_expectations"];
  require(turns.array() && turns.items().size() == std::size_t(task.followup_inputs_size() + 1),
          "dataset_turn_expectations_invalid");
  for (std::size_t i = 0; i < turns.items().size(); ++i) {
    auto &turn = turns.items()[i];
    keys(turn, {"turn", "expected_behavior", "expected_targets", "expected_intents",
                "expected_effects", "conditional", "criteria"});
    require(turn.num("turn", 0) == i + 1 && !turn.str("expected_behavior").empty() &&
                turn.str("expected_behavior").size() <= 32768,
            "dataset_turn_expectations_invalid");
    // 某些表格用分号拼接空单元格，留下的标点不构成验收标准。
    // 仅在没有显式子目标时退回对应轮的用户请求；不读取 Agent 回答，不猜业务操作码。
    // 标准文本一旦含有效内容则原样保留，显式 criteria 的粒度和条件也不改写。
    static const std::regex separators(R"([ \t\r\n,;.|]|；|，|、|。|　)");
    // 逐个移除定长分隔符，避免长占位串触发整串重复匹配的递归栈增长。
    if (!turn.has("criteria") && std::regex_replace(turn.str("expected_behavior"), separators, "").empty()) {
      const auto &request = i == 0 ? task.task_input() : task.followup_inputs(i - 1);
      require(!request.empty() && request.size() <= 32768, "dataset_turn_expectations_invalid");
      turn["expected_behavior"] = request;
    }
    if (turn.has("conditional"))
      turn.flag("conditional", false);
    for (const auto *field : {"expected_targets", "expected_intents", "expected_effects"}) {
      if (!turn.has(field) || turn[field].null())
        continue;
      require(turn[field].array() && turn[field].items().size() <= 128,
              "dataset_turn_expectations_invalid");
      std::set<std::string> unique;
      for (const auto &value : turn[field].items())
        require(
            value.string() && !value.text().empty() && value.text().size() <= 1024 &&
                (std::string(field) != "expected_targets" || unique.insert(value.text()).second),
            "dataset_turn_expectations_invalid");
    }
  }
  return turns;
}

// 把单轮和多轮证据统一成有序轨迹数组，不凭最终答案补造缺失轮次。
inline Json turn_traces(const Json &evidence) {
  return evidence["traces"].array()   ? evidence["traces"]
         : evidence["trace"].object() ? Json(Json::Array{evidence["trace"]})
                                      : Json(Json::Array{});
}

// 评分标签与运行事实分开保存；模型只需读取每轮标准及其证据路径，不复制整段轨迹。
inline Json turn_grading_context(const dataset::v1::EvaluationTask &task, const Json &evidence) {
  const auto expected = turn_expectations(task);
  if (expected.null())
    return nullptr;
  const auto traces = turn_traces(evidence);
  require(traces.items().size() == expected.items().size(), "evaluation_turn_evidence_mismatch");
  Json rows = Json::Array{};
  for (std::size_t i = 0; i < expected.items().size(); ++i) {
    const auto &turn = expected.items()[i];
    Json row{
        {"turn", i + 1},
        {"expected_behavior", turn["expected_behavior"]},
        {"evidence_path", evidence["traces"].array() ? "/traces/" + std::to_string(i) : "/trace"}};
    if (!turn["expected_effects"].null())
      row["expected_effects"] = turn["expected_effects"];
    row["conditional"] = turn.flag("conditional", false);
    rows.push(std::move(row));
  }
  return rows;
}

// 检查逐轮结论的数量、编号、顺序和说明长度，并绑定对应 run 身份。
// 诊断只携带约束名与计数，避免未验证的模型正文泄露到日志。
inline Json validate_turn_verdicts(const Json &verdicts, const Json &expected, const Json &evidence,
                                   Json &diagnostic) {
  // 只返回编号、计数和静态约束名，帮助模型修复格式；不落盘未验证的回答内容。
  auto check = [&](bool valid, const char *constraint, std::size_t turn = 0) {
    if (valid)
      return;
    diagnostic =
        Json{{"constraint", constraint},
             {"expected_count", expected.items().size()},
             {"received_count", verdicts.array() ? Json(verdicts.items().size()) : Json(nullptr)}};
    if (turn)
      diagnostic["turn"] = turn;
    // 目标级的跳过/等待状态不能进入逐轮结论。修复请求给出字段位置及合法值，
    // 不回显模型填入的任意文本，也不擅自将非法状态转换成通过。
    if (turn && std::string(constraint) == "turn_verdict_invalid") {
      diagnostic["verdict_path"] = "/turn_verdicts/" + std::to_string(turn - 1) + "/verdict";
      diagnostic["allowed_verdicts"] = Json::Array{"passed", "failed", "uncertain"};
    }
    throw Error("evaluation_turn_verdict_invalid");
  };
  check(verdicts.array(), "turn_verdicts_array_required");
  check(verdicts.items().size() == expected.items().size(), "turn_count_mismatch");
  const auto traces = turn_traces(evidence);
  require(traces.items().size() == expected.items().size(), "evaluation_turn_evidence_mismatch");
  Json rows = Json::Array{};
  for (std::size_t i = 0; i < verdicts.items().size(); ++i) {
    auto row = verdicts.items()[i];
    keys(row, {"turn", "verdict", "reason"});
    const auto *number = std::get_if<double>(&row["turn"].value);
    check(number && *number == i + 1, "ordered_numeric_turn_required", i + 1);
    check(row["verdict"].string(), "turn_verdict_required", i + 1);
    const auto status = row.str("verdict");
    check(status == "passed" || status == "failed" || status == "uncertain", "turn_verdict_invalid",
          i + 1);
    check(row["reason"].string() && !row["reason"].text().empty() &&
              row["reason"].text().size() <= 4096,
          "turn_reason_required_max_4096_bytes", i + 1);
    row["run_id"] = traces.items().at(i)["identity"]["run_id"];
    rows.push(std::move(row));
  }
  diagnostic = nullptr;
  return rows;
}

// 比较每轮标注目标与实际开始执行的专业子任务，统计意图观察指标。
// 条件未成立或标签缺失单独记账；调用次数和派发建议不能直接代表意图识别成功。
inline Json observe_turn_intents(const dataset::v1::EvaluationTask &task, const Json &evidence) {
  const auto expected = turn_expectations(task);
  if (expected.null())
    return nullptr;
  const auto traces = turn_traces(evidence);
  Json rows = Json::Array{};
  for (std::size_t i = 0; i < expected.items().size(); ++i) {
    const auto &spec = expected.items()[i];
    Json row{{"turn", i + 1},
             {"expected_targets", spec["expected_targets"]},
             {"expected_intents", spec["expected_intents"]},
             {"observation_only", true},
             {"conditional", spec.flag("conditional", false)}};
    if (spec["expected_targets"].null()) {
      row["status"] = "unlabeled";
      rows.push(row);
      continue;
    }
    if (i >= traces.items().size() || !traces.items()[i]["events"].array()) {
      row["status"] = "missing_trace";
      rows.push(row);
      continue;
    }
    std::set<std::string> wanted, actual;
    bool observed = false;
    for (const auto &name : spec["expected_targets"].items())
      wanted.insert(name.text());
    for (const auto &event : traces.items()[i]["events"].items()) {
      const auto &attributes = event["attributes"];
      observed |= attributes.str("type") == "schedule.action" ||
                  attributes.str("type") == "schedule.skipped" ||
                  (attributes.str("type") == "boundary.activity" &&
                   attributes.str("activity_kind") == "schedule");
      // 只统计真正开始的派发；计划、跳过、相同目标重试都不增加意图数量。
      if (event.str("kind") == "action.start" && attributes.str("type") == "schedule.action" &&
          attributes.flag("executed", false))
        actual.insert(event.str("operation"));
    }
    if (!observed) {
      // 有完整对话却没有调度观测，不等于日志丢失，也不能证明意图漏识别。
      // 主模型可能直接回答或复用前轮结果；保持为观察项，不强制额外派发。
      row["status"] = "dispatch_unobserved";
      rows.push(row);
      continue;
    }
    row["actual_targets"] = Json::Array{};
    row["matched"] = Json::Array{};
    row["missing"] = Json::Array{};
    row["extra"] = Json::Array{};
    for (const auto &name : actual) {
      row["actual_targets"].push(name);
      row[wanted.count(name) ? "matched" : "extra"].push(name);
    }
    for (const auto &name : wanted)
      if (!actual.count(name))
        row["missing"].push(name);
    row["complete"] = row["missing"].items().empty();
    row["exact"] = wanted == actual;
    row["status"] = spec.flag("conditional", false) ? "conditional_review" : "measured";
    rows.push(std::move(row));
  }
  return rows;
}

// 汇总各轮意图命中、漏检和多派发观测；条件轮次与无标签轮次不混入同一分母。
inline Json summarize_intents(const Json &rows) {
  std::size_t dispatch_unobserved = 0;
  std::size_t total = 0, measured = 0, conditional = 0, missing_trace = 0, matched = 0, wanted = 0,
              actual = 0, complete = 0, exact = 0;
  for (const auto &row : rows.items()) {
    if (!row["expected_targets"].array() || row["expected_targets"].items().size() < 2)
      continue;
    ++total;
    if (row.str("status") == "conditional_review") {
      ++conditional;
      continue;
    }
    if (row.str("status") == "missing_trace") {
      ++missing_trace;
      continue;
    }
    if (row.str("status") == "dispatch_unobserved") {
      ++dispatch_unobserved;
      continue;
    }
    if (row.str("status") != "measured")
      continue;
    ++measured;
    matched += row["matched"].items().size();
    wanted += row["expected_targets"].items().size();
    actual += row["actual_targets"].items().size();
    complete += row.flag("complete", false);
    exact += row.flag("exact", false);
  }
  auto ratio = [](std::size_t a, std::size_t b) { return b ? Json(double(a) / b) : Json(nullptr); };
  return Json{{"observation_only", true},
              {"basis", "unique_dispatched_targets"},
              {"multi_intent_turns", total},
              {"measured_turns", measured},
              {"conditional_turns", conditional},
              {"missing_trace_turns", missing_trace},
              {"dispatch_unobserved_turns", dispatch_unobserved},
              {"precision", ratio(matched, actual)},
              {"recall", ratio(matched, wanted)},
              {"f1", ratio(2 * matched, actual + wanted)},
              {"complete_rate", ratio(complete, measured)},
              {"exact_rate", ratio(exact, measured)},
              {"missing_target_count", wanted - matched},
              {"extra_target_count", actual - matched}};
}

} // 命名空间结束：trajectory::evolution::integration
