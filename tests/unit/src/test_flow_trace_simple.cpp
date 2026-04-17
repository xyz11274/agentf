// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License
//
// 无 Google Test 时的 FlowTrace 环境变量校验：由 ctest 设置 FLOW_TRACE_TEST_MODE
// 以及 FLOW_TRACE_DIR，在独立进程中验证「未配置目录不异常」与「配置目录可写文件」。

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include "runtime/flow_trace.h"

namespace {

using agentflow::runtime::FlowEventType;
using agentflow::runtime::FlowTraceWriter;
using agentflow::runtime::make_task_event;

int fail(const char* msg) {
  std::cerr << msg << std::endl;
  return 1;
}

int run_noenv() {
  const char* dir = std::getenv("FLOW_TRACE_DIR");
  if (dir != nullptr && dir[0] != '\0') {
    return fail("FLOW_TRACE_TEST_MODE=noenv expects empty FLOW_TRACE_DIR");
  }

  auto& w = FlowTraceWriter::instance();
  w.set_stdout_enabled(false);

  auto e = make_task_event("ut-session", "ut-dialogue", "from-a", "to-b",
                           FlowEventType::TASK_CREATED);
  e.time   = "2026-04-17 12:00:00.000";
  e.labels = "clientSessionId=ut_simple_no_dir";

  try {
    w.write_event(e);
  } catch (...) {
    return fail("write_event threw with no FLOW_TRACE_DIR");
  }
  return 0;
}

int run_withenv() {
  const char* dir = std::getenv("FLOW_TRACE_DIR");
  if (dir == nullptr || dir[0] == '\0') {
    return fail("FLOW_TRACE_TEST_MODE=withenv requires FLOW_TRACE_DIR");
  }

  auto& w = FlowTraceWriter::instance();
  w.set_stdout_enabled(false);

  auto e = make_task_event("ut-session", "ut-dialogue", "from-a", "to-b",
                           FlowEventType::TASK_CREATED);
  e.time   = "2026-04-17 12:00:00.000";
  e.labels = "clientSessionId=ut_simple_env_client";

  try {
    w.write_event(e);
  } catch (...) {
    return fail("write_event threw with FLOW_TRACE_DIR set");
  }

  const std::filesystem::path expected =
      std::filesystem::path(dir) / "flow_trace_2026-04-17_ut_simple_env_client.ndjson";
  if (!std::filesystem::exists(expected)) {
    std::cerr << "missing: " << expected.string() << std::endl;
    return 1;
  }

  std::ifstream in(expected.string());
  std::string line;
  if (!std::getline(in, line)) {
    return fail("expected ndjson line");
  }
  if (line.find("ut_simple_env_client") == std::string::npos) {
    return fail("ndjson missing client id marker");
  }
  return 0;
}

}  // namespace

int main() {
  const char* mode = std::getenv("FLOW_TRACE_TEST_MODE");
  if (mode == nullptr) {
    return fail("FLOW_TRACE_TEST_MODE not set (noenv|withenv)");
  }
  if (std::strcmp(mode, "noenv") == 0) {
    return run_noenv();
  }
  if (std::strcmp(mode, "withenv") == 0) {
    return run_withenv();
  }
  return fail("FLOW_TRACE_TEST_MODE must be noenv or withenv");
}
