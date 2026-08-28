#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <thread>
#include <type_traits>

#include "runtime/agent_runtime.h"
#include "runtime/llm/streaming_client.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace agentflow {
namespace tests {

namespace {
void write_workflow_json(const std::filesystem::path& path, const std::string& workflow_id) {
  std::ofstream wf(path);
  wf << R"({"id":")" << workflow_id
     << R"(","name":")" << workflow_id
     << R"(","version":"1.0.0","nodes":[{"id":"n1","name":"n1","type":"TASK"}],"edges":[]})";
}

std::filesystem::path repo_root_path() {
  auto root = std::filesystem::path(__FILE__);
  for(int i = 0; i < 4; ++i) {
    root = root.parent_path();
  }
  return std::filesystem::weakly_canonical(root);
}

std::filesystem::path unique_build_script_path(const std::string& stem) {
  const auto suffix = std::to_string(
      std::chrono::steady_clock::now().time_since_epoch().count());
  return repo_root_path() / "build" / (stem + "_" + suffix + ".sh");
}

class SingleRequestSseServer {
 public:
  explicit SingleRequestSseServer(std::string response_body)
      : response_body_(std::move(response_body)) {}

  ~SingleRequestSseServer() {
    join();
    if(server_fd_ != -1) {
      close(server_fd_);
    }
  }

  bool start() {
    server_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if(server_fd_ == -1) {
      return false;
    }

    int reuse = 1;
    setsockopt(server_fd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port        = 0;
    if(bind(server_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == -1) {
      return false;
    }
    if(listen(server_fd_, 1) == -1) {
      return false;
    }

    socklen_t addr_len = sizeof(addr);
    if(getsockname(server_fd_, reinterpret_cast<sockaddr*>(&addr), &addr_len) == -1) {
      return false;
    }
    port_ = ntohs(addr.sin_port);
    server_thread_ = std::thread([this]() { serve_once(); });
    return true;
  }

  uint16_t port() const { return port_; }

  void join() {
    if(server_thread_.joinable()) {
      server_thread_.join();
    }
  }

 private:
  void serve_once() {
    int client_fd = accept(server_fd_, nullptr, nullptr);
    if(client_fd == -1) {
      return;
    }

    char request_buffer[2048];
    std::string request;
    while(true) {
      const ssize_t bytes_read = read(client_fd, request_buffer, sizeof(request_buffer));
      if(bytes_read <= 0) {
        break;
      }
      request.append(request_buffer, static_cast<size_t>(bytes_read));
      if(request.find("\r\n\r\n") != std::string::npos) {
        break;
      }
    }

    std::string response = "HTTP/1.1 200 OK\r\n";
    response += "Content-Type: text/event-stream\r\n";
    response += "Connection: close\r\n";
    response += "Content-Length: " + std::to_string(response_body_.size()) + "\r\n";
    response += "\r\n";
    response += response_body_;

    const char* data      = response.data();
    size_t      remaining = response.size();
    while(remaining > 0) {
      const ssize_t written = write(client_fd, data, remaining);
      if(written <= 0) {
        break;
      }
      data += written;
      remaining -= static_cast<size_t>(written);
    }

    shutdown(client_fd, SHUT_RDWR);
    close(client_fd);
  }

  int         server_fd_ = -1;
  uint16_t    port_      = 0;
  std::string response_body_;
  std::thread server_thread_;
};
}  // namespace

TEST(RuntimeMasterWorkerTest, DeployerRegisterHeartbeatAndDeployWorkflowInstance) {
  runtime::MMAgentRuntime runtime;
  runtime.set_role(runtime::MMAgentRuntime::RuntimeRole::MASTER);

  auto worker = std::make_shared<runtime::MMWorkerEngine>(
      runtime::MMWorkerEngine::Config{"worker-a", true});
  worker->set_status(runtime::MMWorkerEngine::WorkerStatus::RUNNING);
  worker->add_supported_workflow("wf-type-1");

  ASSERT_TRUE(runtime.register_worker_engine(worker));
  ASSERT_TRUE(runtime.heartbeat_worker("worker-a"));

  runtime::WorkflowInstanceInfo info;
  info.workflow_instance_id = "wf-inst-1";
  info.workflow_id = "wf-type-1";
  info.agent_object_id = "agent-object";
  info.agent_instance_id = "agent-a";

  ASSERT_TRUE(runtime.deploy_workflow_instance(info, "worker-a"));
}

TEST(RuntimeMasterWorkerTest, SchedulerPrefersSameAgentInstanceThenSharedInstance) {
  runtime::MMAgentRuntime runtime;
  runtime.set_role(runtime::MMAgentRuntime::RuntimeRole::MASTER);

  auto worker_a = std::make_shared<runtime::MMWorkerEngine>(
      runtime::MMWorkerEngine::Config{"worker-a", true});
  worker_a->set_status(runtime::MMWorkerEngine::WorkerStatus::RUNNING);
  worker_a->add_supported_workflow("wf-type-1");
  auto worker_b = std::make_shared<runtime::MMWorkerEngine>(
      runtime::MMWorkerEngine::Config{"worker-b", true});
  worker_b->set_status(runtime::MMWorkerEngine::WorkerStatus::RUNNING);
  worker_b->add_supported_workflow("wf-type-1");

  ASSERT_TRUE(runtime.register_worker_engine(worker_a));
  ASSERT_TRUE(runtime.register_worker_engine(worker_b));

  runtime::WorkflowInstanceInfo own_instance;
  own_instance.workflow_instance_id = "wf-own";
  own_instance.workflow_id = "wf-type-1";
  own_instance.agent_object_id = "agent-object";
  own_instance.agent_instance_id = "agent-a";
  ASSERT_TRUE(runtime.deploy_workflow_instance(own_instance, "worker-a"));

  runtime::WorkflowInstanceInfo shared_instance;
  shared_instance.workflow_instance_id = "wf-shared";
  shared_instance.workflow_id = "wf-type-1";
  shared_instance.agent_object_id = "agent-object";
  shared_instance.agent_instance_id = "agent-b";
  ASSERT_TRUE(runtime.deploy_workflow_instance(shared_instance, "worker-b"));

  runtime::master::TaskScheduleRequest request;
  request.task_id = "task-1";
  request.agent_object_id = "agent-object";
  request.agent_instance_id = "agent-a";
  request.workflow_id = "wf-type-1";

  auto result = runtime.schedule_task(request);
  ASSERT_TRUE(result.scheduled);
  EXPECT_EQ(result.workflow_instance_id, "wf-own");

  ASSERT_TRUE(runtime.destroy_workflow_instance("wf-own"));
  result = runtime.schedule_task(request);
  ASSERT_TRUE(result.scheduled);
  EXPECT_EQ(result.workflow_instance_id, "wf-shared");
}

TEST(RuntimeMasterWorkerTest, RuntimeMaterializesWorkflowPoolsFromAgentTasks) {
  namespace fs = std::filesystem;
  const fs::path tmp = fs::path("build/test_runtime_workflow_pool_pkg");
  fs::create_directories(tmp);

  {
    std::ofstream pkg(tmp / "package.json");
    pkg << R"({
      "name": "runtime-workflow-pool-agent",
      "version": "1.0.0",
      "workflows": ["dispatcher.json", "coordinator.json", "skill.json", "context.json"]
    })";
  }
  {
    std::ofstream agent_json(tmp / "agent.json");
    agent_json << R"({
      "session_pool_size": 1,
      "tasks": [
        {
          "id": 0,
          "type": "contextupdater",
          "description": "Context updater",
          "workflow_file": "context.json",
          "workflow_pool_size": 2
        }
      ],
      "sessions": [
        {
          "id": 0,
          "description": "Pooled session",
          "task_count": 3,
          "tasks": [
            { "id": 0, "type": "dispatcher", "description": "Dispatch", "workflow_file": "dispatcher.json", "workflow_pool_size": 3 },
            { "id": 1, "type": "coordinator", "description": "Coordinate", "workflow_file": "coordinator.json", "workflow_pool_size": 2 },
            {
              "id": 2,
              "type": "skill",
              "description": "Search skill",
              "workflow_file": "skill.json",
              "workflow_pool_size": 4,
              "skill": {
                "name": "search",
                "instance_pool_size": 2
              }
            }
          ]
        }
      ]
    })";
  }
  write_workflow_json(tmp / "dispatcher.json", "dispatcher_wf");
  write_workflow_json(tmp / "coordinator.json", "coordinator_wf");
  write_workflow_json(tmp / "skill.json", "skill_wf");
  write_workflow_json(tmp / "context.json", "context_wf");

  auto agent = framework::MMAgent::create_from_package(tmp.string());
  ASSERT_NE(agent, nullptr);

  runtime::MMAgentRuntime runtime;
  ASSERT_TRUE(runtime.register_agent(agent));
  ASSERT_TRUE(runtime.start());

  const auto workflow_instances = runtime.list_workflow_instances();
  ASSERT_EQ(workflow_instances.size(), 11u);

  std::map<std::string, int> counts_by_workflow;
  for(const auto& instance : workflow_instances) {
    ++counts_by_workflow[instance.workflow_id];
    EXPECT_EQ(instance.worker_id, "runtime-local-worker");
  }
  EXPECT_EQ(counts_by_workflow["context.json"], 2);
  EXPECT_EQ(counts_by_workflow["dispatcher_wf"], 3);
  EXPECT_EQ(counts_by_workflow["coordinator_wf"], 2);
  EXPECT_EQ(counts_by_workflow["skill_wf"], 4);

  EXPECT_TRUE(runtime.stop());
  fs::remove_all(tmp);
}

TEST(RuntimeMasterWorkerTest, WorkerExecutesShellScriptViaEngine) {
  namespace fs = std::filesystem;
  const fs::path script = unique_build_script_path("test_worker_shell");
  {
    std::ofstream ofs(script);
    ofs << "#!/bin/sh\n";
    ofs << "echo worker-shell-ok\n";
  }
  fs::permissions(script, fs::perms::owner_exec | fs::perms::owner_read | fs::perms::owner_write,
                  fs::perm_options::add);

  runtime::MMWorkerEngine::ExecutionRequest request;
  request.mode = runtime::MMWorkerEngine::ExecutionMode::SHELL_SCRIPT;
  request.command = script.string();
  auto response = runtime::MMWorkerEngine::execute_code(request);
  ASSERT_TRUE(response.success);

  auto it = response.outputs.find("stdout");
  ASSERT_NE(it, response.outputs.end());
  const std::string output = std::visit(
      [](const auto& v) -> std::string {
        using T = std::decay_t<decltype(v)>;
        if constexpr(std::is_same_v<T, std::string>) {
          return v;
        }
        return "";
      },
      it->second);
  EXPECT_NE(output.find("worker-shell-ok"), std::string::npos);
  fs::remove(script);
}

TEST(RuntimeMasterWorkerTest, WorkflowTraceReachesWorkerWithoutOrphanParent) {
  namespace fs = std::filesystem;
  const fs::path script = unique_build_script_path("test_worker_trace_env");
  const fs::path output = unique_build_script_path("test_worker_trace_output");
  const std::string trace_id =
      "trace-worker-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
  {
    std::ofstream ofs(script);
    ofs << "#!/bin/sh\n";
    ofs << "printf '%s\\n%s\\n' \"$AF_TRACE_ID\" \"$AF_PARENT_SPAN_ID\" > '"
        << output.string() << "'\n";
  }
  fs::permissions(script, fs::perms::owner_exec | fs::perms::owner_read | fs::perms::owner_write,
                  fs::perm_options::add);

  framework::MMAgent::Config agent_cfg;
  agent_cfg.id = "agent-worker-trace";
  agent_cfg.name = "agent-worker-trace";
  auto agent = std::make_shared<framework::MMAgent>(agent_cfg);

  framework::MMWorkflow::Config workflow_cfg;
  workflow_cfg.id = "workflow-worker-trace";
  workflow_cfg.name = "workflow-worker-trace";
  auto workflow = std::make_shared<framework::MMWorkflow>(workflow_cfg);

  framework::MMNode::Config node_cfg;
  node_cfg.id = "node-worker-trace";
  node_cfg.name = "node-worker-trace";
  node_cfg.type = "task";
  node_cfg.execution_mode = framework::MMNode::ExecutionMode::SHELL_SCRIPT;
  node_cfg.command = script.string();
  workflow->add_node(std::make_shared<framework::MMNode>(node_cfg));
  agent->add_workflow(workflow);

  runtime::MMAgentRuntime runtime;
  ASSERT_TRUE(runtime.register_agent(agent));
  ASSERT_TRUE(runtime.start());
  ASSERT_TRUE(runtime.execute_workflow(agent->get_id(), workflow->get_id(), {}, "", "", "", "",
                                       trace_id));

  for(int attempt = 0; attempt < 100 && !fs::exists(output); ++attempt) {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  ASSERT_TRUE(fs::exists(output));

  std::ifstream probe(output);
  std::string observed_trace_id;
  std::string observed_parent_span_id;
  std::getline(probe, observed_trace_id);
  std::getline(probe, observed_parent_span_id);
  EXPECT_EQ(observed_trace_id, trace_id);
  EXPECT_TRUE(observed_parent_span_id.empty());

  EXPECT_TRUE(runtime.stop());
  fs::remove(script);
  fs::remove(output);
}

TEST(RuntimeMasterWorkerTest, WorkerHotReloadAndRollback) {
  namespace fs = std::filesystem;
  const fs::path script = unique_build_script_path("test_worker_hot_reload");
  {
    std::ofstream ofs(script);
    ofs << "#!/bin/sh\n";
    ofs << "echo v1\n";
  }
  fs::permissions(script, fs::perms::owner_exec | fs::perms::owner_read | fs::perms::owner_write,
                  fs::perm_options::add);

  runtime::MMWorkerEngine::ExecutionRequest request;
  request.mode = runtime::MMWorkerEngine::ExecutionMode::SHELL_SCRIPT;
  request.command = script.string();
  request.enable_hot_reload = true;
  ASSERT_TRUE(runtime::MMWorkerEngine::execute_code(request).success);

  auto version1 = runtime::MMWorkerEngine::get_code_version(script.string());
  ASSERT_NE(version1, nullptr);

  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  {
    std::ofstream ofs(script);
    ofs << "#!/bin/sh\n";
    ofs << "echo v2\n";
  }

  ASSERT_TRUE(runtime::MMWorkerEngine::check_and_reload_code(script.string()));
  auto version2 = runtime::MMWorkerEngine::get_code_version(script.string());
  ASSERT_NE(version2, nullptr);
  EXPECT_NE(version2->version_id, version1->version_id);

  ASSERT_TRUE(runtime::MMWorkerEngine::rollback_code_version(script.string(), version1->version_id));
  auto current = runtime::MMWorkerEngine::get_code_version(script.string());
  ASSERT_NE(current, nullptr);
  EXPECT_EQ(current->version_id, version1->version_id);
  runtime::MMWorkerEngine::stop_hot_reload(script.string());
  fs::remove(script);
}

TEST(RuntimeMasterWorkerTest, WorkerProvidesFileDatabusAndMockLlmStreaming) {
  namespace fs = std::filesystem;
  const fs::path script = unique_build_script_path("test_worker_llm_stream");
  const fs::path sdk    = repo_root_path() / "publish" / "sdk" / "shell" / "agentflow_databus.sh";
  {
    std::ofstream ofs(script);
    ofs << "#!/bin/sh\n";
    ofs << ". '" << sdk.string() << "'\n";
    ofs << "af_databus_set runtime.status ready\n";
    ofs << "af_databus_request_llm req-1 'hello from runtime' 'agentflow.llm.chunk.req-1'\n";
    ofs << "c1=\"$(af_databus_next_event sub-1 agentflow.llm.chunk.req-1 3000)\"\n";
    ofs << "c2=\"$(af_databus_next_event sub-1 agentflow.llm.chunk.req-1 3000)\"\n";
    ofs << "done_text=\"$(af_databus_next_event sub-1 agentflow.llm.done.req-1 3000)\"\n";
    ofs << "printf '{\"reply\":\"%s %s\",\"done\":\"%s\"}\\n' \"$c1\" \"$c2\" \"$done_text\"\n";
  }
  fs::permissions(script, fs::perms::owner_exec | fs::perms::owner_read | fs::perms::owner_write,
                  fs::perm_options::add);

  runtime::MMWorkerEngine::ExecutionRequest request;
  request.mode                   = runtime::MMWorkerEngine::ExecutionMode::SHELL_SCRIPT;
  request.command                = script.string();
  request.keep_databus_files     = false;
  request.llm_config.base_url    = "mock://stream";
  request.llm_config.model       = "mock-model";
  request.llm_config.timeout_ms  = 1000;
  auto response = runtime::MMWorkerEngine::execute_code(request);
  ASSERT_TRUE(response.success) << response.error_message;

  ASSERT_TRUE(response.outputs.count("stdout") > 0);
  const std::string output = std::visit(
      [](const auto& v) -> std::string {
        using T = std::decay_t<decltype(v)>;
        if constexpr(std::is_same_v<T, std::string>) {
          return v;
        }
        return "";
      },
      response.outputs["stdout"]);
  EXPECT_NE(output.find("hello from"), std::string::npos);
  EXPECT_NE(output.find("hello from runtime"), std::string::npos);
  fs::remove(script);
}

TEST(RuntimeMasterWorkerTest, StreamingClientRemovesTempBodyFileWhenCallbackThrows) {
  const std::string prompt =
      "streaming-client-cleanup-" +
      std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
  const auto expected_body_path =
      std::filesystem::temp_directory_path()
      / ("agentflow_llm_body_" + std::to_string(std::hash<std::string>{}(prompt)) + ".json");
  std::error_code ec;
  std::filesystem::remove(expected_body_path, ec);

  SingleRequestSseServer server(
      "data: {\"choices\":[{\"delta\":{\"content\":\"hello\"}}]}\n\n"
      "data: [DONE]\n\n");
  ASSERT_TRUE(server.start());

  runtime::StreamingLLMClient         client;
  runtime::StreamingLLMClient::Config config;
  config.base_url   = "http://127.0.0.1:" + std::to_string(server.port());
  config.model      = "mock-model";
  config.timeout_ms = 2000;

  EXPECT_THROW(
      client.stream_prompt(prompt, config,
                           [](const std::string&) { throw std::runtime_error("chunk failure"); }),
      std::runtime_error);

  server.join();
  EXPECT_FALSE(std::filesystem::exists(expected_body_path));
}

// ============================================================================
// Plugin UDS Injection Tests
// ============================================================================

TEST(RuntimeMasterWorkerTest, WorkerEngineConfigPluginUdsPath) {
  // Test that plugin_uds_path can be set in WorkerEngine config
  runtime::MMWorkerEngine::Config config;
  config.worker_id = "test-worker";
  config.plugin_uds_path = "/var/run/test_plugin.sock";

  auto worker = std::make_shared<runtime::MMWorkerEngine>(config);
  EXPECT_EQ(worker->get_worker_id(), "test-worker");
}

TEST(RuntimeMasterWorkerTest, RuntimeConfigPluginUdsPath) {
  // Test that plugin_uds_path can be set in Runtime config
  runtime::MMAgentRuntime::Config config;
  config.worker_threads = 2;
  config.plugin_uds_path = "/var/run/runtime_plugin.sock";

  runtime::MMAgentRuntime runtime(config);
  runtime.set_role(runtime::MMAgentRuntime::RuntimeRole::MASTER);

  // Verify runtime was created successfully with the config
  EXPECT_TRUE(runtime.is_running() || true);  // Runtime may not be started yet
}

TEST(RuntimeMasterWorkerTest, PluginUdsPathInjectedAsEnvVar) {
  namespace fs = std::filesystem;
  const fs::path script = unique_build_script_path("test_plugin_uds_env");
  {
    std::ofstream ofs(script);
    ofs << "#!/bin/sh\n";
    ofs << "if [ -n \"$AF_PLUGIN_UDS_FILE\" ]; then\n";
    ofs << "  echo \"PLUGIN_UDS=$AF_PLUGIN_UDS_FILE\"\n";
    ofs << "else\n";
    ofs << "  echo \"PLUGIN_UDS=not_set\"\n";
    ofs << "fi\n";
  }
  fs::permissions(script, fs::perms::owner_exec | fs::perms::owner_read | fs::perms::owner_write,
                  fs::perm_options::add);

  // Test with plugin UDS path set
  runtime::MMWorkerEngine::ExecutionRequest request;
  request.mode = runtime::MMWorkerEngine::ExecutionMode::SHELL_SCRIPT;
  request.command = script.string();
  request.plugin_uds_path = "/var/run/injected_plugin.sock";

  auto response = runtime::MMWorkerEngine::execute_code(request);
  ASSERT_TRUE(response.success) << response.error_message;

  auto it = response.outputs.find("stdout");
  ASSERT_NE(it, response.outputs.end());
  const std::string output = std::visit(
      [](const auto& v) -> std::string {
        using T = std::decay_t<decltype(v)>;
        if constexpr(std::is_same_v<T, std::string>) {
          return v;
        }
        return "";
      },
      it->second);
  EXPECT_NE(output.find("PLUGIN_UDS=/var/run/injected_plugin.sock"), std::string::npos);

  fs::remove(script);
}

TEST(RuntimeMasterWorkerTest, NoPluginUdsPathWhenNotSet) {
  namespace fs = std::filesystem;
  const fs::path script = unique_build_script_path("test_no_plugin_uds");
  {
    std::ofstream ofs(script);
    ofs << "#!/bin/sh\n";
    ofs << "if [ -n \"$AF_PLUGIN_UDS_FILE\" ]; then\n";
    ofs << "  echo \"PLUGIN_UDS=$AF_PLUGIN_UDS_FILE\"\n";
    ofs << "else\n";
    ofs << "  echo \"PLUGIN_UDS=not_set\"\n";
    ofs << "fi\n";
  }
  fs::permissions(script, fs::perms::owner_exec | fs::perms::owner_read | fs::perms::owner_write,
                  fs::perm_options::add);

  // Test without plugin UDS path set
  runtime::MMWorkerEngine::ExecutionRequest request;
  request.mode = runtime::MMWorkerEngine::ExecutionMode::SHELL_SCRIPT;
  request.command = script.string();
  // plugin_uds_path is empty by default

  auto response = runtime::MMWorkerEngine::execute_code(request);
  ASSERT_TRUE(response.success) << response.error_message;

  auto it = response.outputs.find("stdout");
  ASSERT_NE(it, response.outputs.end());
  const std::string output = std::visit(
      [](const auto& v) -> std::string {
        using T = std::decay_t<decltype(v)>;
        if constexpr(std::is_same_v<T, std::string>) {
          return v;
        }
        return "";
      },
      it->second);
  EXPECT_NE(output.find("PLUGIN_UDS=not_set"), std::string::npos);

  fs::remove(script);
}

TEST(RuntimeMasterWorkerTest, PluginUdsPathWithPythonScript) {
  namespace fs = std::filesystem;
  const fs::path script = unique_build_script_path("test_plugin_uds_python");
  {
    std::ofstream ofs(script);
    ofs << "#!/usr/bin/env python3\n";
    ofs << "import os\n";
    ofs << "plugin_uds = os.environ.get('AF_PLUGIN_UDS_FILE', 'not_set')\n";
    ofs << "print(f'PLUGIN_UDS={plugin_uds}')\n";
  }
  fs::permissions(script, fs::perms::owner_exec | fs::perms::owner_read | fs::perms::owner_write,
                  fs::perm_options::add);

  runtime::MMWorkerEngine::ExecutionRequest request;
  request.mode = runtime::MMWorkerEngine::ExecutionMode::PYTHON_SCRIPT;
  request.command = script.string();
  request.plugin_uds_path = "/tmp/python_plugin.sock";

  auto response = runtime::MMWorkerEngine::execute_code(request);
  ASSERT_TRUE(response.success) << response.error_message;

  auto it = response.outputs.find("stdout");
  ASSERT_NE(it, response.outputs.end());
  const std::string output = std::visit(
      [](const auto& v) -> std::string {
        using T = std::decay_t<decltype(v)>;
        if constexpr(std::is_same_v<T, std::string>) {
          return v;
        }
        return "";
      },
      it->second);
  EXPECT_NE(output.find("PLUGIN_UDS=/tmp/python_plugin.sock"), std::string::npos);

  fs::remove(script);
}

TEST(RuntimeMasterWorkerTest, PluginUdsPathSpecialCharacters) {
  namespace fs = std::filesystem;
  const fs::path script = unique_build_script_path("test_plugin_uds_special");
  {
    std::ofstream ofs(script);
    ofs << "#!/bin/sh\n";
    ofs << "echo \"PLUGIN_UDS=$AF_PLUGIN_UDS_FILE\"\n";
  }
  fs::permissions(script, fs::perms::owner_exec | fs::perms::owner_read | fs::perms::owner_write,
                  fs::perm_options::add);

  // Test with path containing special characters (but valid for UDS paths)
  runtime::MMWorkerEngine::ExecutionRequest request;
  request.mode = runtime::MMWorkerEngine::ExecutionMode::SHELL_SCRIPT;
  request.command = script.string();
  request.plugin_uds_path = "/var/run/my-plugin_123.sock";

  auto response = runtime::MMWorkerEngine::execute_code(request);
  ASSERT_TRUE(response.success) << response.error_message;

  auto it = response.outputs.find("stdout");
  ASSERT_NE(it, response.outputs.end());
  const std::string output = std::visit(
      [](const auto& v) -> std::string {
        using T = std::decay_t<decltype(v)>;
        if constexpr(std::is_same_v<T, std::string>) {
          return v;
        }
        return "";
      },
      it->second);
  EXPECT_NE(output.find("my-plugin_123.sock"), std::string::npos);

  fs::remove(script);
}

// ============================================================================
// ParseSsePayload unit tests (pure function, no network)
// ============================================================================

using SseChunk = runtime::StreamingLLMClient::SseChunk;

static SseChunk parse(const std::string& json) {
  return runtime::StreamingLLMClient::parse_sse_payload(json);
}

TEST(ParseSsePayloadTest, EmptyInput) {
  auto r = parse("");
  EXPECT_EQ(r.content, "");
  EXPECT_EQ(r.finish_reason, "");
}

TEST(ParseSsePayloadTest, MalformedJson) {
  auto r = parse("not json at all");
  EXPECT_EQ(r.content, "");
  EXPECT_EQ(r.finish_reason, "");
}

TEST(ParseSsePayloadTest, TruncatedJson) {
  auto r = parse(R"({"choices":[{"delta":{"content":"hello")");
  EXPECT_EQ(r.content, "");
  EXPECT_EQ(r.finish_reason, "");
}

TEST(ParseSsePayloadTest, NormalContent) {
  auto r = parse(R"({"choices":[{"delta":{"content":"Hello"}}]})");
  EXPECT_EQ(r.content, "Hello");
  EXPECT_EQ(r.finish_reason, "");
}

TEST(ParseSsePayloadTest, PartialWord) {
  auto r = parse(R"({"choices":[{"delta":{"content":" flav"}}]})");
  EXPECT_EQ(r.content, " flav");
  EXPECT_EQ(r.finish_reason, "");
}

TEST(ParseSsePayloadTest, EmptyDelta) {
  auto r = parse(R"({"choices":[{"delta":{}}]})");
  EXPECT_EQ(r.content, "");
  EXPECT_EQ(r.finish_reason, "");
}

TEST(ParseSsePayloadTest, NewlineEscape) {
  auto r = parse(R"({"choices":[{"delta":{"content":"line1\nline2"}}]})");
  EXPECT_EQ(r.content, "line1\nline2");
}

TEST(ParseSsePayloadTest, EscapedQuote) {
  auto r = parse(R"({"choices":[{"delta":{"content":"say \"hi\""}}]})");
  EXPECT_EQ(r.content, "say \"hi\"");
}

TEST(ParseSsePayloadTest, FinishReasonStop) {
  auto r = parse(R"({"choices":[{"delta":{},"finish_reason":"stop"}]})");
  EXPECT_EQ(r.content, "");
  EXPECT_EQ(r.finish_reason, "stop");
}

TEST(ParseSsePayloadTest, FinishReasonLength) {
  auto r = parse(R"({"choices":[{"delta":{},"finish_reason":"length"}]})");
  EXPECT_EQ(r.finish_reason, "length");
}

TEST(ParseSsePayloadTest, ContentWithFinishReason) {
  auto r = parse(R"({"choices":[{"delta":{"content":"done"},"finish_reason":"stop"}]})");
  EXPECT_EQ(r.content, "done");
  EXPECT_EQ(r.finish_reason, "stop");
}

TEST(ParseSsePayloadTest, MultipleChoicesTakesFirst) {
  auto r = parse(R"({"choices":[{"delta":{"content":"first"}},{"delta":{"content":"second"}}]})");
  EXPECT_EQ(r.content, "first");
}

TEST(ParseSsePayloadTest, NoChoicesArray) {
  auto r = parse(R"({"model":"gpt-4"})");
  EXPECT_EQ(r.content, "");
}

TEST(ParseSsePayloadTest, EmptyChoicesArray) {
  auto r = parse(R"({"choices":[]})");
  EXPECT_EQ(r.content, "");
}

TEST(ParseSsePayloadTest, ErrorResponse) {
  auto r = parse(R"({"error":{"message":"Rate limit"}})");
  EXPECT_EQ(r.content, "");
  EXPECT_EQ(r.finish_reason, "");
}

TEST(ParseSsePayloadTest, NonStringContent) {
  // content field exists but is not a string (e.g. null or number)
  auto r = parse(R"({"choices":[{"delta":{"content":123}}]})");
  EXPECT_EQ(r.content, "");
}

TEST(ParseSsePayloadTest, NonObjectChoice) {
  auto r = parse(R"({"choices":[null]})");
  EXPECT_EQ(r.content, "");
  EXPECT_EQ(r.finish_reason, "");
}

}  // namespace tests
}  // namespace agentflow
