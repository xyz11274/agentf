#include "runtime/worker.h"

#include <sys/time.h>

#include <algorithm>
#include <cctype>
#include <sstream>
#include <type_traits>

#include "utils/logger.h"

namespace agentflow {
namespace runtime {

MMWorkerEngine::ExecutionResponse MMWorkerEngine::execute_code(const ExecutionRequest& request) {
  ExecutionResponse response;
  if(request.command.empty()) {
    response.error_message = "No command specified for execution";
    return response;
  }

  runtime::ExecutionConfig exec_config;
  exec_config.inputs      = request.inputs;
  exec_config.working_dir = request.working_dir;
  exec_config.timeout_ms  = request.timeout_ms > 0 ? request.timeout_ms : 30000;
  auto to_env_suffix = [](const std::string& key) {
    std::string out;
    out.reserve(key.size());
    for(char ch : key) {
      unsigned char uch = static_cast<unsigned char>(ch);
      if(std::isalnum(uch)) {
        out.push_back(static_cast<char>(std::toupper(uch)));
      } else {
        out.push_back('_');
      }
    }
    return out;
  };
  auto variant_to_string = [](const DataVariant& value) -> std::string {
    return std::visit(
        [](const auto& arg) -> std::string {
          using T = std::decay_t<decltype(arg)>;
          if constexpr(std::is_same_v<T, std::string>) {
            return arg;
          } else if constexpr(std::is_same_v<T, bool>) {
            return arg ? "true" : "false";
          } else if constexpr(std::is_arithmetic_v<T>) {
            std::ostringstream oss;
            oss << arg;
            return oss.str();
          }
          return "";
        },
        value);
  };
  for(const auto& pair : request.inputs) {
    std::string env_key = "AF_" + to_env_suffix(pair.first);
    exec_config.env_vars.push_back(env_key + "=" + variant_to_string(pair.second));
  }

  auto& code_loader = runtime::global_code_loader();
  if(request.enable_hot_reload) {
    code_loader.set_hot_reload_config(request.hot_reload_config);
    if(!code_loader.is_watching(request.command)) {
      code_loader.start_watching(request.command);
    }
    code_loader.reload_code(request.command);
  }

  runtime::ExecutionResult result;
  switch(request.mode) {
    case ExecutionMode::SHELL_SCRIPT:
      result = code_loader.execute_shell_script(request.command, request.args, exec_config);
      break;
    case ExecutionMode::EXECUTABLE:
      result = code_loader.execute_shell_script(request.command, request.args, exec_config);
      break;
    case ExecutionMode::PYTHON_SCRIPT:
      result = code_loader.execute_python_script(request.command, request.args, exec_config);
      break;
    case ExecutionMode::GO_SCRIPT:
      result = code_loader.execute_go_script(request.command, request.args, exec_config);
      break;
    case ExecutionMode::JAVA_JAR:
      result = code_loader.execute_jar(request.command, request.main_class, request.args, exec_config);
      break;
    case ExecutionMode::SO_LIBRARY:
      result = code_loader.load_and_execute_so(
          request.command, request.function_name.empty() ? "execute" : request.function_name,
          exec_config);
      break;
    case ExecutionMode::CPP_FUNCTION:
    default:
      result = code_loader.execute(request.command, exec_config);
      break;
  }

  response.success        = result.success;
  response.stdout_content = result.stdout;
  response.stderr_content = result.stderr;
  response.error_message  = result.error_message;
  response.exit_code      = result.exit_code;
  if(!result.stdout.empty()) {
    response.outputs["stdout"] = result.stdout;
  }
  if(!result.stderr.empty()) {
    response.outputs["stderr"] = result.stderr;
  }
  response.outputs["exit_code"] = std::to_string(result.exit_code);

  if(request.mode == ExecutionMode::SO_LIBRARY && result.success && !result.stdout.empty()) {
    std::istringstream iss(result.stdout);
    std::string        line;
    while(std::getline(iss, line)) {
      const size_t pos = line.find('=');
      if(pos == std::string::npos) {
        continue;
      }
      response.outputs[line.substr(0, pos)] = line.substr(pos + 1);
    }
  }
  if(request.enable_hot_reload) {
    auto version = code_loader.get_current_version(request.command);
    if(version) {
      response.active_version_id = version->version_id;
    }
  }
  return response;
}

bool MMWorkerEngine::check_and_reload_code(const std::string& command, const HotReloadConfig& config) {
  if(command.empty()) {
    return false;
  }

  auto& code_loader = runtime::global_code_loader();
  code_loader.set_hot_reload_config(config);
  if(!code_loader.is_watching(command)) {
    code_loader.start_watching(command);
  }

  auto current_before = code_loader.get_current_version(command);
  const std::string before_id = current_before ? current_before->version_id : "";
  auto reload_result = code_loader.reload_code(command);
  if(!reload_result.success) {
    return false;
  }

  auto current_after = code_loader.get_current_version(command);
  return current_after && current_after->version_id != before_id;
}

std::shared_ptr<ExecutableVersion> MMWorkerEngine::get_code_version(const std::string& command) {
  if(command.empty()) {
    return nullptr;
  }
  auto& code_loader = runtime::global_code_loader();
  return code_loader.get_current_version(command);
}

bool MMWorkerEngine::rollback_code_version(const std::string& command, const std::string& version_id) {
  if(command.empty() || version_id.empty()) {
    return false;
  }
  auto& code_loader = runtime::global_code_loader();
  return code_loader.rollback_to_version(command, version_id);
}

bool MMWorkerEngine::stop_hot_reload(const std::string& command) {
  if(command.empty()) {
    return false;
  }
  auto& code_loader = runtime::global_code_loader();
  return code_loader.stop_watching(command);
}

int64_t MMWorkerEngine::now_ms() {
  timeval tv;
  gettimeofday(&tv, nullptr);
  return static_cast<int64_t>(tv.tv_sec) * 1000 + tv.tv_usec / 1000;
}

MMWorkerEngine::MMWorkerEngine(const Config& config) : config_(config) {
  last_heartbeat_ms_ = now_ms();
}

MMWorkerEngine::~MMWorkerEngine() {
  stop();
}

MMWorkerEngine::WorkerStatus MMWorkerEngine::get_status() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return status_;
}

int MMWorkerEngine::get_load() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return load_;
}

int64_t MMWorkerEngine::get_last_heartbeat_ms() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return last_heartbeat_ms_;
}

void MMWorkerEngine::set_status(WorkerStatus status) {
  std::lock_guard<std::mutex> lock(mutex_);
  status_ = status;
}

void MMWorkerEngine::set_load(int load) {
  std::lock_guard<std::mutex> lock(mutex_);
  load_ = load < 0 ? 0 : load;
}

void MMWorkerEngine::heartbeat() {
  std::lock_guard<std::mutex> lock(mutex_);
  last_heartbeat_ms_ = now_ms();
}

void MMWorkerEngine::set_supported_workflows(const std::set<std::string>& workflow_ids) {
  std::lock_guard<std::mutex> lock(mutex_);
  supported_workflows_ = workflow_ids;
}

void MMWorkerEngine::add_supported_workflow(const std::string& workflow_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  supported_workflows_.insert(workflow_id);
}

void MMWorkerEngine::remove_supported_workflow(const std::string& workflow_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  supported_workflows_.erase(workflow_id);
}

bool MMWorkerEngine::supports_workflow(const std::string& workflow_id) const {
  std::lock_guard<std::mutex> lock(mutex_);
  return supported_workflows_.empty() || supported_workflows_.count(workflow_id) > 0;
}

std::set<std::string> MMWorkerEngine::get_supported_workflows() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return supported_workflows_;
}

bool MMWorkerEngine::start() {
  bool expected = false;
  if(!running_.compare_exchange_strong(expected, true)) {
    return false;
  }

  set_status(WorkerStatus::RUNNING);
  const int event_threads = std::max(1, config_.event_threads);
  event_pool_             = std::make_unique<basic::ThreadPool>(static_cast<size_t>(event_threads));
  event_monitor_thread_   = std::thread(&MMWorkerEngine::event_monitor_loop, this);
  return true;
}

void MMWorkerEngine::stop() {
  bool expected = true;
  if(!running_.compare_exchange_strong(expected, false)) {
    return;
  }

  queue_cv_.notify_all();
  if(event_monitor_thread_.joinable()) {
    event_monitor_thread_.join();
  }

  {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    while(!event_queue_.empty()) {
      event_queue_.pop();
    }
    workflow_registrations_ = std::queue<WorkflowRegistration>();
    high_priority_tasks_.clear();
    normal_priority_tasks_.clear();
  }

  workflow_pools_.clear();
  event_pool_.reset();
  set_status(WorkerStatus::STOPPED);
}

bool MMWorkerEngine::enqueue_task(const TaskItem& task) {
  std::lock_guard<std::mutex> lock(queue_mutex_);
  const size_t queued_tasks = high_priority_tasks_.size() + normal_priority_tasks_.size();
  if(queued_tasks >= config_.max_queue_size) {
    return false;
  }

  if(task.priority == TaskPriority::HIGH) {
    high_priority_tasks_.push_back(task);
  } else {
    normal_priority_tasks_.push_back(task);
  }

  WorkerEvent event;
  event.type          = "task_arrived";
  event.created_at_ms = now_ms();
  event.data["task_id"] = task.task_id;
  event.data["workflow_id"] = task.workflow_id;
  event_queue_.push(event);
  queue_cv_.notify_all();
  return true;
}

bool MMWorkerEngine::register_workflow_instance(const WorkflowRegistration& registration) {
  std::lock_guard<std::mutex> lock(queue_mutex_);
  if(workflow_registrations_.size() >= config_.max_queue_size) {
    return false;
  }

  workflow_registrations_.push(registration);
  WorkerEvent event;
  event.type                           = "workflow_registered";
  event.created_at_ms                  = now_ms();
  event.data["workflow_id"]            = registration.workflow_id;
  event.data["workflow_instance_id"]   = registration.workflow_instance_id;
  event.data["thread_count"]           = std::to_string(registration.thread_count);
  event_queue_.push(event);
  queue_cv_.notify_all();
  return true;
}

bool MMWorkerEngine::unregister_workflow_instance(const std::string& workflow_instance_id) {
  std::lock_guard<std::mutex> lock(queue_mutex_);
  WorkerEvent event;
  event.type                         = "workflow_unregistered";
  event.created_at_ms                = now_ms();
  event.data["workflow_instance_id"] = workflow_instance_id;
  event_queue_.push(event);
  queue_cv_.notify_all();
  return true;
}

bool MMWorkerEngine::enqueue_event(const WorkerEvent& event) {
  std::lock_guard<std::mutex> lock(queue_mutex_);
  if(event_queue_.size() >= config_.max_queue_size) {
    return false;
  }
  event_queue_.push(event);
  queue_cv_.notify_all();
  return true;
}

void MMWorkerEngine::register_event_handler(const std::string& event_type, WorkerEventHandler handler) {
  std::lock_guard<std::mutex> lock(handler_mutex_);
  event_handlers_[event_type].push_back(std::move(handler));
}

std::string MMWorkerEngine::workflow_pool_key(const TaskItem& task) const {
  if(!task.workflow_instance_id.empty()) {
    return task.workflow_instance_id;
  }
  return task.workflow_id;
}

void MMWorkerEngine::ensure_workflow_pool(const std::string& pool_key, int thread_count) {
  const int effective_threads = std::max(1, thread_count <= 0 ? config_.default_workflow_threads : thread_count);
  auto      it                = workflow_pools_.find(pool_key);
  if(it == workflow_pools_.end()) {
    workflow_pools_[pool_key] =
        std::make_unique<basic::ThreadPool>(static_cast<size_t>(effective_threads));
  }
}

void MMWorkerEngine::handle_internal_event(const WorkerEvent& event) {
  if(event.type == "task_arrived") {
    process_task_queues();
    return;
  }

  if(event.type == "workflow_registered") {
    const std::string& workflow_id = event.data.count("workflow_instance_id") > 0
                                         ? event.data.at("workflow_instance_id")
                                         : event.data.at("workflow_id");
    int thread_count = config_.default_workflow_threads;
    auto thread_it = event.data.find("thread_count");
    if(thread_it != event.data.end()) {
      thread_count = std::stoi(thread_it->second);
    }
    ensure_workflow_pool(workflow_id, thread_count);
    return;
  }

  if(event.type == "workflow_unregistered") {
    auto it = event.data.find("workflow_instance_id");
    if(it != event.data.end()) {
      workflow_pools_.erase(it->second);
    }
  }
}

void MMWorkerEngine::process_task_queues() {
  std::vector<TaskItem> tasks_to_run;
  {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    while(!high_priority_tasks_.empty()) {
      tasks_to_run.push_back(high_priority_tasks_.front());
      high_priority_tasks_.pop_front();
    }
    while(!normal_priority_tasks_.empty()) {
      tasks_to_run.push_back(normal_priority_tasks_.front());
      normal_priority_tasks_.pop_front();
    }
  }

  for(const auto& task : tasks_to_run) {
    const std::string key = workflow_pool_key(task);
    if(key.empty()) {
      continue;
    }

    ensure_workflow_pool(key, config_.default_workflow_threads);
    auto pool_it = workflow_pools_.find(key);
    if(pool_it == workflow_pools_.end()) {
      continue;
    }

    auto* pool = pool_it->second.get();
    pool->submit([this, task]() {
      set_load(get_load() + 1);
      heartbeat();
      Logger::instance().debug_f("[WORKER_TASK] worker=%s workflow=%s task=%s",
                                 get_worker_id().c_str(), task.workflow_id.c_str(),
                                 task.task_id.c_str());
      set_load(get_load() - 1);
    });
  }
}

void MMWorkerEngine::event_monitor_loop() {
  while(running_) {
    WorkerEvent event;
    {
      std::unique_lock<std::mutex> lock(queue_mutex_);
      queue_cv_.wait(lock, [this]() { return !event_queue_.empty() || !running_; });
      if(!running_) {
        break;
      }
      event = event_queue_.front();
      event_queue_.pop();
    }

    handle_internal_event(event);

    std::vector<WorkerEventHandler> handlers;
    {
      std::lock_guard<std::mutex> lock(handler_mutex_);
      auto                        it = event_handlers_.find(event.type);
      if(it != event_handlers_.end()) {
        handlers = it->second;
      }
    }

    for(const auto& handler : handlers) {
      if(event_pool_) {
        event_pool_->submit([handler, event]() { handler(event); });
      } else {
        handler(event);
      }
    }
  }
}

}  // namespace runtime
}  // namespace agentflow
