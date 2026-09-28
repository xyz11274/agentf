// cpp 客户端：通过统一协议收集执行事件并接收策略建议。
// 传输与业务执行分离；接收回执不代表 Agent 已完成整个任务。
// 打点的输入是建议修改前的请求，执行后必须回传实际输入、输出与状态。
// 取消、超时和采集缺口需要显式处理，不能把缺失证据当成正常完成。
// 本文件包含协议验证或接入示例，不作为真实业务效果的评测成绩。

#include "evolution_points.hpp"
#include "../c/evolution_points.h"

#include <rapidjson/document.h>

#include <cassert>
#include <stdexcept>
#include <string>

namespace {
struct Fixture {
  unsigned executions = 0;
  unsigned after = 0;
  unsigned aborts = 0;
  std::string response;
  bool fail_next = false;
  bool saw_capture_gap = false;

  // 发送一份已构造的协议请求，保持请求身份、超时和响应上限。
  std::string send(const std::string &request) {
    rapidjson::Document value;
    value.Parse(request.c_str());
    assert(value.IsObject());
    if (value.HasMember("capture_gap") && value["capture_gap"].IsBool() &&
        value["capture_gap"].GetBool())
      saw_capture_gap = true;
    if (fail_next) {
      fail_next = false;
      throw std::runtime_error("offline");
    }
    const std::string operation = value["operation"].GetString();
    if (operation == "evaluation.next")
      return R"({"ok":true,"result":{"state":"done"}})";
    if (operation == "evaluation.fail" || operation == "capture.gap")
      return R"({"ok":true,"result":{}})";
    assert(operation == "point");
    const std::string phase = value["phase"].GetString();
    if (phase == "before") {
      const bool reject =
          value.HasMember("name") && std::string(value["name"].GetString()) == "blocked";
      return reject
                 ? R"({"ok":true,"result":{"input":{"prepared":true},"decision":"reject","reason":"policy_rejected","context":{"run_id":"run","activity_id":"activity"}}})"
                 : R"({"ok":true,"result":{"input":{"prepared":true},"decision":"allow","context":{"run_id":"run","activity_id":"activity"}}})";
    }
    if (phase == "guard")
      return R"({"ok":true,"result":{"decision":"stop","event_id":"loop-receipt","context":{"run_id":"run","control_id":"loop"}}})";
    if (phase == "abort")
      ++aborts;
    if (phase == "after")
      ++after;
    return R"({"ok":true,"result":{"event_id":"receipt","ended":true,"context":{"run_id":"run"}}})";
  }
};

// 测试辅助函数：transport。
// 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
const char *transport(const char *request, void *context) {
  auto &fixture = *static_cast<Fixture *>(context);
  fixture.response = fixture.send(request);
  return fixture.response.c_str();
}

// 测试辅助函数：execute_ok。
// 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
int execute_ok(const char *input, void *context, const char **output) {
  auto &fixture = *static_cast<Fixture *>(context);
  ++fixture.executions;
  assert(std::string(input) == R"({"prepared":true})");
  *output = R"({"done":true})";
  return 0;
}

// 测试辅助函数：execute_failed。
// 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
int execute_failed(const char *, void *context, const char **) {
  ++static_cast<Fixture *>(context)->executions;
  return 47;
}
} // 结束当前匿名命名空间。

// 回归用例：main。
// 关键断言：assert(input == R"({"prepared":true})");；此处只验证测试构造的场景。
int main() {
  Fixture fixture;
  trajectory_evolution_http::Points points(
      [&](const std::string &request) { return fixture.send(request); });

  const auto allowed = points.call(
      R"({"kind":"action","name":"allowed","ref":{"run":"run","key":"one"},"input":{"raw":true}})",
      [&](const std::string &input) {
        ++fixture.executions;
        assert(input == R"({"prepared":true})");
        return std::string(R"({"done":true})");
      });
  assert(allowed.executed && !allowed.rejected);
  assert(allowed.output == R"({"done":true})");
  assert(fixture.executions == 1 && fixture.after == 1);

  const auto rejected = points.call(
      R"({"kind":"action","name":"blocked","ref":{"run":"run","key":"two"},"input":{}})",
      [&](const std::string &) {
        ++fixture.executions;
        return std::string("{}");
      });
  assert(!rejected.executed && rejected.rejected);
  assert(fixture.executions == 1 && fixture.after == 2);

  const std::runtime_error business("business_failure");
  try {
    points.call(
        R"({"kind":"action","name":"allowed","ref":{"run":"run","key":"three"},"input":{}})",
        [&](const std::string &) -> std::string {
          ++fixture.executions;
          throw business;
        });
    assert(false);
  } catch (const std::runtime_error &error) {
    assert(std::string(error.what()) == "business_failure");
  }
  assert(fixture.executions == 2 && fixture.aborts == 1);

  const auto loop = points.allow_loop(R"({"ref":{"run":"run","key":"loop"},"state":{"round":2}})");
  assert(!loop.allowed && loop.receipt.find("loop-receipt") != std::string::npos);
  const auto legacy =
      points.allow_loop(R"({"context":{"run_id":"run","caused_by":[]},"state":{"round":3}})");
  assert(legacy.allowed && fixture.after == 3);

  fixture.fail_next = true;
  const auto degraded = points.point(
      R"({"phase":"before","kind":"action","ref":{"run":"gap-run","key":"one"},"input":{}})");
  assert(degraded.find(R"("degraded":true)") != std::string::npos);
  points.point(
      R"({"phase":"before","kind":"action","ref":{"run":"gap-run","key":"two"},"input":{}})");
  assert(fixture.saw_capture_gap);

  assert(points.evaluation_next("consumer", 0).find("done") != std::string::npos);
  assert(points.evaluation_fail("consumer") == "{}");

  Fixture c_fixture;
  te_points_handle *c = te_points_open_callback(&transport, &c_fixture);
  assert(c);
  const char *error = nullptr;
  te_points_call_result result{};
  assert(te_points_call(
             c,
             R"({"kind":"action","name":"blocked","ref":{"run":"run","key":"c-one"},"input":{}})",
             &execute_ok, &c_fixture, &result, &error) == 0);
  assert(result.rejected && !result.executed && c_fixture.executions == 0);
  te_points_call_result_free(&result);

  assert(te_points_call(
             c,
             R"({"kind":"action","name":"allowed","ref":{"run":"run","key":"c-two"},"input":{}})",
             &execute_ok, &c_fixture, &result, &error) == 0);
  assert(result.executed && result.business_code == 0 && c_fixture.executions == 1);
  te_points_call_result_free(&result);

  assert(te_points_call(
             c,
             R"({"kind":"action","name":"allowed","ref":{"run":"run","key":"c-three"},"input":{}})",
             &execute_failed, &c_fixture, &result, &error) == 0);
  assert(result.business_code == 47 && c_fixture.executions == 2 && c_fixture.aborts == 1 &&
         result.receipt_json);
  te_points_call_result_free(&result);

  int continue_loop = 1;
  char *receipt = nullptr;
  assert(te_points_allow_loop(c, R"({"ref":{"run":"run","key":"c-loop"},"state":{"round":2}})",
                              &continue_loop, &receipt, &error) == 0);
  assert(!continue_loop && receipt);
  te_points_free(receipt);
  te_points_close(c);
}
