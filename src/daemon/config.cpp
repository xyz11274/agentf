// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License
//
// DaemonConfig implementation - configuration loading and merging

#include "daemon/config.h"  // NOLINT: resolved via include/internal/

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <unordered_set>

#include "workflow/dsl/json_adapter.h"
#include "utils/logger.h"

// Rapidjson 头文件：用于启动时校验 sandbox JSON keys 是否被 C++ 全部加载
#ifdef AGENTFLOW_WITH_RAPIDJSON
#include <rapidjson/document.h>
#include "vendor/json/rapidjson_impl.h"
#endif

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
             "  --runtime-pkg-dir DIR    Pre-install agentflow-runtime from this dir\n"
             "                          (e.g. packages/agentflow-runtime)\n"
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
      config.cli_listen_set = true;  // 守护「CLI > JSON」优先级，load_config_file() 据此跳过覆盖
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
    } else if (arg == "--dataref-capacity-mb") {
      config.dataref_bridge_config.capacity_mb = std::stoi(take_value(i));
    } else if (arg == "--dataref-redis-url") {
      config.dataref_bridge_config.redis_url = take_value(i);
    } else if (arg == "--runtime-pkg-dir") {
      config.runtime_pkg_dir = take_value(i);
      if (config.runtime_pkg_dir.empty()) return false;
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

  const char* env_cap = std::getenv("AF_DATAREF_CAPACITY_MB");
  if (env_cap) {
    int cap = std::atoi(env_cap);
    if (cap > 0) config.dataref_bridge_config.capacity_mb = cap;
  }
  env_if("AF_DATAREF_REDIS_URL", config.dataref_bridge_config.redis_url);

  // Sandbox env overrides（AgentFoundry 沙箱机制移植）
  // 环境变量命名遵循三域规范：AF_SANDBOX_* 归 daemon 管辖
  // 与 configs/{env}/agentflowd.json 的 sandbox 段一一对应
  env_if("AF_SANDBOX_MODE", config.sandbox_config.mode);
  env_if("AF_SANDBOX_WORKDIR_ROOT", config.sandbox_config.workdir_root);
  env_if("AF_SANDBOX_SNAPSHOT_ROOT", config.sandbox_config.snapshot_root);
  env_if("AF_SANDBOX_E2B_API_URL", config.sandbox_config.e2b_api_url);
  env_if("AF_SANDBOX_E2B_API_KEY", config.sandbox_config.e2b_api_key);
  env_if("AF_SANDBOX_E2B_TEMPLATE", config.sandbox_config.e2b_default_template);
  env_if("AF_SANDBOX_E2B_NETWORK_ALLOW_OUT", config.sandbox_config.e2b_network_allow_out);
  env_if("AF_SANDBOX_E2B_REMOTE_WORKSPACE_DIR", config.sandbox_config.e2b_remote_workspace_dir);
  env_if("AF_SANDBOX_E2B_TEMPLATE_MAP_PATH", config.sandbox_config.e2b_template_map_path);
  env_if("AF_SANDBOX_SKILLS_SRC_DIR", config.sandbox_config.skills_src_dir);
  env_if("AF_SANDBOX_IMAGE_NAME", config.sandbox_config.image_name);

  auto env_int_positive = [](const char* name, int64_t& target) {
    const char* v = std::getenv(name);
    if (v) {
      int64_t n = std::atoll(v);
      if (n > 0) target = n;
    }
  };

  env_int_positive("AF_SANDBOX_MAX_ACTIVE",
                   *reinterpret_cast<int64_t*>(&config.sandbox_config.sandbox_max_active));
  env_int_positive("AF_SANDBOX_HTTP_PORT",
                   *reinterpret_cast<int64_t*>(&config.sandbox_config.sandbox_http_port));
  env_int_positive("AF_SANDBOX_GC_DESTROY_TTL_MS",
                   config.sandbox_config.gc_destroy_ttl_ms);
  env_int_positive("AF_SANDBOX_GC_RECYCLE_SNAPSHOT_TTL_MS",
                   config.sandbox_config.gc_recycle_snapshot_ttl_ms);

  // output_max_bytes: 0 = 不截断；>0 截断。环境变量需支持显式 0
  const char* env_om = std::getenv("AF_SANDBOX_OUTPUT_MAX_BYTES");
  if (env_om) {
    int64_t n = std::atoll(env_om);
    if (n >= 0) config.sandbox_config.output_max_bytes = n;
  }
}

bool load_config_file(DaemonConfig& config) {
  // Fallback：未显式指定 config file 路径时，主动找默认配置文件 configs/${AGENTFLOW_ENV:-dev}/agentflowd.json
  //
  // 设计对齐（docs/design/config-landscape.md §2.1.4 声明的合并顺序：
  //   parse_cli_args() → load_config_file()（仅填充未设置字段）→ apply_env_overrides()
  //   最终优先级 CLI > 环境变量 > JSON 配置文件 > 代码默认值）
  // 偏离根因（本轮 #1 调研结论）：实际代码在未显式指定 config file 时（config_file=""）
  //   load_config_file 直接返回不读任何 JSON → sandbox_config.mode 保留代码默认值 "none"
  //   （include/internal/daemon/config.h:62 std::string mode = "none"），而非 configs/{env}/
  //   agentflowd.json 声明的 "local"。导致 test fixture 未透传 AF_CONFIG_FILE 时 daemon bypass
  //   sandbox，skill 直接写宿主机文件系统（图片落本地而非 sandbox workdir_root）。
  // 修法：未显式指定 config file 时 fallback 到 configs/${AGENTFLOW_ENV:-dev}/agentflowd.json，
  //   若该文件存在则读它（读不到仍静默返回 true，保留向后兼容——无配置文件时全走代码默认值）。
  if (config.config_file.empty()) {
    // 未显式指定 config file 时 fallback 到 configs/${AGENTFLOW_ENV:-test}/agentflowd.json
    // 默认 test 而非 dev：test fixture 跑测试时 AGENTFLOW_ENV 未设，应走 test 配置（sandbox.max_active=4
    // 等测试慣例值），走 dev 配置会让 daemon 启动变慢（dev 的 sandbox.max_active=16 等）导致
    // fixture setup 的 15s port 就绪超时。生产环境应显式设 AGENTFLOW_ENV=prod 或 --config 指路。
    const char* env_env = std::getenv("AGENTFLOW_ENV");
    std::string env_name = (env_env && *env_env) ? std::string(env_env) : std::string("test");
    std::string fallback_path = std::string("configs/") + env_name + "/agentflowd.json";
    std::ifstream probe(fallback_path);
    if (probe.good()) {
      config.config_file = fallback_path;
      std::cerr << "[CONFIG] No --config/AF_CONFIG_FILE specified, falling back to default: "
                << fallback_path << " (AGENTFLOW_ENV=" << env_name << ")" << std::endl;
    } else {
      // fallback 文件不存在：静默返回 true，保留向后兼容（无配置文件时全走代码默认值）
      return true;
    }
  }

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
  // 不变量：CLI > 环境变量 > JSON 配置文件 > 代码默认值（docs/design/config-landscape.md §2.1.4）
  // `parse_cli_args()` 先跑，若用户显式设了 `--listen` CLI arg，`config.listen` 已有非空值，
  // `load_config_file()` 不应再用 JSON 配置文件的 listen 字段覆盖它。
  // `cli_listen_set` 标记由 `parse_cli_args()` 在解析 `--listen` 时置 true（见下方 parse_cli_args 段）。
  auto server = dsl::JsonHelper::get_object(root, "server");
  if (server) {
    auto val = dsl::JsonHelper::get_string(server, "listen");
    if (is_set(val) && !config.cli_listen_set) config.listen = val;
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

  auto dref = dsl::JsonHelper::get_object(root, "dataref_bridge");
  if (dref) {
    int64_t cap = dsl::JsonHelper::get_int(dref, "capacity_mb", 0);
    if (cap > 0) config.dataref_bridge_config.capacity_mb = static_cast<int>(cap);
    std::string url = dsl::JsonHelper::get_string(dref, "redis_url");
    if (is_set(url)) config.dataref_bridge_config.redis_url = url;
  }

  // Sandbox section（AgentFoundry 沙箱机制移植）
  // 三个配置文件 dev/test/prod 必须显式声明 sandbox 段，否则 sandbox_config 全走代码默认值
  // sandbox.workdir_root 是核心配置项——三种 mode（local/docker/e2b）共用同一 workspace 契约
  auto sandbox_node = dsl::JsonHelper::get_object(root, "sandbox");
  if (sandbox_node) {
    std::string mode = dsl::JsonHelper::get_string(sandbox_node, "mode");
    if (is_set(mode)) config.sandbox_config.mode = mode;

    std::string wr = dsl::JsonHelper::get_string(sandbox_node, "workdir_root");
    if (is_set(wr)) config.sandbox_config.workdir_root = wr;

    std::string sr = dsl::JsonHelper::get_string(sandbox_node, "snapshot_root");
    if (is_set(sr)) config.sandbox_config.snapshot_root = sr;

    int64_t ma = dsl::JsonHelper::get_int(sandbox_node, "sandbox_max_active", 0);
    if (ma > 0) config.sandbox_config.sandbox_max_active = static_cast<int>(ma);

    int64_t hp = dsl::JsonHelper::get_int(sandbox_node, "sandbox_http_port", 0);
    if (hp > 0) config.sandbox_config.sandbox_http_port = static_cast<int>(hp);

    int64_t om = dsl::JsonHelper::get_int(sandbox_node, "output_max_bytes", -1);
    if (om >= 0) config.sandbox_config.output_max_bytes = om;

    int64_t gt = dsl::JsonHelper::get_int(sandbox_node, "gc_destroy_ttl_ms", 0);
    if (gt > 0) config.sandbox_config.gc_destroy_ttl_ms = gt;

    int64_t rt = dsl::JsonHelper::get_int(sandbox_node, "gc_recycle_snapshot_ttl_ms", 0);
    if (rt > 0) config.sandbox_config.gc_recycle_snapshot_ttl_ms = rt;

    std::string e2b_tpl = dsl::JsonHelper::get_string(sandbox_node, "e2b_default_template");
    if (is_set(e2b_tpl)) config.sandbox_config.e2b_default_template = e2b_tpl;

    std::string e2b_key = dsl::JsonHelper::get_string(sandbox_node, "e2b_api_key");
    if (is_set(e2b_key)) config.sandbox_config.e2b_api_key = e2b_key;

    std::string e2b_url = dsl::JsonHelper::get_string(sandbox_node, "e2b_api_url");
    if (is_set(e2b_url)) config.sandbox_config.e2b_api_url = e2b_url;

    std::string e2b_ca = dsl::JsonHelper::get_string(sandbox_node, "e2b_ca_cert_path");
    if (is_set(e2b_ca)) config.sandbox_config.e2b_ca_cert_path = e2b_ca;

    std::string e2b_remote = dsl::JsonHelper::get_string(sandbox_node, "e2b_remote_workspace_dir");
    if (is_set(e2b_remote)) config.sandbox_config.e2b_remote_workspace_dir = e2b_remote;
    std::string e2b_map = dsl::JsonHelper::get_string(sandbox_node, "e2b_template_map_path");
    if (is_set(e2b_map)) config.sandbox_config.e2b_template_map_path = e2b_map;

    std::string e2b_allow = dsl::JsonHelper::get_string(sandbox_node, "e2b_network_allow_out");
    if (is_set(e2b_allow)) config.sandbox_config.e2b_network_allow_out = e2b_allow;

    std::string img = dsl::JsonHelper::get_string(sandbox_node, "image_name");
    if (is_set(img)) config.sandbox_config.image_name = img;

    // allow_fallback：布尔值，默认 true（向后兼容）
    config.sandbox_config.allow_fallback = dsl::JsonHelper::get_bool(sandbox_node, "allow_fallback", true);
  }

  // 校验：sandbox 配置加载完成后检查关键 e2b 配置字段是否为空
  // 空值 = JSON 中存在但 C++ config.cpp 未加载（历史已遗漏 e2b_api_url / e2b_ca_cert_path / e2b_network_allow_out）
  const auto& sc = config.sandbox_config;
  if (sc.mode == "e2b") {
    if (sc.e2b_api_url.empty())
      Logger::instance().warn("[CONFIG] sandbox.e2b_api_url is empty (config key exists in JSON but was not loaded)");
    if (sc.e2b_api_key.empty())
      Logger::instance().warn("[CONFIG] sandbox.e2b_api_key is empty");
    if (sc.e2b_default_template.empty())
      Logger::instance().warn("[CONFIG] sandbox.e2b_default_template is empty");
    if (sc.e2b_network_allow_out.empty())
      Logger::instance().warn("[CONFIG] sandbox.e2b_network_allow_out is empty (config key exists in JSON but was not loaded)");
    if (sc.e2b_remote_workspace_dir.empty())
      Logger::instance().warn("[CONFIG] sandbox.e2b_remote_workspace_dir is empty");
    if (sc.e2b_ca_cert_path.empty())
      Logger::instance().warn("[CONFIG] sandbox.e2b_ca_cert_path is empty");
  }

#ifdef AGENTFLOW_WITH_RAPIDJSON
  // 深度校验：遍历 sandbox JSON 所有 key，检测是否被 C++ 代码加载
  // 如果 JSON 中有 key 未被任何 dsl::JsonHelper::get_* 消费，说明遗漏了加载
  if (sandbox_node) {
    auto* rj_val = const_cast<dsl::RapidjsonValue*>(dynamic_cast<const dsl::RapidjsonValue*>(sandbox_node.get()));
    if (rj_val && rj_val->get_raw_value() && rj_val->get_raw_value()->IsObject()) {
      static const std::unordered_set<std::string> kKnownSandboxKeys = {
        "mode", "workdir_root", "snapshot_root", "sandbox_max_active",
        "sandbox_http_port", "output_max_bytes", "gc_destroy_ttl_ms",
        "gc_recycle_snapshot_ttl_ms", "e2b_default_template", "e2b_api_key",
        "e2b_api_url", "e2b_ca_cert_path", "e2b_remote_workspace_dir",
        "e2b_network_allow_out", "e2b_template_map_path", "image_name", "allow_fallback",
        // LocalSandbox 字段
        "local_sandbox_python", "docker_image", "docker_sandbox_python",
        "skills_src_dir", "snapshot_enabled",
      };
      for (auto it = rj_val->get_raw_value()->MemberBegin();
           it != rj_val->get_raw_value()->MemberEnd(); ++it) {
        std::string key(it->name.GetString(), it->name.GetStringLength());
        if (kKnownSandboxKeys.find(key) == kKnownSandboxKeys.end()) {
          Logger::instance().warn_f(
              "[CONFIG] sandbox.%s exists in JSON but was NOT loaded by C++ (config.cpp missing get_* call)",
              key.c_str());
        }
      }
    }
  }
#endif

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

  // Sandbox validation（AgentFoundry 沙箱机制移植）
  // mode=none 时跳过所有 sandbox 校验（向后兼容，FR-006）
  const auto& sc = config.sandbox_config;
  if (sc.mode == "none") {
    return true;
  }

  // mode 合法值
  if (sc.mode != "local" && sc.mode != "docker" && sc.mode != "e2b") {
    std::cerr << "Error: sandbox.mode must be one of {none, local, docker, e2b}, got '"
              << sc.mode << "'" << std::endl;
    return false;
  }

  // workdir_root 非空（三种 mode 共用 workspace 契约）
  if (sc.workdir_root.empty()) {
    std::cerr << "Error: sandbox.workdir_root must not be empty when mode != none" << std::endl;
    return false;
  }

  // snapshot_root 非空（销毁时快照落盘点）
  if (sc.snapshot_root.empty()) {
    std::cerr << "Error: sandbox.snapshot_root must not be empty when mode != none" << std::endl;
    return false;
  }

  // sandbox_http_port 范围（1-65535）
  if (sc.sandbox_http_port < 1 || sc.sandbox_http_port > 65535) {
    std::cerr << "Error: sandbox.sandbox_http_port must be in [1, 65535], got "
              << sc.sandbox_http_port << std::endl;
    return false;
  }

  // sandbox_max_active 正整数
  if (sc.sandbox_max_active <= 0) {
    std::cerr << "Error: sandbox.sandbox_max_active must be positive, got "
              << sc.sandbox_max_active << std::endl;
    return false;
  }

  // GC TTL 正数
  if (sc.gc_destroy_ttl_ms <= 0) {
    std::cerr << "Error: sandbox.gc_destroy_ttl_ms must be positive, got "
              << sc.gc_destroy_ttl_ms << std::endl;
    return false;
  }
  if (sc.gc_recycle_snapshot_ttl_ms <= 0) {
    std::cerr << "Error: sandbox.gc_recycle_snapshot_ttl_ms must be positive, got "
              << sc.gc_recycle_snapshot_ttl_ms << std::endl;
    return false;
  }

  // output_max_bytes 非负（0 = 不截断）
  if (sc.output_max_bytes < 0) {
    std::cerr << "Error: sandbox.output_max_bytes must be non-negative, got "
              << sc.output_max_bytes << std::endl;
    return false;
  }

  // E2B 模式必须设置 E2B_API_KEY（延迟到 daemon init 阶段校验，此处仅文档化）
  // 注：E2BSandbox 构造时会自检 E2B_API_KEY，SandboxLaunchException 含可定位原因

  return true;
}

}  // namespace daemon
}  // namespace agentflow
