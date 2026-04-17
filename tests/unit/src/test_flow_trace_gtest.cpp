// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#include "runtime/flow_trace.h"

using agentflow::runtime::FlowEventType;
using agentflow::runtime::FlowTraceWriter;
using agentflow::runtime::make_task_event;

// 与 CMake 中两条 ctest 配合：分别在「清空 FLOW_TRACE_DIR」与「设置 FLOW_TRACE_DIR」
// 的进程环境下各跑一条，避免单例只在首次访问时读取环境变量带来的耦合。

TEST(FlowTraceWriterTest, WriteWithoutConfiguredDir) {
  auto& w = FlowTraceWriter::instance();
  w.set_stdout_enabled(false);

  auto e = make_task_event("ut-session", "ut-dialogue", "from-a", "to-b",
                             FlowEventType::TASK_CREATED);
  e.time   = "2026-04-17 12:00:00.000";
  e.labels = "clientSessionId=ut_no_dir";

  EXPECT_NO_THROW(w.write_event(e));
}

TEST(FlowTraceWriterTest, WriteWithFlowTraceDirEnv) {
  const char* dir = std::getenv("FLOW_TRACE_DIR");
  ASSERT_TRUE(dir != nullptr && dir[0] != '\0');

  auto& w = FlowTraceWriter::instance();
  w.set_stdout_enabled(false);

  auto e = make_task_event("ut-session", "ut-dialogue", "from-a", "to-b",
                             FlowEventType::TASK_CREATED);
  e.time   = "2026-04-17 12:00:00.000";
  e.labels = "clientSessionId=ut_env_client";

  EXPECT_NO_THROW(w.write_event(e));

  const std::filesystem::path expected =
      std::filesystem::path(dir) / "flow_trace_2026-04-17_ut_env_client.ndjson";
  ASSERT_TRUE(std::filesystem::exists(expected)) << expected.string();

  std::ifstream in(expected.string());
  std::string line;
  ASSERT_TRUE(static_cast<bool>(std::getline(in, line)));
  EXPECT_NE(line.find("ut_env_client"), std::string::npos);
}
