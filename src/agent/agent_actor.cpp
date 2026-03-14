// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License

#include "agent/agent_actor.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <algorithm>

#ifdef AGENTFLOW_WITH_RAPIDJSON
#include <rapidjson/document.h>
#endif

#include "agent/task.h"
#include "utils/logger.h"

namespace agentflow {
namespace agent {

namespace {
int64_t file_mtime_ms(const std::string& path) {
  namespace fs = std::filesystem;
  if(!fs::exists(path)) {
    return 0;
  }
  auto ftime = fs::last_write_time(path);
  auto sctp = std::chrono::time_point_cast<std::chrono::milliseconds>(ftime).time_since_epoch();
  return static_cast<int64_t>(sctp.count());
}
}  // namespace

MMAgentActor::MMAgentActor(std::shared_ptr<framework::MMAgent> agent)
    : config_path_("AgentService.config"), runtime_actor_(nullptr) {
  if(agent) {
    ManagedAgent managed;
    managed.instance_id = agent->get_id() + ":" + agent->get_config().version;
    managed.package_id = agent->get_id();
    managed.version = agent->get_config().version;
    managed.package_path = "";
    managed.agent = std::move(agent);
    managed_agents_[managed.instance_id] = managed;
  }
}

MMAgentActor::MMAgentActor(const std::string& config_path)
    : config_path_(config_path.empty() ? "AgentService.config" : config_path), runtime_actor_(nullptr) {
}

MMAgentActor::MMAgentActor(const std::string& config_path,
                       std::shared_ptr<runtime::MMRuntimeActor> runtime_actor)
    : config_path_(config_path.empty() ? "AgentService.config" : config_path),
      runtime_actor_(std::move(runtime_actor)) {
}

MMAgentActor::~MMAgentActor() {
  on_stop();
}

bool MMAgentActor::parse_service_config() {
#ifdef AGENTFLOW_WITH_RAPIDJSON
  namespace fs = std::filesystem;
  if(!fs::exists(config_path_)) {
    return false;
  }
  std::ifstream input(config_path_);
  if(!input.good()) {
    return false;
  }
  std::string content((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
  rapidjson::Document doc;
  doc.Parse(content.c_str());
  if(doc.HasParseError() || !doc.IsObject()) {
    return false;
  }

  ServiceConfig parsed;
  if(doc.HasMember("package_root") && doc["package_root"].IsString()) {
    parsed.package_root = doc["package_root"].GetString();
  }
  if(doc.HasMember("scan_interval_ms") && doc["scan_interval_ms"].IsInt()) {
    parsed.scan_interval_ms = doc["scan_interval_ms"].GetInt();
  }
  if(doc.HasMember("heartbeat_interval_ms") && doc["heartbeat_interval_ms"].IsInt()) {
    parsed.heartbeat_interval_ms = doc["heartbeat_interval_ms"].GetInt();
  }
  if(doc.HasMember("manifests") && doc["manifests"].IsArray()) {
    for(auto& v : doc["manifests"].GetArray()) {
      if(v.IsString()) {
        parsed.manifests.push_back(v.GetString());
      }
    }
  }

  std::lock_guard<std::mutex> lock(mutex_);
  service_config_ = parsed;
  return true;
#else
  return false;
#endif
}

bool MMAgentActor::should_reload(const std::string& package_path, int64_t& out_mtime_ms) const {
  out_mtime_ms = file_mtime_ms(package_path + "/package.json");
  if(out_mtime_ms == 0) {
    return false;
  }
  for(const auto& kv : managed_agents_) {
    if(kv.second.package_path == package_path && kv.second.last_mtime_ms == out_mtime_ms) {
      return false;
    }
  }
  return true;
}

bool MMAgentActor::create_agent_from_package(const std::string& package_path) {
  try {
    int64_t mtime_ms = 0;
    if(!should_reload(package_path, mtime_ms)) {
      return false;
    }

    auto agent = framework::MMAgent::create_from_package(package_path);
    if(!agent) {
      Logger::instance().error("Failed to create agent from package: " + package_path);
      return false;
    }

    if(!agent->initialize()) {
      Logger::instance().error("Failed to initialize agent: " + package_path);
      return false;
    }

    if(!agent->start()) {
      Logger::instance().error("Failed to start agent: " + package_path);
      return false;
    }

    ManagedAgent managed;
    managed.package_id = agent->get_id();
    managed.version = agent->get_config().version;
    managed.package_path = package_path;
    managed.instance_id =
        managed.package_id + ":" + managed.version + ":" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    managed.agent = agent;
    managed.last_mtime_ms = mtime_ms;

    {
      std::lock_guard<std::mutex> lock(mutex_);
      managed_agents_[managed.instance_id] = managed;
    }
    report_agent_registered(managed);
    report_agent_status(managed);
    return true;
  } catch(const std::exception& e) {
    Logger::instance().error("Exception in create_agent_from_package: " + std::string(e.what()));
    return false;
  } catch(...) {
    Logger::instance().error("Unknown exception in create_agent_from_package");
    return false;
  }
}

void MMAgentActor::load_agents_from_config() {
  ServiceConfig snapshot;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    snapshot = service_config_;
  }
  namespace fs = std::filesystem;
  for(const auto& entry : snapshot.manifests) {
    fs::path p(entry);
    if(p.is_relative()) {
      p = fs::path(snapshot.package_root) / p;
    }
    create_agent_from_package(p.string());
  }
}

void MMAgentActor::report_agent_registered(const ManagedAgent& managed) {
  if(!runtime_actor_) {
    return;
  }
  runtime::AgentRuntimeReport report;
  report.agent_instance_id = managed.instance_id;
  report.state = managed.agent ? managed.agent->get_state() : AgentState::CREATED;
  report.agent = managed.agent;
  actor::adapter::ActorMessage msg;
  msg.message_type = "agent.register";
  msg.payload = std::make_shared<runtime::AgentRuntimeReport>(report);
  runtime_actor_->handle_message(msg);
}

void MMAgentActor::report_agent_heartbeat(const ManagedAgent& managed) {
  if(!runtime_actor_) {
    return;
  }
  runtime::AgentRuntimeReport report;
  report.agent_instance_id = managed.instance_id;
  report.state = managed.agent ? managed.agent->get_state() : AgentState::CREATED;
  actor::adapter::ActorMessage msg;
  msg.message_type = "agent.heartbeat";
  msg.payload = std::make_shared<runtime::AgentRuntimeReport>(report);
  runtime_actor_->handle_message(msg);
}

void MMAgentActor::report_agent_status(const ManagedAgent& managed) {
  if(!runtime_actor_) {
    return;
  }
  runtime::AgentRuntimeReport report;
  report.agent_instance_id = managed.instance_id;
  report.state = managed.agent ? managed.agent->get_state() : AgentState::CREATED;
  actor::adapter::ActorMessage msg;
  msg.message_type = "agent.status";
  msg.payload = std::make_shared<runtime::AgentRuntimeReport>(report);
  runtime_actor_->handle_message(msg);
}

void MMAgentActor::report_task_status(const std::string& task_id, const std::string& status, int progress) {
  if(!runtime_actor_) {
    return;
  }
  runtime::TaskRuntimeReport report;
  report.task_id = task_id;
  report.status = status;
  report.progress = progress;
  actor::adapter::ActorMessage msg;
  msg.message_type = (status == "scheduled") ? "task.register" : "task.status";
  msg.payload = std::make_shared<runtime::TaskRuntimeReport>(report);
  runtime_actor_->handle_message(msg);
}

void MMAgentActor::collect_agent_tasks_and_report() {
  std::vector<ManagedAgent> snapshot;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    for(const auto& kv : managed_agents_) {
      snapshot.push_back(kv.second);
    }
  }

  for(const auto& managed : snapshot) {
    if(!managed.agent) {
      continue;
    }
    std::shared_ptr<framework::MMAgentTask> task;
    while(managed.agent->pop_generated_task(task)) {
      if(!task) {
        continue;
      }
      {
        std::lock_guard<std::mutex> lock(mutex_);
        managed_tasks_[task->get_task_id()] = task;
      }
      report_task_status(task->get_task_id(), "scheduled", 0);
    }
  }
}

void MMAgentActor::monitor_packages_loop() {
  while(running_) {
    parse_service_config();
    load_agents_from_config();
    collect_agent_tasks_and_report();
    std::this_thread::sleep_for(std::chrono::milliseconds(std::max(100, service_config_.scan_interval_ms)));
  }
}

void MMAgentActor::heartbeat_loop() {
  while(running_) {
    std::vector<ManagedAgent> snapshot;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      for(const auto& kv : managed_agents_) {
        snapshot.push_back(kv.second);
      }
    }
    for(const auto& managed : snapshot) {
      report_agent_heartbeat(managed);
      report_agent_status(managed);
    }
    std::this_thread::sleep_for(
        std::chrono::milliseconds(std::max(100, service_config_.heartbeat_interval_ms)));
  }
}

void MMAgentActor::apply_agent_control(const std::string& action, const std::string& instance_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  for(auto& kv : managed_agents_) {
    if(!instance_id.empty() && kv.first != instance_id) {
      continue;
    }
    if(!kv.second.agent) {
      continue;
    }
    if(action == "start") {
      kv.second.agent->start();
    } else if(action == "pause") {
      kv.second.agent->pause();
    } else if(action == "resume") {
      kv.second.agent->resume();
    } else if(action == "stop") {
      kv.second.agent->stop();
    }
    report_agent_status(kv.second);
  }
}

void MMAgentActor::apply_task_control(const std::string& action, const std::string& task_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = managed_tasks_.find(task_id);
  if(it == managed_tasks_.end() || !it->second) {
    return;
  }
  if(action == "start") {
    it->second->start();
    report_task_status(task_id, "running", static_cast<int>(it->second->get_progress() * 100));
  } else if(action == "pause" || action == "stop") {
    it->second->cancel();
    report_task_status(task_id, "cancelled", static_cast<int>(it->second->get_progress() * 100));
  }
}

void MMAgentActor::on_start() {
  if(running_.exchange(true)) {
    return;
  }
  parse_service_config();
  load_agents_from_config();
  package_monitor_thread_ = std::thread(&MMAgentActor::monitor_packages_loop, this);
  heartbeat_thread_ = std::thread(&MMAgentActor::heartbeat_loop, this);
}

void MMAgentActor::on_stop() {
  if(!running_.exchange(false)) {
    return;
  }
  if(package_monitor_thread_.joinable()) {
    package_monitor_thread_.join();
  }
  if(heartbeat_thread_.joinable()) {
    heartbeat_thread_.join();
  }
}

void MMAgentActor::on_message(std::shared_ptr<actor::adapter::IContext> ctx) {
  if(!ctx) {
    return;
  }
  auto msg = ctx->current_message();
  if(msg.message_type == "agent.start") {
    auto id = msg.get_payload<std::string>();
    apply_agent_control("start", id ? *id : "");
    return;
  }
  if(msg.message_type == "agent.pause") {
    auto id = msg.get_payload<std::string>();
    apply_agent_control("pause", id ? *id : "");
    return;
  }
  if(msg.message_type == "agent.resume") {
    auto id = msg.get_payload<std::string>();
    apply_agent_control("resume", id ? *id : "");
    return;
  }
  if(msg.message_type == "agent.stop") {
    auto id = msg.get_payload<std::string>();
    apply_agent_control("stop", id ? *id : "");
    return;
  }
  if(msg.message_type == "task.start") {
    auto id = msg.get_payload<std::string>();
    if(id) {
      apply_task_control("start", *id);
    }
    return;
  }
  if(msg.message_type == "task.pause") {
    auto id = msg.get_payload<std::string>();
    if(id) {
      apply_task_control("pause", *id);
    }
    return;
  }
  if(msg.message_type == "task.stop") {
    auto id = msg.get_payload<std::string>();
    if(id) {
      apply_task_control("stop", *id);
    }
    return;
  }
  if(msg.message_type == "request.message") {
    auto payload = msg.get_payload<framework::MMAgentMsgQue::Message>();
    if(!payload) {
      return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    for(auto& kv : managed_agents_) {
      if(kv.second.agent) {
        kv.second.agent->route_request_message(*payload);
      }
    }
    return;
  }
}

}  // namespace agent
}  // namespace agentflow
