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

  std::string send(const std::string& request) {
    rapidjson::Document value;
    value.Parse(request.c_str());
    assert(value.IsObject());
    if (value.HasMember("capture_gap") &&
        value["capture_gap"].IsBool() && value["capture_gap"].GetBool())
      saw_capture_gap = true;
    if (fail_next) { fail_next = false; throw std::runtime_error("offline"); }
    const std::string operation = value["operation"].GetString();
    if (operation == "evaluation.next")
      return R"({"ok":true,"result":{"state":"done"}})";
    if (operation == "evaluation.fail" || operation == "capture.gap")
      return R"({"ok":true,"result":{}})";
    assert(operation == "point");
    const std::string phase = value["phase"].GetString();
    if (phase == "before") {
      const bool reject = value.HasMember("name") &&
          std::string(value["name"].GetString()) == "blocked";
      return reject
          ? R"({"ok":true,"result":{"input":{"prepared":true},"decision":"reject","reason":"policy_rejected","context":{"run_id":"run","activity_id":"activity"}}})"
          : R"({"ok":true,"result":{"input":{"prepared":true},"decision":"allow","context":{"run_id":"run","activity_id":"activity"}}})";
    }
    if (phase == "guard")
      return R"({"ok":true,"result":{"decision":"stop","event_id":"loop-receipt","context":{"run_id":"run","control_id":"loop"}}})";
    if (phase == "abort") ++aborts;
    if (phase == "after") ++after;
    return R"({"ok":true,"result":{"event_id":"receipt","ended":true,"context":{"run_id":"run"}}})";
  }
};

const char* transport(const char* request, void* context) {
  auto& fixture = *static_cast<Fixture*>(context);
  fixture.response = fixture.send(request);
  return fixture.response.c_str();
}

int execute_ok(const char* input, void* context, const char** output) {
  auto& fixture = *static_cast<Fixture*>(context);
  ++fixture.executions;
  assert(std::string(input) == R"({"prepared":true})");
  *output = R"({"done":true})";
  return 0;
}

int execute_failed(const char*, void* context, const char**) {
  ++static_cast<Fixture*>(context)->executions;
  return 47;
}
}  // namespace

int main() {
  Fixture fixture;
  trajectory_evolution_http::Points points(
      [&](const std::string& request) { return fixture.send(request); });

  const auto allowed = points.call(
      R"({"kind":"action","name":"allowed","ref":{"run":"run","key":"one"},"input":{"raw":true}})",
      [&](const std::string& input) {
        ++fixture.executions;
        assert(input == R"({"prepared":true})");
        return std::string(R"({"done":true})");
      });
  assert(allowed.executed && !allowed.rejected);
  assert(allowed.output == R"({"done":true})");
  assert(fixture.executions == 1 && fixture.after == 1);

  const auto rejected = points.call(
      R"({"kind":"action","name":"blocked","ref":{"run":"run","key":"two"},"input":{}})",
      [&](const std::string&) {
        ++fixture.executions;
        return std::string("{}");
      });
  assert(!rejected.executed && rejected.rejected);
  assert(fixture.executions == 1 && fixture.after == 2);

  const std::runtime_error business("business_failure");
  try {
    points.call(
        R"({"kind":"action","name":"allowed","ref":{"run":"run","key":"three"},"input":{}})",
        [&](const std::string&) -> std::string {
          ++fixture.executions;
          throw business;
        });
    assert(false);
  } catch (const std::runtime_error& error) {
    assert(std::string(error.what()) == "business_failure");
  }
  assert(fixture.executions == 2 && fixture.aborts == 1);

  const auto loop = points.allow_loop(
      R"({"ref":{"run":"run","key":"loop"},"state":{"round":2}})");
  assert(!loop.allowed && loop.receipt.find("loop-receipt") != std::string::npos);
  const auto legacy = points.allow_loop(
      R"({"context":{"run_id":"run","caused_by":[]},"state":{"round":3}})");
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
  te_points_handle* c = te_points_open_callback(&transport, &c_fixture);
  assert(c);
  const char* error = nullptr;
  te_points_call_result result{};
  assert(te_points_call(c,
      R"({"kind":"action","name":"blocked","ref":{"run":"run","key":"c-one"},"input":{}})",
      &execute_ok, &c_fixture, &result, &error) == 0);
  assert(result.rejected && !result.executed && c_fixture.executions == 0);
  te_points_call_result_free(&result);

  assert(te_points_call(c,
      R"({"kind":"action","name":"allowed","ref":{"run":"run","key":"c-two"},"input":{}})",
      &execute_ok, &c_fixture, &result, &error) == 0);
  assert(result.executed && result.business_code == 0 &&
         c_fixture.executions == 1);
  te_points_call_result_free(&result);

  assert(te_points_call(c,
      R"({"kind":"action","name":"allowed","ref":{"run":"run","key":"c-three"},"input":{}})",
      &execute_failed, &c_fixture, &result, &error) == 0);
  assert(result.business_code == 47 && c_fixture.executions == 2 &&
         c_fixture.aborts == 1 && result.receipt_json);
  te_points_call_result_free(&result);

  int continue_loop = 1;
  char* receipt = nullptr;
  assert(te_points_allow_loop(c,
      R"({"ref":{"run":"run","key":"c-loop"},"state":{"round":2}})",
      &continue_loop, &receipt, &error) == 0);
  assert(!continue_loop && receipt);
  te_points_free(receipt);
  te_points_close(c);
}
