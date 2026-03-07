#include "runtime/worker.h"

#include <sys/time.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <map>
#include <sstream>
#include <thread>
#include <type_traits>

#include "runtime/databus/file_databus_bridge.h"
#include "runtime/llm/streaming_client.h"
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

  FileDataBusBridge file_databus(
      request.databus_root_dir.empty() ? FileDataBusBridge::make_temp_root() : request.databus_root_dir);
  if(file_databus.initialize()) {
    exec_config.env_vars.push_back("AF_DATABUS_DIR=" + file_databus.root_dir().string());
    exec_config.env_vars.push_back("AF_DATABUS_KV_DIR=" + file_databus.kv_dir().string());
    exec_config.env_vars.push_back("AF_DATABUS_EVENTS_DIR=" + file_databus.events_dir().string());
    if(!request.llm_config.base_url.empty()) {
      exec_config.env_vars.push_back("AF_LLM_BASE_URL=" + request.llm_config.base_url);
    }
    if(!request.llm_config.api_key.empty()) {
      exec_config.env_vars.push_back("AF_LLM_API_KEY=" + request.llm_config.api_key);
    }
    if(!request.llm_config.model.empty()) {
      exec_config.env_vars.push_back("AF_LLM_MODEL=" + request.llm_config.model);
    }
  }
  for(const auto& pair : request.inputs) {
    std::string env_key = "AF_" + to_env_suffix(pair.first);
    exec_config.env_vars.push_back(env_key + "=" + variant_to_string(pair.second));
    file_databus.put(pair.first, pair.second);
  }

  std::atomic<bool>                  monitor_running{true};
  std::set<std::string>              seen_event_files;
  std::set<std::string>              handled_llm_requests;
  std::mutex                         llm_mutex;
  std::vector<std::thread>           llm_threads;
  std::thread                        event_monitor_thread([&]() {
    while(monitor_running.load()) {
      auto events = file_databus.poll_events(seen_event_files);
      for(const auto& event : events) {
        if(event.topic != "agentflow.llm.request") {
          continue;
        }
        const auto* payload = std::get_if<std::string>(&event.value);
        if(payload == nullptr) {
          continue;
        }
        std::map<std::string, std::string> request_fields;
        if(!FileDataBusBridge::parse_scalar_json_object(*payload, request_fields)) {
          continue;
        }
        const std::string request_id =
            request_fields.count("request_id") > 0 ? request_fields["request_id"] : "";
        if(request_id.empty() || handled_llm_requests.count(request_id) > 0) {
          continue;
        }
        handled_llm_requests.insert(request_id);
        const std::string prompt =
            request_fields.count("prompt") > 0 ? request_fields["prompt"] : "";
        const std::string channel = request_fields.count("channel") > 0
                                        ? request_fields["channel"]
                                        : "agentflow.llm.chunk." + request_id;
        const std::string done_channel = request_fields.count("done_channel") > 0
                                             ? request_fields["done_channel"]
                                             : "agentflow.llm.done." + request_id;
        const std::string error_channel = request_fields.count("error_channel") > 0
                                              ? request_fields["error_channel"]
                                              : "agentflow.llm.error." + request_id;

        std::lock_guard<std::mutex> lock(llm_mutex);
        llm_threads.emplace_back([=, &file_databus, &request]() {
          StreamingLLMClient        client;
          StreamingLLMClient::Config cfg;
          cfg.base_url   = request.llm_config.base_url;
          cfg.api_key    = request.llm_config.api_key;
          cfg.model      = request.llm_config.model;
          cfg.timeout_ms = request.llm_config.timeout_ms;
          std::string final_text;
          const bool ok = client.stream_prompt(
              prompt, cfg,
              [&](const std::string& chunk) {
                final_text += chunk;
                file_databus.publish(channel, chunk);
              },
              [&](const std::string& content) {
                file_databus.put("agentflow.llm.response." + request_id, content);
                file_databus.publish(done_channel, content.empty() ? final_text : content);
              },
              [&](const std::string& error_message) {
                file_databus.put("agentflow.llm.error." + request_id, error_message);
                file_databus.publish(error_channel, error_message);
              });
          if(!ok) {
            file_databus.publish(error_channel, std::string("llm stream failed"));
          }
        });
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
  });

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

  monitor_running = false;
  if(event_monitor_thread.joinable()) {
    event_monitor_thread.join();
  }
  {
    std::lock_guard<std::mutex> lock(llm_mutex);
    for(auto& thread : llm_threads) {
      if(thread.joinable()) {
        thread.join();
      }
    }
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
  response.outputs["databus_dir"] = file_databus.root_dir().string();
  if(request.enable_hot_reload) {
    auto version = code_loader.get_current_version(request.command);
    if(version) {
      response.active_version_id = version->version_id;
    }
  }
  if(!request.keep_databus_files) {
    file_databus.cleanup();
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
  const int control_threads = std::max(1, config_.control_threads);
  control_pool_             =
      std::make_unique<basic::ThreadPool>(static_cast<size_t>(control_threads));
  control_thread_ = std::thread(&MMWorkerEngine::control_message_loop, this);
  business_thread_ = std::thread(&MMWorkerEngine::business_message_loop, this);
  return true;
}

void MMWorkerEngine::stop() {
  bool expected = true;
  if(!running_.compare_exchange_strong(expected, false)) {
    return;
  }

  queue_cv_.notify_all();
  if(control_thread_.joinable()) {
    control_thread_.join();
  }
  if(business_thread_.joinable()) {
    business_thread_.join();
  }

  {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    control_messages_.clear();
    business_messages_.clear();
  }

  workflow_pools_.clear();
  control_pool_.reset();
  set_status(WorkerStatus::STOPPED);
}

bool MMWorkerEngine::enqueue_task(const TaskItem& task) {
  BusinessMessage message;
  message.task = task;
  message.created_at_ms = now_ms();
  return enqueue_business_message(message);
}

bool MMWorkerEngine::enqueue_business_message(const BusinessMessage& message) {
  std::lock_guard<std::mutex> lock(queue_mutex_);
  const size_t queued_tasks = business_messages_.size();
  if(queued_tasks >= config_.max_queue_size) {
    return false;
  }
  if(message.task.priority == TaskPriority::HIGH) {
    business_messages_.push_front(message);
  } else {
    business_messages_.push_back(message);
  }
  queue_cv_.notify_all();
  return true;
}

bool MMWorkerEngine::register_workflow_instance(const WorkflowRegistration& registration) {
  ControlMessage message;
  message.type = "workflow_registered";
  message.created_at_ms = now_ms();
  message.data["workflow_id"] = registration.workflow_id;
  message.data["workflow_instance_id"] = registration.workflow_instance_id;
  message.data["thread_count"] = std::to_string(registration.thread_count);
  return enqueue_control_message(message);
}

bool MMWorkerEngine::unregister_workflow_instance(const std::string& workflow_instance_id) {
  ControlMessage message;
  message.type = "workflow_unregistered";
  message.created_at_ms = now_ms();
  message.data["workflow_instance_id"] = workflow_instance_id;
  return enqueue_control_message(message);
}

bool MMWorkerEngine::enqueue_control_message(const ControlMessage& message) {
  std::lock_guard<std::mutex> lock(queue_mutex_);
  if(control_messages_.size() >= config_.max_queue_size) {
    return false;
  }
  control_messages_.push_back(message);
  queue_cv_.notify_all();
  return true;
}

void MMWorkerEngine::register_control_handler(const std::string& event_type,
                                              WorkerControlHandler handler) {
  std::lock_guard<std::mutex> lock(handler_mutex_);
  control_handlers_[event_type].push_back(std::move(handler));
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

void MMWorkerEngine::handle_control_message(const ControlMessage& message) {
  if(message.type == "workflow_registered") {
    const std::string& workflow_id = message.data.count("workflow_instance_id") > 0
                                         ? message.data.at("workflow_instance_id")
                                         : message.data.at("workflow_id");
    int thread_count = config_.default_workflow_threads;
    auto thread_it = message.data.find("thread_count");
    if(thread_it != message.data.end()) {
      thread_count = std::stoi(thread_it->second);
    }
    ensure_workflow_pool(workflow_id, thread_count);
    return;
  }

  if(message.type == "workflow_unregistered") {
    auto it = message.data.find("workflow_instance_id");
    if(it != message.data.end()) {
      workflow_pools_.erase(it->second);
    }
  }
}

void MMWorkerEngine::process_business_messages() {
  std::vector<BusinessMessage> messages_to_run;
  {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    while(!business_messages_.empty()) {
      messages_to_run.push_back(business_messages_.front());
      business_messages_.pop_front();
    }
  }

  for(const auto& message : messages_to_run) {
    const auto& task = message.task;
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

void MMWorkerEngine::control_message_loop() {
  while(running_) {
    ControlMessage message;
    {
      std::unique_lock<std::mutex> lock(queue_mutex_);
      queue_cv_.wait(lock, [this]() { return !control_messages_.empty() || !running_; });
      if(!running_) {
        break;
      }
      message = control_messages_.front();
      control_messages_.pop_front();
    }

    handle_control_message(message);

    std::vector<WorkerControlHandler> handlers;
    {
      std::lock_guard<std::mutex> lock(handler_mutex_);
      auto                        it = control_handlers_.find(message.type);
      if(it != control_handlers_.end()) {
        handlers = it->second;
      }
    }

    for(const auto& handler : handlers) {
      if(control_pool_) {
        control_pool_->submit([handler, message]() { handler(message); });
      } else {
        handler(message);
      }
    }
  }
}

void MMWorkerEngine::business_message_loop() {
  while(running_) {
    {
      std::unique_lock<std::mutex> lock(queue_mutex_);
      queue_cv_.wait(lock, [this]() { return !business_messages_.empty() || !running_; });
      if(!running_) {
        break;
      }
    }
    process_business_messages();
  }
}

}  // namespace runtime
}  // namespace agentflow
