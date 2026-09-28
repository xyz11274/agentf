// 目标评分：从冻结标准建立稳定目标标识，维护完成期限、关键约束和判定覆盖率。
// 最终完成度与严格逐轮验收并存；重复提问不重复加分，关键错误不能被普通目标抵消。
// 未知留在分母中，并形成分数上下界；搜索与发布共用同一质量读取约定。

#pragma once

#include "turn_contract.h"
#include <cmath>

namespace trajectory::evolution::integration {

// 严格验收失败可以同时包含未知子目标，不能仅看整题 verdict 判断裁判是否完成。
// 直接读取 Protobuf 中的小型覆盖率字段，避免为恢复判断复制整份大轨迹。
inline bool incomplete_goal_assessment(const google::protobuf::Struct &evidence) {
  const auto assessment = evidence.fields().find("assessment");
  if (assessment == evidence.fields().end() ||
      assessment->second.kind_case() != google::protobuf::Value::kStructValue)
    return false;
  const auto &fields = assessment->second.struct_value().fields();
  const auto coverage = fields.find("coverage");
  if (coverage == fields.end() ||
      coverage->second.kind_case() != google::protobuf::Value::kNumberValue)
    return false;
  const double value = coverage->second.number_value();
  // 与 assessment_quality 的浮点容差一致。非法值仍由原有校验拒绝，
  // 这里不修补损坏评分，也不把缺少目标评分的旧模式误判为未知。
  return std::isfinite(value) && value >= 0 && value < 1 - 1e-12;
}

// 验收目标来自冻结数据，而不是从 Agent 的回答中倒推。旧数据没有细分标准时，
// 每轮保留一个目标；明确提供 criteria 时才按子目标计分，避免分句改变条件语义。
inline Json assessment_goals(const dataset::v1::EvaluationTask &task) {
  const auto turns = turn_expectations(task);
  Json goals = Json::Array{};
  std::set<std::string> ids;
  const std::size_t count = task.followup_inputs_size() + 1;
  auto append = [&](const Json &criterion, std::size_t turn, std::size_t ordinal) {
    keys(criterion, {"id", "expected_behavior", "due_turn", "critical", "weight", "conditional"});
    const auto id =
        criterion.str("id", "turn-" + std::to_string(turn) + "-" + std::to_string(ordinal));
    const auto due = criterion.num("due_turn", turn);
    const auto text = criterion.str("expected_behavior");
    require(!criterion.has("weight") || std::holds_alternative<double>(criterion["weight"].value),
            "evaluation_goal_contract_invalid");
    const double weight =
        criterion.has("weight") ? std::get<double>(criterion["weight"].value) : 1.0;
    require(identifier(id) && ids.insert(id).second && !text.empty() && text.size() <= 32768 &&
                due >= turn && due <= count && std::isfinite(weight) && weight > 0 && weight <= 100,
            "evaluation_goal_contract_invalid");
    goals.push(Json{{"id", id},
                    {"turn", turn},
                    {"due_turn", due},
                    {"expected_behavior", text},
                    {"critical", criterion.flag("critical", false)},
                    {"conditional", criterion.flag("conditional", false)},
                    {"weight", weight}});
  };
  if (turns.null()) {
    append(
        Json{{"id", "task"}, {"expected_behavior", task.expected_behavior()}, {"due_turn", count}},
        1, 1);
  } else {
    for (const auto &turn : turns.items()) {
      const auto index = turn["turn"].integer();
      if (turn.has("criteria")) {
        require(turn["criteria"].array() && !turn["criteria"].items().empty() &&
                    turn["criteria"].items().size() <= 32,
                "evaluation_goal_contract_invalid");
        std::size_t ordinal = 0;
        for (const auto &criterion : turn["criteria"].items())
          append(criterion, index, ++ordinal);
      } else {
        append(Json{{"expected_behavior", turn["expected_behavior"]},
                    {"conditional", turn.flag("conditional", false)}},
               index, 1);
      }
    }
  }
  // 全局禁止项在所有批次持续检查，但不因“没有违规”给任务完成度额外加分。
  // 否则什么都不执行的策略可能依靠大量禁止项获得很高分数。
  for (int i = 0; i < task.constraints_size(); ++i) {
    append(Json{{"id", "constraint-" + std::to_string(i + 1)},
                {"expected_behavior", task.constraints(i)},
                {"critical", true},
                {"due_turn", count}},
           1, 1);
    goals.items().back()["constraint"] = true;
  }
  require(goals.items().size() <= 256, "evaluation_goal_contract_invalid");
  return goals;
}

// 只接受当前批次明确要求的目标，顺序、编号、状态均由程序校验。
// pending 仅表示尚未到验收期限；它不是失败，也不能被当作已完成。
inline Json validate_goal_verdicts(const Json &rows, const Json &goals, std::size_t last_turn,
                                  Json *diagnostic = nullptr) {
  // 诊断只包含契约里的编号、数量和静态约束，不保存无效回答正文或任意模型字段。
  // 错误类型保持兼容；修复请求可以明确知道是哪一个目标、哪一种状态不合法。
  auto check = [&](bool valid, const char *constraint, std::size_t index) {
    if (valid)
      return;
    if (diagnostic) {
      *diagnostic = Json{{"constraint", constraint},
                         {"expected_count", goals.items().size()},
                         {"received_count", rows.array() ? Json(rows.items().size()) : Json(nullptr)}};
      if (index < goals.items().size()) {
        const auto &goal = goals.items()[index];
        Json states = Json::Array{"passed", "failed", "uncertain"};
        if (goal.flag("conditional", false))
          states.push("not_applicable");
        if (last_turn < goal["due_turn"].integer())
          states.push("pending");
        (*diagnostic)["goal_verdict_index"] = index;
        (*diagnostic)["expected_id"] = goal["id"];
        (*diagnostic)["allowed_statuses"] = std::move(states);
      }
    }
    throw Error("evaluation_goal_verdict_invalid");
  };
  check(rows.array(), "goal_verdicts_array_required", goals.items().size());
  check(rows.items().size() == goals.items().size(), "goal_count_mismatch", goals.items().size());
  for (std::size_t i = 0; i < rows.items().size(); ++i) {
    const auto &row = rows.items()[i];
    const auto &goal = goals.items()[i];
    check(row.object(), "goal_verdict_object_required", i);
    keys(row, {"id", "status", "reason", "evidence_paths"});
    check(row["id"].string() && row.str("id") == goal.str("id"), "ordered_goal_id_required", i);
    check(row["reason"].string() && !row.str("reason").empty() && row.str("reason").size() <= 2048,
          "goal_reason_required_max_2048_bytes", i);
    const auto status = row["status"].string() ? row.str("status") : "";
    check(status == "passed" || status == "failed" || status == "uncertain" ||
              (status == "not_applicable" && goal.flag("conditional", false)) ||
              (status == "pending" && last_turn < goal["due_turn"].integer()),
          "goal_status_not_allowed", i);
    check(row["evidence_paths"].array() && row["evidence_paths"].items().size() <= 16 &&
              ((status == "uncertain" || status == "pending") || !row["evidence_paths"].items().empty()),
          "goal_evidence_required_max_16_paths", i);
  }
  if (diagnostic)
    *diagnostic = nullptr;
  return rows;
}

// 所有适用目标都在分母中。未知目标只扩大上下界，不会因被排除而抬高通过率。
// 关键目标的失败独立报告；其余目标再成功也不能抵消关键违规。
inline Json summarize_goals(const Json &goals, const Json &latest) {
  double total = 0, passed = 0, unknown = 0;
  double checked_weight = 0, unknown_weight = 0;
  std::size_t critical = 0, recovered = 0;
  Json rows = Json::Array{};
  for (const auto &goal : goals.items()) {
    auto row = latest[goal.str("id")];
    if (row.null())
      row = Json{{"id", goal["id"]}, {"status", "uncertain"}, {"reason", "缺少有效评测证据"}};
    const auto status = row.str("status");
    const auto weight = std::get<double>(goal["weight"].value);
    row["turn"] = goal["turn"];
    row["due_turn"] = goal["due_turn"];
    row["weight"] = weight;
    if (status != "not_applicable") {
      const bool missing = status == "uncertain" || status == "pending";
      checked_weight += weight;
      if (missing)
        unknown_weight += weight;
      if (!goal.flag("constraint", false)) {
        total += weight;
        if (status == "passed")
          passed += weight;
        if (missing)
          unknown += weight;
      }
    }
    critical +=
        goal.flag("critical", false) && (status == "failed" || row.flag("critical_failure", false));
    recovered += row.flag("recovered", false);
    rows.push(std::move(row));
  }
  const double lower = total ? passed / total : 1.0;
  const double upper = total ? (passed + unknown) / total : 1.0;
  return Json{{"version", "goal-assessment-v1"},
              {"goals", rows},
              {"quality", lower},
              {"quality_upper", upper},
              {"coverage", checked_weight ? 1 - unknown_weight / checked_weight : 1.0},
              {"critical_failures", critical},
              {"recovered_goals", recovered}};
}

// 搜索和发布共用同一个质量读取入口；没有新评测结果的旧报告仍按原来的 0/1 处理。
// 不把新完成度冒充原始严格通过率，也不允许 NaN、未知或越界分数进入选优。
inline double assessment_quality(const Json &evidence, bool strict_passed) {
  const auto &assessment = evidence["assessment"];
  if (assessment.null())
    return strict_passed ? 1.0 : 0.0;
  require(assessment.str("version") == "goal-assessment-v1",
          "evaluation_assessment_version_invalid");
  const auto numeric = [&](const char *key) {
    require(std::holds_alternative<double>(assessment[key].value),
            "evaluation_assessment_score_invalid");
    const auto value = std::get<double>(assessment[key].value);
    require(std::isfinite(value) && value >= 0 && value <= 1,
            "evaluation_assessment_score_invalid");
    return value;
  };
  const auto quality = numeric("quality"), upper = numeric("quality_upper");
  require(quality <= upper && numeric("coverage") >= 1 - 1e-12, "evaluation_assessment_incomplete");
  return assessment.num("critical_failures", 0) ? 0.0 : quality;
}

} // 命名空间结束：trajectory::evolution::integration
