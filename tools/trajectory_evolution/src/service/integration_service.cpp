// 接入服务：统一处理打点、轨迹落盘、后台学习与评测作业。
// 主请求只提交有界事件并读取可用策略；耗时反思和模型评测进入后台流程。
// 并发请求围绕明确的状态锁与身份边界执行，用户和会话不能互相串用状态。
// 已处理日志、待处理批次和重试时间分别维护，避免重复分析与空闲忙轮询。

// 服务层接收语言无关打点并返回策略决定，不依赖具体 Agent 框架实现。
#include "trajectory_evolution/integration.h"
#include "trajectory_evolution/evolution.h"
#include "trajectory_evolution/request_binding.h"
#include "trajectory_evolution/task_evaluation.h"
#include "integration_json.h"
#include "boundary_runtime.h"
#include "instrumentation.h"
#include "control_generation.h"
#include "local_support.h"
#include "trace_internal.h"
#include "shared_memory.h"
#include "agent_evaluation_support.h"
#include "evaluation_entry.h"
#include "turn_contract.h"
#include "learning_evidence.h"
#include "code_projection.h"
#include "gepa_feedback.h"
#include "gepa_search.h"

#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <fstream>
#include <future>
#include <iterator>
#include <mutex>
#include <set>
#include <sstream>
#include <thread>
#include <openssl/crypto.h>
#include <openssl/rand.h>
#include <openssl/sha.h>
#include <google/protobuf/util/message_differencer.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

namespace trajectory::evolution {
namespace {
using namespace integration;
namespace fs = std::filesystem;
namespace legacy = agentflow::trajectory;
namespace ep = legacy::evaluation;

// 仅提取允许公开的评测错误码，模型或文件异常正文不越过诊断边界。
std::string evaluation_error_code(const legacy::EvaluationError &error) {
  // 异常消息可能包含路径或提供方数据，只公开已知严格评测错误码，
  // 不公开原始异常消息或模型响应。
  const std::string message = error.what();
  std::string code;
  for (const std::string prefix : {"strict dialogue model evaluation failed: ",
                                   "strict dialogue model evaluation could not run: "})
    if (message.compare(0, prefix.size(), prefix) == 0)
      code = message.substr(prefix.size());
  static const std::set<std::string> allowed{"model_request_too_large",
                                             "model_call_limit_reached",
                                             "adapter_not_configured",
                                             "adapter_exception",
                                             "transport_error",
                                             "invalid_response_size",
                                             "credential_like_output",
                                             "invalid_json",
                                             "missing_dialogue_section",
                                             "invalid_task_outcome",
                                             "episode_assessment_count_mismatch",
                                             "invalid_episode_assessment",
                                             "invalid_episode_verdict",
                                             "invalid_episode_confidence",
                                             "invalid_episode_state_change",
                                             "invalid_episode_concise_reason",
                                             "invalid_episode_evidence_event_ids",
                                             "unknown_or_duplicate_episode_id",
                                             "invalid_reflection",
                                             "invalid_reflection_summary",
                                             "invalid_reflection_root_causes",
                                             "invalid_reflection_skill_orchestration_changes",
                                             "invalid_reflection_stop_condition_changes",
                                             "invalid_reflection_wasted_episode_ids",
                                             "invalid_reflection_evidence_event_ids",
                                             "unknown_wasted_episode_id",
                                             "missing_record",
                                             "request_too_large",
                                             "client_initialization_failed",
                                             "header_allocation_failed",
                                             "output_token_limit_reached",
                                             "invalid_api_response",
                                             "response_too_large",
                                             "transport_exhausted"};
  bool known = allowed.count(code);
  // HTTP 和 libcurl 诊断只保留受限数值状态。
  for (const std::string prefix : {"http_", "transport_"}) {
    if (code.compare(0, prefix.size(), prefix) != 0)
      continue;
    const auto number = code.substr(prefix.size());
    known |= !number.empty() && number.size() <= 3 &&
             std::all_of(number.begin(), number.end(), [](char c) { return c >= '0' && c <= '9'; });
  }
  return known ? "dialogue_model_" + code : "trace_evaluation_failed";
}

// 计算内容摘要，作为版本或内容身份；相同输入必须得到相同结果。
std::string digest(const std::string &text) {
  unsigned char bytes[SHA256_DIGEST_LENGTH];
  require(SHA256(reinterpret_cast<const unsigned char *>(text.data()), text.size(), bytes),
          "hash_failed", 500);
  std::string out;
  constexpr char hex[] = "0123456789abcdef";
  for (auto b : bytes) {
    out += hex[b >> 4];
    out += hex[b & 15];
  }
  return out;
}

// 校验或编码单段身份字段，供当前模块构造隔离路径或稳定标识。
std::string component(const std::string &value) {
  std::string out;
  constexpr char hex[] = "0123456789ABCDEF";
  for (unsigned char c : value) {
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' ||
        c == '_' || c == '.')
      out += c;
    else {
      out += '%';
      out += hex[c >> 4];
      out += hex[c & 15];
    }
  }
  return out;
}

// 把身份转为安全路径片段，确保用户输入不能改变父目录。
std::string path_component(const std::string &value) {
  auto result = component(value);
  // UTF-8 身份编码后可能超过文件系统单段名称限制。
  // component() 不会产生字面波浪号，因此该命名空间不会与普通编码冲突。
  if (result.size() > 200)
    return "~" + digest(value);
  if (result == ".")
    return "%2E";
  if (result == "..")
    return "%2E%2E";
  return result;
}

// 读取受大小限制的文件内容；保留读取错误，交由上层决定是否重试或中止。
std::string read_file(const fs::path &path, std::size_t limit) {
  require(!fs::is_symlink(path), "symlink_not_allowed");
  std::ifstream stream(path, std::ios::binary | std::ios::ate);
  require(stream && stream.tellg() >= 0 && static_cast<std::uint64_t>(stream.tellg()) <= limit,
          "file_unavailable_or_too_large", 500);
  auto size = static_cast<std::size_t>(stream.tellg());
  stream.seekg(0);
  std::string bytes(size, '\0');
  require(size == 0 || bool(stream.read(bytes.data(), size)), "file_read_failed", 500);
  return bytes;
}

// 创建受限访问目录，避免其他用户读取私有轨迹和记忆。
void private_directory(const fs::path &path) {
  fs::path prefix;
  for (const auto &part : fs::absolute(path)) {
    prefix /= part;
    require(!fs::is_symlink(prefix), "symlink_not_allowed");
  }
  fs::create_directories(path);
  require(::chmod(path.c_str(), 0700) == 0, "directory_permission_failed", 500);
}

// 通过临时文件和原子替换提交完整内容，避免读者看到中间写入状态。
void atomic_write(const fs::path &path, const std::string &bytes) {
  private_directory(path.parent_path());
  require(!fs::is_symlink(path), "symlink_not_allowed");
  std::string temp = path.string() + ".pending.XXXXXX";
  int fd = ::mkstemp(temp.data());
  require(fd >= 0, "state_write_failed", 507);
  bool success = false;
  try {
    std::size_t written = 0;
    while (written < bytes.size()) {
      const auto n = ::write(fd, bytes.data() + written, bytes.size() - written);
      require(n > 0, "state_write_failed", 507);
      written += n;
    }
    require(::fsync(fd) == 0, "state_write_failed", 507);
    ::close(fd);
    fd = -1;
    require(::rename(temp.c_str(), path.c_str()) == 0, "state_publish_failed", 507);
    int directory = ::open(path.parent_path().c_str(), O_RDONLY | O_DIRECTORY);
    if (directory >= 0) {
      ::fsync(directory);
      ::close(directory);
    }
    success = true;
  } catch (...) {
    if (fd >= 0)
      ::close(fd);
    if (!success)
      ::unlink(temp.c_str());
    throw;
  }
}

// 识别可能承载凭据的字段，结合内容扫描进行隐私检查。
bool secret_key(std::string key) {
  key.erase(std::remove_if(key.begin(), key.end(), [](char c) { return c == '-' || c == '_'; }),
            key.end());
  std::transform(key.begin(), key.end(), key.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  return key == "authorization" || key == "apikey" || key == "password" || key == "passwd" ||
         key == "accesstoken" || key == "refreshtoken" || key == "clientsecret" ||
         key == "privatekey" || key == "cookie" || key == "setcookie" || key == "secret" ||
         key == "credentials";
}

// 验证外部请求的结构和敏感内容，拒绝未经检查的数据进入持久化状态。
void safe_input(const Json &value) {
  if (value.object())
    for (const auto &pair : value.fields()) {
      require(!secret_key(pair.first), "sensitive_field_rejected");
      safe_input(pair.second);
    }
  if (value.array())
    for (const auto &item : value.items())
      safe_input(item);
}

// 递归检查待输出数据的隐私边界，禁止凭据随着错误或模型内容落盘。
void safe_data(const Json &value) {
  safe_input(value);
  try {
    legacy::ensure_safe_evolution_data(proto<google::protobuf::Value>(value));
  } catch (const legacy::EvaluationError &) {
    throw Error("credential_value_rejected");
  }
}

// 解析公开进化能力等级，和数据集业务用例等级保持区分。
int level(const std::string &value) {
  require(value.size() == 2 && value[0] == 'L' && value[1] >= '0' && value[1] <= '3',
          "level_not_supported");
  return value[1] - '0';
}

// 把公开能力等级映射为协议枚举，拒绝无效名称。
v1::Level proto_level(int value) {
  return static_cast<v1::Level>(static_cast<int>(v1::L0) + value);
}

// 读取当前墙上时钟并换算为微秒，与轨迹事件的时间单位保持一致。
std::string now_us() {
  return std::to_string(std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::system_clock::now().time_since_epoch())
                            .count());
}

// 检查运行是否进入终态；终态之后的迟到事件不能重新启动同一个执行实例。
bool ended(const Json &item) {
  return item.flag("ended", false);
}

// 判断状态或事件是否已经终结，供收尾与重复回执处理使用。
std::string terminal(const Json &value) {
  const auto status = value.str("status", "completed");
  require(status == "completed" || status == "failed" || status == "cancelled" ||
              status == "unknown",
          "status_invalid");
  return status;
}

// 构造固定结构的错误信息，禁止携带任意异常原文。
Json error_info(const std::string &code) {
  return {{"code", code}, {"retryability", "unknown"}};
}

// 核对外部错误对象的固定结构，防止任意异常正文进入对外诊断。
void validate_error(const Json &error) {
  const auto parsed = proto<v1::ErrorInfo>(error);
  require(identifier(parsed.code()) &&
              (parsed.retryability() == "unknown" || parsed.retryability() == "retryable" ||
               parsed.retryability() == "non_retryable"),
          "error_info_invalid");
}

struct FileLock {
  int fd = -1;

  // 销毁对象时释放本类持有的资源；收尾不能抛出业务异常，也不能继续接收新请求。
  ~FileLock() {
    if (fd >= 0) {
      ::flock(fd, LOCK_UN);
      ::close(fd);
    }
  }
};

} // 结束当前匿名命名空间。

// 对本地打点数据执行同样的隐私约束，不因没有 HTTP 而跳过检查。
void integration::validate_local_data(const Json &value) {
  safe_data(value);
}

struct IntegrationService::Impl {
  Json config;
  fs::path root, config_path, evaluator_path, memory_source_root;
  std::map<std::string, std::string> principals;
  std::mutex mutex, evaluation_mutex, publication_mutex;
  std::condition_variable work_changed;
  std::uint64_t work_generation = 1;
  std::map<std::string, std::uint64_t> memory_versions;
  // 运行持久关闭后，只保留小型队列记录常驻内存。
  std::map<std::string, Json> learning_queue;
  std::set<std::string> unpublished;
  Json schedule;
  Json sharing_state, published_shared;
  std::size_t sharing_due_ms = 0;
  std::size_t sharing_pending_users = 0;
  std::string sharing_status = "idle";
  Json memory_management = Json::Object{};
  Json memory_management_errors = Json::Object{};
  std::size_t memory_due_ms = 0, memory_full_threshold = 20;
  bool shared_available = true;
  std::size_t next_batch_ms = 0, storage_retry_ms = 0;
  bool evaluating = false;
  std::map<std::string, Json> runs;
  // One immutable policy interpreter per active run, just as LocalRuntime.
  // Access is covered by the existing service mutex; no second lock or
  // persistent ledger duplicates the authoritative run events.
  std::map<std::string, std::unique_ptr<PolicyRuntime>> runtime_policies;
  std::map<std::string, std::string> evaluation_outcomes;
  std::size_t stored_count = 0;
  std::set<std::string> archivable;
  FileLock lock;
  std::size_t request_limit, run_limit, event_limit, active_limit, recent_reports;
  unsigned listen_port, timeout, thread_count;
  std::unique_ptr<AgentEvaluationService> assessments;
  std::map<std::string, Json> approved_versions;
  // 发布变化只使原生快照失效，不伪装成新增经验而再次触发 GEPA。
  std::map<std::string, std::uint64_t> publication_versions;
  std::map<std::string, std::uint64_t> assessed_memory_versions;
  // 完成通知只要求消费评测结果，不等于出现新记忆；否则其他用户完成也会重开搜索。
  std::uint64_t assessment_notifications = 0;
  std::map<std::string, std::uint64_t> assessed_notifications;
  std::string assessment_error;
  std::size_t assessment_retry_ms = 0, assessment_failures = 0;
  // 原库暂被其他管理器占用只延迟本用户投递，不消耗全局评测故障预算。
  std::map<std::string, std::size_t> gepa_feedback_retry_ms;

  struct CaptureWrite {
    std::string bytes;
    std::uint64_t revision = 0;
  };

  std::map<std::string, CaptureWrite> capture_writes;
  std::map<std::string, std::uint64_t> capture_dirty;
  std::set<std::string> capture_poisoned;
  std::condition_variable capture_changed;
  std::thread capture_writer;
  bool capture_stopping = false;
  std::size_t capture_bytes = 0, capture_limit = 67108864;
  std::uint64_t capture_revision = 0;
  std::string capture_epoch;
  bool buffered_capture = false;
  EvaluationEntry evaluation_entry;

  // 初始化 Impl 的状态和资源；输入参数的校验与所有权在构造阶段确定。
  // 对象创建后复用既有配置，实际执行与收尾分别由后续接口负责。
  explicit Impl(const fs::path &path, const std::map<std::string, std::string> &environment,
                bool enable_auto_evaluation)
      : config_path(fs::absolute(path)) {
    config = Json::parse(read_file(config_path, 262144));
    keys(config, {"schema_version",
                  "agent_id",
                  "data_dir",
                  "memory_source_dir",
                  "port",
                  "http_timeout_ms",
                  "workers",
                  "max_request_bytes",
                  "max_run_bytes",
                  "max_events",
                  "max_active_runs",
                  "max_stored_runs",
                  "recent_reports",
                  "max_level",
                  "principals",
                  "actors",
                  "runtime_config",
                  "auto_evaluate",
                  "auto_approve_experiences",
                  "shadow_options",
                  "runtime_options",
                  "experience_template_file",
                  "finalization_template_file",
                  "application_id",
                  "policy_scopes",
                  "activity_types",
                  "learning",
                  "sharing",
                  "policy_evaluation_file",
                  "evaluation_version_file",
                  "policy_version_file",
                  "query_context",
                  "instrumentation",
                  "run_lease_ms",
                  "http_persistence",
                  "preserve_runtime_policy_state",
                  "control_learning",
                  "evaluation_levels"});
    // 管理端显式恢复指定评测时，先关闭“从当前记忆生成新候选”的自动入口。
    // 已落盘的 AgentEvaluationService 作业及显式 retry 仍照常运行；这个开关
    // 只消除服务启动与 retry 请求之间抢先创建空壳作业的竞态。
    if (!enable_auto_evaluation)
      config["auto_evaluate"] = false;
    if (config.has("preserve_runtime_policy_state"))
      (void)config["preserve_runtime_policy_state"].boolean();
    validate_control_learning(config["control_learning"]);
    if (config["control_learning"].flag("enabled", false))
      require(config.has("policy_evaluation_file") || config.has("evaluation_version_file") ||
                  config.has("policy_version_file"),
              "control_learning_requires_evaluation");
    if (config.has("http_persistence")) {
      keys(config["http_persistence"], {"mode", "max_buffer_bytes"});
      const auto mode = config["http_persistence"].str("mode", "buffered");
      require(mode == "buffered" || mode == "durable", "http_persistence_mode_invalid");
      buffered_capture = mode == "buffered";
      capture_limit = config["http_persistence"].num("max_buffer_bytes", 67108864);
      require(capture_limit >= 1048576 && capture_limit <= 1073741824,
              "capture_buffer_limit_invalid");
    }
    unsigned char epoch_bytes[16];
    require(RAND_bytes(epoch_bytes, sizeof(epoch_bytes)) == 1, "random_unavailable");
    capture_epoch = digest(std::string(reinterpret_cast<char *>(epoch_bytes), sizeof(epoch_bytes)))
                        .substr(0, 32);
    if (config.has("application_id")) {
      require(!config.has("agent_id") || config.str("agent_id") == config.str("application_id"),
              "application_identity_conflict");
      config["agent_id"] = config["application_id"];
    }
    if (config.has("policy_scopes")) {
      require(!config.has("actors") || config["actors"].dump() == config["policy_scopes"].dump(),
              "policy_scopes_conflict");
      config["actors"] = config["policy_scopes"];
    }
    if (!config.has("actors"))
      config["actors"] = Json::Array{"default"};
    validate_boundary_configuration(config);
    validate_instrumentation(config);
    require(config.num("schema_version", 0) == 1 && identifier(config.str("agent_id")),
            "configuration_invalid");
    level(config.str("max_level"));
    if (config.has("evaluation_levels")) {
      require(config["evaluation_levels"].array() &&
                  config["evaluation_levels"].items().size() <= 4,
              "evaluation_levels_invalid");
      std::set<std::string> selected;
      for (const auto &value : config["evaluation_levels"].items())
        require(value.string() && level(value.text()) <= level(config.str("max_level")) &&
                    selected.insert(value.text()).second,
                "evaluation_levels_invalid");
    }
    require(!config.str("data_dir").empty(), "data_directory_required");
    root = fs::absolute(config.str("data_dir")).lexically_normal();
    require(root != root.root_path(), "data_directory_invalid");
    if (config.has("memory_source_dir")) {
      require(config.has("policy_evaluation_file") && !config.str("memory_source_dir").empty(),
              "memory_source_requires_evaluation");
      memory_source_root = fs::absolute(config.str("memory_source_dir")).lexically_normal();
      require(fs::is_directory(memory_source_root) && !fs::is_symlink(memory_source_root) &&
                  memory_source_root != root, "memory_source_invalid");
    }
    if (config.has("policy_version_file")) {
      require(!config.has("policy_evaluation_file") && !config.has("evaluation_version_file"),
              "offline_policy_mode_conflict");
      fs::path file(config.str("policy_version_file"));
      if (file.is_relative())
        file = config_path.parent_path() / file;
      const auto bundle = Json::parse(read_file(file, 4194304));
      keys(bundle, {"schema_version", "version", "artifact", "evaluation"});
      require(bundle.num("schema_version", 0) == 1 &&
                  bundle.str("version") == agent_version_id(bundle["artifact"].dump()) &&
                  bundle["evaluation"]["gate"].flag("passed", false),
              "offline_policy_bundle_invalid");
      require((bundle["artifact"].str("level") == "L0" ||
               bundle["artifact"]["policy"]["snapshot"]["identity"].str("agent_id") ==
                   config.str("agent_id")) &&
                  bundle["artifact"].str("level") <= config.str("max_level"),
              "offline_policy_not_authorized");
      config["offline_version"] = bundle["artifact"];
      config["auto_evaluate"] = false;
      config["sharing"] = Json{{"enabled", false}};
    }
    if (config.has("evaluation_version_file")) {
      fs::path version_file(config.str("evaluation_version_file"));
      if (version_file.is_relative())
        version_file = config_path.parent_path() / version_file;
      version_file = fs::absolute(version_file).lexically_normal();
      const auto relative = root.lexically_relative(version_file.parent_path());
      require(!relative.empty() && *relative.begin() != ".." && relative != ".",
              "evaluation_data_directory_not_isolated");
      const auto version = Json::parse(read_file(version_file, 4194304));
      keys(version, {"version", "artifact", "trial_id", "entry", "published_policy"});
      require(version.str("version") == agent_version_id(version["artifact"].dump()),
              "evaluation_version_invalid");
      require(identifier(version.str("trial_id")), "evaluation_trial_id_required");
      if (version.has("entry")) {
        EvaluationEntry::validate(version["entry"]["binding"]);
        evaluation_entry.specification = version["entry"];
      }
      config["evaluation_version"] = version;
      if (version.flag("published_policy", false))
        config["offline_version"] = version["artifact"];
      config["auto_evaluate"] = false;
      config["sharing"] = Json{{"enabled", false}};
      config.fields().erase("policy_evaluation_file");
    }
    auto local = [&](const std::string &name) {
      fs::path value(config.str(name));
      require(!value.empty(), "configuration_path_missing");
      return value.is_absolute() ? value : config_path.parent_path() / value;
    };
    if (!config.has("offline_version")) {
      evaluator_path = local("runtime_config");
      const auto evaluation = legacy::load_runtime_configuration(evaluator_path);
      memory_full_threshold = evaluation.evolution.full_curation_new_entry_threshold;
      if (!evaluation.model.endpoint.empty() || !evaluation.model.model_id.empty()) {
        require(!evaluation.model.endpoint.empty() && !evaluation.model.model_id.empty() &&
                    (!evaluation.api_key_env.empty() != evaluation.no_api_key),
                "evaluation_configuration_invalid");
      }
      config["runtime_options"]["experience_template"] =
          read_file(local("experience_template_file"), 16384);
    } else if (!config.has("runtime_options")) {
      config["runtime_options"] = Json::Object{};
    }
    // 收尾模板随包交付；显式路径可替换，旧部署缺少模板时不启用收尾。
    const auto finalization_path =
        config.has("finalization_template_file") ? local("finalization_template_file")
        : config.has("experience_template_file")
            ? local("experience_template_file").parent_path() / "runtime-finalization.txt"
            : fs::path{};
    if (!finalization_path.empty() &&
        (config.has("finalization_template_file") || fs::exists(finalization_path)))
      config["finalization_context"] = read_file(finalization_path, 16384);
    config["sharing"] = shared_memory_options(config["sharing"]);
    if (config["sharing"].flag("enabled", false)) {
      for (const auto &pair : std::vector<std::pair<std::string, std::string>>{
               {"distill", "shared-experience-distill.txt"},
               {"review", "shared-experience-review.txt"},
               {"relation", "cross-memory-relation.txt"}}) {
        fs::path p(config["sharing"].str(pair.first + "_prompt_file", "prompts/" + pair.second));
        if (p.is_relative())
          p = evaluator_path.parent_path() / p;
        config["shared_" + pair.first + "_prompt_path"] = p.string();
      }
      fs::path batch_prompt(config["sharing"].str("relation_batch_prompt_file", "prompts/cross-memory-batch.txt"));
      if (batch_prompt.is_relative()) batch_prompt = evaluator_path.parent_path() / batch_prompt;
      // 旧部署缺少批量模板时继续单项协议，不要求宿主新增适配代码。
      if (fs::exists(batch_prompt)) config["shared_relation_batch_prompt_path"] = batch_prompt.string();
    }
    for (const auto *name : {"action_rules", "loop_rules"}) {
      if (!config["runtime_options"].has(name))
        continue;
      for (auto &rule : config["runtime_options"][name].items()) {
        if (rule.has("scope_id")) {
          require(!rule.has("actor_id") && !rule.has("target_scope"), "ambiguous_rule_scope");
          rule["actor_id"] = rule["scope_id"];
          rule.fields().erase("scope_id");
        }
        if (!rule.has("actor_id"))
          continue;
        require(!rule.has("target_scope"), "ambiguous_rule_scope");
        bool found = false;
        for (const auto &actor : config["actors"].items())
          found |= actor.text() == rule.str("actor_id");
        require(found, "rule_actor_not_configured");
        rule["target_scope"] = scope(rule.str("actor_id"));
        rule.fields().erase("actor_id");
      }
    }
    auto opts = proto<v1::RuntimeOptions>(config["runtime_options"]);
    validate_request_binding(opts);
    request_limit = config.num("max_request_bytes", 1048576);
    run_limit = config.num("max_run_bytes", 16777216);
    event_limit = config.num("max_events", 10000);
    active_limit = config.num("max_active_runs", 128);
    require(config.num("run_lease_ms", 86400000) >= 100 &&
                config.num("run_lease_ms", 86400000) <= 604800000,
            "run_lease_invalid");
    require(config.num("max_stored_runs", 10000) > 0, "stored_run_limit_invalid");
    recent_reports = config.num("recent_reports", 32);
    schedule = config.has("learning") ? config["learning"] : Json(Json::Object{});
    keys(schedule, {"min_new_runs", "max_wait_ms", "min_interval_ms", "max_batch_runs",
                    "max_batch_bytes", "retry_backoff_ms", "max_analysis_retries",
                    "analysis_parallelism"});
    const Json defaults{{"min_new_runs", 20},          {"max_wait_ms", 600000},
                        {"min_interval_ms", 60000},    {"max_batch_runs", 32},
                        {"max_batch_bytes", 33554432}, {"retry_backoff_ms", 60000},
                        {"max_analysis_retries", 0}, {"analysis_parallelism", 1}};
    for (const auto &item : defaults.fields())
      if (!schedule.has(item.first))
        schedule[item.first] = item.second;
    require(schedule.num("min_new_runs", 0) > 0 &&
                schedule.num("min_new_runs", 0) <= schedule.num("max_batch_runs", 0) &&
                schedule.num("max_batch_runs", 0) <= 256 && schedule.num("max_wait_ms", 0) > 0 &&
                schedule.num("max_wait_ms", 0) <= 86400000 &&
                schedule.num("min_interval_ms", 0) <= 86400000 &&
                schedule.num("retry_backoff_ms", 0) >= 10 &&
                schedule.num("retry_backoff_ms", 0) <= 86400000 &&
                schedule.num("max_analysis_retries", 0) <= 10 &&
                schedule.num("analysis_parallelism", 0) > 0 &&
                schedule.num("analysis_parallelism", 0) <= 32 &&
                schedule.num("max_batch_bytes", 0) >= run_limit &&
                schedule.num("max_batch_bytes", 0) <= 268435456,
            "learning_schedule_invalid");
    config["learning"] = schedule;
    listen_port = config.num("port", 8766);
    timeout = config.num("http_timeout_ms", 5000);
    thread_count = config.num("workers", 4);
    require(request_limit >= 1024 && request_limit <= 2097152 && run_limit >= request_limit &&
                run_limit <= 134217728 && event_limit >= 16 && event_limit <= 100000 &&
                active_limit > 0 && active_limit <= 1024 && recent_reports > 0 &&
                recent_reports <= 256 && listen_port <= 65535 && timeout >= 100 &&
                timeout <= 60000 && thread_count > 0 && thread_count <= 32,
            "configuration_limits_invalid");
    require(config["actors"].array() && !config["actors"].items().empty() &&
                config["actors"].items().size() <= 64,
            "actors_required");
    for (const auto &actor : config["actors"].items())
      require(identifier(actor.text()), "actor_invalid");
    for (const auto &principal : config["principals"].items()) {
      keys(principal, {"user_id", "token_env"});
      const auto user = principal.str("user_id"), name = principal.str("token_env");
      require(identifier(user) && !name.empty(), "principal_invalid");
      const auto supplied = environment.find(name);
      const char *token =
          supplied == environment.end() ? std::getenv(name.c_str()) : supplied->second.c_str();
      require(token && std::string(token).size() >= 24 && std::string(token).size() <= 4096,
              "authentication_secret_missing_or_short");
      require(principals.emplace(token, user).second, "duplicate_principal_token");
    }
    require(!principals.empty() && principals.size() <= 256, "principals_required");
    private_directory(root);
    private_directory(root / "runs");
    require(!fs::is_symlink(root / "service.lock"), "symlink_not_allowed");
    lock.fd = ::open((root / "service.lock").c_str(), O_CREAT | O_RDWR | O_NOFOLLOW, 0600);
    require(lock.fd >= 0 && ::flock(lock.fd, LOCK_EX | LOCK_NB) == 0,
            "data_directory_already_in_use", 409);
    private_directory(root / "learning/jobs");
    if (fs::exists(root / "learning/schedule.json")) {
      const auto state = Json::parse(read_file(root / "learning/schedule.json", 4096));
      require(state.num("schema_version", 0) == 1, "learning_state_invalid");
      next_batch_ms =
          std::min(state.num("next_batch_ms", 0), millis() + schedule.num("min_interval_ms", 0));
      storage_retry_ms = std::min(state.num("retry_after_ms", 0),
                                  millis() + schedule.num("retry_backoff_ms", 60000));
    }
    for (const auto &entry : fs::directory_iterator(root / "runs")) {
      if (entry.path().extension() != ".json")
        continue;
      auto run = Json::parse(read_file(entry.path(), run_limit));
      require(run.str("kind") == "integration-run-v1", "stored_state_invalid", 500);
      const bool interrupted_local =
          (run.flag("local_capture", false) || run.flag("buffered_capture", false)) &&
          !run.flag("closed", false);
      if (interrupted_local) {
        // 内存中的生产者退出前未发布终态快照。
        run["incomplete"] = true;
        run["local_degradation_reason"] = "producer_interrupted";
        run["capture_durable"] = true;
        dispatch_boundary(
            run, Json{{"operation", "run.abort"}, {"status", "unknown"}, {"output", nullptr}},
            config);
      }
      if (run.str("evaluation_status") == "running")
        run["evaluation_status"] = "pending";
      require(run["identity"].str("agent_id") == config.str("agent_id"), "stored_agent_mismatch");
      const auto rid = run["identity"].str("run_id");
      require(rid.size() == 40 && rid.find_first_not_of("0123456789abcdef") == std::string::npos &&
                  entry.path().stem() == rid && identifier(run["identity"].str("user_id")) &&
                  identifier(run["identity"].str("conversation_id")),
              "stored_state_invalid", 500);
      ++stored_count;
      evaluation_outcomes[rid] = run.str("evaluation_status", "not_scheduled");
      if (run.flag("closed", false))
        run["trace_path"] = trace_path(run).string();
      if (run.flag("closed", false)) {
        if (!run["learning"].has("queued_at_ms")) {
          run["learning"]["queued_at_ms"] = millis();
          save(run);
        } else
          observe_queue(run);
      }
      if (interrupted_local)
        save(run);
      if (evictable(run))
        continue;
      runs.emplace(run["identity"].str("run_id"), std::move(run));
    }
    // 历史终态快照可归档，同时保留回放身份。
    recover_jobs();
    sharing_state = new_shared_memory(config.str("agent_id"));
    if (config["sharing"].flag("enabled", false)) {
      const auto state_path =
          (memory_source_root.empty() ? root : memory_source_root) / "learning/shared-memory.json";
      if (fs::exists(state_path))
        sharing_state = Json::parse(read_file(state_path, 268435456));
      else if (!memory_source_root.empty())
        throw Error("memory_source_shared_missing");
      else if (fs::exists(root / "memory/evolution-memory.pb")) {
        v1::SharedExperienceStore previous;
        require(
            previous.ParseFromString(read_file(root / "memory/evolution-memory.pb", 16777216)) &&
                previous.application_id() == config.str("agent_id") &&
                previous.experiences().empty(),
            "legacy_memory_requires_explicit_migration");
      }
      validate_shared_memory(sharing_state, config.str("agent_id"), config["sharing"]);
      if (!memory_source_root.empty()) {
        published_shared = sharing_state;
        return;
      }
      reconcile_shared_memory(sharing_state, config["sharing"]);
      // 台账才是权威状态；检查点与公共视图之间发生崩溃时，
      // 不能为不同内容复用同一修订号。
      if (fs::exists(root / "memory/evolution-memory.pb")) {
        v1::SharedExperienceStore previous;
        require(
            previous.ParseFromString(read_file(root / "memory/evolution-memory.pb", 16777216)) &&
                previous.application_id() == config.str("agent_id"),
            "shared_projection_invalid");
        auto current = shared_memory_view(sharing_state);
        const auto revision = std::max<std::size_t>(current.revision(), previous.revision());
        previous.clear_revision();
        current.clear_revision();
        sharing_state["revision"] =
            revision + (json(previous).dump() != json(current).dump() ? 1 : 0);
      }
      for (const auto &p : sharing_state["users"].fields())
        enqueue_shared_memory(sharing_state, p.second.text());
      for (const auto &p : principals)
        if (fs::exists(memory_path(p.second)))
          enqueue_shared_memory(sharing_state, p.second);
      if (!sharing_state["pending_users"].items().empty()) {
        const auto ready = std::max(millis(), sharing_state.num("last_cycle_ms", 0) +
                                                  config["sharing"].num("min_interval_ms", 60000));
        const auto previous = sharing_state.num("next_due_ms", 0);
        sharing_state["next_due_ms"] = previous ? std::min(previous, ready) : ready;
      }
      sharing_due_ms = sharing_state.num("next_due_ms", 0);
      sharing_pending_users = sharing_state["pending_users"].items().size();
      sharing_status = sharing_pending_users ? "pending" : "idle";
      checkpoint_sharing(sharing_state);
      const auto public_store = shared_memory_view(sharing_state);
      atomic_write(root / "memory/evolution-memory.pb", public_store.SerializeAsString());
      atomic_write(root / "memory/evolution-memory.json", json(public_store).dump());
    }
    published_shared = sharing_state;
    if (memory_source_root.empty())
      refresh_memory_management(true);
  }

  // 令牌只用于确认调用身份；日志和持久化运行记录使用用户标识，不保存令牌原文。
  std::string authenticate(const std::string &token) const {
    for (const auto &pair : principals)
      if (pair.first.size() == token.size() &&
          CRYPTO_memcmp(pair.first.data(), token.data(), token.size()) == 0)
        return pair.second;
    throw Error("unauthorized", 401);
  }

  // 根据认证身份计算当前数据和策略的隔离作用域。
  std::string scope(const std::string &actor) const {
    return "agent:" + component(config.str("agent_id")) + "/actor:" + component(actor);
  }

  // 定位当前用户私有数据目录，路径编码保持唯一和安全。
  fs::path user_dir(const std::string &user) const {
    return root / "by-user" / path_component(user);
  }

  // 按用户作用域定位私有记忆库，公共记忆使用独立路径。
  fs::path memory_path(const std::string &user) const {
    return user_dir(user) / "memory" / "evolution-memory.pb";
  }

  // 离线评测直接读取训练阶段的权威记忆，工作区只保存反馈和版本。
  fs::path memory_input_path(const std::string &user) const {
    return memory_source_root.empty()
               ? memory_path(user)
               : memory_source_root / "by-user" / path_component(user) /
                     "memory/evolution-memory.pb";
  }

  // 按用户和会话身份定位轨迹文件，保持不同对话可区分且可回放。
  fs::path trace_path(const Json &run) const {
    return user_dir(run["identity"].str("user_id")) /
           (run.flag("incomplete", false) ? "quarantine" : "traces") /
           path_component(run["identity"].str("conversation_id")) /
           (run["identity"].str("run_id") +
            (run.str("import_format") == "sessionlog" ? ".jsonl" : ".trace.json"));
  }

  // 检查运行状态是否满足安全淘汰条件，避免清除仍有在途分支的上下文。
  bool evictable(const Json &run) const {
    return !capture_dirty.count(run["identity"].str("run_id")) && run.flag("closed", false) &&
           run.str("evaluation_status") != "running" &&
           (run["learning"].flag("trace_published", false) ||
            run["learning"].has("publication_error"));
  }

  // 回收满足条件的已结束运行，控制常驻内存占用。
  void evict_finished() {
    if (evaluation_entry.enabled())
      return;
    for (auto it = runs.begin(); it != runs.end();) {
      if (evictable(it->second)) {
        runtime_policies.erase(it->first);
        it = runs.erase(it);
      } else
        ++it;
    }
  }

  // 按运行身份定位持久化状态文件，确保不同用户与会话互不覆盖。
  fs::path run_file(const std::string &rid) const {
    require(rid.size() == 40 && rid.find_first_not_of("0123456789abcdef") == std::string::npos,
            "stored_run_id_invalid");
    const auto live = root / "runs" / (rid + ".json");
    const auto archived = root / "archive/runs" / rid.substr(0, 2) / (rid + ".json");
    return fs::exists(live) || !fs::exists(archived) ? live : archived;
  }

  // 清理与维护运行状态时，区分仍有异步子任务的 run 和已经真正结束的 run。
  void maintain_runs() {
    const auto now = millis(), lease = config.num("run_lease_ms", 86400000);
    for (auto &pair : runs) {
      auto &run = pair.second;
      if (run.flag("closed", false) || run.flag("local_capture", false) ||
          run.str("profile") != "evolution.boundary.v1")
        continue;
      if (!run.has("last_activity_ms")) {
        run["last_activity_ms"] = now;
        continue;
      }
      const auto last = run.num("last_activity_ms", now);
      if (now < last || now - last < lease)
        continue;
      run["incomplete"] = true;
      run["local_degradation_reason"] = "run_lease_expired";
      auto unlimited = config;
      unlimited["max_events"] = event_limit * 8 + 32;
      dispatch_boundary(
          run, Json{{"operation", "run.abort"}, {"status", "unknown"}, {"output", nullptr}},
          unlimited);
      run["trace_path"] = trace_path(run).string();
      save(run);
    }
    const auto limit = config.num("max_stored_runs", 10000);
    if (stored_count < limit)
      return;
    const auto target = limit * 3 / 4;
    unsigned moved = 0;
    while (stored_count > target && !archivable.empty() && moved++ < 64) {
      const auto rid = *archivable.begin();
      const auto source = root / "runs" / (rid + ".json");
      const auto dest = root / "archive/runs" / rid.substr(0, 2) / (rid + ".json");
      private_directory(dest.parent_path());
      require(!fs::exists(dest) && !fs::is_symlink(source), "archive_path_conflict");
      require(::rename(source.c_str(), dest.c_str()) == 0, "run_archive_failed", 507);
      for (const auto &directory : {source.parent_path(), dest.parent_path()}) {
        const int fd = ::open(directory.c_str(), O_RDONLY | O_DIRECTORY);
        if (fd >= 0) {
          ::fsync(fd);
          ::close(fd);
        }
      }
      --stored_count;
      archivable.erase(rid);
      runtime_policies.erase(rid);
      runs.erase(rid);
      evaluation_outcomes.erase(rid);
    }
  }

  // 确保当前用户具有有效记忆状态，不覆盖已存在的经验。
  void ensure_memory(const std::string &user) {
    if (fs::exists(memory_path(user)))
      return;
    ep::EvolutionMemoryStore store;
    store.set_schema_version(1);
    store.set_store_kind("trajectory_evolution_memory");
    store.set_store_version("evolution-memory-store-v1");
    atomic_write(memory_path(user), store.SerializeAsString());
    atomic_write(user_dir(user) / "memory" / "evolution-memory.json", json(store).dump());
  }

  // 计算当前用户可见的公共经验视图，排除与私有经验冲突或已失效的条目。
  v1::SharedExperienceStore shared_view(const std::string &user) const {
    return shared_available && config["sharing"].flag("enabled", false)
               ? shared_memory_view(published_shared, user)
               : v1::SharedExperienceStore{};
  }

  // 保存公共与私有经验协同管理的检查点，支持断点恢复。
  void checkpoint_sharing(const Json &state) const {
    const auto bytes = state.dump();
    require(bytes.size() <= 268435456, "shared_ledger_capacity", 507);
    atomic_write(root / "learning/shared-memory.json", bytes);
  }

  // GEPA 消费边界同步权威公共库，不启动新的管理服务，也不把台账复制到评测目录。
  // 外置原库只在本次本地事务内申请已有服务锁；原管理器在运行则保留待投递状态，
  // 绝不等待或覆盖其内存快照。普通在线路径已由本服务持有该锁。
  void sync_feedback_sharing(const std::string &user, const ep::EvolutionMemoryStore &memory) {
    if (!config["sharing"].flag("enabled", false)) return;
    const auto authority = memory_source_root.empty() ? root : memory_source_root;
    FileLock source_lock;
    Json current = sharing_state;
    if (!memory_source_root.empty()) {
      const auto lock_path = authority / "service.lock";
      require(!fs::is_symlink(lock_path), "symlink_not_allowed");
      source_lock.fd = ::open(lock_path.c_str(), O_CREAT | O_RDWR | O_NOFOLLOW, 0600);
      require(source_lock.fd >= 0, "memory_source_lock_failed", 409);
      if (::flock(source_lock.fd, LOCK_EX | LOCK_NB) != 0)
        throw Error(errno == EWOULDBLOCK ? "memory_source_management_busy"
                                       : "memory_source_lock_failed", 409);
      current = Json::parse(read_file(authority / "learning/shared-memory.json", 268435456));
    }
    validate_shared_memory(current, config.str("agent_id"), config["sharing"]);
    reconcile_shared_feedback(current, config, user, memory);
    validate_shared_memory(current, config.str("agent_id"), config["sharing"]);
    const auto bytes = current.dump();
    require(bytes.size() <= 268435456, "shared_ledger_capacity", 507);
    // 台账先于可再生视图提交；若中途失败，重复调用只补齐视图，不重复加支持票。
    atomic_write(authority / "learning/shared-memory.json", bytes);
    const auto view = shared_memory_view(current);
    atomic_write(authority / "memory/evolution-memory.pb", view.SerializeAsString());
    atomic_write(authority / "memory/evolution-memory.json", json(view).dump());
    std::lock_guard<std::mutex> guard(mutex);
    for (auto &pair : memory_versions)
      if (json(shared_memory_view(published_shared, pair.first)).dump() !=
          json(shared_memory_view(current, pair.first)).dump())
        pair.second = std::max<std::uint64_t>(pair.second, 1) + 1;
    sharing_state = std::move(current);
    published_shared = sharing_state;
    shared_available = true;
  }

  // 后台读取整理检查点，状态查询只读这份小摘要，不扫描记忆或调用模型。
  // 兼容旧库的未消费阈值：首次升级会恢复整理，不重新评测已经完成的轨迹。
  void refresh_memory_management(bool wake_waiting = false) {
    Json summary = Json::Object{};
    std::size_t due = 0;
    std::set<std::string> users;
    for (const auto &p : principals) users.insert(p.second);
    for (const auto &user : users) {
      if (!fs::exists(memory_path(user))) continue;
      Json state;
      try {
        const fs::path path = memory_path(user).string() + ".curation.json";
        if (fs::exists(path)) {
          const auto journal = Json::parse(read_file(path, 33554432));
          require(journal.num("schema_version", 0) == 1, "memory_management_state_invalid");
          state = Json{{"status", journal.str("status", "idle")},
                       {"pending", journal.num("pending_count", 0)},
                       {"unresolved", journal.num("unresolved_count", 0)},
                       {"next_due_ms", journal.num("next_due_ms", 0)},
                       {"model_calls", journal.num("total_model_calls", 0)}};
          if (journal.flag("active", false) && !state.num("pending", 0)) state["pending"] = 1;
          if (journal.flag("commit_pending", false) || (wake_waiting &&
              (state.str("status") == "waiting_for_model" || state.str("status") == "unresolved"))) {
            state["pending"] = std::max<std::size_t>(1, state.num("pending", 0));
            state["status"] = "pending";
            state["next_due_ms"] = millis();
          }
          if (journal.flag("active", false) && state.str("status") != "waiting_for_model" && !state.num("next_due_ms", 0))
            state["next_due_ms"] = millis();
        } else {
          const auto memory = legacy::TrajectoryEvolutionPipeline::load_memory(memory_path(user), run_limit);
          const bool pending = memory.new_entries_since_full_curation() >= memory_full_threshold;
          state = Json{{"status", pending ? "legacy_pending" : "idle"}, {"pending", pending ? 1 : 0},
                       {"unresolved", 0}, {"next_due_ms", pending ? millis() : 0}};
        }
      } catch (...) {
        state = Json{{"status", "failed"}, {"error", "memory_management_state_invalid"}, {"pending", 0}};
      }
      if (memory_management_errors.has(user)) state = memory_management_errors[user];
      const auto next = state.num("next_due_ms", 0);
      if (next && (!due || next < due)) due = next;
      summary[user] = std::move(state);
    }
    std::lock_guard<std::mutex> guard(mutex);
    memory_management = std::move(summary);
    memory_due_ms = due;
  }

  // 入库和全库管理各有完成状态。即使不再有新轨迹，也会按期限续跑旧复核工作。
  // 这里在后台评测互斥区执行，不占用主 Agent 的打点锁等待远程模型。
  void run_memory_maintenance(const legacy::RuntimeConfiguration &configuration,
                              const std::shared_ptr<legacy::ModelAdapter> &model) {
    std::vector<std::string> ready;
    {
      std::lock_guard<std::mutex> guard(mutex);
      for (const auto &p : memory_management.fields())
        if (p.second.num("next_due_ms", 0) && p.second.num("next_due_ms", 0) <= millis()) ready.push_back(p.first);
    }
    legacy::TrajectoryEvolutionPipeline pipeline(configuration.evaluator, configuration.evolution, model);
    for (const auto &user : ready) {
      try {
      ep::HybridEvaluationBatchReport maintenance;
      const auto update = pipeline.curate_memory_file(maintenance, memory_path(user),
          user_dir(user) / "memory/evolution-memory.json", run_limit);
      if (update.result.revision_before() != update.result.revision_after()) {
        {
          std::lock_guard<std::mutex> guard(mutex);
          ++memory_versions[user];
          notify_work();
        }
        queue_sharing(user);
      }
      memory_management_errors.fields().erase(user);
      } catch (...) {
        // 私库文件或提交故障只隔离该用户；不能让排在前面的坏库饿死后续用户。
        const auto attempts = memory_management_errors[user].num("attempts", 0) + 1;
        memory_management_errors[user] = Json{{"status", attempts < 2 ? "retrying" : "failed"},
            {"attempts", attempts}, {"pending", attempts < 2 ? 1 : 0},
            {"next_due_ms", attempts < 2 ? millis() + schedule.num("retry_backoff_ms", 60000) : 0},
            {"error", "memory_management_processing_failed"}};
      }
    }
    refresh_memory_management();
  }

  // 只登记共享管理需求；主请求不在这里等待公共库模型审查。
  void queue_sharing(const std::string &user) {
    if (!config["sharing"].flag("enabled", false))
      return;
    enqueue_shared_memory(sharing_state, user);
    const auto ready = std::max(millis(), sharing_state.num("last_cycle_ms", 0) +
                                              config["sharing"].num("min_interval_ms", 60000));
    const auto previous = sharing_state.num("next_due_ms", 0);
    sharing_state["next_due_ms"] = previous ? std::min(previous, ready) : ready;
    checkpoint_sharing(sharing_state);
    std::lock_guard<std::mutex> guard(mutex);
    sharing_due_ms = sharing_state.num("next_due_ms", 0);
    sharing_pending_users = sharing_state["pending_users"].items().size();
    sharing_status = "pending";
    notify_work();
  }

  // 检查经验是否具备进入公共库的真实支持来源与共享资格。
  Json public_evidence(const std::string &user, const ep::EvolutionMemoryEntry &entry,
                       std::map<std::string, std::string> &training_tasks,
                       std::set<std::string> &loaded_datasets,
                       std::map<std::string, std::pair<Json, std::string>> &trace_cache) const {
    std::set<std::string> sources, training_sources, groups;
    Json review_context = Json::Array{};
    for (const auto &evidence : entry.candidate().support_evidence()) {
      try {
        const auto &origin = evidence.source();
        if (origin.user_id() != user || !identifier(origin.conversation_id()) ||
            !identifier(origin.dialogue_id()))
          continue;
        const auto id = origin.dialogue_id();
        const auto rid =
            id.size() == 40 && id.find_first_not_of("0123456789abcdef") == std::string::npos
                ? id
                : digest("import\n" + config.str("agent_id") + '\n' + user + '\n' +
                         origin.conversation_id() + '\n' + id)
                      .substr(0, 40);
        auto found_trace = trace_cache.find(rid);
        if (found_trace == trace_cache.end()) {
          auto state = read_run(rid);
          auto raw = read_file(trace_path(state), run_limit);
          found_trace = trace_cache.emplace(rid, std::make_pair(std::move(state), std::move(raw))).first;
        }
        const auto &run = found_trace->second.first;
        const bool training = run.has("training_source");
        // 冻结训练来源可以支持公共经验，不要求启动 GEPA 或任务裁判。
        // 训练只按独立题组计数，合成身份不能作为真实用户的公共支持票。
        if (training && !config["sharing"].flag("allow_training_sources", true))
          continue;
        if (!run.flag("closed", false) || run.flag("incomplete", false) ||
            run.str("evaluation_status") != "completed" || run["identity"].str("user_id") != user ||
            run["identity"].str("conversation_id") != origin.conversation_id())
          continue;
        const auto &raw = found_trace->second.second;
        const auto expected = run.flag("imported", false)
                                  ? run.str("import_evidence_fingerprint")
                                  : digest(trace_content_jsonl(proto<v1::TraceRun>(trace(run))));
        if (evidence.source_fingerprint() != expected ||
            digest(raw) != run["learning"].str("source_fingerprint"))
          continue;
        // 重复复述被注入的经验，不构成独立支持证据。
        // 保守排除绑定情况不明的旧版策略回执。
        if (raw.find("trajectory-experience") != std::string::npos ||
            raw.find("evolution.policy.receipt") != std::string::npos)
          continue;
        if (run.str("import_format") != "sessionlog") {
          const auto t = proto<v1::TraceRun>(Json::parse(raw));
          bool influenced = false;
          for (const auto &event : t.events()) {
            const auto attributes = json(event.attributes());
            influenced |= event.kind() == "advice.applied" &&
                          event.application().status() == "applied" &&
                          attributes["receipt"]["experiences"].array() &&
                          !attributes["receipt"]["experiences"].items().empty();
          }
          if (influenced)
            continue;
        }
        if (training) {
          const auto &source = run["training_source"];
          const auto version = source.str("dataset_version");
          if (!loaded_datasets.count(version)) {
            fs::path path;
            const auto registration = root / "learning/training-datasets" / (digest(version) + ".json");
            if (fs::exists(registration)) {
              const auto registered = Json::parse(read_file(registration, 65536));
              require(registered.str("dataset_version") == version, "training_dataset_mismatch");
              path = registered.str("directory");
            } else {
              // 兼容此前自动训练产生的来源；新导入无需另配策略评测器。
              fs::path settings(config.str("policy_evaluation_file"));
              if (settings.empty())
                continue;
              if (settings.is_relative())
                settings = config_path.parent_path() / settings;
              const auto evaluation = Json::parse(read_file(settings, 1048576));
              path = evaluation.str("dataset");
              if (path.is_relative())
                path = settings.parent_path() / path;
            }
            const auto dataset = read_evaluation_dataset(path);
            for (const auto &issue : dataset.issues())
              require(issue.severity() != "error", "training_dataset_invalid");
            require(dataset.dataset_version() == version, "training_dataset_mismatch");
            loaded_datasets.insert(version);
            for (const auto &task : dataset.tasks())
              if (task.split() == "train" && task.review_status() == "reviewed" &&
                  !task.group_id().empty())
                training_tasks[version + '\n' + task.task_id() + '\n' + task.task_version()] =
                    digest(version + '\n' + task.group_id());
          }
          const auto task =
              training_tasks.find(version + '\n' + source.str("task_id") + '\n' + source.str("task_version"));
          if (source.str("kind") != "baseline_training" ||
              task == training_tasks.end() ||
              !run.flag("imported", false) || run.num("level", 0) != 0 ||
              !identifier(source.str("trial_id")) ||
              source.str("report_fingerprint").size() != 64 ||
              source.str("source_evidence_fingerprint").size() != 64)
            continue;
          const auto captured = Json::parse(raw);
          bool bound = false;
          for (const auto &event : captured["events"].items())
            if (event.str("kind") == "run.start")
              bound |= event["attributes"].str("evaluation_trial_id") == source.str("trial_id");
          if (!bound)
            continue;
          const auto &accepted = captured["attributes"]["training_evaluation"];
          const bool ungraded = accepted.str("verdict") == "uncertain" &&
                                accepted.str("task_assessment_status") == "not_requested";
          if (accepted.str("verdict") != "passed" && accepted.str("verdict") != "failed" && !ungraded)
            continue;
          groups.insert(task->second);
          training_sources.insert(rid + ':' + expected);
        } else
          sources.insert(rid + ':' + expected);
        // 只为已经核验身份、版本与来源的证据生成有界审核片段。
        if (review_context.items().size() < 2 && run.str("import_format") != "sessionlog") {
          const std::set<std::string> cited(evidence.evidence_event_ids().begin(),
                                             evidence.evidence_event_ids().end());
          auto context = learning_review_context(Json::parse(raw), cited);
          // 公共审核只需事实和结果，不需能包含用户名字的 run/event 标识。
          // 原始引用仍留在私有证据及指纹中；模型用当前请求内的序号区分事实。
          std::size_t index = 0;
          for (auto &fact : context["evidence"].items()) {
            fact.fields().erase("run_id");
            fact.fields().erase("event_id");
            fact["evidence_index"] = ++index;
          }
          review_context.push(std::move(context));
        }
      } catch (...) { /* 来源不可用或不可验证时，不能据此授权共享。 */
      }
    }
    const bool online = sources.size() >= config["sharing"].num("min_source_runs", 2);
    if (!online && groups.empty())
      return Json::Object{};
    std::string material;
    for (const auto &source : sources)
      material += source + '\n';
    for (const auto &source : training_sources)
      material += source + '\n';
    Json independent = Json::Array{};
    for (const auto &group : groups)
      independent.push(group);
    return Json{{"fingerprint", digest(material + independent.dump())},
                {"online_verified", online},
                {"training_groups", independent}, {"review_context", review_context}};
  }

  // 后台处理公共/私有关系变化，保留候选来源与审查状态，失败后可按预算重试。
  void run_sharing(const std::shared_ptr<legacy::ModelAdapter> &model) {
    if (!config["sharing"].flag("enabled", false))
      return;
    if (sharing_state.num("next_due_ms", 0) > millis())
      return;
    {
      std::lock_guard<std::mutex> guard(mutex);
      sharing_status = "running";
    }
    try {
      auto settings = config;
      auto semantic_options = config["sharing"];
      // 并发和分组大小不改变已审核内容的语义，不得因调度参数调整清空关系缓存。
      for (const auto *key : {"model_parallelism", "relation_batch_size", "relation_batch_prompt_file"})
        semantic_options.fields().erase(key);
      std::string signature = semantic_options.dump();
      const auto reviewer = model ? digest(model->name() + '\0' + model->model_id())
                                  : sharing_state.str("reviewer_fingerprint");
      signature += '\0' + reviewer;
      for (const auto &part : {"distill", "review", "relation"}) {
        auto prompt = read_file(config.str(std::string("shared_") + part + "_prompt_path"), 32768);
        signature += '\0' + prompt;
        settings[std::string("shared_") + part + "_prompt"] = std::move(prompt);
      }
      if (config.has("shared_relation_batch_prompt_path"))
        settings["shared_relation_batch_prompt"] = read_file(config.str("shared_relation_batch_prompt_path"), 32768);
      signature = digest(signature);
      if (sharing_state.str("configuration") != signature) {
        for (auto &p : sharing_state["sources"].fields()) {
          p.second["state"] = "pending";
          p.second["entry_id"] = "";
          p.second["attempts"] = 0;
        }
        for (auto it = sharing_state["relations"].fields().begin();
             it != sharing_state["relations"].fields().end();) {
          // 更换审查器不能推翻已有反证。
          if (it->second.str("state") == "accepted" &&
              it->second["relation"].str("relation") == "conflict")
            ++it;
          else
            it = sharing_state["relations"].fields().erase(it);
        }
        for (const auto &p : sharing_state["users"].fields())
          enqueue_shared_memory(sharing_state, p.second.text());
        sharing_state["configuration"] = signature;
        sharing_state["reviewer_fingerprint"] = reviewer;
      }
      // 同一管理周期共享已验证语料索引和轨迹读取；下一周期重新取证，避免跨周期过期授权。
      std::map<std::string, std::string> training_tasks;
      std::set<std::string> loaded_datasets;
      std::map<std::string, std::pair<Json, std::string>> trace_cache;
      process_shared_memory(
          sharing_state, settings,
          [&](const auto &user) {
            bool authorized = false;
            for (const auto &p : principals)
              authorized |= p.second == user;
            require(authorized, "sharing_user_not_configured");
            return legacy::TrajectoryEvolutionPipeline::load_memory(memory_path(user), run_limit);
          },
          model, [&](const auto &state) { checkpoint_sharing(state); },
          [&](const auto &user, const auto &entry) { return public_evidence(user, entry, training_tasks, loaded_datasets, trace_cache); });
      sharing_state["failure_attempts"] = 0;
      checkpoint_sharing(sharing_state);
      const auto public_store = shared_memory_view(sharing_state);
      atomic_write(root / "memory/evolution-memory.pb", public_store.SerializeAsString());
      atomic_write(root / "memory/evolution-memory.json", json(public_store).dump());
      for (const auto &p : sharing_state["users"].fields()) {
        const auto view = shared_memory_view(sharing_state, p.second.text());
        Json relations = Json::Array{};
        for (const auto &r : view.relations())
          relations.push(json(r));
        atomic_write(user_dir(p.second.text()) / "memory/shared-relations.json", relations.dump());
      }
      std::lock_guard<std::mutex> guard(mutex);
      bool changed = false;
      for (auto &p : memory_versions)
        if (!shared_available || json(shared_memory_view(sharing_state, p.first)).dump() !=
                                     json(shared_memory_view(published_shared, p.first)).dump()) {
          p.second = std::max<std::uint64_t>(p.second, 1) + 1;
          changed = true;
        }
      published_shared = sharing_state;
      shared_available = true;
      sharing_due_ms = sharing_state.num("next_due_ms", 0);
      sharing_pending_users = sharing_state["pending_users"].items().size();
      sharing_status = sharing_state.str("last_status", "idle");
      if (changed)
        notify_work();
    } catch (...) {
      std::lock_guard<std::mutex> guard(mutex);
      sharing_state["failure_attempts"] = sharing_state.num("failure_attempts", 0) + 1;
      sharing_due_ms =
          sharing_state.num("failure_attempts", 0) < config["sharing"].num("max_attempts", 2)
              ? millis() + config["sharing"].num("min_interval_ms", 60000)
              : 0;
      sharing_state["next_due_ms"] = sharing_due_ms;
      sharing_pending_users = sharing_state["pending_users"].items().size();
      sharing_status = "failed";
      if (shared_available) {
        shared_available = false;
        for (auto &p : memory_versions)
          p.second = std::max<std::uint64_t>(p.second, 1) + 1;
        notify_work();
      }
      // 公共库管理失败不能把已经成功的私有学习改为失败。
      sharing_state["last_error"] = "shared_memory_processing_failed";
      try {
        checkpoint_sharing(sharing_state);
      } catch (...) {
      }
    }
  }

  // 保存当前模块的完整状态，使后台恢复或后续读取能看到一致结果。
  void save(Json &run) {
    if (run.flag("closed", false)) {
      if (!run["learning"].has("queued_at_ms"))
        run["learning"]["queued_at_ms"] = millis();
      if (!run["learning"].has("capture_bytes"))
        run["learning"]["capture_bytes"] = run.dump().size();
    }
    const auto bytes = run.dump();
    require(bytes.size() <= run_limit, "run_capacity_reached", 507);
    const auto rid = run["identity"].str("run_id");
    if (run.flag("buffered_capture", false) && !run.flag("capture_durable", false)) {
      const auto old = capture_writes.count(rid) ? capture_writes.at(rid).bytes.size() : 0;
      require(capture_bytes - old + bytes.size() <= capture_limit, "capture_buffer_full", 507);
      const auto revision = ++capture_revision;
      capture_writes[rid] = CaptureWrite{bytes, revision};
      capture_dirty[rid] = revision;
      capture_bytes = capture_bytes - old + bytes.size();
      capture_changed.notify_one();
      return;
    }
    atomic_write(run_file(rid), bytes);
    evaluation_outcomes[rid] = run.str("evaluation_status", "not_scheduled");
    if (run.flag("closed", false))
      observe_queue(run);
  }

  // 将采集队列落盘；持久化失败必须暴露采集缺口，不能继续宣称日志完整。
  void write_capture() {
    std::unique_lock<std::mutex> guard(mutex);
    for (;;) {
      capture_changed.wait(guard, [&] { return capture_stopping || !capture_writes.empty(); });
      if (capture_writes.empty()) {
        if (capture_stopping)
          return;
        else
          continue;
      }
      const auto rid = capture_writes.begin()->first;
      auto item = std::move(capture_writes.begin()->second);
      capture_writes.erase(capture_writes.begin());
      const bool incomplete = capture_poisoned.count(rid);
      guard.unlock();
      bool saved = false;
      Json snapshot;
      try {
        snapshot = Json::parse(item.bytes);
        if (incomplete) {
          snapshot["incomplete"] = true;
          if (snapshot.flag("closed", false))
            snapshot["evaluation_status"] = "quarantined";
        }
        if (snapshot.flag("closed", false))
          snapshot["capture_durable"] = true;
        atomic_write(root / "runs" / (rid + ".json"), snapshot.dump());
        saved = true;
      } catch (...) {
      }
      guard.lock();
      capture_bytes -= item.bytes.size();
      if (!saved) {
        // 保留最新快照并在写入线程重试，不在业务调用方重试。
        if (!capture_writes.count(rid)) {
          capture_bytes += item.bytes.size();
          capture_writes[rid] = std::move(item);
        }
        if (capture_stopping)
          return;
        capture_changed.wait_for(guard, std::chrono::milliseconds(1000),
                                 [&] { return capture_stopping; });
        continue;
      }
      if (capture_poisoned.count(rid) && !snapshot.flag("incomplete", false) &&
          !capture_writes.count(rid)) {
        // 被拒绝的回调可能与磁盘写入并发发生；
        // 在向任何后台评测器暴露该运行前，必须先持久化隔离标记。
        capture_bytes += item.bytes.size();
        capture_writes[rid] = std::move(item);
        continue;
      }
      if (capture_dirty.at(rid) == item.revision) {
        capture_dirty.erase(rid);
        auto current = runs.find(rid);
        if (current != runs.end()) {
          if (capture_poisoned.count(rid)) {
            current->second["incomplete"] = true;
            if (snapshot.flag("closed", false))
              current->second["evaluation_status"] = "quarantined";
          }
          current->second["capture_durable"] = snapshot.flag("closed", false);
          if (snapshot.flag("closed", false))
            observe_queue(current->second);
        }
        evaluation_outcomes[rid] = snapshot.str("evaluation_status", "not_scheduled");
        if (snapshot.flag("closed", false))
          capture_poisoned.erase(rid);
      }
      capture_changed.notify_all();
    }
  }

  // 将内部时间转换为毫秒，统一后台调度与报告单位。
  static std::size_t millis() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
  }

  // 通知后台出现待处理工作，避免空闲时频繁轮询。
  void notify_work() {
    ++work_generation;
    work_changed.notify_all();
  }

  // 记录队列变化供后台调度，控制重复提交和积压。
  void observe_queue(const Json &run) {
    const auto rid = run["identity"].str("run_id"), state = run.str("evaluation_status");
    if (state != "pending" && state != "running" &&
        run["learning"].flag("trace_published", false) &&
        fs::exists(root / "runs" / (rid + ".json")))
      archivable.insert(rid);
    else
      archivable.erase(rid);
    if (!run["learning"].flag("trace_published", false) &&
        !run["learning"].has("publication_error")) {
      if (unpublished.insert(rid).second)
        notify_work();
    } else
      unpublished.erase(rid);
    if (state == "pending" || state == "running") {
      Json item{{"user", run["identity"]["user_id"]},
                {"state", state},
                {"queued_at_ms", run["learning"].num("queued_at_ms", millis())},
                {"bytes", run["learning"].num("capture_bytes", run_limit)},
                {"retry_after_ms", run["learning"].num("retry_after_ms", 0)},
                {"job_id", run["learning"].str("job_id")}};
      if (!learning_queue.count(rid) || learning_queue.at(rid).dump() != item.dump()) {
        learning_queue[rid] = std::move(item);
        notify_work();
      }
    } else if (learning_queue.erase(rid))
      notify_work();
  }

  // 载入运行状态并验证归属，不能把其他用户的运行作为当前上下文。
  Json read_run(const std::string &rid) const {
    require(rid.size() == 40 && rid.find_first_not_of("0123456789abcdef") == std::string::npos,
            "stored_run_id_invalid");
    auto run = Json::parse(read_file(run_file(rid), run_limit));
    require(run.str("kind") == "integration-run-v1" && run["identity"].str("run_id") == rid &&
                run["identity"].str("agent_id") == config.str("agent_id"),
            "stored_state_invalid");
    return run;
  }

  // 持久化已关闭运行及其最终轨迹，保证后台评测读取的是完整收尾数据。
  void save_closed(Json &run) {
    save(run);
    const auto rid = run["identity"].str("run_id");
    if (runs.count(rid))
      runs[rid] = run;
  }

  // 按作业身份定位持久记录，避免不同用户的作业互相覆盖。
  fs::path job_path(const std::string &id) const {
    require(id.size() == 64 && id.find_first_not_of("0123456789abcdef") == std::string::npos,
            "learning_job_invalid");
    return root / "learning/jobs" / (id + ".json");
  }

  // 载入并校验后台作业记录，区分不存在、损坏与有效待处理状态。
  Json read_job(const std::string &id) const {
    auto job = Json::parse(read_file(job_path(id), 262144));
    require(job.num("schema_version", 0) == 1 && job.str("id") == id &&
                identifier(job.str("user")) && job["runs"].array() &&
                !job["runs"].items().empty() && job["runs"].items().size() <= 256,
            "learning_job_invalid");
    std::set<std::string> unique;
    for (const auto &rid : job["runs"].items())
      require(unique.insert(rid.text()).second, "learning_job_duplicate_run");
    return job;
  }

  // 持久化后台作业状态，为进程重启恢复和幂等处理提供依据。
  void save_job(const Json &job) {
    atomic_write(job_path(job.str("id")), job.dump());
  }

  // 按候选身份定位待评策略文件，保持版本和作业关联。
  fs::path candidate_path(const Json &job) const {
    return root / "learning/jobs" / (job.str("id") + ".candidates.pb");
  }

  // 验证当前认证作用域是否拥有该运行，禁止跨用户读取或写入。
  bool owns_run(const Json &job, const Json &run) const {
    const auto current = run["learning"].str("job_id");
    return current.empty() ? run["learning"].num("retry_count", 0) ==
                                 job["retry_counts"].num(run["identity"].str("run_id"), 0)
                           : current == job.str("id");
  }

  // 为暂时网络失败及模型响应的格式、枚举、证据引用错误安排有界重试。
  // 合法的业务失败结论不是协议错误；鉴权、隐私、输入/输出限额和不确定中断不在此名单中。
  // 不把无效输出降级为成功，也不重跑已经完成的其他轮次。旧作业保留原失败证据，
  // 新作业只携带失败运行。次数与期限一起落盘，重启不重置预算。
  bool queue_analysis_retry(Json &run, const Json &job) {
    const auto id = run["identity"].str("run_id");
    const auto error = job["errors"].str(id);
    static const std::set<std::string> transient{
        "dialogue_model_http_408", "dialogue_model_http_429", "dialogue_model_http_500",
        "dialogue_model_http_502", "dialogue_model_http_503", "dialogue_model_http_504",
        "dialogue_model_transport_6", "dialogue_model_transport_7",
        "dialogue_model_transport_28", "dialogue_model_transport_52",
        "dialogue_model_transport_55", "dialogue_model_transport_56",
        "dialogue_model_invalid_json", "dialogue_model_missing_dialogue_section",
        "dialogue_model_invalid_task_outcome",
        "dialogue_model_episode_assessment_count_mismatch",
        "dialogue_model_invalid_episode_assessment",
        "dialogue_model_unknown_or_duplicate_episode_id",
        "dialogue_model_invalid_episode_verdict", "dialogue_model_invalid_episode_confidence",
        "dialogue_model_invalid_episode_state_change", "dialogue_model_invalid_episode_concise_reason",
        "dialogue_model_invalid_episode_evidence_event_ids",
        "dialogue_model_invalid_reflection", "dialogue_model_invalid_reflection_summary",
        "dialogue_model_invalid_reflection_root_causes",
        "dialogue_model_invalid_reflection_skill_orchestration_changes",
        "dialogue_model_invalid_reflection_stop_condition_changes",
        "dialogue_model_invalid_reflection_wasted_episode_ids",
        "dialogue_model_invalid_reflection_evidence_event_ids",
        "dialogue_model_unknown_wasted_episode_id"};
    auto &learning = run["learning"];
    if (!config.flag("auto_evaluate", false) || job.str("state") != "completed" ||
        !transient.count(error) || learning.str("analysis_state") == "completed" ||
        learning.num("analysis_retry_count", 0) >= schedule.num("max_analysis_retries", 0))
      return false;
    const auto attempt = learning.num("analysis_retry_count", 0) + 1;
    const auto delay = std::min<std::size_t>(
        86400000, schedule.num("retry_backoff_ms", 60000) * (std::size_t{1} << (attempt - 1)));
    learning["analysis_retry_count"] = attempt;
    learning["retry_count"] = learning.num("retry_count", 0) + 1;
    learning["retry_after_ms"] = millis() + delay;
    learning["last_analysis_error"] = error;
    learning["analysis_state"] = "retry_authorized";
    learning["phase"] = "analysis_retry_wait";
    learning.fields().erase("job_id");
    run["evaluation_status"] = "pending";
    run["evaluation_error"] = "";
    return true;
  }

  // 记录作业终态和必要结果索引，释放队列占用并保留失败原因。
  void finish_job(const Json &job) {
    for (const auto &id : job["runs"].items()) {
      auto run = read_run(id.text());
      if (!owns_run(job, run))
        continue;
      run["learning"]["job_id"] = job["id"];
      const auto error = job["errors"].str(id.text(), job.str("error"));
      run["evaluation_status"] = error.empty() ? "completed" : "failed";
      run["evaluation_error"] = error;
      run["learning"]["phase"] = error.empty() ? "completed" : "failed";
      // 已收尾的失败作业不能仍显示分析运行中；若分析已经完成、仅后续
      // 反思或入库失败，则保留完成标记，重试继续复用原分析报告。
      if (!error.empty() && run["learning"].str("analysis_state") == "running")
        run["learning"]["analysis_state"] = "failed";
      queue_analysis_retry(run, job);
      save_closed(run);
    }
  }

  // 恢复中断的后台作业，检查已有产物与状态，避免重复导入已完成结果。
  void recover_jobs() {
    std::size_t count = 0;
    for (const auto &entry : fs::directory_iterator(root / "learning/jobs")) {
      if (entry.path().extension() != ".json")
        continue;
      auto job = read_job(entry.path().stem().string());
      // 归档历史用于回放，不计入在线队列容量。
      if (job.str("state") != "completed" && job.str("state") != "failed")
        require(++count <= config.num("max_stored_runs", 10000), "learning_job_limit_reached");
      for (const auto &id : job["runs"].items()) {
        auto run = read_run(id.text());
        require(run.flag("closed", false) && run["identity"].str("user_id") == job.str("user"),
                "learning_job_identity_mismatch");
        if (!owns_run(job, run))
          continue;
        const auto previous_job = run["learning"].str("job_id");
        run["learning"]["job_id"] = job["id"];
        if (job.str("state") == "completed" || job.str("state") == "failed") {
          const auto error = job["errors"].str(id.text(), job.str("error"));
          const auto expected = error.empty() ? "completed" : "failed";
          // 兼容旧记录中外层已失败、内部仍运行的状态；仅修复终态，
          // 不重新排队或调用模型，重试仍通过 evaluation.retry 显式触发。
          const bool analysis_failed =
              !error.empty() && run["learning"].str("analysis_state") == "running";
          if (queue_analysis_retry(run, job)) {
            save_closed(run);
            continue;
          }
          if (run.str("evaluation_status") == expected && previous_job == job.str("id") &&
              !analysis_failed)
            continue;
          run["evaluation_status"] = expected;
          run["evaluation_error"] = error;
          run["learning"]["phase"] = expected;
          if (analysis_failed)
            run["learning"]["analysis_state"] = "failed";
        } else {
          run["evaluation_status"] = "pending";
        }
        save_closed(run);
      }
    }
    evict_finished();
  }

  // 在可处理的用户和作业组之间按先进先出调度；
  // 冻结重试批次不混入新数据，低流量用户最终也会按等待时间获得处理。
  // 按新增日志与时间预算选择学习批次，跳过已处理数据，避免每个请求触发一次进化。
  std::vector<std::string> select_batch(bool flush, std::size_t *due = nullptr) const {
    const auto now = millis();
    std::map<std::string, std::vector<std::string>> groups;
    std::size_t earliest = 0, selected_age = 0;
    for (const auto &pair : learning_queue)
      if (pair.second.str("state") == "pending" && !unpublished.count(pair.first)) {
        const auto retry_at = pair.second.num("retry_after_ms", 0);
        // 重试中的一轮不阻塞同用户的新数据；即使离线 flush，也遵守退避。
        if (retry_at > now) {
          if (!earliest || retry_at < earliest)
            earliest = retry_at;
          continue;
        }
        groups[pair.second.str("user") + '\0' + pair.second.str("job_id")].push_back(pair.first);
      }
    std::vector<std::string> selected;
    for (auto &group : groups) {
      auto &ids = group.second;
      std::sort(ids.begin(), ids.end(), [&](const auto &a, const auto &b) {
        const auto x = learning_queue.at(a).num("queued_at_ms", now),
                   y = learning_queue.at(b).num("queued_at_ms", now);
        return x != y ? x < y : a < b;
      });
      const auto &first = learning_queue.at(ids.front());
      const auto age = std::min(now, first.num("queued_at_ms", now));
      const bool retry_ready = std::any_of(ids.begin(), ids.end(), [&](const auto &id) {
        return learning_queue.at(id).num("retry_after_ms", 0) != 0;
      });
      const auto ready =
          flush || retry_ready || !first.str("job_id").empty() ||
                  ids.size() >= schedule.num("min_new_runs", 20)
              ? now
              : age + schedule.num("max_wait_ms", 600000);
      const auto at =
          std::max(ready, flush ? storage_retry_ms : std::max(next_batch_ms, storage_retry_ms));
      if (!earliest || at < earliest)
        earliest = at;
      if (at <= now && (selected.empty() || age < selected_age)) {
        selected = ids;
        selected_age = age;
      }
    }
    if (due)
      *due = earliest;
    if (!selected.empty() && learning_queue.at(selected.front()).str("job_id").empty()) {
      std::size_t bytes = 0, count = 0;
      for (const auto &id : selected) {
        const auto size = learning_queue.at(id).num("bytes", run_limit);
        if (count && (count >= schedule.num("max_batch_runs", 32) ||
                      bytes + size > schedule.num("max_batch_bytes", 33554432)))
          break;
        bytes += size;
        ++count;
      }
      selected.resize(count);
    }
    return selected;
  }

  // 将当前事实写为标准事件，保留身份和因果链接，不把建议冒充执行。
  std::string emit(Json &run, const std::string &kind, Json fields, const std::string &producer,
                   Json causes = Json::Array{}) {
    require(run["events"].items().size() < event_limit, "event_capacity_reached", 507);
    const auto id = "event-" + std::to_string(run["events"].items().size() + 1);
    const auto sequence = run["sequences"].num(producer, 0) + 1;
    run["sequences"][producer] = sequence;
    fields["event_id"] = id;
    fields["producer_id"] = producer;
    fields["sequence"] = std::to_string(sequence);
    fields["timestamp_us"] = now_us();
    fields["kind"] = kind;
    if (!fields.has("actor_id"))
      fields["actor_id"] = "task";
    if (!causes.items().empty())
      fields["caused_by"] = std::move(causes);
    run["events"].push(std::move(fields));
    return id;
  }

  // 构造当前运行或步骤的规范上下文，后续事件复用同一身份。
  Json context(const Json &step) const {
    return {{"actor_id", step["actor_id"]},
            {"execution_id", step["execution_id"]},
            {"step_id", step["step_id"]},
            {"loop_id", step["execution_id"]},
            {"iteration", step["iteration"]}};
  }

  // 从已记录事件导出标准轨迹，保留实际终态与完整性信息。
  Json trace(const Json &run) const {
    return boundary_trace(run);
  }

  // 基于当前观测和冻结策略查询建议或控制决定，查询成功不等于真实执行。
  v1::RuntimeQuery query(const Json &run, const Json &step, const std::string &boundary,
                         const std::string &id) const {
    v1::RuntimeQuery q;
    *q.mutable_advice_query()->mutable_identity() = proto<v1::Identity>(run["identity"]);
    q.mutable_advice_query()->set_query_id("query-" + id);
    q.mutable_advice_query()->set_decision_id("decision-" + id);
    q.mutable_advice_query()->set_target_scope(scope(step.str("actor_id")));
    if (run.num("level", 0) > 0) {
      q.mutable_advice_query()->set_task(bounded_query_text(run["input"], config, true));
      q.mutable_advice_query()->set_state(bounded_query_text(step["state"], config, false));
    }
    q.set_execution_id(step.str("execution_id"));
    q.set_boundary(boundary);
    return q;
  }

  // 记录策略查询及其依据，但不把查询命中视为实际应用。
  void record_query(Json &run, const Json &step, const v1::RuntimeDecision &result) {
    Json fields = context(step);
    fields["attributes"] = Json{{"type", "policy.query"}, {"decision", json(result)}};
    emit(run, "custom", fields, step.str("execution_id"));
  }

  // Restore only actual prepared native queries. This retains the original
  // receipt identity across process restart or a failed persistence attempt;
  // it neither creates a business action nor edits its result.
  const PolicyRuntime *runtime_policy(const Json &run) {
    if (!config.flag("preserve_runtime_policy_state", false) ||
        run.num("level", 0) == 0 || !run["policy"].object())
      return nullptr;
    const auto rid = run["identity"].str("run_id");
    auto policy = proto<v1::RuntimePolicy>(run["policy"]);
    auto found = runtime_policies.find(rid);
    if (found != runtime_policies.end() &&
        google::protobuf::util::MessageDifferencer::Equals(found->second->policy(), policy))
      return found->second.get();
    auto runtime = std::make_unique<PolicyRuntime>(std::move(policy));
    for (const auto &event : run["events"].items()) {
      const auto &decision = event["attributes"]["decision"];
      if (event.str("kind") == "custom" &&
          event["attributes"].str("type") == "policy.query" &&
          decision.str("policy_id") == runtime->policy().policy_id() &&
          decision["query"].str("boundary") == "llm")
        runtime->decide(proto<v1::RuntimeQuery>(decision["query"]));
    }
    auto *result = runtime.get();
    runtime_policies[rid] = std::move(runtime);
    return result;
  }

  // 构造或解析标准回执，使宿主可以区分建议、应用、降级和业务结果。
  Json receipt(Json &run, const Json &step, const Json &stored_query,
               const v1::RuntimeObservation &observation) {
    if (stored_query.null())
      return Json::Object{};
    PolicyRuntime runtime(proto<v1::RuntimePolicy>(run["policy"]));
    const auto result = runtime.verify(proto<v1::RuntimeQuery>(stored_query), observation);
    auto fields = context(step);
    fields["application"] = json(result.application());
    fields["attributes"] = Json{{"receipt", json(result)}};
    const auto event_id = emit(run, "advice.applied", fields, step.str("execution_id"));
    run["receipt_counts"][result.application().status()] =
        run["receipt_counts"].num(result.application().status(), 0) + 1;
    auto summary = json(result);
    summary["event_id"] = event_id;
    return summary;
  }

  // 检查当前上下文是否仍有待结束工作，父节点据此决定能否收尾。
  bool pending(const Json &run, const std::string &step_id) const {
    for (auto key : {"models", "actions"})
      for (const auto &pair : run[key].fields())
        if (pair.second.str("step_id") == step_id && !ended(pair.second))
          return true;
    return false;
  }

  // 恢复当前执行实例的上下文，防止并发分支共享局部状态。
  Json execution_context(const Json &run, const std::string &execution) const {
    return Json{{"run_id", run["identity"]["run_id"]},
                {"execution_id", execution},
                {"actor_id", run["executions"][execution]["actor_id"]}};
  }

  // 只关闭指定动作；一个动作失败不能直接关闭同一 run 中的独立并发分支。
  void finish_action(Json &run, const std::string &call, const std::string &status,
                     const Json &output, bool executed, const Json &error = Json{},
                     Json causes = Json::Array{}, bool uncertain = false) {
    auto &item = run["actions"][call];
    const auto &step = run["steps"][item.str("step_id")];
    auto fields = context(step);
    fields["action_id"] = call;
    fields["status"] = status;
    fields["output"] = output;
    fields["attributes"] = Json{
        {"executed", executed}, {"host_action_id", item["id"]}, {"decision", item["decision"]}};
    if (item.has("delegate_execution_id")) {
      fields["attributes"]["delegate_execution_id"] = item["delegate_execution_id"];
      fields["attributes"]["completion"] = item["completion"];
    }
    if (uncertain)
      fields["attributes"].fields().erase("executed");
    if (!error.null())
      fields["error"] = error;
    causes.push(item["start_event"]);
    item["end_event"] = emit(run, "action.end", fields, call, causes);
    item["ended"] = true;
    item["executed"] = executed;
    auto &batch = run["batches"][item.str("batch_id")];
    bool complete = true;
    if (uncertain)
      batch["uncertain"] = true;
    v1::RuntimeObservation observed;
    for (const auto &id : batch["ids"].items()) {
      const auto &action = run["actions"][id.text()];
      complete &= ended(action);
      if (action.flag("executed", false))
        observed.add_executed_action_ids(id.text());
      else
        observed.add_skipped_action_ids(id.text());
    }
    if (complete && !batch.flag("observed", false)) {
      receipt(run, step, batch["query"],
              batch.flag("uncertain", false) ? v1::RuntimeObservation{} : observed);
      batch["observed"] = true;
    }
    run["observed_boundaries"]["actions"] = true;
  }

  // 汇总单个执行分支的终态，并保留与父任务的关联；不是整个会话的最终判定。
  Json finish_execution(Json &run, const std::string &execution, const std::string &status,
                        const Json &output, bool interrupted) {
    auto &exec = run["executions"][execution];
    require(!ended(exec), "execution_already_ended", 409);
    if (!interrupted)
      for (const auto &pair : run["executions"].fields())
        require(pair.second.str("parent_execution_id") != execution || ended(pair.second),
                "execution_has_pending_children", 409);
    // 接收进程中断意味着在途结果未知，不是成功跳过。
    // 只关闭当前执行，后代处理范围由调用方明确选择。
    if (interrupted) {
      run["incomplete"] = true;
      for (auto key : {"models", "actions"})
        for (auto &pair : run[key].fields()) {
          auto &item = pair.second;
          const auto &step = run["steps"][item.str("step_id")];
          if (ended(item) || step.str("execution_id") != execution)
            continue;
          if (std::string(key) == "actions") {
            finish_action(run, pair.first, "unknown", Json{}, false,
                          error_info("execution_interrupted"), Json::Array{}, true);
          } else {
            auto fields = context(step);
            fields["action_id"] = pair.first;
            fields["status"] = "unknown";
            fields["error"] = error_info("execution_interrupted");
            item["end_event"] =
                emit(run, "action.end", fields, pair.first, Json::Array{item["start_event"]});
            item["ended"] = true;
            receipt(run, step, item["query"], v1::RuntimeObservation{});
          }
        }
    }
    for (auto &pair : run["steps"].fields())
      if (pair.second.str("execution_id") == execution && !ended(pair.second))
        end_step(run, pair.second, status, output);
    Json causes = Json::Array{};
    if (exec.has("step_id"))
      causes.push(run["steps"][exec.str("step_id")]["end_event"]);
    else if (exec.has("parent_call_id"))
      causes.push(run["actions"][exec.str("parent_call_id")]["start_event"]);
    auto fields =
        Json{{"actor_id", exec["actor_id"]},
             {"execution_id", execution},
             {"status", status},
             {"output", output},
             {"attributes", Json{{"type", "execution.end"}, {"interrupted", interrupted}}}};
    exec["end_event"] = emit(run, "custom", fields, execution, causes);
    exec["ended"] = true;
    exec["status"] = status;
    exec["output"] = output;
    if (exec.has("parent_call_id") &&
        run["actions"][exec.str("parent_call_id")].str("completion", "execution") == "execution") {
      const auto call = exec.str("parent_call_id");
      finish_action(run, call, status, output, exec.num("iteration", 0) > 0,
                    interrupted ? error_info("execution_interrupted") : Json{},
                    Json::Array{exec["end_event"]}, interrupted);
    }
    return Json{{"ended", true}, {"event_id", exec["end_event"]}};
  }

  // 接收宿主实际采用的循环决定，确认规则应用而不只是返回建议。
  void accept_loop(Json &run, Json &step, const std::string &decision) {
    if (step.flag("loop_observed", false))
      return;
    require(decision == step.str("decision"), "loop_decision_mismatch", 409);
    auto fields = context(step);
    fields["decision"] = decision;
    fields["reason"] = step.str("reason", "host_step_boundary");
    fields["state_before"] = step.str("fingerprint");
    fields["state_after"] = step.str("fingerprint");
    emit(run, "loop.decision", fields, step.str("execution_id"));
    step["loop_observed"] = true;
    if (!step["loop_query"].null()) {
      if (decision == "replan")
        return;
      v1::RuntimeObservation observation;
      observation.set_loop_decision(decision);
      receipt(run, step, step["loop_query"], observation);
    }
  }

  // 记录当前步骤的真实输出和结束状态，保留与开始事件的对应关系。
  void end_step(Json &run, Json &step, const std::string &status, Json output) {
    require(!ended(step) && !pending(run, step.str("step_id")), "step_has_pending_calls", 409);
    if (!step.flag("loop_observed", false)) {
      // 只有宿主明确确认，才能声称停止策略已经应用。
      if (step.str("decision") == "continue")
        accept_loop(run, step, "continue");
      else if (step.str("decision") == "stop")
        receipt(run, step, step["loop_query"], v1::RuntimeObservation{});
    }
    // Replan is applied only when the following model activity sends the
    // proposed recovery context.  Ending a step must not manufacture an L3
    // success receipt from the decision alone.
    auto fields = context(step);
    fields["status"] = status;
    fields["output"] = std::move(output);
    Json causes = Json::Array{};
    for (auto key : {"models", "actions"})
      for (const auto &pair : run[key].fields())
        if (pair.second.str("step_id") == step.str("step_id") && pair.second.has("end_event"))
          causes.push(pair.second["end_event"]);
    // 契约最多允许 64 个直接原因，用显式汇聚链
    // 保留所有动作因果，不虚构兄弟任务之间的顺序。
    while (causes.items().size() > 64) {
      Json part = Json::Array{};
      for (unsigned i = 0; i < 64; ++i) {
        part.push(causes.items().back());
        causes.items().pop_back();
      }
      auto join = context(step);
      join["attributes"] = Json{{"type", "actions.join"}};
      causes.push(emit(run, "custom", join, step.str("execution_id"), part));
    }
    step["end_event"] = emit(run, "step.end", fields, step.str("execution_id"), causes);
    step["ended"] = true;
  }

  // 绑定用户、会话、运行身份和固定策略版本；后续事件必须沿用这组归属。
  Json start_run(const std::string &user, const Json &request, const std::string &rid) {
    keys(request, {"operation", "request_id", "conversation_id", "input", "level", "capabilities"});
    require(identifier(request.str("conversation_id")) && request.has("input"),
            "task_identity_or_input_missing");
    const int requested = level(request.str("level", "L0"));
    require(requested <= level(config.str("max_level")), "level_not_authorized", 403);
    std::set<std::string> caps;
    for (const auto &cap : request["capabilities"].items()) {
      require(cap.text() == "model" || cap.text() == "actions" || cap.text() == "loop" ||
                  cap.text() == "schedule",
              "capability_not_supported");
      caps.insert(cap.text());
    }
    if (requested > 0)
      require(caps.count(std::vector<std::string>{"model", "actions", "loop"}[requested - 1]) ||
                  (requested == 2 && caps.count("schedule")),
              "required_boundary_missing", 409);
    maintain_runs();
    std::size_t active = 0;
    for (const auto &pair : runs)
      active += !pair.second.flag("closed", false);
    require(active < active_limit && stored_count < config.num("max_stored_runs", 10000),
            "service_capacity_reached", 507);
    Json run{{"kind", "integration-run-v1"},
             {"identity", Json{{"agent_id", config["agent_id"]},
                               {"user_id", user},
                               {"conversation_id", request["conversation_id"]},
                               {"run_id", rid}}},
             {"input", request["input"]},
             {"level", requested},
             {"capabilities", request["capabilities"]},
             {"closed", false},
             {"events", Json::Array{}},
             {"sequences", Json::Object{}},
             {"executions", Json::Object{}},
             {"steps", Json::Object{}},
             {"models", Json::Object{}},
             {"actions", Json::Object{}},
             {"batches", Json::Object{}},
             {"requests", Json::Object{}},
             {"receipt_counts", Json::Object{}},
             {"observed_boundaries", Json::Object{}}};
    if (!config.has("offline_version"))
      ensure_memory(user);
    if (config.has("offline_version")) {
      std::vector<std::string> scopes;
      for (const auto &actor : config["actors"].items())
        scopes.push_back(scope(actor.text()));
      apply_agent_version(run, config, approved_versions[user], nullptr, scopes);
    } else if (requested > 0) {
      auto shadow = proto<v1::ShadowOptions>(config["shadow_options"]);
      shadow.clear_target_scopes();
      for (const auto &actor : config["actors"].items())
        shadow.add_target_scopes(scope(actor.text()));
      const auto memory = legacy::TrajectoryEvolutionPipeline::load_memory(
          memory_path(user), shadow.max_memory_bytes());
      auto snapshot = compile_shadow_policy(memory, proto<v1::Identity>(run["identity"]), shadow,
                                            shared_view(user));
      auto options = proto<v1::RuntimeOptions>(config["runtime_options"]);
      options.set_requested_level(proto_level(requested));
      options.set_fallback("strict");
      options.clear_allowed_users();
      options.add_allowed_users(user);
      options.clear_allowed_conversations();
      options.add_allowed_conversations(request.str("conversation_id"));
      options.clear_host_capabilities();
      for (const auto &pair :
           std::vector<std::pair<std::string, std::string>>{{"model", "context.request"},
                                                            {"actions", "action.filter"},
                                                            {"loop", "loop.decide"},
                                                            {"schedule", "schedule.dispatch"}})
        if (caps.count(pair.first))
          options.add_host_capabilities(pair.second);
      if (config.flag("auto_approve_experiences", false)) {
        options.clear_approved_experiences();
        for (const auto &experience : snapshot.experiences()) {
          auto *approval = options.add_approved_experiences();
          approval->set_memory_id(experience.memory_id());
          approval->set_content_revision(experience.content_revision());
        }
      }
      const auto current = compile_runtime_policy(snapshot, options);
      apply_agent_version(run, config, approved_versions[user], &current);
    } else
      apply_agent_version(run, config, approved_versions[user]);
    Json fields{{"input", request["input"]}};
    if (run.has("agent_version_id"))
      fields["attributes"] = Json{{"agent_version_id", run["agent_version_id"]}};
    if (run.has("evaluation_trial_id"))
      fields["attributes"]["evaluation_trial_id"] = run["evaluation_trial_id"];
    emit(run, "run.start", fields, "task");
    return run;
  }

  // 解释公开打点协议；宿主代码只提供边界事实，不需要重写本服务的内部状态机。
  Json dispatch(Json &run, const Json &request) {
    const auto op = request.str("operation"), req_id = request.str("request_id");
    const auto id = digest(run["identity"].str("run_id") + ":" + req_id).substr(0, 28);
    const int active_level = run.num("level", 0);
    if (op == "task.status") {
      keys(request, {"operation", "request_id", "run_id"});
      return Json{{"run_id", run["identity"]["run_id"]},
                  {"closed", run["closed"]},
                  {"level", "L" + std::to_string(active_level)},
                  {"capture_complete", !run.flag("incomplete", false)},
                  {"event_count", run["events"].items().size()},
                  {"evaluation_status", run.str("evaluation_status", "not_scheduled")},
                  {"observed_boundaries", run["observed_boundaries"]},
                  {"receipts", run["receipt_counts"]},
                  {"evaluation_error", run.str("evaluation_error")}};
    }
    if (op == "evaluation.retry") {
      keys(request, {"operation", "request_id", "run_id"});
      require(run.flag("closed", false) && run.str("evaluation_status") == "failed",
              "evaluation_not_retryable", 409);
      run["learning"].fields().erase("publication_error");
      run["learning"]["retry_count"] = run["learning"].num("retry_count", 0) + 1;
      const auto job_id = run["learning"].str("job_id");
      if (!job_id.empty()) {
        auto job = read_job(job_id);
        if (job.str("state") == "failed" && !job["errors"].has(run["identity"].str("run_id"))) {
          job["state"] = "pending";
          job["error"] = "";
          job["resume_authorized"] = true;
          save_job(job);
          for (const auto &id : job["runs"].items()) {
            if (job["errors"].has(id.text()) || id.text() == run["identity"].str("run_id"))
              continue;
            auto other = read_run(id.text());
            if (other["learning"].str("job_id") != job_id)
              continue;
            other["evaluation_status"] = "pending";
            other["evaluation_error"] = "";
            save_closed(other);
          }
        } else {
          run["learning"].fields().erase("job_id");
          if (run["learning"].str("analysis_state") != "completed")
            run["learning"]["analysis_state"] = "retry_authorized";
        }
      }
      run["evaluation_status"] = "pending";
      run["evaluation_error"] = "";
      return Json{{"evaluation_status", "pending"}};
    }
    if (op == "execution.status") {
      keys(request, {"operation", "request_id", "run_id", "execution_id"});
      const auto execution = request.str("execution_id");
      require(run["executions"].has(execution), "execution_not_found", 404);
      const auto &exec = run["executions"][execution];
      const bool closed = ended(exec) || run.flag("closed", false);
      return Json{
          {"context", execution_context(run, execution)},
          {"ended", closed},
          {"status",
           exec.str("status",
                    closed ? "unknown" : (exec.num("iteration", 0) == 0 ? "queued" : "running"))},
          {"output", exec["output"]},
          {"event_id", exec.str("end_event")}};
    }
    require(!run.flag("closed", false), "task_already_closed", 409);
    if (op == "execution.end" || op == "execution.abort") {
      keys(request, {"operation", "request_id", "run_id", "execution_id", "status", "output"});
      const auto execution = request.str("execution_id"), status = terminal(request);
      require(run["executions"].has(execution), "execution_not_found", 404);
      require(!ended(run["executions"][execution]), "execution_already_ended", 409);
      const bool interrupted = op == "execution.abort";
      if (!interrupted) {
        require(run["executions"][execution].num("iteration", 0) > 0, "execution_not_started", 409);
        return finish_execution(run, execution, status, request["output"], false);
      }
      require(status == "cancelled" || status == "failed" || status == "unknown",
              "abort_status_invalid");
      // 迭代式后序遍历避免深层 Agent 委派触发递归深度限制。
      std::vector<std::string> descendants{execution};
      for (std::size_t i = 0; i < descendants.size(); ++i)
        for (const auto &pair : run["executions"].fields())
          if (!ended(pair.second) && pair.second.str("parent_execution_id") == descendants[i])
            descendants.push_back(pair.first);
      for (auto it = descendants.rbegin(); it != descendants.rend(); ++it)
        finish_execution(run, *it, status, request["output"], true);
      return Json{{"ended", true}, {"event_id", run["executions"][execution]["end_event"]}};
    }
    if (op == "capture.gap") {
      keys(request, {"operation", "request_id", "run_id", "reason", "error_code"});
      const auto reason = request.str("reason");
      require(reason == "observer_unavailable" || reason == "privacy_rejected" ||
                  reason == "missing_boundary",
              "gap_reason_invalid");
      Json attributes{{"type", "capture.gap"}, {"reason", reason}};
      if (request.has("error_code")) {
        const auto code = request.str("error_code");
        require(!code.empty() && code.size() <= 96 && code.front() >= 'a' && code.front() <= 'z' &&
                    code.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_") ==
                        std::string::npos,
                "gap_error_code_invalid");
        attributes["error_code"] = code;
      }
      run["incomplete"] = true;
      emit(run, "custom", Json{{"attributes", attributes}}, "capture");
      return Json{{"capture_complete", false}};
    }
    if (op == "step.begin") {
      keys(request,
           {"operation", "request_id", "run_id", "execution_id", "actor_id", "state", "caused_by"});
      const auto execution = request.str("execution_id", "main");
      const auto actor = request.str("actor_id", run["executions"].has(execution)
                                                     ? run["executions"][execution].str("actor_id")
                                                     : config["actors"].items().front().text());
      require(identifier(execution) && identifier(actor), "execution_identity_invalid");
      bool found = false;
      for (const auto &a : config["actors"].items())
        found |= a.text() == actor;
      require(found, "actor_not_authorized", 403);
      auto &exec = run["executions"][execution];
      if (exec.null())
        exec = Json{{"actor_id", actor}, {"iteration", 0}};
      require(exec.str("actor_id") == actor && !exec.flag("stopped", false) && !ended(exec),
              "execution_stopped_or_actor_changed", 409);
      if (exec.has("parent_call_id"))
        require(run["actions"][exec.str("parent_call_id")].str("decision") == "allow",
                "delegation_rejected", 409);
      if (exec.has("step_id")) {
        auto &previous = run["steps"][exec.str("step_id")];
        require(previous.str("decision") != "stop", "execution_stop_required", 409);
        if (!ended(previous))
          end_step(run, previous, "completed", Json{{"reason", "next_step_requested"}});
      }
      const auto iteration = exec.num("iteration", 0) + 1;
      Json step{{"step_id", "step-" + id},
                {"execution_id", execution},
                {"actor_id", actor},
                {"iteration", std::to_string(iteration)},
                {"state", request.has("state") ? request["state"] : Json{}},
                {"decision", "continue"},
                {"ended", false}};
      step["fingerprint"] = digest(step["state"].dump());
      Json causes = Json::Array{};
      if (exec.has("step_id"))
        causes.push(run["steps"][exec.str("step_id")]["end_event"]);
      else if (exec.has("parent_call_id"))
        causes.push(run["actions"][exec.str("parent_call_id")]["start_event"]);
      if (request.has("caused_by")) {
        require(request["caused_by"].items().size() <= 64, "too_many_causes");
        for (const auto &cause : request["caused_by"].items()) {
          bool exists = false;
          for (const auto &event : run["events"].items())
            exists |= event.str("event_id") == cause.text();
          require(exists, "causal_event_not_found", 409);
          bool duplicate = false;
          for (const auto &prior : causes.items())
            duplicate |= prior.text() == cause.text();
          if (!duplicate)
            causes.push(cause);
        }
      }
      if (causes.items().size() > 64) {
        Json joined = Json::Array{causes.items().back()};
        causes.items().pop_back();
        joined.push(causes.items().back());
        causes.items().pop_back();
        causes.push(emit(run, "custom",
                         Json{{"actor_id", actor},
                              {"execution_id", execution},
                              {"attributes", Json{{"type", "execution.join"}}}},
                         execution, joined));
      }
      auto fields = context(step);
      fields["input"] = step["state"];
      step["start_event"] = emit(run, "step.start", fields, execution, causes);
      if (active_level > 0) {
        auto q = query(run, step, "loop", id);
        q.set_iteration(iteration);
        q.set_state_fingerprint(step.str("fingerprint"));
        if (!exec["progress"].null())
          *q.mutable_previous() = proto<v1::LoopProgress>(exec["progress"]);
        auto decision = PolicyRuntime(proto<v1::RuntimePolicy>(run["policy"])).decide(q);
        step["loop_query"] = json(q);
        step["loop_context"] = decision.context();
        step["decision"] = decision.progress().decision();
        step["reason"] = decision.progress().reason();
        exec["progress"] = json(decision.progress());
        record_query(run, step, decision);
      }
      exec["iteration"] = iteration;
      exec["step_id"] = step["step_id"];
      run["steps"][step.str("step_id")] = step;
      return Json{{"step_id", step["step_id"]},
                  {"decision", step["decision"]},
                  {"reason", step.str("reason")},
                  {"iteration", iteration},
                  {"event_id", step["start_event"]}};
    }
    if (op == "task.end" || op == "task.abort") {
      keys(request, {"operation", "request_id", "run_id", "output", "status", "business_status",
                     "capture_complete"});
      const auto status = terminal(request), business = request.str("business_status", "unknown");
      require(business == "succeeded" || business == "failed" || business == "partial" ||
                  business == "unknown",
              "business_status_invalid");
      if (!request.flag("capture_complete", true))
        run["incomplete"] = true;
      // 交付确认不代表接收方最终结果。
      for (const auto &pair : run["executions"].fields())
        if (pair.second.has("parent_call_id") && !ended(pair.second)) {
          require(op == "task.abort", "task_has_pending_executions", 409);
          run["incomplete"] = true;
        }
      if (op == "task.abort") {
        require(status == "cancelled" || status == "failed" || status == "unknown",
                "abort_status_invalid");
        for (auto key : {"models", "actions"})
          for (auto &pair : run[key].fields())
            if (!ended(pair.second)) {
              auto &item = pair.second;
              const auto &step = run["steps"][item.str("step_id")];
              auto fields = context(step);
              fields["action_id"] = pair.first;
              fields["status"] = "unknown";
              fields["output"] = nullptr;
              fields["error"] = error_info("execution_interrupted");
              item["end_event"] =
                  emit(run, "action.end", fields, pair.first, Json::Array{item["start_event"]});
              item["ended"] = true;
              run["incomplete"] = true;
              if (std::string(key) == "models")
                receipt(run, step, item["query"], v1::RuntimeObservation{});
            }
        for (auto &pair : run["batches"].fields())
          if (!pair.second.flag("observed", false)) {
            auto &batch = pair.second;
            receipt(run, run["steps"][batch.str("step_id")], batch["query"],
                    v1::RuntimeObservation{});
            batch["observed"] = true;
          }
      }
      for (auto &pair : run["steps"].fields())
        if (!ended(pair.second))
          end_step(run, pair.second, status, request["output"]);
      for (auto &pair : run["executions"].fields())
        if (!ended(pair.second)) {
          pair.second["ended"] = true;
          pair.second["status"] = status;
          pair.second["output"] = request["output"];
        }
      Json fields{{"output", request["output"]},
                  {"outcome", Json{{"execution_status", status}, {"business_status", business}}}};
      emit(run, "run.end", fields, "task");
      run["closed"] = true;
      auto data = proto<v1::TraceRun>(trace(run));
      if (!run.flag("incomplete", false))
        require(validate_trace(data).report.trace_valid(), "trace_integrity_failed", 409);
      run["trace_path"] = trace_path(run).string();
      run["evaluation_status"] =
          run.flag("incomplete", false)
              ? "quarantined"
              : (config.flag("auto_evaluate", true) ? "pending" : "disabled");
      // 崩溃后 process_evaluations() 可以安全重复发布。
      return Json{{"closed", true},
                  {"capture_complete", !run.flag("incomplete", false)},
                  {"evaluation_status", run["evaluation_status"]}};
    }
    require(request.has("step_id") && run["steps"].has(request.str("step_id")), "step_not_found",
            404);
    auto &step = run["steps"][request.str("step_id")];
    require(!ended(step), "step_already_ended", 409);
    if (op == "state.record") {
      keys(request, {"operation", "request_id", "run_id", "step_id", "before", "after"});
      require(request.has("before") && request.has("after"), "state_required");
      auto fields = context(step);
      fields["input"] = request["before"];
      fields["output"] = request["after"];
      fields["state_before"] = digest(request["before"].dump());
      fields["state_after"] = digest(request["after"].dump());
      return Json{{"event_id", emit(run, "state.changed", fields, step.str("execution_id"))}};
    }
    if (op == "step.end") {
      keys(request,
           {"operation", "request_id", "run_id", "step_id", "output", "status", "decision"});
      if (request.has("decision"))
        accept_loop(run, step, request.str("decision"));
      end_step(run, step, terminal(request), request["output"]);
      if (step.str("decision") == "stop")
        run["executions"][step.str("execution_id")]["stopped"] = true;
      run["observed_boundaries"]["loop"] = true;
      return Json{{"ended", true}, {"event_id", step["end_event"]}};
    }
    require(step.str("decision") != "stop", "execution_stop_required", 409);
    if (op == "model.prepare") {
      keys(request, {"operation", "request_id", "run_id", "step_id", "request"});
      require(request["request"].object(), "model_request_required");
      require(!pending(run, step.str("step_id")), "step_has_pending_calls", 409);
      for (const auto &pair : run["models"].fields())
        require(pair.second.str("step_id") != step.str("step_id") || ended(pair.second),
                "model_already_pending", 409);
      accept_loop(run, step, step.str("decision"));
      auto prepared = proto<google::protobuf::Struct>(request["request"]);
      Json model{{"step_id", step["step_id"]}, {"ended", false}};
      if (active_level > 0) {
        auto q = query(run, step, "llm", id);
        const auto policy = proto<v1::RuntimePolicy>(run["policy"]);
        const auto decision = PolicyRuntime(policy).decide(q);
        prepared = inject_request_context(prepared, decision.context(), policy.options());
        if (!step.flag("replan_observed", false))
          prepared = inject_request_context(prepared, step.str("loop_context"), policy.options());
        model["query"] = json(q);
        record_query(run, step, decision);
      }
      const auto call = "model-" + id;
      auto fields = context(step);
      fields["action_id"] = call;
      fields["operation"] = "model";
      fields["input"] = json(prepared);
      fields["attributes"] = Json{{"phase", "prepared"}};
      model["start_event"] =
          emit(run, "action.start", fields, call, Json::Array{step["start_event"]});
      model["prepared"] = json(prepared);
      run["models"][call] = model;
      return Json{
          {"call_id", call}, {"request", json(prepared)}, {"event_id", model["start_event"]}};
    }
    if (op == "model.observe") {
      keys(request, {"operation", "request_id", "run_id", "step_id", "call_id", "request", "output",
                     "status", "transport_allowed", "error"});
      require(run["models"].has(request.str("call_id")), "model_call_not_found", 404);
      auto &item = run["models"][request.str("call_id")];
      require(item.str("step_id") == step.str("step_id") && !ended(item),
              "model_call_context_mismatch", 409);
      const auto status = terminal(request);
      require(request["request"].object() && request.has("transport_allowed"),
              "actual_request_required");
      v1::RuntimeObservation observed;
      observed.set_call_id(request.str("call_id"));
      observed.set_transport_allowed(request["transport_allowed"].boolean());
      require(observed.transport_allowed() || status != "completed",
              "blocked_model_cannot_succeed");
      *observed.mutable_request() = proto<google::protobuf::Struct>(request["request"]);
      auto fields = context(step);
      fields["action_id"] = request["call_id"];
      fields["status"] = status;
      fields["input"] = request["request"];
      fields["output"] = request["output"];
      fields["attributes"] = Json{{"transport_allowed", observed.transport_allowed()}};
      if (request.has("error")) {
        require(status != "completed", "completed_with_error");
        validate_error(request["error"]);
        fields["error"] = request["error"];
      }
      item["end_event"] =
          emit(run, "action.end", fields, request.str("call_id"), Json::Array{item["start_event"]});
      item["ended"] = true;
      receipt(run, step, item["query"], observed);
      if (step.str("decision") == "replan" && !step.flag("replan_observed", false)) {
        receipt(run, step, step["loop_query"], observed);
        step["replan_observed"] = true;
      }
      run["observed_boundaries"]["model"] = true;
      return Json{{"ended", true}, {"event_id", item["end_event"]}};
    }
    if (op == "actions.prepare") {
      keys(request, {"operation", "request_id", "run_id", "step_id", "actions"});
      require(request["actions"].array() && !request["actions"].items().empty() &&
                  request["actions"].items().size() <= 128,
              "actions_invalid");
      for (const auto &pair : run["models"].fields())
        require(pair.second.str("step_id") != step.str("step_id") || ended(pair.second),
                "model_already_pending", 409);
      accept_loop(run, step, step.str("decision"));
      auto q = query(run, step, "actions", id);
      Json results = Json::Array{};
      std::set<std::string> originals;
      for (const auto &a : request["actions"].items()) {
        keys(a, {"id", "operation", "arguments", "delegate"});
        require(identifier(a.str("id")) && originals.insert(a.str("id")).second &&
                    identifier(a.str("operation")) && a["arguments"].object(),
                "action_invalid");
        if (a.has("delegate")) {
          keys(a["delegate"], {"actor_id", "execution_id", "completion"});
          require(identifier(a["delegate"].str("actor_id")) &&
                      identifier(a["delegate"].str("execution_id")),
                  "execution_identity_invalid");
          const auto completion = a["delegate"].str("completion", "execution");
          require(completion == "execution" || completion == "delivery",
                  "delegation_completion_invalid");
          bool found = false;
          for (const auto &actor : config["actors"].items())
            found |= actor.text() == a["delegate"].str("actor_id");
          require(found, "actor_not_authorized", 403);
          require(!run["executions"].has(a["delegate"].str("execution_id")),
                  "execution_id_conflict", 409);
        }
        auto *action = q.add_actions();
        action->set_action_id("action-" + digest(id + ":" + a.str("id")).substr(0, 28));
        action->set_operation(a.str("operation"));
        *action->mutable_arguments() = proto<google::protobuf::Struct>(a["arguments"]);
      }
      v1::RuntimeDecision decision;
      if (active_level > 0) {
        decision = PolicyRuntime(proto<v1::RuntimePolicy>(run["policy"])).decide(q);
        record_query(run, step, decision);
      }
      Json batch{{"step_id", step["step_id"]},
                 {"ids", Json::Array{}},
                 {"query", active_level > 0 ? json(q) : Json{}},
                 {"observed", false}};
      for (int i = 0; i < q.actions_size(); ++i) {
        const auto &action = q.actions(i);
        const auto &original = request["actions"].items()[i];
        const auto verdict = active_level > 0 ? decision.actions(i).decision() : "allow";
        Json item{{"id", original["id"]},
                  {"step_id", step["step_id"]},
                  {"batch_id", "batch-" + id},
                  {"decision", verdict},
                  {"ended", false},
                  {"operation", original["operation"]}};
        auto fields = context(step);
        fields["action_id"] = action.action_id();
        fields["operation"] = action.operation();
        fields["input"] = original["arguments"];
        fields["attributes"] =
            Json{{"phase", "prepared"}, {"host_action_id", original["id"]}, {"decision", verdict}};
        if (original.has("delegate")) {
          fields["attributes"]["delegate_execution_id"] = original["delegate"]["execution_id"];
          fields["attributes"]["completion"] = original["delegate"].str("completion", "execution");
        }
        item["start_event"] =
            emit(run, "action.start", fields, action.action_id(), Json::Array{step["start_event"]});
        run["actions"][action.action_id()] = item;
        batch["ids"].push(action.action_id());
        Json result{{"id", original["id"]},
                    {"call_id", action.action_id()},
                    {"decision", verdict},
                    {"reason", active_level > 0 ? decision.actions(i).reason() : ""},
                    {"event_id", item["start_event"]}};
        if (original.has("delegate")) {
          const auto execution = original["delegate"].str("execution_id");
          require(!run["executions"].has(execution), "execution_id_conflict", 409);
          run["executions"][execution] = Json{{"actor_id", original["delegate"]["actor_id"]},
                                              {"iteration", 0},
                                              {"parent_call_id", action.action_id()},
                                              {"parent_execution_id", step["execution_id"]},
                                              {"ended", false}};
          run["actions"][action.action_id()]["delegate_execution_id"] = execution;
          run["actions"][action.action_id()]["completion"] =
              original["delegate"].str("completion", "execution");
          result["context"] = execution_context(run, execution);
        }
        results.push(result);
      }
      run["batches"]["batch-" + id] = batch;
      return Json{{"actions", results}};
    }
    if (op == "action.observe") {
      keys(request, {"operation", "request_id", "run_id", "step_id", "call_id", "status", "output",
                     "executed", "error"});
      require(run["actions"].has(request.str("call_id")) && request.has("executed"),
              "action_call_not_found", 404);
      auto &item = run["actions"][request.str("call_id")];
      require(item.str("step_id") == step.str("step_id") && !ended(item), "action_context_mismatch",
              409);
      const auto status = terminal(request);
      const bool executed = request["executed"].boolean();
      require(executed || status != "completed", "skipped_action_cannot_succeed");
      if (request.has("error")) {
        require(status != "completed", "completed_with_error");
        validate_error(request["error"]);
      }
      if (item.has("delegate_execution_id")) {
        auto &exec = run["executions"][item.str("delegate_execution_id")];
        if (item.str("completion") == "execution" || !executed) {
          require(!executed && exec.num("iteration", 0) == 0 && !ended(exec),
                  "delegation_requires_execution_end", 409);
          exec["ended"] = true;
          exec["status"] = status;
          exec["output"] = request["output"];
        }
      }
      finish_action(run, request.str("call_id"), status, request["output"], executed,
                    request["error"]);
      if (item.has("delegate_execution_id") && !executed)
        run["executions"][item.str("delegate_execution_id")]["end_event"] = item["end_event"];
      return Json{{"ended", true}, {"event_id", item["end_event"]}};
    }
    throw Error("operation_not_found", 404);
  }

  // 包装宿主业务调用：先查询策略，再最多执行一次原业务函数，最后记录结果。
  Json call(const std::string &token, Json request) {
    require(memory_source_root.empty(), "offline_evaluation_only", 409);
    const auto user = authenticate(token);
    evict_finished();
    require(identifier(request.str("request_id")), "request_id_required");
    safe_data(request);
    const auto bytes = request.dump();
    for (const auto &principal : principals)
      require(bytes.find(principal.first) == std::string::npos, "credential_value_rejected");
    const auto op = request.str("operation");
    if (op == "evaluation.next" || op == "evaluation.fail") {
      require(evaluation_entry.enabled(), "evaluation_entry_not_enabled", 409);
      require(user == evaluation_entry.specification.str("user_id"),
              "evaluation_entry_user_mismatch", 403);
      if (op == "evaluation.fail") {
        keys(request, {"operation", "request_id", "consumer"});
        require(!evaluation_entry.consumer.empty() &&
                    request.str("consumer") == evaluation_entry.consumer,
                "evaluation_entry_already_bound", 409);
        if (evaluation_entry.failure.empty())
          evaluation_entry.failure = "evaluation_entry_handler_failed";
        return Json{{"state", "failed"}};
      }
      const auto found = runs.find(evaluation_entry.run_id);
      auto result = evaluation_entry.next(request, found == runs.end() ? Json{} : found->second,
                                          !capture_dirty.count(evaluation_entry.run_id));
      safe_data(result);
      return result;
    }
    const bool referenced = op == "point" && request.has("ref");
    if (referenced)
      request["request_id"] = point_request_key(request);
    if (op == "contract.describe") {
      keys(request, {"operation", "request_id"});
      return describe_boundary_contract(config);
    }
    if (op == "memory.status") {
      keys(request, {"operation", "request_id"});
      const auto memory =
          legacy::TrajectoryEvolutionPipeline::load_memory(memory_path(user), run_limit);
      Json own = Json::Object{}, shared = Json::Object{}, relations = Json::Object{};
      for (const auto &entry : memory.entries()) {
        const auto status = ep::EvolutionMemoryStatus_Name(entry.status());
        own[status] = own.num(status, 0) + 1;
      }
      for (const auto &p : published_shared["entries"].fields()) {
        const auto status = p.second.str("state");
        shared[status] = shared.num(status, 0) + 1;
      }
      const auto view = shared_view(user);
      for (const auto &r : view.relations())
        relations[r.relation()] = relations.num(r.relation(), 0) + 1;
      return Json{{"application_id", config["agent_id"]},
                  {"user_id", user},
                  {"private_revision", std::size_t(memory.revision())},
                  {"private_entries", own},
                  {"public_entries", shared},
                  {"public_revision", published_shared.num("revision", 0)},
                  {"cross_relations", relations},
                  {"sharing_enabled", config["sharing"].flag("enabled", false)},
                  {"sharing_status", sharing_status},
                  {"public_available", shared_available},
                  {"retired_public_entries", published_shared.num("retired_count", 0)}};
    }
    const bool beginning =
        op == "task.begin" || op == "run.begin" ||
        (referenced && request.str("kind") == "run" && request.str("phase") == "before");
    const auto run_id =
        beginning || referenced
            ? digest(config.str("agent_id") + "\n" + user + "\n" +
                     (referenced ? point_run_key(request) : request.str("request_id")))
                  .substr(0, 40)
            : request.str("run_id");
    require(run_id.size() == 40 &&
                run_id.find_first_not_of("0123456789abcdef") == std::string::npos,
            "run_id_invalid");
    const auto fingerprint = digest(request.dump()), key = request.str("request_id");
    auto found = runs.find(run_id);
    if (found == runs.end() && fs::exists(run_file(run_id))) {
      auto restored = Json::parse(read_file(run_file(run_id), run_limit));
      require(restored.str("kind") == "integration-run-v1" &&
                  restored["identity"].str("run_id") == run_id &&
                  restored["identity"].str("agent_id") == config.str("agent_id"),
              "stored_state_invalid", 500);
      if (restored.flag("closed", false))
        restored["trace_path"] = trace_path(restored).string();
      found = runs.emplace(run_id, std::move(restored)).first;
    }
    if (found != runs.end()) {
      require(found->second["identity"].str("user_id") == user, "task_not_found", 404);
      if (found->second["requests"].has(key)) {
        const auto &previous = found->second["requests"][key];
        require(previous.str("fingerprint") == fingerprint, "request_id_conflict", 409);
        return previous["response"];
      }
    }
    if (op == "task.status" || op == "execution.status" || op == "run.status" ||
        op == "activity.status") {
      require(found != runs.end(), "task_not_found", 404);
      if (op == "run.status" || op == "activity.status") {
        require(found->second.str("profile") == "evolution.boundary.v1", "run_protocol_mismatch",
                409);
        if (op == "activity.status")
          return dispatch_boundary(found->second, request, config);
        auto legacy_request = request;
        legacy_request["operation"] = "task.status";
        auto result = dispatch(found->second, legacy_request);
        result["persisted"] = !capture_dirty.count(run_id);
        return result;
      }
      return dispatch(found->second, request);
    }
    Json run;
    Json response;
    if (beginning) {
      require(found == runs.end(), "request_id_conflict", 409);
      evaluation_entry.before_run();
      Json empty;
      const auto root_point = referenced ? resolve_point_reference(request, empty, run_id) : Json{};
      const auto root_request = referenced ? point_request(root_point, config) : request;
      const auto start =
          op == "run.begin" || referenced ? boundary_begin_request(root_request, run_id) : request;
      run = start_run(user, start, run_id);
      response = Json{{"run_id", run_id},
                      {"level", "L" + std::to_string(run.num("level", 0))},
                      {"capabilities", start["capabilities"]}};
      if (run.has("policy_gate"))
        response["policy_gate"] = run["policy_gate"];
      if (run.has("agent_version_id"))
        response["agent_version_id"] = run["agent_version_id"];
      if (op == "run.begin" || referenced) {
        initialize_boundary_run(run, root_request);
        response["protocol"] = "evolution.boundary.v1";
        if (buffered_capture)
          run["buffered_capture"] = true;
      }
      if (referenced) {
        response = point_result(root_point, root_request, response);
        record_point_reference(run, request, response);
      }
    } else {
      require(found != runs.end(), "task_not_found", 404);
      run = found->second;
      if (run.str("profile") == "evolution.boundary.v1" && op != "capture.gap" &&
          op != "evaluation.retry") {
        try {
          const auto *policy = runtime_policy(run);
          response = referenced ? dispatch_referenced_point(run, request, config, policy)
                                : dispatch_boundary(run, request, config, policy);
        } catch (...) {
          // A rejected/uncommitted request cannot consume a future injection.
          runtime_policies.erase(run_id);
          throw;
        }
        if (run.flag("closed", false)) {
          if (!run.flag("incomplete", false))
            require(validate_trace(proto<v1::TraceRun>(trace(run))).report.trace_valid(),
                    "trace_integrity_failed", 409);
          run["trace_path"] = trace_path(run).string();
        }
      } else
        response = dispatch(run, request);
    }
    try {
      safe_data(response);
      if (run.flag("buffered_capture", false) && !run.flag("capture_durable", false))
        response["persisted"] = false;
      if (!run.flag("closed", false))
        run["last_activity_ms"] = millis();
      run["requests"][key] = Json{{"fingerprint", fingerprint}, {"response", response}};
      save(run);
    } catch (...) {
      runtime_policies.erase(run_id);
      throw;
    }
    if (beginning)
      ++stored_count;
    if (beginning && evaluation_entry.enabled())
      evaluation_entry.run_id = run_id;
    runs[run_id] = std::move(run);
    if (runs[run_id].flag("closed", false))
      runtime_policies.erase(run_id);
    return response;
  }
};

// 初始化 IntegrationService 的状态和资源；输入参数的校验与所有权在构造阶段确定。
// 对象创建后复用既有配置，实际执行与收尾分别由后续接口负责。
IntegrationService::IntegrationService(const fs::path &path,
                                       const std::map<std::string, std::string> &environment)
    : IntegrationService(path, environment, true) {}

IntegrationService::IntegrationService(const fs::path &path,
                                       const std::map<std::string, std::string> &environment,
                                       bool enable_auto_evaluation)
    : impl_(std::make_unique<Impl>(path, environment, enable_auto_evaluation)) {
  if (impl_->config.has("policy_evaluation_file")) {
    fs::path file(impl_->config.str("policy_evaluation_file"));
    if (file.is_relative())
      file = impl_->config_path.parent_path() / file;
    impl_->assessments = std::make_unique<AgentEvaluationService>(
        file, impl_->root / "agent-evaluation",
        [this] {
          std::lock_guard<std::mutex> guard(impl_->mutex);
          ++impl_->assessment_notifications;
          impl_->notify_work();
        },
        impl_->config["control_learning"].dump());
  }
  if (impl_->buffered_capture)
    impl_->capture_writer = std::thread([this] { impl_->write_capture(); });
}

// 销毁对象时释放本类持有的资源；收尾不能抛出业务异常，也不能继续接收新请求。
IntegrationService::~IntegrationService() {
  impl_->assessments.reset();
  {
    std::lock_guard<std::mutex> guard(impl_->mutex);
    impl_->capture_stopping = true;
    impl_->capture_changed.notify_all();
  }
  if (impl_->capture_writer.joinable())
    impl_->capture_writer.join();
}

// 等待已接受的采集缓冲完成持久化；不等待模型学习或策略评测。
bool IntegrationService::flush_capture(unsigned timeout_ms) {
  std::unique_lock<std::mutex> guard(impl_->mutex);
  return impl_->capture_changed.wait_for(guard, std::chrono::milliseconds(timeout_ms),
                                         [&] { return impl_->capture_dirty.empty(); });
}

// 检查指定运行的采集是否真正结束，不能仅看派发进程是否退出。
bool IntegrationService::capture_finished() const {
  return capture_finished(1);
}

// 检查指定运行的采集是否真正结束，不能仅看派发进程是否退出。
bool IntegrationService::capture_finished(std::size_t expected_runs) const {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  if (impl_->stored_count < expected_runs || !impl_->stored_count || !impl_->capture_dirty.empty())
    return false;
  for (const auto &item : impl_->runs)
    if (!item.second.flag("closed", false))
      return false;
  return true;
}

// 读取隔离评测交付入口状态，区分等待、运行和完成。
std::string IntegrationService::evaluation_entry_status() const {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  const auto &entry = impl_->evaluation_entry;
  if (!entry.failure.empty())
    return entry.failure;
  return entry.done && impl_->capture_dirty.empty() ? "completed" : "";
}

// 只在后台和评测管理入口调用。读取评测器快照时不持有业务锁，
// 串行化“读取→替换”，避免慢刷新把较新的发布快照覆盖回旧版本。
void IntegrationService::refresh_published_versions(const std::string &user) {
  if (!impl_->assessments)
    return;
  std::lock_guard<std::mutex> publisher(impl_->publication_mutex);
  auto publications = Json::parse(impl_->assessments->publications(user));
  std::lock_guard<std::mutex> guard(impl_->mutex);
  auto &current = impl_->approved_versions[user];
  if (current.dump() == publications.dump())
    return;
  current = std::move(publications);
  ++impl_->publication_versions[user];
  impl_->notify_work();
}

// 在后台刷新候选评测与发布状态，业务打点不进入阻塞评测路径。
void IntegrationService::refresh_agent_evaluations() {
  if (!impl_->assessments)
    return;
  {
    std::lock_guard<std::mutex> guard(impl_->mutex);
    if (impl_->assessment_failures >= 2 || Impl::millis() < impl_->assessment_retry_ms)
      return;
  }
  const auto agent = Json::parse(impl_->assessments->agent_configuration());
  std::set<std::string> users;
  for (const auto &principal : impl_->principals)
    users.insert(principal.second);
  for (const auto &user : users) {
    // 发布投递不依赖经验写回成功；某个用户的文件锁重试不能阻止策略可见。
    refresh_published_versions(user);
    std::uint64_t notification;
    {
      std::lock_guard<std::mutex> guard(impl_->mutex);
      const auto retry = impl_->gepa_feedback_retry_ms.find(user);
      if (retry != impl_->gepa_feedback_retry_ms.end() && retry->second > Impl::millis())
        continue;
      // 先取通知代数再读结果；读取途中完成的作业必须留给下一次刷新，不能被吞掉。
      notification = impl_->assessment_notifications;
    }
    const auto feedback = Json::parse(impl_->assessments->feedback(user));
    const auto learning_feedback = Json::parse(impl_->assessments->gepa_feedback(user));
    const auto training_progress = Json::parse(impl_->assessments->status(user))["training"];
    const auto training_state = training_progress.str("status");
    ep::EvolutionMemoryStore memory;
    v1::SharedExperienceStore shared;
    Json config;
    std::uint64_t generation;
    bool memory_needs_assessment;
    {
      std::lock_guard<std::mutex> guard(impl_->mutex);
      auto &current_generation = impl_->memory_versions[user];
      if (!current_generation)
        current_generation = 1;
      generation = current_generation;
      memory_needs_assessment = impl_->assessed_memory_versions[user] != generation;
      if (!memory_needs_assessment && impl_->assessed_notifications[user] == notification &&
          learning_feedback.fields().empty() &&
          training_state != "waiting_for_learning" && training_state != "ready_to_import")
        continue;
      if (impl_->memory_source_root.empty())
        impl_->ensure_memory(user);
      memory = legacy::TrajectoryEvolutionPipeline::load_memory(impl_->memory_input_path(user),
                                                                impl_->run_limit);
      shared = impl_->shared_view(user);
      config = impl_->config;
      // 普通评测保留独立阅读记录；GEPA 训练反馈在下方直接提交到权威记忆。
      const auto feedback_path = impl_->memory_source_root.empty()
                                     ? impl_->user_dir(user) / "memory/evaluation-state.json"
                                     : impl_->root / "agent-evaluation/memory-feedback" /
                                           (path_component(user) + ".json");
      atomic_write(feedback_path, feedback.dump());
    }
    auto shadow = proto<v1::ShadowOptions>(config["shadow_options"]);
    shadow.clear_target_scopes();
    for (const auto &actor : config["actors"].items())
      shadow.add_target_scopes(impl_->scope(actor.text()));
    v1::Identity identity;
    identity.set_agent_id(config.str("agent_id"));
    identity.set_user_id(user);
    identity.set_run_id("evaluation-version");
    identity.set_conversation_id("evaluation-version");
    auto snapshot = compile_shadow_policy(memory, identity, shadow, shared);
    bool feedback_deferred = false;
    for (const auto &delivery : learning_feedback.fields()) {
      const auto path = impl_->memory_input_path(user);
      const bool updated = legacy::TrajectoryEvolutionPipeline::transform_memory_file(
          path, path.parent_path() / "evolution-memory.json",
          [&](ep::EvolutionMemoryStore &latest) {
            // 在与训练相同的文件锁内重新读取来源；并发更新不能被旧搜索覆盖。
            const auto current = compile_shadow_policy(latest, identity, shadow, shared);
            const auto combined = merge_gepa_memory_feedback(
                json(latest.gepa_feedback()), delivery.second, current);
            if (combined.object())
              *latest.mutable_gepa_feedback() = proto<google::protobuf::Struct>(combined);
            update_gepa_memory_states(latest, current);
          }, impl_->run_limit);
      memory = legacy::TrajectoryEvolutionPipeline::load_memory(path, impl_->run_limit);
      // 公私状态同步之后才确认消费，任何中断都能从待投递反馈恢复。
      try {
        impl_->sync_feedback_sharing(user, memory);
      } catch (const Error &error) {
        if (std::string(error.what()) != "memory_source_management_busy") throw;
        std::lock_guard<std::mutex> guard(impl_->mutex);
        impl_->gepa_feedback_retry_ms[user] = Impl::millis() +
            impl_->schedule.num("retry_backoff_ms", 60000);
        feedback_deferred = true;
        break;
      }
      impl_->assessments->complete_gepa_feedback(user, delivery.first,
          Json{{"memory_revision", std::size_t(memory.revision())},
               {"records", json(memory.gepa_feedback())["records"].fields().size()},
               {"updated", updated}}.dump());
      snapshot = compile_shadow_policy(memory, identity, shadow, shared);
    }
    if (feedback_deferred) continue;
    if (!learning_feedback.fields().empty()) {
      std::lock_guard<std::mutex> guard(impl_->mutex);
      impl_->gepa_feedback_retry_ms.erase(user);
      if (!memory_needs_assessment) {
        // 本次已启动评测的正常写回不再连锁触发新 GEPA，避免无人限频的循环。
        impl_->assessed_memory_versions[user] = impl_->memory_versions[user];
        impl_->assessed_notifications[user] = notification;
        continue;
      }
      // 新启动先消费历史反馈，再编译本次起点；不能把首次评测机会一起标记为已完成。
      // 同步公私视图，避免用反馈处理前的旧公共状态生成新策略。
      shared = impl_->shared_view(user);
      snapshot = compile_shadow_policy(memory, identity, shadow, shared);
    }
    bool memory_changed = false, eligibility_changed = false;
    // 在调用方的文件事务内重新计算，不能把锁外旧快照中的结果覆盖回新库。
    // 这里只做已有确定性状态合并，不在文件锁内调用模型。
    auto refresh_feedback = [&](ep::EvolutionMemoryStore &latest) {
      const auto current = compile_shadow_policy(latest, identity, shadow, shared);
      for (auto &entry : *latest.mutable_entries()) {
        const v1::ShadowExperience *experience = nullptr;
        for (const auto &item : current.experiences())
          if (!item.application_shared() && item.memory_id() == entry.memory_id())
            experience = &item;
        if (!experience)
          continue;
        ep::MemoryEvaluationFeedback next;
        for (const auto &pair : feedback.fields()) {
          const auto &value = pair.second;
          if (value.flag("application_shared", false) ||
              value.str("memory_id") != entry.memory_id() ||
              value.str("content_revision") != experience->content_revision())
            continue;
          next.set_content_revision(experience->content_revision());
          next.set_status(value.str("status"));
          next.set_positive_trial_count(value.num("positive_trial_count", 0));
          next.set_negative_trial_count(value.num("negative_trial_count", 0));
          for (const auto &report : value["reports"].fields())
            next.add_report_ids(report.first);
        }
        // 普通评测刷新不能清空已经入库的 GEPA 证据；两种入口使用同一状态机制。
        next = gepa_source_feedback(latest, *experience, next);
        const auto previous_status = entry.status();
        const bool feedback_changed =
            next.SerializeAsString() != entry.evaluation_feedback().SerializeAsString();
        if (entry.evaluation_feedback().status() == "validated" &&
            next.content_revision() != entry.evaluation_feedback().content_revision() &&
            entry.status() == ep::EVOLUTION_MEMORY_STATUS_VALIDATED)
          entry.set_status(ep::EVOLUTION_MEMORY_STATUS_CANDIDATE);
        if (feedback_changed)
          *entry.mutable_evaluation_feedback() = next;
        if (next.status() == "validated")
          entry.set_status(ep::EVOLUTION_MEMORY_STATUS_VALIDATED);
        if (next.status() == "conflicting_trial_results" &&
            entry.status() == ep::EVOLUTION_MEMORY_STATUS_VALIDATED)
          entry.set_status(ep::EVOLUTION_MEMORY_STATUS_CANDIDATE);
        if (next.status() == "regressed") {
          entry.set_status(ep::EVOLUTION_MEMORY_STATUS_ARCHIVED);
          eligibility_changed = true;
        }
        memory_changed |= feedback_changed || entry.status() != previous_status;
      }
    };
    if (impl_->memory_source_root.empty()) {
      // 普通评测沿用原状态规则，写入改为与训练/GEPA 相同的文件锁事务。
      // 其他写者先提交时，直接使用其最新内容合并，正常竞争不计为评测故障。
      // 无内容变化时也允许修复上次 PB 成功而 JSON 视图失败的中断。
      legacy::TrajectoryEvolutionPipeline::transform_memory_file(
          impl_->memory_path(user), impl_->user_dir(user) / "memory/evolution-memory.json",
          refresh_feedback, impl_->run_limit);
      memory = legacy::TrajectoryEvolutionPipeline::load_memory(impl_->memory_path(user), impl_->run_limit);
      if (memory_changed) {
        std::lock_guard<std::mutex> guard(impl_->mutex);
        generation = ++impl_->memory_versions[user];
        impl_->notify_work();
      }
      if (eligibility_changed)
        impl_->queue_sharing(user);
      snapshot = compile_shadow_policy(memory, identity, shadow, shared);
    } else {
      // 外置记忆的普通版本评测仍只更新本次编译视图；GEPA 投递走上方独立写回。
      refresh_feedback(memory);
    }
    std::vector<Json> artifacts;
    for (int n = 0; n <= config.str("max_level")[1] - '0'; ++n) {
      Json artifact{{"schema_version", 1},
                    {"agent", agent},
                    {"user_id", user},
                    {"level", "L" + std::to_string(n)},
                    {"policy", nullptr}};
      if (n > 0) {
        // 在候选生成时固定收尾行为，评测、导出和独立导入均使用同一版本。
        // 仅白名单字段进入产物；未配置模板时也显式冻结为空。
        artifact["runtime_options"] =
            Json{{"finalization_context", config.str("finalization_context")}};
        auto options = proto<v1::RuntimeOptions>(config["runtime_options"]);
        options.set_requested_level(proto_level(n));
        options.set_fallback("strict");
        options.clear_allowed_users();
        options.add_allowed_users(user);
        options.clear_allowed_conversations();
        options.add_allowed_conversations(identity.conversation_id());
        options.clear_host_capabilities();
        if (config["instrumentation"].has("capabilities")) {
          std::set<std::string> caps;
          for (const auto &cap : config["instrumentation"]["capabilities"].items())
            caps.insert(cap.text());
          if (!caps.count(std::vector<std::string>{"model", "actions", "loop"}[n - 1]) &&
              !(n == 2 && caps.count("schedule")))
            continue;
          for (int i = 0; i < 3; ++i)
            if (caps.count(std::vector<std::string>{"model", "actions", "loop"}[i]))
              options.add_host_capabilities(
                  std::vector<std::string>{"context.request", "action.filter", "loop.decide"}[i]);
          if (caps.count("schedule"))
            options.add_host_capabilities("schedule.dispatch");
        } else
          for (int i = 0; i < n; ++i)
            options.add_host_capabilities(
                std::vector<std::string>{"context.request", "action.filter", "loop.decide"}[i]);
        if (config.flag("auto_approve_experiences", false)) {
          options.clear_approved_experiences();
          for (const auto &item : snapshot.experiences()) {
            auto *approval = options.add_approved_experiences();
            approval->set_memory_id(item.memory_id());
            approval->set_content_revision(item.content_revision());
          }
        }
        artifact["policy"] = json(compile_runtime_policy(snapshot, options));
        artifact = apply_gepa_memory_feedback(artifact, json(memory.gepa_feedback()),
                                              config["control_learning"]);
      }
      artifacts.push_back(std::move(artifact));
    }
    if (config.flag("auto_evaluate", true) &&
        (!config.has("evaluation_levels") || !config["evaluation_levels"].items().empty()) &&
        artifacts.back().str("level") == config.str("max_level") &&
        config.str("max_level") != "L0" &&
        (config.flag("auto_approve_experiences", false) ||
         !proto<v1::RuntimeOptions>(config["runtime_options"]).approved_experiences().empty())) {
      bool pending = false, failed = false;
      {
        std::lock_guard<std::mutex> guard(impl_->mutex);
        for (const auto &item : impl_->learning_queue)
          pending |= item.second.str("user") == user;
        if (training_progress["run_ids"].array())
          for (const auto &id : training_progress["run_ids"].items()) {
            const auto found = impl_->evaluation_outcomes.find(id.text());
            const auto outcome = found == impl_->evaluation_outcomes.end()
                                     ? impl_->read_run(id.text()).str("evaluation_status")
                                     : found->second;
            failed |= outcome != "pending" && outcome != "running" && outcome != "completed";
          }
      }
      const auto training = Json::parse(
          impl_->assessments->prepare_training(user, artifacts.back().dump(), pending, failed));
      const auto state = training.str("status");
      if (state == "ready_to_import") {
        const auto response =
            import_training_report(training.str("report_directory"),
                                   training.str("dataset_directory"), training["baseline"].dump());
        impl_->assessments->complete_training_import(user, training.str("job_id"), response.body);
        std::lock_guard<std::mutex> guard(impl_->mutex);
        ++impl_->memory_versions[user];
        impl_->notify_work();
        continue;
      }
      if (state != "ready" && state != "disabled") {
        std::lock_guard<std::mutex> guard(impl_->mutex);
        impl_->assessed_memory_versions[user] = generation;
        impl_->assessed_notifications[user] = notification;
        continue;
      }
    }
    // 单纯的完成通知已经消费完毕，不因历史反证刚落库就递归生成下一场搜索。
    // 新训练改变记忆或普通评测改变可用状态时仍按原逻辑评测；重启调用会重新读原库。
    // 管理端恢复指定作业时会临时关闭 auto_evaluate。训练准备入口已经遵守
    // 该开关，候选提交也必须使用同一条件；否则服务启动后仍会抢先创建一个
    // 0 试验空壳，并再次把显式 retry 的原搜索树标为 superseded。
    if (config.flag("auto_evaluate", true))
      for (const auto &artifact : artifacts) {
        if (!memory_needs_assessment && !memory_changed) break;
        if (config.has("evaluation_levels")) {
          bool selected = false;
          for (const auto &value : config["evaluation_levels"].items())
            selected |= value.text() == artifact.str("level");
          if (!selected)
            continue;
        }
        impl_->assessments->submit(user, artifact.dump(), json(memory.gepa_feedback()).dump());
      }
    {
      std::lock_guard<std::mutex> guard(impl_->mutex);
      impl_->assessed_memory_versions[user] = generation;
      impl_->assessed_notifications[user] = notification;
    }
  }
  {
    std::lock_guard<std::mutex> guard(impl_->mutex);
    impl_->assessment_error.clear();
    impl_->assessment_failures = 0;
    impl_->assessment_retry_ms = 0;
  }
}

// 显式本地后处理：权威已成功分析可以补齐旧版本遗留的未判证据。
// 不发模型请求，不重写原报告，不创建训练来源或把未知证据升级为有效。
IntegrationResponse IntegrationService::refresh_evidence() {
  try {
    std::lock_guard<std::mutex> guard(impl_->mutex);
    require(impl_->memory_source_root.empty(), "evidence_refresh_requires_private_store", 409);
    require(!impl_->evaluating, "evidence_refresh_learning_busy", 409);
    std::set<std::string> users;
    for (const auto &principal : impl_->principals) users.insert(principal.second);
    std::size_t updated = 0, unchanged = 0, rejected = 0, scanned = 0, changed_users = 0;
    for (const auto &user : users) {
      const auto memory_path = impl_->memory_path(user);
      if (!fs::exists(memory_path)) continue;
      std::map<std::string, ep::HybridEvaluationReport> reports;
      const auto evaluation_dir = impl_->user_dir(user) / "evaluation";
      if (fs::exists(evaluation_dir)) {
        for (const auto &entry : fs::recursive_directory_iterator(evaluation_dir)) {
          if (entry.is_symlink() || !entry.is_regular_file()) continue;
          const auto filename = entry.path().filename().string();
          const std::string suffix = ".evaluation.json";
          if (filename.size() <= suffix.size() ||
              filename.compare(filename.size() - suffix.size(), suffix.size(), suffix) != 0) continue;
          try {
            const auto id = filename.substr(0, filename.size() - suffix.size());
            auto run = impl_->read_run(id);
            const auto bytes = read_file(entry.path(), impl_->run_limit);
            const auto report = proto<ep::HybridEvaluationReport>(Json::parse(bytes));
            const auto &state = run["learning"];
            require(state.str("analysis_state") == "completed" &&
                        state.str("report_fingerprint") == digest(bytes), "evidence_refresh_cache_mismatch");
            require(run["identity"].str("user_id") == user &&
                        report.source().user_id() == user &&
                        report.source().conversation_id() == run["identity"].str("conversation_id") &&
                        report.source().dialogue_id() == run.str("import_source_id", id),
                    "evidence_refresh_identity_mismatch");
            const auto raw = read_file(impl_->trace_path(run), impl_->run_limit);
            require(run.flag("imported", false) ? digest(raw) == run.str("import_content_fingerprint")
                                                : raw == impl_->trace(run).dump(),
                    "evidence_refresh_trace_changed");
            require(report.source_fingerprint() ==
                        (run.flag("imported", false) ? run.str("import_evidence_fingerprint")
                         : digest(trace_content_jsonl(proto<v1::TraceRun>(impl_->trace(run))))),
                    "evidence_refresh_source_mismatch");
            require(report.source_trace_integrity_valid() &&
                        report.dialogue_assessment().status() == ep::MODEL_CALL_STATUS_SUCCEEDED,
                    "evidence_refresh_analysis_unassessed");
            legacy::ensure_safe_evolution_data(report);
            const auto key = report.source().conversation_id() + '\0' + report.source().dialogue_id();
            require(reports.emplace(key, report).second, "evidence_refresh_duplicate_source");
          } catch (...) {
            // Unavailable/failed/mismatched caches remain unassessed. They
            // cannot supply a positive verdict or trigger semantic reanalysis.
          }
        }
      }
      const bool changed = legacy::TrajectoryEvolutionPipeline::transform_memory_file(
          memory_path, memory_path.parent_path() / "evolution-memory.json",
          [&](ep::EvolutionMemoryStore &memory) {
            for (auto &entry : *memory.mutable_entries()) {
              for (auto &evidence : *entry.mutable_candidate()->mutable_support_evidence()) {
                if (evidence.kind() != ep::CANDIDATE_EVIDENCE_KIND_DIALOGUE_REFLECTION ||
                    evidence.verdict() != ep::SEMANTIC_VERDICT_UNASSESSED) continue;
                ++scanned;
                const auto key = evidence.source().conversation_id() + '\0' + evidence.source().dialogue_id();
                const auto found = reports.find(key);
                if (evidence.source().user_id() != user || found == reports.end()) { ++rejected; continue; }
                if (legacy::refresh_scoped_experience_evidence(found->second,
                        entry.candidate().target_scope(), &evidence)) ++updated;
                else ++unchanged;
              }
            }
          }, impl_->run_limit);
      if (changed) {
        ++changed_users;
        auto &version = impl_->memory_versions[user];
        version = std::max<std::uint64_t>(version, 1) + 1;
        impl_->notify_work();
      }
    }
    return {200, Json{{"status", "completed"}, {"model_calls", 0}, {"scanned_evidence", scanned},
                      {"refreshed_evidence", updated}, {"unchanged_evidence", unchanged},
                      {"rejected_evidence", rejected}, {"changed_users", changed_users}}.dump()};
  } catch (const Error &error) {
    return {error.status, Json{{"status", "error"}, {"reason", error.what()}, {"model_calls", 0}}.dump()};
  } catch (...) {
    return {500, Json{{"status", "error"}, {"reason", "evidence_refresh_failed"}, {"model_calls", 0}}.dump()};
  }
}

// 通过可信本地入口导入指定轨迹，先校验来源和幂等身份。
IntegrationResponse IntegrationService::import_file(const fs::path &path) {
  return import_source(path, "", "");
}

// 识别并校验导入来源，将有效日志送入与在线采集相同的后台队列。
IntegrationResponse IntegrationService::import_source(const fs::path &path,
                                                      const std::string &training_trace,
                                                      const std::string &training_source) {
  try {
    require(impl_->memory_source_root.empty(), "offline_evaluation_only", 409);
    auto bytes = training_trace.empty() ? read_file(path, impl_->run_limit) : training_trace;
    require(bytes.size() <= impl_->run_limit, "import_trace_size_limit");
    std::string user, conversation, source_id, evidence_hash, format;
    if (!training_trace.empty() || is_trace_path(path)) {
      const auto validated = validate_trace(
          training_trace.empty() ? read_trace(path, {impl_->run_limit, impl_->event_limit})
                                 : parse_trace_json(bytes),
          {impl_->run_limit, impl_->event_limit});
      require(validated.report.trace_valid(), "import_trace_invalid");
      const auto &identity = validated.trace.identity();
      for (const auto &event : validated.trace.events())
        if (event.kind() == "run.start")
          require(!training_source.empty() || !json(event.attributes()).has("evaluation_trial_id"),
                  "evaluation_trace_not_learning_source");
      require(identity.agent_id() == impl_->config.str("agent_id"), "import_application_mismatch");
      user = identity.user_id();
      conversation = identity.conversation_id();
      source_id = identity.run_id();
      if (training_trace.empty())
        require(bytes == read_file(path, impl_->run_limit), "import_source_changed");
      bytes = json(validated.trace).dump();
      format = "trace";
      safe_data(json(validated.trace));
      evidence_hash = digest(trace_content_jsonl(validated.trace));
    } else {
      require(path.extension() == ".jsonl", "import_format_unsupported");
      legacy::AnalyzerOptions options;
      options.max_file_bytes = impl_->run_limit;
      const auto analysis = legacy::LoopEpisodeAnalyzer().analyze_file(path, options);
      require(analysis.trace_integrity_valid, "import_trace_invalid");
      require(bytes == read_file(path, impl_->run_limit), "import_source_changed");
      user = analysis.user_id;
      conversation = analysis.conversation_id;
      source_id = analysis.dialogue_id;
      evidence_hash = digest(bytes);
      format = "sessionlog";
      std::istringstream rows(bytes);
      std::string row;
      while (std::getline(rows, row))
        if (row.find_first_not_of(" \t\r") != std::string::npos)
          safe_data(Json::parse(row));
    }
    require(identifier(user) && identifier(conversation) && identifier(source_id),
            "import_identity_missing");
    safe_data(Json(bytes));
    std::lock_guard<std::mutex> guard(impl_->mutex);
    bool authorized = false;
    for (const auto &p : impl_->principals)
      authorized |= p.second == user;
    require(authorized, "import_user_not_configured", 403);
    const auto rid = source_id.size() == 40 &&
                             source_id.find_first_not_of("0123456789abcdef") == std::string::npos
                         ? source_id
                         : digest("import\n" + impl_->config.str("agent_id") + '\n' + user + '\n' +
                                  conversation + '\n' + source_id)
                               .substr(0, 40);
    const auto manifest = impl_->run_file(rid);
    if (fs::exists(manifest)) {
      auto old = impl_->read_run(rid);
      require(old.flag("closed", false) && old["identity"].str("user_id") == user &&
                  old["identity"].str("conversation_id") == conversation,
              "import_identity_conflict", 409);
      const auto expected =
          old.flag("imported", false)
              ? old.str("import_evidence_fingerprint")
              : digest(trace_content_jsonl(proto<v1::TraceRun>(impl_->trace(old))));
      bool same_training_source = false;
      if (!training_source.empty() && old.has("training_source")) {
        const auto source = Json::parse(training_source);
        auto previous = old["training_source"], incoming = source;
        previous.fields().erase("source_evidence_fingerprint");
        incoming.fields().erase("source_evidence_fingerprint");
        same_training_source =
            previous.dump() == incoming.dump() &&
            !source.str("source_evidence_fingerprint").empty() &&
            old["training_source"].str("source_evidence_fingerprint", expected) ==
                source.str("source_evidence_fingerprint");
      }
      // 补充离线验收上下文不能重新导入已经处理的报告。
      require(expected == evidence_hash || same_training_source, "import_content_conflict", 409);
      // 同一份官方训练轨迹在语义模型传输或建作业失败时不能被幂等去重
      // 永久卡住。只恢复明确失败的评审，或尚未保留任何模型调用的授权作业。
      // 原始 trace、训练分数和旧评审证据都保留，成功重试后不会再次触发。
      // Some historical stores lack the optional training_source fields or
      // serialized them with a different field order. The immutable trace
      // fingerprint is the stronger identity check for a retry request.
      const bool retry_training_source = !training_source.empty() &&
                                          (same_training_source || expected == evidence_hash);
      if (retry_training_source) {
        const auto evaluation = impl_->user_dir(user) / "evaluation" /
                                path_component(conversation) / (rid + ".evaluation.json");
        bool retryable = false;
        try {
          if (fs::exists(evaluation)) {
            const auto cached = Json::parse(read_file(evaluation, impl_->run_limit));
            // Protobuf-JSON emits counters as strings in some historical
            // reports and as numbers in newer ones; accept both encodings.
            std::size_t failed = 0;
            if (std::holds_alternative<std::string>(cached["model_failed_count"].value)) {
              const auto encoded = cached.str("model_failed_count");
              require(!encoded.empty() &&
                          std::all_of(encoded.begin(), encoded.end(), [](unsigned char c) {
                            return c >= '0' && c <= '9';
                          }),
                      "model_failed_count_invalid");
              failed = static_cast<std::size_t>(std::stoull(encoded));
            } else {
              failed = cached.num("model_failed_count", 0);
            }
            retryable = failed > 0;
            const auto &dialogue = cached["dialogue_assessment"];
            retryable |= dialogue.str("status") == "MODEL_CALL_STATUS_SKIPPED" &&
                         dialogue.str("error_code") == "model_request_too_large";
          } else if (old.str("evaluation_status") == "failed" &&
                     old["learning"].str("analysis_state") == "retry_authorized" &&
                     !old["learning"].str("job_id").empty()) {
            const auto previous = impl_->read_job(old["learning"].str("job_id"));
            const auto &ids = previous["runs"].items();
            static const std::set<std::string> setup_failures{
                "evaluation_configuration_failed", "analysis_job_setup_failed",
                "learning_job_already_exists", "evaluation_api_key_missing"};
            std::size_t reserved = previous.num("synthesis_model_calls", 0);
            for (const auto &id : ids)
              reserved += previous["analysis_model_calls"].num(id.text(), 0);
            retryable = previous.str("state") == "failed" &&
                        previous.str("phase") == "analysis" &&
                        previous.str("user") == user &&
                        std::any_of(ids.begin(), ids.end(), [&](const auto &id) {
                          return id.text() == rid;
                        }) && impl_->owns_run(previous, old) &&
                        setup_failures.count(previous.str("error")) && reserved == 0;
          }
        } catch (...) {
          // 损坏的旧缓存交给常规完整性校验，不把它误判为可重试。
        }
        if (retryable) {
          if (fs::exists(evaluation)) {
            const auto history = impl_->user_dir(user) / "evaluation-retry-history" /
                                 path_component(conversation);
            fs::create_directories(history);
            const auto tag = digest(read_file(evaluation, impl_->run_limit)).substr(0, 16);
            fs::copy_file(evaluation, history / (rid + "." + tag + ".evaluation.json"),
                          fs::copy_options::overwrite_existing);
            const auto protobuf = evaluation.parent_path() / (rid + ".evaluation.pb");
            if (fs::exists(protobuf))
              fs::copy_file(protobuf, history / (rid + "." + tag + ".evaluation.pb"),
                            fs::copy_options::overwrite_existing);
            fs::remove(evaluation);
            if (fs::exists(protobuf)) fs::remove(protobuf);
          }
          old["learning"]["retry_count"] = old["learning"].num("retry_count", 0) + 1;
          old["learning"].fields().erase("report_fingerprint");
          old["learning"].fields().erase("job_id");
          old["learning"]["analysis_state"] = "retry_authorized";
          old["learning"]["phase"] = "analysis_retry_wait";
          old["evaluation_status"] = "pending";
          old["evaluation_error"] = "";
          impl_->save_closed(old);
          return {200, Json{{"ok", true},
                            {"result", Json{{"run_id", rid},
                                             {"reused", false},
                                             {"review_retry", true},
                                             {"evaluation_status", "pending"}}}}
                           .dump()};
        }
      }
      // 无失败评审缓存时保持原有幂等语义。
      return {200, Json{{"ok", true},
                        {"result", Json{{"run_id", rid},
                                        {"reused", true},
                                        {"evaluation_status", old.str("evaluation_status")}}}}
                       .dump()};
    }
    require(impl_->stored_count < impl_->config.num("max_stored_runs", 10000),
            "stored_run_limit_reached", 507);
    Json run{
        {"kind", "integration-run-v1"},
        {"imported", true},
        {"import_format", format},
        {"import_source_id", source_id},
        {"import_evidence_fingerprint", evidence_hash},
        {"import_content_fingerprint", digest(bytes)},
        {"identity", Json{{"agent_id", impl_->config["agent_id"]},
                          {"user_id", user},
                          {"conversation_id", conversation},
                          {"run_id", rid}}},
        {"closed", true},
        {"level", 0},
        {"events", Json::Array{}},
        {"requests", Json::Object{}},
        {"evaluation_status", impl_->config.flag("auto_evaluate", true) ? "pending" : "disabled"},
        {"learning", Json{{"trace_published", true}, {"capture_bytes", bytes.size()}}}};
    if (!training_source.empty())
      run["training_source"] = Json::parse(training_source);
    const auto target = impl_->trace_path(run);
    if (fs::exists(target))
      require(read_file(target, impl_->run_limit) == bytes, "import_content_conflict", 409);
    else
      atomic_write(target, bytes);
    impl_->ensure_memory(user);
    impl_->save(run);
    ++impl_->stored_count;
    return {200, Json{{"ok", true},
                      {"result", Json{{"run_id", rid},
                                      {"reused", false},
                                      {"evaluation_status", run["evaluation_status"]}}}}
                     .dump()};
  } catch (const Error &error) {
    return {error.status, Json{{"ok", false}, {"error", error.what()}}.dump()};
  } catch (...) {
    return {400, R"({"ok":false,"error":"import_rejected"})"};
  }
}

// 检查冻结 L0 训练报告与准入条件，验证或留出轨迹不能混入经验学习。
IntegrationResponse
IntegrationService::import_training_evaluation(const fs::path &directory,
                                               const fs::path &dataset_directory) {
  return import_training_report(directory, dataset_directory, "");
}

// 导入已验证训练报告的各条真实轨迹，按来源身份避免重复学习。
IntegrationResponse IntegrationService::import_training_report(const fs::path &directory,
                                                               const fs::path &dataset_directory,
                                                               const std::string &baseline_json) {
  try {
    const auto dataset = read_evaluation_dataset(dataset_directory);
    for (const auto &issue : dataset.issues())
      require(issue.severity() != "error", "training_dataset_invalid");
    const auto bytes = read_file(directory / "report.json", kMaxEvaluationReportBytes);
    const auto report_fingerprint = digest(bytes);
    const auto manifest = Json::parse(read_file(directory / "manifest.json", 1048576));
    const auto report = Json::parse(bytes);
    require(manifest.flag("complete", false) && report.flag("complete", false) &&
                manifest["files"].str("report.json") == report_fingerprint &&
                manifest.str("run_id") == report.str("run_id"),
            "training_report_invalid");
    require(report.str("dataset_version") == dataset.dataset_version() &&
                manifest.str("dataset_version") == dataset.dataset_version(),
            "training_dataset_mismatch");
    require(report["trials"].array() && !report["trials"].items().empty(),
            "training_trials_required");
    std::map<std::string, const dataset::v1::EvaluationTask *> tasks;
    for (const auto &task : dataset.tasks())
      tasks[task.task_id()] = &task;
    std::map<std::string, std::string> revisions;
    for (const auto &variant : report["configuration"]["variants"].items())
      require(revisions.emplace(variant.str("id"), variant.str("version")).second,
              "training_variant_duplicate");
    std::vector<std::pair<std::string, std::string>> pending;
    std::set<std::string> run_ids, trial_ids;
    std::size_t rejected = 0;
    // 先验证整批再导入任何记录，不能从混有验证或留出数据的报告里
    // 悄悄挑出训练行导入。
    for (const auto &trial : report["trials"].items()) {
      const auto found = tasks.find(trial.str("task_id"));
      require(found != tasks.end() && trial.str("split") == "train" &&
                  found->second->split() == "train" &&
                  trial.str("task_version") == found->second->task_version() &&
                  found->second->review_status() == "reviewed",
              "training_task_not_admissible");
      require(trial_ids.insert(trial.str("trial_id")).second, "training_trial_duplicate");
      const auto revision = revisions.find(trial.str("variant_id"));
      require(revision != revisions.end() && !revision->second.empty(), "training_variant_missing");
      // 冷启动可以只执行训练任务并采集轨迹，不必先请求任务裁判。此时多轮验收
      // 保持 uncertain，不能为满足入库条件伪造成 passed。仅识别明确未请求评分
      // 的完整执行；裁判未知、裁判错误和执行异常仍走原有拒绝路径。
      const bool ungraded_training =
          trial.str("verdict") == "uncertain" &&
          report["configuration"].has("model_enabled") &&
          !report["configuration"].flag("model_enabled", true) &&
          trial.str("execution_status") == "completed" && trial.str("error_code").empty() &&
          std::any_of(trial["warnings"].items().begin(), trial["warnings"].items().end(),
                      [](const Json &warning) {
                        return warning.text() == "turn_grading_required" ||
                               warning.text() == "task_grading_not_requested";
                      }) &&
          std::all_of(trial["checks"].items().begin(), trial["checks"].items().end(),
                      [](const Json &check) {
                      return check.str("check_id") != "model" && check.str("status") == "passed";
                      });
      // 保留带完整轨迹的执行/裁判错误，供确定性规则和失败经验分析使用。
      // 错误结论不会被当作 passed，也不会获得公共记忆支持资格；没有轨迹的
      // 错误仍然拒绝，避免从空记录猜测经验。
      const bool errored_training_with_trace =
          trial.str("verdict") == "error" &&
          trial.str("execution_status") == "completed" &&
          !trial.str("error_code").empty() &&
          (trial["evidence"].has("trace") || trial["evidence"].has("traces"));
      if (trial.str("verdict") != "passed" && trial.str("verdict") != "failed" &&
          !ungraded_training && !errored_training_with_trace) {
        ++rejected;
        continue;
      }
      const auto &evidence = trial["evidence"];
      // 超时试验可能只有部分轨迹；把它的失败保留在报告中，
      // 但不能阻止其他试验的完整轨迹继续处理。
      if (trial.str("execution_status") == "timeout" &&
          trial.str("error_code") == "runner_timeout" && !evidence.has("trace") &&
          !evidence.has("traces")) {
        ++rejected;
        continue;
      }
      // 一条错误试验的证据即使损坏，也不能使同批其他完整训练轨迹失效。
      // 逐 trial 暂存，全部校验通过后才提交；passed/failed 仍保持严格报错。
      std::vector<std::pair<std::string, std::string>> trial_pending;
      std::set<std::string> trial_run_ids;
      try {
        require(evidence.has("trace") != evidence.has("traces"), "training_trace_required");
        const auto code_projection = validated_code_projection(evidence);
        const auto traces =
            evidence.has("traces") ? evidence["traces"] : Json(Json::Array{evidence["trace"]});
        require(traces.array() && !traces.items().empty() && traces.items().size() <= 1024,
                "training_trace_limit");
        const auto turn_standards = turn_expectations(*found->second);
        if (!turn_standards.null())
          require(traces.items().size() == turn_standards.items().size(),
                  "training_turn_evidence_mismatch");
        Json trial_runs = Json::Array{}, constraints = Json::Array{};
        for (const auto &trace : traces.items())
          trial_runs.push(trace["identity"]["run_id"]);
        for (const auto &constraint : found->second->constraints())
          constraints.push(constraint);
        std::size_t turn_index = 0;
        for (const auto &trace : traces.items()) {
          const auto validated =
              validate_trace(parse_trace_json(trace.dump()), {impl_->run_limit, impl_->event_limit});
          require(validated.report.trace_valid(), "training_trace_invalid");
          const auto &identity = validated.trace.identity();
          require(identity.agent_id() == impl_->config.str("agent_id") &&
                      trial_run_ids.insert(identity.run_id()).second && !run_ids.count(identity.run_id()),
                  "training_trace_identity_invalid");
          const Json baseline =
              baseline_json.empty()
                  ? Json{{"schema_version", 1},
                         {"agent", Json{{"revision", revision->second},
                                        {"name", impl_->config.str("application_id",
                                                                   impl_->config.str("agent_id"))}}},
                         {"user_id", identity.user_id()},
                         {"level", "L0"},
                         {"policy", nullptr}}
                  : Json::parse(baseline_json);
          require(baseline.str("level") == "L0" && baseline["policy"].null() &&
                      baseline.str("user_id") == identity.user_id(),
                  "training_requires_unmodified_baseline");
          const auto version = agent_version_id(baseline.dump());
          if (!baseline_json.empty())
            require(revision->second == version, "training_trace_version_mismatch");
          require(evidence.str("loaded_version") == version, "training_requires_unmodified_baseline");
          bool bound = false;
          for (const auto &event : validated.trace.events())
            if (event.kind() == "run.start") {
              const auto attributes = json(event.attributes());
              bound = attributes.str("agent_version_id") == version &&
                      attributes.str("evaluation_trial_id") == trial.str("trial_id");
            }
          require(bound, "training_trace_version_mismatch");
          const Json source{
              {"kind", "baseline_training"},
              {"dataset_version", dataset.dataset_version()},
              {"task_id", found->second->task_id()},
              {"task_version", found->second->task_version()},
              {"trial_id", trial.str("trial_id")},
              {"report_fingerprint", report_fingerprint},
              {"source_evidence_fingerprint", digest(trace_content_jsonl(validated.trace))}};
          auto enriched = json(validated.trace);
          // 这里只补充离线试验上下文，不是新执行事件或给 Agent 的指令。
          // 原报告和原事件保持不变。
          enriched["attributes"]["training_evaluation"] =
              Json{{"expected_behavior", found->second->expected_behavior()},
                   {"constraints", constraints},
                   {"verdict", trial["verdict"]},
                   {"checks", trial["checks"]},
                   {"run_ids", trial_runs}};
          const auto run_projection = code_projection_for_run(code_projection, identity.run_id());
          if (!run_projection.null())
            enriched["attributes"]["training_evaluation"]["evidence_projection"] =
                run_projection;
          if (ungraded_training)
            enriched["attributes"]["training_evaluation"]["task_assessment_status"] = "not_requested";
          if (errored_training_with_trace) {
            enriched["attributes"]["training_evaluation"]["task_assessment_status"] = "error";
            enriched["attributes"]["training_evaluation"]["error_code"] = trial.str("error_code");
          }
          if (!turn_standards.null()) {
            auto &context = enriched["attributes"]["training_evaluation"];
            const auto &expected = turn_standards.items().at(turn_index);
            context["expected_behavior"] = expected["expected_behavior"];
            context["turn"] = turn_index + 1;
            context["turn_count"] = traces.items().size();
            context["trial_verdict"] = trial["verdict"];
            // 整题失败不能成为每轮都失败的标签；缺逐轮判定保持未知，不伪造反证。
            context["verdict"] = "uncertain";
            context["checks"] = Json::Array{};
            if (evidence["turn_evaluation"].array())
              for (const auto &verdict : evidence["turn_evaluation"].items())
                if (verdict.num("turn", 0) == turn_index + 1 &&
                    verdict.str("run_id") == identity.run_id()) {
                  context["verdict"] = verdict["verdict"];
                  context["reason"] = verdict["reason"];
                }
            context["attribution"] = "current_turn_only";
            context["current_input"] = learning_user_input(trace);
            context["history"] = learning_history(traces, turn_index);
          }
          ++turn_index;
          safe_data(enriched);
          const auto enriched_bytes = enriched.dump();
          require(enriched_bytes.size() <= impl_->run_limit, "import_trace_size_limit");
          trial_pending.emplace_back(enriched_bytes, source.dump());
        }
      } catch (...) {
        if (!errored_training_with_trace) throw;
        ++rejected;
        continue;
      }
      run_ids.insert(trial_run_ids.begin(), trial_run_ids.end());
      pending.insert(pending.end(), std::make_move_iterator(trial_pending.begin()),
                     std::make_move_iterator(trial_pending.end()));
    }
    require(!pending.empty(), "training_no_complete_traces");
    require(bytes == read_file(directory / "report.json", kMaxEvaluationReportBytes), "training_report_changed");
    // 只在整批来源通过校验后登记冻结语料位置，供公共管理独立复核。
    // 该索引不进入模型输入，也不修改已导入轨迹或缓存分析；重复导入可补齐旧来源。
    atomic_write(impl_->root / "learning/training-datasets" / (digest(dataset.dataset_version()) + ".json"),
                 Json{{"dataset_version", dataset.dataset_version()},
                      {"directory", fs::absolute(dataset_directory).lexically_normal().string()}}.dump());
    std::size_t imported = 0, reused = 0;
    Json admitted_runs = Json::Array{};
    for (const auto &item : pending) {
      const auto response = import_source({}, item.first, item.second);
      if (response.status != 200) {
        ++rejected;
        continue;
      }
      const auto result = Json::parse(response.body)["result"];
      admitted_runs.push(result["run_id"]);
      if (result.flag("reused", false))
        ++reused;
      else
        ++imported;
    }
    require(imported || reused, "training_no_admitted_traces");
    return {200, Json{{"ok", true},
                      {"result", Json{{"imported", imported},
                                      {"reused", reused},
                                      {"rejected", rejected},
                                      {"run_ids", admitted_runs}}}}
                     .dump()};
  } catch (const Error &error) {
    return {error.status, Json{{"ok", false}, {"error", error.what()}}.dump()};
  } catch (...) {
    return {400, R"({"ok":false,"error":"training_import_rejected"})"};
  }
}

// 返回后台学习队列和处理状态，不触发额外模型请求。
std::string IntegrationService::learning_status() const {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  Json outcomes = Json::Object{};
  for (const auto &p : impl_->evaluation_outcomes)
    outcomes[p.second] = outcomes.num(p.second, 0) + 1;
  std::set<std::string> users;
  for (const auto &p : impl_->principals)
    if (fs::exists(impl_->memory_input_path(p.second)))
      users.insert(p.second);
  std::size_t assessment_pending = 0, training_pending = 0, training_failed = 0;
  std::size_t memory_pending = 0, memory_unresolved = 0, memory_failed = 0, shared_unresolved = 0;
  for (const auto &p : impl_->memory_management.fields()) {
    if (p.second.str("status") != "waiting_for_model") memory_pending += p.second.num("pending", 0);
    memory_unresolved += p.second.num("unresolved", 0);
    memory_failed += p.second.str("status") == "failed";
  }
  for (const auto &p : impl_->published_shared["relations"].fields())
    shared_unresolved += p.second.str("state") == "unresolved" || p.second.str("state") == "rejected";
  if (impl_->assessments)
    for (const auto &user : users) {
      const auto assessment = Json::parse(impl_->assessments->status(user));
      for (const auto &job : assessment["jobs"].fields())
        // 搜索结束但反馈尚未落库，也属于未完成；命令不能提前返回成功。
        // 合并计数，避免同一个作业被重复计算。
        assessment_pending +=
            job.second.str("status") == "pending" || job.second.str("status") == "running" ||
            job.second["memory_feedback"].str("status") == "pending";
      const auto state = assessment["training"].str("status");
      training_pending +=
          state == "training" || state == "ready_to_import" || state == "waiting_for_learning";
      training_failed += state == "training_failed" || state == "import_failed" ||
                         state == "learning_failed" || state == "budget_exhausted" ||
                         state == "training_exhausted";
    }
  return Json{
      {"pending_runs", impl_->learning_queue.size()},
      {"unpublished_runs", impl_->unpublished.size()},
      {"agent_evaluation_pending_jobs", assessment_pending},
      {"training_pending_users", training_pending},
      {"training_failed_users", training_failed},
      {"evaluating", impl_->evaluating},
      {"shared_pending_users", impl_->sharing_pending_users},
      {"shared_status", impl_->sharing_status},
      {"memory_management", impl_->memory_management},
      {"memory_pending_reviews", memory_pending},
      {"memory_unresolved_reviews", memory_unresolved},
      {"memory_management_failed_users", memory_failed},
      {"shared_unresolved_relations", shared_unresolved},
      {"shared_model_calls", impl_->published_shared.num("model_calls", 0)},
      {"shared_published_entries", shared_memory_view(impl_->published_shared).experiences_size()},
      {"agent_evaluation_error", impl_->assessment_error},
      {"shared_next_due_ms", impl_->sharing_due_ms},
      {"run_outcomes", outcomes},
      {"shared_quarantined_users", impl_->published_shared["user_errors"].object()
                                       ? impl_->published_shared["user_errors"].fields().size()
                                       : 0},
      {"managed_users", users.size()},
      {"data_dir", impl_->root.string()}}
      .dump();
}

// 提供本地接入所需的受限环境信息，不返回凭据原文。
std::string IntegrationService::local_environment(const std::string &token,
                                                  std::uint64_t known_version) {
  require(impl_->memory_source_root.empty(), "offline_evaluation_only", 409);
  std::lock_guard<std::mutex> guard(impl_->mutex);
  const auto user = impl_->authenticate(token);
  auto &version = impl_->memory_versions[user];
  if (!version)
    version = 1;
  // 原生缓存同时包含记忆和发布策略；任一变化都必须刷新。
  // 两个独立的单调计数相加，不让发布事件进入“新增记忆”的评测调度。
  const auto snapshot_version = version + impl_->publication_versions[user];
  Json result{{"memory_version", static_cast<std::size_t>(snapshot_version)},
              {"work_version", static_cast<std::size_t>(impl_->work_generation)}};
  if (known_version == snapshot_version)
    return result.dump();
  ep::EvolutionMemoryStore memory;
  if (!impl_->config.has("offline_version")) {
    impl_->ensure_memory(user);
    require(memory.ParseFromString(read_file(impl_->memory_path(user), impl_->run_limit)),
            "memory_invalid");
  }
  result["config"] = impl_->config;
  result["user_id"] = user;
  result["memory"] = json(memory);
  result["shared_memory"] = json(impl_->shared_view(user));
  result["published_versions"] = impl_->approved_versions[user];
  return result.dump();
}

// 把本地运行的可恢复状态交给后台持久化，保证采集缺口同步保存。
void IntegrationService::checkpoint_local(const std::string &token, const std::string &instance,
                                          const std::string &metrics) {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  require(instance.size() == 32 &&
              instance.find_first_not_of("0123456789abcdef") == std::string::npos,
          "instance_invalid");
  const auto user = impl_->authenticate(token);
  const auto value = Json::parse(metrics);
  safe_data(value);
  atomic_write(impl_->user_dir(user) / "local-runtime" / (instance + ".status.json"), value.dump());
}

// 持久化本地已接受状态，后台写入与业务运行锁分离。
IntegrationResponse IntegrationService::persist_local(const std::string &token,
                                                      const std::string &bytes) {
  try {
    auto run = Json::parse(bytes);
    std::lock_guard<std::mutex> guard(impl_->mutex);
    const auto user = impl_->authenticate(token), rid = run["identity"].str("run_id");
    impl_->evict_finished();
    require(run.flag("local_capture", false) && run.str("kind") == "integration-run-v1" &&
                run.str("profile") == "evolution.boundary.v1" && rid.size() == 40 &&
                rid.find_first_not_of("0123456789abcdef") == std::string::npos &&
                run["identity"].str("user_id") == user &&
                run["identity"].str("agent_id") == impl_->config.str("agent_id") &&
                identifier(run["identity"].str("conversation_id")),
            "local_state_invalid");
    safe_data(run);
    for (const auto &principal : impl_->principals)
      require(bytes.find(principal.first) == std::string::npos, "credential_value_rejected");
    const auto path = impl_->run_file(rid);
    const bool existed = fs::exists(path);
    require(existed || impl_->stored_count < impl_->config.num("max_stored_runs", 10000),
            "service_capacity_reached", 507);
    if (existed) {
      const auto previous = Json::parse(read_file(path, impl_->run_limit));
      require(previous.flag("local_capture", false), "run_id_conflict", 409);
      if (previous.flag("closed", false))
        return {200, R"({"ok":true})"};
    }
    if (run.flag("closed", false)) {
      if (!run.flag("incomplete", false))
        require(validate_trace(proto<v1::TraceRun>(impl_->trace(run))).report.trace_valid(),
                "trace_integrity_failed");
      run["trace_path"] = impl_->trace_path(run).string();
    }
    impl_->save(run);
    if (!existed)
      ++impl_->stored_count;
    impl_->runs[rid] = std::move(run);
    return {200, R"({"ok":true})"};
  } catch (const Error &error) {
    return {error.status, Json{{"ok", false}, {"error", error.what()}}.dump()};
  } catch (...) {
    return {500, R"({"ok":false,"error":"local_persistence_failed"})"};
  }
}

// 包装宿主业务调用：先查询策略，再最多执行一次原业务函数，最后记录结果。
IntegrationResponse IntegrationService::call(const std::string &token, const std::string &request) {
  try {
    require(request.size() <= impl_->request_limit, "request_too_large", 413);
    auto message = Json::parse(request);
    if (message.str("operation").rfind("agent_evaluation.", 0) == 0) {
      std::string user;
      {
        std::lock_guard<std::mutex> guard(impl_->mutex);
        user = impl_->authenticate(token);
        for (const auto &principal : impl_->principals)
          require(request.find(principal.first) == std::string::npos, "credential_value_rejected");
      }
      require(bool(impl_->assessments), "agent_evaluation_not_configured");
      safe_data(message);
      require(identifier(message.str("request_id")), "request_id_required");
      const auto op = message.str("operation");
      Json result;
      if (op == "agent_evaluation.status" || op == "agent_evaluation.feedback") {
        keys(message, {"operation", "request_id"});
        result = Json::parse(op == "agent_evaluation.status" ? impl_->assessments->status(user)
                                                             : impl_->assessments->feedback(user));
        // 管理端看见“已发布”时，HTTP 运行入口必须已经能取到该快照。
        // 这里只同步现成结果，不执行评测、经验管理或远程模型调用。
        if (op == "agent_evaluation.status")
          refresh_published_versions(user);
      } else if (op == "agent_evaluation.tree") {
        keys(message, {"operation", "request_id", "job_id"});
        result = Json::parse(impl_->assessments->search_tree(user, message.str("job_id")));
      } else if (op == "agent_evaluation.replay_holdout") {
        keys(message, {"operation", "request_id", "job_id"});
        result = Json::parse(impl_->assessments->replay_holdout(user, message.str("job_id")));
      } else if (op == "agent_evaluation.submit") {
        keys(message, {"operation", "request_id", "artifact"});
        // 上传实验不能扩大配置用户已有的授权范围。
        const auto &artifact = message["artifact"];
        require(artifact.str("level") <= impl_->config.str("max_level"),
                "evaluation_level_not_authorized", 403);
        if (artifact["policy"].object())
          require(artifact["policy"]["snapshot"]["identity"].str("agent_id") ==
                      impl_->config.str("agent_id"),
                  "evaluation_application_mismatch", 403);
        result = Json{{"job_id", impl_->assessments->submit(user, message["artifact"].dump())}};
      } else {
        keys(message, {"operation", "request_id", "job_id"});
        require(op == "agent_evaluation.retry" || op == "agent_evaluation.cancel",
                "operation_not_supported");
        if (op == "agent_evaluation.retry")
          impl_->assessments->retry(user, message.str("job_id"));
        else
          impl_->assessments->cancel(user, message.str("job_id"));
        result = Json{{"accepted", true}};
        std::lock_guard<std::mutex> guard(impl_->mutex);
        ++impl_->memory_versions[user];
        impl_->notify_work();
      }
      return {200, Json{{"ok", true}, {"result", result}}.dump()};
    }
    std::lock_guard<std::mutex> guard(impl_->mutex);
    if (message.str("operation") == "point") {
      impl_->authenticate(token);
      try {
        if (message.has("ref")) {
          auto result = impl_->call(token, message);
          return {200, Json{{"ok", true}, {"result", result}}.dump_for_transport()};
        }
        require(!message["context"].flag("degraded", false), "upstream_capture_degraded");
        if (impl_->buffered_capture && message["context"].has("capture_epoch"))
          require(message["context"].str("capture_epoch") == impl_->capture_epoch,
                  "capture_producer_restarted");
        const auto normalized = point_request(message, impl_->config);
        auto result = point_result(message, normalized, impl_->call(token, normalized));
        if (impl_->buffered_capture)
          result["context"]["capture_epoch"] = impl_->capture_epoch;
        return {200, Json{{"ok", true}, {"result", result}}.dump_for_transport()};
      } catch (...) {
        // 观测失败时业务继续，但不学习采集不完整的运行。
        // 只保留经过隐私检查且限长的错误码，绝不保留异常正文。
        std::string failure_code = "point_capture_degraded";
        try {
          throw;
        } catch (const Error &error) {
          const std::string code = error.what();
          if (!code.empty() && code.size() <= 96 && code.front() >= 'a' && code.front() <= 'z' &&
              code.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_") ==
                  std::string::npos) {
            try {
              safe_data(code);
              failure_code = code;
            } catch (...) {
            }
          }
        } catch (...) {
        }
        auto parent = static_cast<const Json &>(message)["context"];
        if (message.has("ref")) {
          try {
            const auto id = digest(impl_->config.str("agent_id") + "\n" +
                                   impl_->authenticate(token) + "\n" + point_run_key(message))
                                .substr(0, 40);
            parent = Json{{"run_id", id}, {"kind", message.str("kind", "custom")}};
            const auto existing = impl_->runs.find(id);
            if (existing != impl_->runs.end() &&
                existing->second["point_bindings"].has(point_call_key(message)))
              parent = existing->second["point_bindings"][point_call_key(message)]["before"];
          } catch (...) {
          }
        }
        const auto rid = parent["run_id"].string() ? parent["run_id"].text() : "";
        const auto request_id =
            message["request_id"].string() ? message["request_id"].text() : "invalid";
        const auto active = impl_->runs.find(rid);
        // 业务打点继续 fail-open；隔离评测必须及时报告采集失败，不能等整题超时。
        // 根打点失败时尚无 run 可写 gap；旧轮迟到的失败不得污染当前轮。
        auto &entry = impl_->evaluation_entry;
        if (entry.enabled() && entry.delivered && !entry.done && entry.failure.empty() &&
            impl_->authenticate(token) == entry.specification.str("user_id") &&
            ((!rid.empty() && entry.run_id == rid) ||
             (entry.run_id.empty() && active == impl_->runs.end() &&
              (!rid.empty() ||
               (message.str("kind") == "run" && message.str("phase") == "before")))))
          entry.failure = "evaluation_entry_capture_" + failure_code;
        if (active != impl_->runs.end() &&
            active->second["identity"].str("user_id") == impl_->authenticate(token) &&
            active->second.flag("buffered_capture", false) &&
            (!active->second.flag("closed", false) || impl_->capture_dirty.count(rid))) {
          active->second["incomplete"] = true;
          impl_->capture_poisoned.insert(rid);
        }
        try {
          impl_->call(token, Json{{"operation", "capture.gap"},
                                  {"request_id", "gap-" + digest(request_id).substr(0, 32)},
                                  {"run_id", rid},
                                  {"reason", "missing_boundary"},
                                  {"error_code", failure_code}});
          const auto kind = message["kind"].string()  ? message["kind"].text()
                            : parent["kind"].string() ? parent["kind"].text()
                                                      : "custom";
          const auto phase = message["phase"].string() ? message["phase"].text() : "invalid";
          if (message.has("ref") && impl_->runs.count(rid)) {
            auto damaged = impl_->runs.at(rid);
            abort_point_reference(damaged, message, impl_->config);
            if (damaged.flag("closed", false))
              damaged["trace_path"] = impl_->trace_path(damaged).string();
            impl_->save(damaged);
            impl_->runs[rid] = std::move(damaged);
          } else if (kind == "run" && (phase == "after" || phase == "abort"))
            impl_->call(token, Json{{"operation", "run.abort"},
                                    {"request_id", "abort-" + digest(request_id).substr(0, 32)},
                                    {"run_id", rid},
                                    {"status", "unknown"},
                                    {"output", nullptr}});
          else if (phase == "after" || phase == "abort") {
            // 回调已完成，但载荷不可用；只将当前边界
            // 以未知状态关闭，兄弟分支保持各自生命周期。
            if (kind == "loop" && parent["control_id"].string())
              impl_->call(token, Json{{"operation", "loop.abort"},
                                      {"request_id", "abort-" + digest(request_id).substr(0, 32)},
                                      {"run_id", rid},
                                      {"control_id", parent["control_id"]},
                                      {"decision", "continue"}});
            else if (parent["activity_id"].string())
              impl_->call(token, Json{{"operation", "activity.abort"},
                                      {"request_id", "abort-" + digest(request_id).substr(0, 32)},
                                      {"run_id", rid},
                                      {"activity_id", parent["activity_id"]},
                                      {"status", "unknown"},
                                      {"output", nullptr}});
          }
        } catch (...) {
        }
        return {200, Json{{"ok", true}, {"result", point_fallback(message, failure_code)}}
                         .dump_for_transport()};
      }
    }
    return {200, Json{{"ok", true}, {"result", impl_->call(token, message)}}.dump_for_transport()};
  } catch (const Error &error) {
    return {error.status, Json{{"ok", false}, {"error", error.what()}}.dump()};
  } catch (const legacy::EvaluationError &) {
    return {400, R"({"ok":false,"error":"evolution_contract_rejected"})"};
  } catch (...) {
    return {500, R"({"ok":false,"error":"integration_internal_error"})"};
  }
}

// 返回监听端口或配置端口，不触发新服务启动。
unsigned IntegrationService::port() const {
  return impl_->listen_port;
}

// 返回 HTTP 请求总期限配置，不在重试时重新扩大预算。
unsigned IntegrationService::http_timeout_ms() const {
  return impl_->timeout;
}

// 返回服务工作线程配置，供启动和容量管理使用。
unsigned IntegrationService::workers() const {
  return impl_->thread_count;
}

// 返回单请求大小上限，避免热路径无界编码。
std::size_t IntegrationService::max_request_bytes() const {
  return impl_->request_limit;
}

// 离线搜索只提交和等待候选评测，不消耗训练轨迹或触发记忆整理。
void IntegrationService::process_agent_evaluations() {
  std::unique_lock<std::mutex> worker(impl_->evaluation_mutex, std::try_to_lock);
  if (!worker.owns_lock())
    return;
  try {
    refresh_agent_evaluations();
  } catch (...) {
    std::lock_guard<std::mutex> guard(impl_->mutex);
    impl_->assessment_error = "agent_evaluation_refresh_failed";
    ++impl_->assessment_failures;
    impl_->assessment_retry_ms = Impl::millis() + 60000;
  }
}
// 在后台处理已到期的评测工作，遵守批次、频率和取消约束。
void IntegrationService::process_evaluations() {
  process_evaluations(false);
}

// 读取后台工作版本，供通知与等待避免频繁空闲刷新。
std::uint64_t IntegrationService::work_version() const {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  return impl_->work_generation;
}

// 唤醒等待中的后台线程，主调用方不等待实际学习完成。
void IntegrationService::wake_background() {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  impl_->notify_work();
}

// 等待工作版本变化或关闭通知，避免持续空转扫描。
void IntegrationService::wait_for_work(std::uint64_t observed, unsigned max_wait_ms) {
  std::unique_lock<std::mutex> guard(impl_->mutex);
  if (impl_->work_generation != observed)
    return;
  const auto now = Impl::millis();
  std::size_t due = 0, delay = max_wait_ms;
  if (!impl_->evaluating) {
    impl_->select_batch(false, &due);
    const auto shared_due =
        impl_->sharing_due_ms ? std::max(impl_->sharing_due_ms, impl_->storage_retry_ms) : 0;
    if (shared_due && (!due || shared_due < due))
      due = shared_due;
    const auto memory_due = impl_->memory_due_ms ? std::max(impl_->memory_due_ms, impl_->storage_retry_ms) : 0;
    if (memory_due && (!due || memory_due < due)) due = memory_due;
    if (impl_->assessment_retry_ms && impl_->assessment_failures < 2 &&
        (!due || impl_->assessment_retry_ms < due))
      due = impl_->assessment_retry_ms;
    for (const auto &retry : impl_->gepa_feedback_retry_ms)
      if (!due || retry.second < due)
        due = retry.second;
    if (!impl_->unpublished.empty())
      due = std::max(now, impl_->storage_retry_ms);
    if (due)
      delay = std::min(delay, due > now ? due - now : 0);
  }
  impl_->work_changed.wait_for(guard, std::chrono::milliseconds(delay),
                               [&] { return impl_->work_generation != observed; });
}

// 在后台处理已到期的评测工作，遵守批次、频率和取消约束。
void IntegrationService::process_evaluations(bool flush) {
  if (!impl_->memory_source_root.empty()) {
    process_agent_evaluations();
    return;
  }
  std::unique_lock<std::mutex> worker(impl_->evaluation_mutex, std::try_to_lock);
  if (!worker.owns_lock())
    return;
  // 独立工作线程负责任务执行与评分，此处只刷新快照并提交变更版本。
  // 主 Agent 打点永远不进入这条阻塞路径。
  try {
    refresh_agent_evaluations();
  } catch (...) {
    std::lock_guard<std::mutex> guard(impl_->mutex);
    impl_->assessment_error = "agent_evaluation_refresh_failed";
    ++impl_->assessment_failures;
    impl_->assessment_retry_ms = Impl::millis() + 60000;
  }
  Json job;
  std::string stage = "trace_publication";
  try {
    std::vector<std::string> selected;
    {
      std::lock_guard<std::mutex> guard(impl_->mutex);
      if (Impl::millis() < impl_->storage_retry_ms)
        return;
      impl_->maintain_runs();
      const auto unpublished = impl_->unpublished;
      std::size_t published = 0;
      for (const auto &id : unpublished) {
        if (published++ >= impl_->schedule.num("max_batch_runs", 32))
          break;
        auto run = impl_->read_run(id);
        try {
          const auto path = impl_->trace_path(run);
          const auto bytes = impl_->trace(run).dump();
          if (fs::exists(path))
            require(read_file(path, impl_->run_limit) == bytes, "stored_trace_changed");
          else
            atomic_write(path, bytes);
          run["learning"]["trace_published"] = true;
        } catch (...) {
          run["learning"]["publication_error"] = "trace_publication_failed";
          if (!run.flag("incomplete", false) && run.str("evaluation_status") != "disabled") {
            run["evaluation_status"] = "failed";
            run["evaluation_error"] = "trace_publication_failed";
          }
        }
        impl_->save_closed(run);
      }
      impl_->evict_finished();
      selected = impl_->select_batch(flush);
      if (selected.empty() && (!impl_->sharing_due_ms || impl_->sharing_due_ms > Impl::millis()) &&
          (!impl_->memory_due_ms || impl_->memory_due_ms > Impl::millis()))
        return;
      impl_->evaluating = true;
    }
    stage = "evaluation_configuration";
    auto configuration = legacy::load_runtime_configuration(impl_->evaluator_path);
    // 后台写入不能发布大到下一份运行快照无法读取的私有库，
    // 管理器和状态查询也必须能够读取它。
    configuration.evolution.max_memory_bytes = std::min<std::size_t>(
        configuration.evolution.max_memory_bytes,
        std::min(impl_->run_limit,
                 impl_->config["shadow_options"].num("max_memory_bytes", 16777216)));
    if (!configuration.max_output_tokens_explicit)
      configuration.model.max_output_tokens = configuration.sessionlog_max_output_tokens;
    std::shared_ptr<legacy::ModelAdapter> model;
    if (!configuration.model.endpoint.empty()) {
      if (!configuration.model_selection_explicit)
        configuration.evaluator.model_selection = legacy::ModelSelection::suspicious;
      // 规则分析和确定性记忆合并不依赖模型。关闭分析与元反思后，即使保留
      // 连接配置但未加载密钥，也应能完成入库；公共提炼等待模型，不阻断私有库。
      const bool model_required =
          configuration.evaluator.model_selection != legacy::ModelSelection::none ||
          configuration.evolution.enable_meta_reflection;
      if (!configuration.api_key_env.empty()) {
        const auto *key = std::getenv(configuration.api_key_env.c_str());
        if (!key || !*key) {
          require(selected.empty() || !model_required, "evaluation_api_key_missing");
        } else {
          configuration.model.api_key = key;
          configuration.evaluator.redaction_secrets.push_back(key);
        }
      } else
        require(configuration.no_api_key || !model_required, "evaluation_api_key_missing");
      if (configuration.no_api_key || !configuration.model.api_key.empty())
        model = legacy::make_openai_compatible_model_adapter(configuration.model);
    }
    if (selected.empty()) {
      impl_->run_memory_maintenance(configuration, model);
      impl_->run_sharing(model);
      std::lock_guard<std::mutex> guard(impl_->mutex);
      impl_->evaluating = false;
      return;
    }
    {
      std::lock_guard<std::mutex> guard(impl_->mutex);
      stage = "analysis_job_setup";
      auto first = impl_->read_run(selected.front());
      if (!first["learning"].str("job_id").empty())
        job = impl_->read_job(first["learning"].str("job_id"));
      else {
        if (model && configuration.evaluator.model_selection != legacy::ModelSelection::none &&
            configuration.evaluator.max_model_calls)
          selected.resize(std::min(selected.size(), configuration.evaluator.max_model_calls));
        Json ids = Json::Array{}, retries = Json::Object{};
        std::string material = first["identity"].str("user_id");
        for (const auto &id : selected) {
          const auto run = impl_->read_run(id);
          ids.push(id);
          retries[id] = run["learning"].num("retry_count", 0);
          material += '\0' + id + ':' + std::to_string(run["learning"].num("retry_count", 0));
        }
        const auto job_id = digest(material);
        // A collision must not overwrite the existing failed job and its evidence.
        require(!fs::exists(impl_->job_path(job_id)), "learning_job_already_exists");
        job = Json{{"schema_version", 1},
                   {"id", job_id},
                   {"user", first["identity"]["user_id"]},
                   {"runs", ids},
                   {"retry_counts", retries},
                   {"errors", Json::Object{}},
                   {"phase", "analysis"},
                   {"state", "pending"}};
      }
      job["state"] = "running";
      impl_->save_job(job);
      impl_->next_batch_ms = Impl::millis() + impl_->schedule.num("min_interval_ms", 60000);
      atomic_write(impl_->root / "learning/schedule.json",
                   Json{{"schema_version", 1}, {"next_batch_ms", impl_->next_batch_ms}}.dump());
      for (const auto &id : job["runs"].items()) {
        if (job["errors"].has(id.text()))
          continue;
        auto run = impl_->read_run(id.text());
        require(run.flag("closed", false) && !run.flag("incomplete", false) &&
                    run["identity"].str("user_id") == job.str("user"),
                "learning_job_identity_mismatch");
        run["learning"]["job_id"] = job["id"];
        run["learning"]["phase"] = "analysis";
        run["evaluation_status"] = "running";
        impl_->save_closed(run);
      }
    }
    const auto directory = impl_->user_dir(job.str("user"));
    std::vector<ep::HybridEvaluationReport> reports;
    std::size_t model_calls = 0, failed_calls = 0, report_bytes = 0;
    std::string report_material;
    stage = "trace_evaluation";
    // 只有独立轨迹分析进入工作线程；作业状态、预算、报告提交和后续记忆管理
    // 始终由当前学习线程处理，避免并发分支改写同一用户的记忆或错误记录。
    struct PendingAnalysis {
      std::size_t index;
      Json run;
      fs::path report_path;
      std::future<ep::HybridEvaluationReport> result;
    };
    const auto &run_ids = job["runs"].items();
    std::vector<ep::HybridEvaluationReport> ordered_reports(run_ids.size());
    std::vector<std::string> report_hashes(run_ids.size());
    std::vector<PendingAnalysis> pending;
    const auto parallelism = impl_->schedule.num("analysis_parallelism", 1);
    pending.reserve(parallelism);
    // 已完成、失败和正在请求的分析共同占用批次预算；重启不能重新获得预算。
    std::size_t reserved_calls = 0;
    for (const auto &id : run_ids)
      reserved_calls += job["analysis_model_calls"].num(id.text(), 0);

    const auto fail_analysis = [&](const std::string &id, std::exception_ptr failure) {
      std::string code = "trace_evaluation_failed";
      try {
        std::rethrow_exception(failure);
      } catch (const Error &error) {
        code = error.what();
      } catch (const legacy::EvaluationError &error) {
        code = evaluation_error_code(error);
      } catch (...) {
      }
      failed_calls += job["analysis_model_calls"].num(id, 0);
      job["errors"][id] = code;
      impl_->save_job(job);
    };
    const auto commit_analysis = [&](std::size_t index, Json &run, const fs::path &report_path,
                                     ep::HybridEvaluationReport report, bool cached) {
      const auto id = run_ids[index].text();
      legacy::ensure_safe_evolution_data(report);
      require(report.source_trace_integrity_valid(), "analysis_trace_invalid");
      require(report.source().user_id() == run["identity"].str("user_id") &&
                  report.source().conversation_id() == run["identity"].str("conversation_id") &&
                  report.source().dialogue_id() == run.str("import_source_id", id),
              "analysis_identity_mismatch");
      const auto bytes = json(report).dump();
      if (!cached)
        atomic_write(report_path, bytes);
      const auto protobuf_path = report_path.parent_path() / (id + ".evaluation.pb");
      if (!fs::exists(protobuf_path))
        atomic_write(protobuf_path, report.SerializeAsString());
      auto &state = run["learning"];
      state["analysis_state"] = "completed";
      state["report_fingerprint"] = digest(read_file(report_path, impl_->run_limit));
      {
        std::lock_guard<std::mutex> guard(impl_->mutex);
        impl_->save_closed(run);
      }
      reserved_calls -= job["analysis_model_calls"].num(id, 0);
      reserved_calls += report.model_requested_count();
      job["analysis_model_calls"][id] = std::size_t(report.model_requested_count());
      model_calls += report.model_requested_count();
      report_bytes += bytes.size();
      report_hashes[index] = state.str("report_fingerprint");
      ordered_reports[index] = std::move(report);
    };
    const auto collect_ready = [&] {
      // 收取任意已完成分支，不让一个慢请求挡住其他结果落盘和后续请求启动。
      // 仅在有请求在途时短暂等待；空队列仍由原调度器阻塞等待通知。
      for (;;) {
        for (auto it = pending.begin(); it != pending.end(); ++it) {
          if (it->result.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready)
            continue;
          try {
            commit_analysis(it->index, it->run, it->report_path, it->result.get(), false);
          } catch (...) {
            fail_analysis(run_ids[it->index].text(), std::current_exception());
          }
          pending.erase(it);
          return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
      }
    };
    for (std::size_t index = 0; index < run_ids.size(); ++index) {
      const auto &id = run_ids[index];
      if (job["errors"].has(id.text())) {
        failed_calls += job["analysis_model_calls"].num(id.text(), 0);
        continue;
      }
      auto run = impl_->read_run(id.text());
      try {
        const auto raw = read_file(impl_->trace_path(run), impl_->run_limit);
        require(run.flag("imported", false) ? digest(raw) == run.str("import_content_fingerprint")
                                            : raw == impl_->trace(run).dump(),
                "analysis_source_changed");
        const auto source_hash = digest(raw);
        auto &state = run["learning"];
        require(!state.has("source_fingerprint") || state.str("source_fingerprint") == source_hash,
                "analysis_source_changed");
        state["source_fingerprint"] = source_hash;
        const auto path =
            directory / "evaluation" / path_component(run["identity"].str("conversation_id"));
        const auto report_path = path / (id.text() + ".evaluation.json");
        if (fs::exists(report_path)) {
          const auto bytes = read_file(report_path, impl_->run_limit);
          require(!state.has("report_fingerprint") ||
                      state.str("report_fingerprint") == digest(bytes),
                  "analysis_cache_changed");
          auto report = proto<ep::HybridEvaluationReport>(Json::parse(bytes));
          require(
              report.source().user_id() == run["identity"].str("user_id") &&
                  report.source().conversation_id() == run["identity"].str("conversation_id") &&
                  report.source().dialogue_id() == run.str("import_source_id", id.text()) &&
                  report.source_fingerprint() ==
                      (run.flag("imported", false)
                           ? run.str("import_evidence_fingerprint")
                           : digest(trace_content_jsonl(proto<v1::TraceRun>(impl_->trace(run))))),
              "analysis_cache_source_mismatch");
          commit_analysis(index, run, report_path, std::move(report), true);
        } else {
          require(state.str("analysis_state") != "completed", "analysis_cache_missing");
          require(state.str("analysis_state") != "running", "analysis_interrupted_unknown_outcome");
          while (pending.size() >= parallelism)
            collect_ready();
          state["analysis_state"] = "running";
          {
            std::lock_guard<std::mutex> guard(impl_->mutex);
            impl_->save_closed(run);
          }
          auto options = configuration.evaluator;
          // 标准轨迹与 SessionLog 每轮最多一次内容分析，不能让并发请求各自
          // 拿到整批剩余预算，挤占后续元反思与记忆整理的调用额度。
          options.max_model_calls = reserved_calls < options.max_model_calls ? 1 : 0;
          // 进入适配器之前就预留一次内容评测调用，
          // 严格模式失败、报告尚未到达输出边界的调用也要计入。
          job["analysis_model_calls"][id.text()] =
              std::size_t(model && options.model_selection != legacy::ModelSelection::none &&
                          options.max_model_calls > 0);
          reserved_calls += job["analysis_model_calls"].num(id.text(), 0);
          impl_->save_job(job);
          const auto source = impl_->trace_path(run);
          const bool sessionlog = run.str("import_format") == "sessionlog";
          // 子线程只持有不可变输入和独立评测器。共享模型适配器内部对连接池
          // 加锁，每次请求使用独立句柄；不捕获 impl、job 或可变运行状态。
          pending.push_back({index, std::move(run), report_path,
                             std::async(std::launch::async, [options, model, source, sessionlog] {
                               const legacy::HybridTrajectoryEvaluator evaluator(options, model);
                               return (sessionlog ? evaluator.evaluate_sessionlog(source)
                                                  : evaluator.evaluate_file(source)).report;
                             })});
        }
      } catch (...) {
        fail_analysis(id.text(), std::current_exception());
      }
    }
    while (!pending.empty())
      collect_ready();
    // 完成顺序可变，但合成输入必须遵循冻结批次顺序，保持重启后的候选指纹稳定。
    for (std::size_t index = 0; index < run_ids.size(); ++index) {
      if (report_hashes[index].empty())
        continue;
      report_material += run_ids[index].text() + ':' + report_hashes[index] + '\n';
      reports.push_back(std::move(ordered_reports[index]));
    }
    require(report_bytes <= impl_->schedule.num("max_batch_bytes", 33554432),
            "learning_report_batch_too_large");
    auto batch_options = configuration.evaluator;
    batch_options.max_model_calls = failed_calls < batch_options.max_model_calls
                                        ? batch_options.max_model_calls - failed_calls
                                        : 0;
    legacy::TrajectoryEvolutionPipeline pipeline(batch_options, configuration.evolution, model);
    if (!reports.empty()) {
      stage = "batch_synthesis";
      ep::HybridEvaluationBatchReport batch;
      const auto path = impl_->candidate_path(job);
      const auto report_hash = digest(report_material);
      require(!job.has("reports_fingerprint") || job.str("reports_fingerprint") == report_hash,
              "candidate_cache_source_mismatch");
      job["reports_fingerprint"] = report_hash;
      if (fs::exists(path)) {
        const auto bytes = read_file(path, configuration.evolution.max_memory_bytes);
        require(!job.has("candidate_fingerprint") ||
                    job.str("candidate_fingerprint") == digest(bytes),
                "candidate_cache_changed");
        require(batch.ParseFromString(bytes), "candidate_cache_invalid");
        require(batch.runs_size() == static_cast<int>(reports.size()),
                "candidate_cache_source_mismatch");
        legacy::ensure_safe_evolution_data(batch);
      } else {
        require(job.str("phase") != "synthesis_started" ||
                    !job.flag("synthesis_uses_model", false) ||
                    job.flag("resume_authorized", false),
                "synthesis_interrupted_unknown_outcome");
        job["phase"] = "synthesis_started";
        job["synthesis_uses_model"] =
            model != nullptr && configuration.evolution.enable_meta_reflection;
        job["resume_authorized"] = false;
        impl_->save_job(job);
        const auto memory = legacy::TrajectoryEvolutionPipeline::load_memory(
            impl_->memory_path(job.str("user")), configuration.evolution.max_memory_bytes);
        auto generated = pipeline.synthesize_reports(reports, memory);
        batch = std::move(generated.report);
        job["synthesis_model_calls"] = generated.model_requests.size();
        impl_->save_job(job);
        const auto bytes = batch.SerializeAsString();
        atomic_write(path, bytes);
        job["candidate_fingerprint"] = digest(bytes);
      }
      job["phase"] = "memory_curation";
      impl_->save_job(job);
      stage = "memory_curation";
      auto update = pipeline.curate_memory_file(
          batch, impl_->memory_path(job.str("user")), directory / "memory/evolution-memory.json",
          configuration.evolution.max_memory_bytes,
          model_calls + job.num("synthesis_model_calls", configuration.evaluator.max_model_calls));
      *batch.mutable_memory_update() = update.result;
      {
        std::lock_guard<std::mutex> guard(impl_->mutex);
        if (update.result.revision_after() != update.result.revision_before()) {
          auto &version = impl_->memory_versions[job.str("user")];
          version = std::max<std::uint64_t>(version, 1) + 1;
          impl_->notify_work();
        }
      }
      const auto output = directory / "batches" / digest(batch.batch_id());
      atomic_write(output / "batch.evaluation.json", json(batch).dump());
      atomic_write(output / "batch.evaluation.pb", batch.SerializeAsString());
      job["batch_id"] = batch.batch_id();
      try {
        impl_->queue_sharing(job.str("user"));
      } catch (...) {
        job["sharing_error"] = "shared_queue_failed";
      }
    }
    job["state"] = "completed";
    job["phase"] = "completed";
    job["error"] = "";
    {
      std::lock_guard<std::mutex> guard(impl_->mutex);
      impl_->save_job(job);
      impl_->finish_job(job);
      impl_->next_batch_ms = Impl::millis() + impl_->schedule.num("min_interval_ms", 60000);
      atomic_write(impl_->root / "learning/schedule.json",
                   Json{{"schema_version", 1}, {"next_batch_ms", impl_->next_batch_ms}}.dump());
      impl_->notify_work();
      impl_->evict_finished();
    }
    impl_->refresh_memory_management();
    impl_->run_sharing(model);
    {
      std::lock_guard<std::mutex> guard(impl_->mutex);
      impl_->evaluating = false;
    }
  } catch (...) {
    std::string code = stage + "_failed";
    try {
      throw;
    } catch (const Error &error) {
      // Only static codes cross this boundary; never persist exception/provider text.
      static const std::set<std::string> allowed{
          "evaluation_api_key_missing", "learning_job_already_exists",
          "learning_job_identity_mismatch", "learning_job_invalid",
          "learning_job_duplicate_run"};
      if (allowed.count(error.what())) code = error.what();
    } catch (const legacy::EvaluationError &error) {
      if (stage == "trace_evaluation") code = evaluation_error_code(error);
    } catch (...) {
    }
    std::lock_guard<std::mutex> guard(impl_->mutex);
    impl_->evaluating = false;
    impl_->storage_retry_ms = Impl::millis() + impl_->schedule.num("retry_backoff_ms", 60000);
    impl_->next_batch_ms = std::max(impl_->next_batch_ms, impl_->storage_retry_ms);
    try {
      if (!job.null()) {
        job["state"] = "failed";
        job["error"] = code;
        impl_->save_job(job);
        impl_->finish_job(job);
      }
      atomic_write(impl_->root / "learning/schedule.json",
                   Json{{"schema_version", 1},
                        {"next_batch_ms", impl_->next_batch_ms},
                        {"retry_after_ms", impl_->storage_retry_ms},
                        {"last_error", code}}
                       .dump());
    } catch (...) {
      for (auto &pair : impl_->learning_queue)
        if (pair.second.str("state") == "running")
          pair.second["state"] = "pending";
    }
    impl_->notify_work();
    impl_->evict_finished();
  }
}
} // 命名空间结束：trajectory::evolution
