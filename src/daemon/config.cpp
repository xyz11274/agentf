// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License
//
// DaemonConfig implementation - configuration loading and merging

#include "daemon/config.h"  // NOLINT: resolved via include/internal/

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>

#include "workflow/dsl/json_adapter.h"

namespace agentflow {
namespace daemon {

namespace {

std::string get_env(const char* name, const std::string& default_val = "") {
  const char* val = std::getenv(name);
  return val ? std::string(val) : default_val;
}

bool is_set(const std::string& val) {
  return !val.empty();
}

bool parse_listen_addr(const std::string& addr, std::string& host, int& port) {
  auto pos = addr.rfind(':');
  if (pos == std::string::npos) return false;
  host = addr.substr(0, pos);
  try {
    port = std::stoi(addr.substr(pos + 1));
  } catch (...) {
    return false;
  }
  return !host.empty() && port > 0 && port <= 65535;
}

}  // namespace

bool parse_cli_args(int argc, char* argv[], DaemonConfig& config) {
  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];

    if (arg == "--help" || arg == "-h") {
      std::cout
          << "Usage: agentflowd [options]\n"
             "\n"
             "Agent loading:\n"
             "  --package PATH           Agent package directory (repeatable)\n"
             "  --validate               Validate config + package structure then exit\n"
             "\n"
             "Server:\n"
             "  --listen ADDR:PORT       TCP listen address (default 0.0.0.0:7890)\n"
             "  --uds-biz-path PATH      UDS business socket path\n"
             "  --uds-ctrl-path PATH     UDS control socket path\n"
             "  --max-connections N      Max concurrent client connections (default 1024)\n"
             "\n"
             "Runtime:\n"
             "  --role ROLE              master|worker (default master)\n"
             "  --worker-threads N       Worker thread pool size (default 4)\n"
             "  --worker-id ID           Local Worker ID\n"
             "  --workflow-threads N     Default workflow threads per worker (default 1)\n"
             "  --control-threads N      Control threads per worker (default 1)\n"
             "  --business-threads N     Business threads per worker (default 1)\n"
             "  --queue-size N           Worker queue size (default 1024)\n"
             "\n"
             "Operations:\n"
             "  --config FILE            Load JSON config file\n"
             "  --daemonize              Run as daemon\n"
             "  --pid-file FILE          PID file path\n"
             "  --log-level LEVEL        trace|debug|info|warn|error (default info)\n"
             "  --log-file FILE          Log file (default stdout only)\n"
             "  --flow-trace-file FILE   Flow trace output file (single .ndjson)\n"
             "  --flow-trace-dir DIR     Flow trace output directory (per-session .ndjson)\n"
             "  --drain-timeout MS       Graceful shutdown timeout (default 30000)\n"
             "\n"
             "  --version / --help\n";
      return false;
    }

    if (arg == "--version") {
      std::cout << "agentflowd 0.1.0" << std::endl;
      return false;
    }

    auto take_value = [&](int& idx) -> std::string {
      if (idx + 1 >= argc) {
        std::cerr << "Error: " << arg << " requires a value" << std::endl;
        return "";
      }
      return argv[++idx];
    };

    if (arg == "--package") {
      auto val = take_value(i);
      if (val.empty()) return false;
      config.agents.push_back({val, ""});
    } else if (arg == "--validate") {
      config.validate = true;
    } else if (arg == "--listen") {
      config.listen = take_value(i);
      if (config.listen.empty()) return false;
    } else if (arg == "--uds-biz-path") {
      config.uds_biz_path = take_value(i);
      if (config.uds_biz_path.empty()) return false;
    } else if (arg == "--uds-ctrl-path") {
      config.uds_ctrl_path = take_value(i);
      if (config.uds_ctrl_path.empty()) return false;
    } else if (arg == "--max-connections") {
      config.max_connections = std::stoi(take_value(i));
    } else if (arg == "--role") {
      auto val = take_value(i);
      if (val == "master")
        config.runtime_config.role = runtime::MMAgentRuntime::RuntimeRole::MASTER;
      else if (val == "worker")
        config.runtime_config.role = runtime::MMAgentRuntime::RuntimeRole::WORKER;
      else {
        std::cerr << "Error: invalid role '" << val << "' (master|worker)" << std::endl;
        return false;
      }
    } else if (arg == "--worker-threads") {
      config.runtime_config.worker_threads = std::stoi(take_value(i));
    } else if (arg == "--worker-id") {
      config.runtime_config.local_worker_id = take_value(i);
    } else if (arg == "--workflow-threads") {
      config.runtime_config.worker_default_workflow_threads = std::stoi(take_value(i));
    } else if (arg == "--control-threads") {
      config.runtime_config.worker_control_threads = std::stoi(take_value(i));
    } else if (arg == "--business-threads") {
      config.runtime_config.worker_business_threads = std::stoi(take_value(i));
    } else if (arg == "--queue-size") {
      config.runtime_config.worker_queue_size = std::stoul(take_value(i));
    } else if (arg == "--deploy-interval") {
      config.deployer_config.schedule_interval_ms = std::stoi(take_value(i));
    } else if (arg == "--heartbeat-timeout") {
      config.deployer_config.heartbeat_timeout_ms = std::stoll(take_value(i));
    } else if (arg == "--schedule-interval") {
      config.scheduler_config.schedule_interval_ms = std::stoi(take_value(i));
    } else if (arg == "--config") {
      config.config_file = take_value(i);
      if (config.config_file.empty()) return false;
    } else if (arg == "--daemonize") {
      config.daemonize = true;
    } else if (arg == "--pid-file") {
      config.pid_file = take_value(i);
    } else if (arg == "--log-level") {
      config.log_level = take_value(i);
    } else if (arg == "--log-file") {
      config.log_file = take_value(i);
    } else if (arg == "--flow-trace-file") {
      config.flow_trace_file = take_value(i);
      if (config.flow_trace_file.empty()) return false;
    } else if (arg == "--flow-trace-dir") {
      config.flow_trace_dir = take_value(i);
      if (config.flow_trace_dir.empty()) return false;
    } else if (arg == "--drain-timeout") {
      config.drain_timeout_ms = std::stoi(take_value(i));
    } else {
      std::cerr << "Error: unknown option '" << arg << "'" << std::endl;
      return false;
    }
  }
  return true;
}

void apply_env_overrides(DaemonConfig& config) {
  auto env_if = [](const char* name, std::string& target) {
    std::string val = get_env(name);
    if (!val.empty()) {
      target = val;
    }
  };

  env_if("AF_LISTEN", config.listen);
  env_if("AF_UDS_BIZ_PATH", config.uds_biz_path);
  env_if("AF_UDS_CTRL_PATH", config.uds_ctrl_path);
  env_if("AF_PLUGIN_UDS_FILE", config.runtime_config.plugin_uds_path);
  env_if("AF_LOG_LEVEL", config.log_level);
  env_if("AF_LOG_FILE", config.log_file);
  env_if("AF_FLOW_TRACE_FILE", config.flow_trace_file);
  env_if("AF_FLOW_TRACE_DIR", config.flow_trace_dir);
  env_if("AF_CONFIG_FILE", config.config_file);
}

bool load_config_file(DaemonConfig& config) {
  if (config.config_file.empty()) return true;

  auto doc = dsl::JsonDocumentFactory::create();
  if (!doc->parse_file(config.config_file) || !doc->is_valid()) {
    std::cerr << "Error: failed to parse config file: " << config.config_file << std::endl;
    if (!doc->get_error().empty()) {
      std::cerr << "  " << doc->get_error() << std::endl;
    }
    return false;
  }

  auto root = doc->root();
  if (!root || !root->is_object()) {
    std::cerr << "Error: config file root is not an object" << std::endl;
    return false;
  }

  // Server section
  auto server = dsl::JsonHelper::get_object(root, "server");
  if (server) {
    auto val = dsl::JsonHelper::get_string(server, "listen");
    if (is_set(val)) config.listen = val;
    val = dsl::JsonHelper::get_string(server, "uds_biz_path");
    if (is_set(val)) config.uds_biz_path = val;
    val = dsl::JsonHelper::get_string(server, "uds_ctrl_path");
    if (is_set(val)) config.uds_ctrl_path = val;
    int64_t mc = dsl::JsonHelper::get_int(server, "max_connections", 0);
    if (mc > 0) config.max_connections = static_cast<int>(mc);
  }

  // Runtime section
  auto runtime_node = dsl::JsonHelper::get_object(root, "runtime");
  if (runtime_node) {
    std::string role = dsl::JsonHelper::get_string(runtime_node, "role");
    if (role == "master")
      config.runtime_config.role = runtime::MMAgentRuntime::RuntimeRole::MASTER;
    else if (role == "worker")
      config.runtime_config.role = runtime::MMAgentRuntime::RuntimeRole::WORKER;

    int64_t wt = dsl::JsonHelper::get_int(runtime_node, "worker_threads", 0);
    if (wt > 0) config.runtime_config.worker_threads = static_cast<int>(wt);
    std::string wid = dsl::JsonHelper::get_string(runtime_node, "local_worker_id");
    if (is_set(wid)) config.runtime_config.local_worker_id = wid;
    int64_t wft = dsl::JsonHelper::get_int(runtime_node, "worker_default_workflow_threads", 0);
    if (wft > 0) config.runtime_config.worker_default_workflow_threads = static_cast<int>(wft);
    int64_t wct = dsl::JsonHelper::get_int(runtime_node, "worker_control_threads", 0);
    if (wct > 0) config.runtime_config.worker_control_threads = static_cast<int>(wct);
    int64_t wbt = dsl::JsonHelper::get_int(runtime_node, "worker_business_threads", 0);
    if (wbt > 0) config.runtime_config.worker_business_threads = static_cast<int>(wbt);
    int64_t qs = dsl::JsonHelper::get_int(runtime_node, "worker_queue_size", 0);
    if (qs > 0) config.runtime_config.worker_queue_size = static_cast<size_t>(qs);
    std::string puds = dsl::JsonHelper::get_string(runtime_node, "plugin_uds_path");
    if (is_set(puds)) config.runtime_config.plugin_uds_path = puds;
  }

  // Deployer section
  auto deployer = dsl::JsonHelper::get_object(root, "deployer");
  if (deployer) {
    int64_t di = dsl::JsonHelper::get_int(deployer, "schedule_interval_ms", 0);
    if (di > 0) config.deployer_config.schedule_interval_ms = static_cast<int>(di);
    int64_t ht = dsl::JsonHelper::get_int(deployer, "heartbeat_timeout_ms", 0);
    if (ht > 0) config.deployer_config.heartbeat_timeout_ms = ht;
  }

  // Scheduler section
  auto scheduler = dsl::JsonHelper::get_object(root, "scheduler");
  if (scheduler) {
    int64_t si = dsl::JsonHelper::get_int(scheduler, "schedule_interval_ms", 0);
    if (si > 0) config.scheduler_config.schedule_interval_ms = static_cast<int>(si);
  }

  // Agents section (append with dedup)
  auto agents_arr = dsl::JsonHelper::get_array(root, "agents");
  if (agents_arr) {
    for (size_t i = 0; i < agents_arr->array_size(); ++i) {
      auto entry = agents_arr->array_at(i);
      if (!entry || !entry->is_object()) continue;
      DaemonConfig::AgentEntry ae;
      ae.package_path = dsl::JsonHelper::get_string(entry, "package_path");
      ae.name_override = dsl::JsonHelper::get_string(entry, "name_override");
      if (!ae.package_path.empty()) {
        bool dup = false;
        for (auto& existing : config.agents) {
          if (existing.package_path == ae.package_path) {
            dup = true;
            break;
          }
        }
        if (!dup) config.agents.push_back(ae);
      }
    }
  }

  // Logging section
  auto logging = dsl::JsonHelper::get_object(root, "logging");
  if (logging) {
    std::string ll = dsl::JsonHelper::get_string(logging, "level");
    if (is_set(ll)) config.log_level = ll;
    std::string lf = dsl::JsonHelper::get_string(logging, "file");
    if (is_set(lf)) config.log_file = lf;
  }

  // Trace section
  auto trace = dsl::JsonHelper::get_object(root, "trace");
  if (trace) {
    std::string ftf = dsl::JsonHelper::get_string(trace, "flow_file");
    if (is_set(ftf)) config.flow_trace_file = ftf;
    std::string ftd = dsl::JsonHelper::get_string(trace, "flow_dir");
    if (is_set(ftd)) config.flow_trace_dir = ftd;
  }

  // Daemon section
  auto daemon_node = dsl::JsonHelper::get_object(root, "daemon");
  if (daemon_node) {
    std::string pf = dsl::JsonHelper::get_string(daemon_node, "pid_file");
    if (is_set(pf)) config.pid_file = pf;
    int64_t dt = dsl::JsonHelper::get_int(daemon_node, "drain_timeout_ms", 0);
    if (dt > 0) config.drain_timeout_ms = static_cast<int>(dt);
  }

  return true;
}

bool merge_config(DaemonConfig& config) {
  if (!load_config_file(config)) return false;
  apply_env_overrides(config);
  return true;
}

bool validate_config(const DaemonConfig& config) {
  std::string host;
  int port;
  if (!parse_listen_addr(config.listen, host, port)) {
    std::cerr << "Error: invalid listen address '" << config.listen
              << "' (expected host:port)" << std::endl;
    return false;
  }
  if (config.max_connections <= 0) {
    std::cerr << "Error: max_connections must be positive" << std::endl;
    return false;
  }
  if (config.runtime_config.worker_threads <= 0) {
    std::cerr << "Error: worker_threads must be positive" << std::endl;
    return false;
  }
  if (config.drain_timeout_ms <= 0) {
    std::cerr << "Error: drain_timeout_ms must be positive" << std::endl;
    return false;
  }
  return true;
}

}  // namespace daemon
}  // namespace agentflow
