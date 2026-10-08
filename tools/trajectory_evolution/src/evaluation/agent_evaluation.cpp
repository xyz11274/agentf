// 后台评测服务：管理候选作业、训练引导、GEPA 调度与发布验收。
// 主 Agent 的打点不等待这里的模型评测；队列、取消和频率限制由服务统一维护。
// 搜索分数与发布结论分开：候选表现改善不等于已经满足发布条件。

// 后台评测负责训练引导与候选验证；提出候选本身不等于批准发布。
#include "trajectory_evolution/agent_evaluation.h"
#include "trajectory_evolution/task_evaluation.h"
#include "integration_json.h"
#include "local_support.h"
#include "control_generation.h"
#include "gepa_search.h"
#include "gepa_feedback.h"
#include "agentflow/trajectory/runtime_config.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <thread>
#include <openssl/sha.h>
#include <openssl/evp.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

namespace trajectory::evolution {
namespace {
using namespace integration;
namespace fs = std::filesystem;

// 计算内容摘要，作为版本或内容身份；相同输入必须得到相同结果。
std::string digest(const std::string &bytes) {
  unsigned char out[SHA256_DIGEST_LENGTH];
  require(SHA256(reinterpret_cast<const unsigned char *>(bytes.data()), bytes.size(), out),
          "evaluation_hash_failed");
  std::string result;
  for (auto c : out) {
    result += "0123456789abcdef"[c >> 4];
    result += "0123456789abcdef"[c & 15];
  }
  return result;
}

// 读取当前墙上时钟并换算为毫秒；用于持久化时间标记，不代替单调时钟的超时计时。
std::size_t now_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

// 按调用方给出的大小上限读取文件；读取失败向上层报告，不能用空数据冒充成功。
std::string read(const fs::path &path, std::size_t limit = 4194304) {
  fs::path prefix;
  for (const auto &part : fs::absolute(path)) {
    prefix /= part;
    require(!fs::is_symlink(prefix), "evaluation_symlink_rejected");
  }
  std::ifstream stream(path, std::ios::binary | std::ios::ate);
  require(stream && stream.tellg() >= 0 && static_cast<std::size_t>(stream.tellg()) <= limit,
          "evaluation_file_unavailable");
  std::string bytes(static_cast<std::size_t>(stream.tellg()), '\0');
  stream.seekg(0);
  require(bytes.empty() || bool(stream.read(bytes.data(), bytes.size())), "evaluation_read_failed");
  return bytes;
}

// 创建并验证工作目录，为后续持久化准备路径。
void directory(const fs::path &path) {
  fs::path prefix;
  for (const auto &part : fs::absolute(path)) {
    prefix /= part;
    require(!fs::is_symlink(prefix), "evaluation_symlink_rejected");
  }
  fs::create_directories(path);
  require(::chmod(path.c_str(), 0700) == 0, "evaluation_permission_failed");
}

// 按照本模块的输出约定写入内容；调用方负责提供已验证的目标和数据。
void write(const fs::path &path, const std::string &bytes) {
  directory(path.parent_path());
  require(!fs::is_symlink(path), "evaluation_symlink_rejected");
  std::string pending = path.string() + ".pending.XXXXXX";
  int fd = ::mkstemp(pending.data());
  require(fd >= 0, "evaluation_write_failed");
  try {
    std::size_t offset = 0;
    while (offset < bytes.size()) {
      const auto n = ::write(fd, bytes.data() + offset, bytes.size() - offset);
      require(n > 0, "evaluation_write_failed");
      offset += n;
    }
    require(::fsync(fd) == 0, "evaluation_write_failed");
    ::close(fd);
    fd = -1;
    require(::rename(pending.c_str(), path.c_str()) == 0, "evaluation_publish_failed");
    int parent = ::open(path.parent_path().c_str(), O_RDONLY | O_DIRECTORY);
    if (parent >= 0) {
      ::fsync(parent);
      ::close(parent);
    }
  } catch (...) {
    if (fd >= 0)
      ::close(fd);
    ::unlink(pending.c_str());
    throw;
  }
}

// 读取数值指标，供比较与发布门槛使用；不通过隐式转换伪造观测。
double numeric(const Json &value, double fallback = 0) {
  return std::holds_alternative<double>(value.value) ? std::get<double>(value.value) : fallback;
}

// 生成规范化表示，消除容器遍历顺序对版本判断的影响。
Json canonical(Json artifact) {
  if (artifact["policy"].object()) {
    auto &policy = artifact["policy"];
    policy.fields().erase("policy_id");
    auto &snapshot = policy["snapshot"];
    for (const auto *key : {"snapshot_id", "memory_revision", "shared_revision", "excluded_counts"})
      snapshot.fields().erase(key);
    snapshot["identity"].fields().erase("run_id");
    snapshot["identity"].fields().erase("conversation_id");
    policy["options"].fields().erase("allowed_conversations");
  }
  return artifact;
}

// 按应用与用户划分后台评测通道，防止不同策略的发布状态相互覆盖。
std::string lane(const std::string &user, const std::string &level) {
  return digest(user + '\n' + level);
}

// 从运行配置构造固定基线变体，确保它不加载待评候选策略。
Json baseline(const Json &artifact) {
  Json result{{"schema_version", 1},
              {"agent", artifact["agent"]},
              {"user_id", artifact["user_id"]},
              {"level", "L0"},
              {"policy", nullptr}};
  // AppWorld's window mode controls only L1 advice. Keeping it on L0 would
  // create a different artifact identity for an unchanged baseline execution.
  auto &agent = result["agent"];
  if (agent.has("appworld_options") && agent["appworld_options"].object()) {
    agent["appworld_options"].fields().erase("l1_window_mode");
    agent["appworld_options"].fields().erase("l1_observed_phase_experiences");
    agent["appworld_options"].fields().erase("l1_saved_result_experiences");
    agent["appworld_options"].fields().erase("l1_auth_parameter_experiences");
    agent["appworld_options"].fields().erase("l1_auth_structured_missing");
    agent["appworld_options"].fields().erase("l1_shape_error_auth_experiences");
    agent["appworld_options"].fields().erase("l1_shape_error_state_forwarding");
    if (agent["appworld_options"].fields().empty()) agent.fields().erase("appworld_options");
  }
  return result;
}

// 读取候选关联的来源记忆，供真实效果回传与状态管理使用。
Json memories(const Json &artifact) {
  Json result = Json::Object{};
  const auto &policy = artifact["policy"];
  if (!policy.object() || !policy["options"]["approved_experiences"].array())
    return result;
  for (const auto &approval : policy["options"]["approved_experiences"].items()) {
    const auto &experiences = policy["snapshot"]["experiences"];
    if (!experiences.array())
      continue;
    for (const auto &item : experiences.items())
      if (item.str("memory_id") == approval.str("memory_id") &&
          item.str("content_revision") == approval.str("content_revision"))
        result[item.str("memory_id")] = item;
  }
  return result;
}
} // 结束当前匿名命名空间。

// 生成宿主执行版本身份，绑定实际运行配置与策略。
std::string agent_version_id(const std::string &bytes) {
  require(bytes.size() <= 2097152, "agent_version_too_large");
  auto artifact = Json::parse(bytes);
  keys(artifact, {"schema_version", "agent", "user_id", "level", "policy", "runtime_options"});
  require(artifact.num("schema_version", 0) == 1 && identifier(artifact.str("user_id")) &&
              artifact["agent"].object() && !artifact["agent"].str("revision").empty(),
          "agent_version_invalid");
  const auto level = artifact.str("level");
  require(level.size() == 2 && level[0] == 'L' && level[1] >= '0' && level[1] <= '3',
          "agent_version_level_invalid");
  if (artifact.has("runtime_options")) {
    // 只允许冻结实际影响收尾行为的模板，不能把部署配置或凭据带入策略包。
    const auto &options = artifact["runtime_options"];
    require(options.object(), "agent_version_runtime_options_invalid");
    keys(options, {"finalization_context"});
    require(options["finalization_context"].string() &&
                options.str("finalization_context").size() <= 16384,
            "agent_version_finalization_context_invalid");
  }
  validate_local_data(artifact);
  if (level == "L0")
    require(artifact["policy"].null(), "baseline_policy_must_be_empty");
  else {
    PolicyRuntime policy(proto<v1::RuntimePolicy>(artifact["policy"]));
    require(policy.policy().snapshot().identity().user_id() == artifact.str("user_id") &&
                int(policy.policy().effective_level()) == int(v1::L0) + level[1] - '0',
            "agent_version_policy_mismatch");
  }
  return "agent-version-v1:" + digest(canonical(std::move(artifact)).dump());
}

// 把候选策略绑定到评测宿主配置，避免只改报告等级却未真正加载规则。
v1::RuntimePolicy bind_runtime_policy(const v1::RuntimePolicy &policy,
                                      const v1::Identity &identity) {
  PolicyRuntime checked(policy);
  auto options = policy.options();
  options.clear_allowed_conversations();
  options.add_allowed_conversations(identity.conversation_id());
  return compile_runtime_policy(bind_shadow_policy(policy.snapshot(), identity), options);
}

// 检查已发布策略是否符合当前宿主能力和授权，不要求复用历史实验目录。
bool published_policy_compatible(const v1::RuntimePolicy &published,
                                 const v1::RuntimePolicy &current,
                                 const std::string &control_learning_json) {
  if (published.effective_level() != current.effective_level())
    return false;
  // Binding narrows both policies to the active conversation. Check the
  // original permission lists first so normalization cannot hide an expanded
  // or unrelated conversation scope. Retain the existing portable binding
  // from an unbound policy (or its own single conversation) to the host's
  // single active conversation.
  const auto &published_conversations = published.options().allowed_conversations();
  const auto &current_conversations = current.options().allowed_conversations();
  const bool same_conversations =
      published_conversations.size() == current_conversations.size() &&
      std::equal(published_conversations.begin(), published_conversations.end(),
                 current_conversations.begin());
  const bool portable_conversation =
      current_conversations.size() == 1 &&
      current_conversations.Get(0) == current.snapshot().identity().conversation_id() &&
      (published_conversations.empty() ||
       (published_conversations.size() == 1 &&
        published_conversations.Get(0) == published.snapshot().identity().conversation_id()));
  if (!same_conversations && !portable_conversation)
    return false;
  auto old = bind_runtime_policy(published, current.snapshot().identity());
  const auto bound_current = bind_runtime_policy(current, current.snapshot().identity());
  const auto controls = control_learning_json.empty() ? Json() : Json::parse(control_learning_json);
  if (!generated_controls_authorized(old, bound_current, controls))
    return false;
  auto a = old.options(), b = bound_current.options();
  a = without_generated_controls(a);
  b = without_generated_controls(b);
  a.clear_approved_experiences();
  b.clear_approved_experiences();
  a.clear_experience_overrides();
  b.clear_experience_overrides();
  a.clear_disabled_injections();
  b.clear_disabled_injections();
  // compile_runtime_policy has checked that these L1 windows refer only to
  // approved, active overrides. They are mutable retrieval hints, not wider
  // identity permissions, model budgets or executable control capabilities.
  a.clear_l1_prevention_sources();
  b.clear_l1_prevention_sources();
  if (json(a).dump() != json(b).dump() ||
      json(old.snapshot().options()).dump() != json(bound_current.snapshot().options()).dump())
    return false;
  for (const auto &approved : old.options().approved_experiences()) {
    bool allowed = false, present = false;
    for (const auto &other : bound_current.options().approved_experiences())
      allowed |= approved.memory_id() == other.memory_id() &&
                 approved.content_revision() == other.content_revision();
    for (const auto &item : bound_current.snapshot().experiences())
      present |= approved.memory_id() == item.memory_id() &&
                 approved.content_revision() == item.content_revision();
    if (!allowed || !present)
      return false;
  }
  // 即使两段经验文本都仍存在，关系变化也可能改变它们能否共同被选中。
  if (old.snapshot().cross_relations_size() != bound_current.snapshot().cross_relations_size())
    return false;
  for (int i = 0; i < old.snapshot().cross_relations_size(); ++i)
    if (json(old.snapshot().cross_relations(i)).dump() !=
        json(bound_current.snapshot().cross_relations(i)).dump())
      return false;
  return true;
}

  // A cancelled reflection plan is not an executing batch. Keep its original
// parent evidence and paid drafts when resuming under the same judge/dataset.
// Unknown states, unbound plans and any open evaluation remain non-resumable.
bool closed_reflection_checkpoint(const Json &tree, const Json &job,
                                  const std::string &evaluation_context,
                                  const std::string &dataset_version,
                                  const std::string &candidate_context,
                                  const v1::RuntimePolicy *approved_source) {
  if (job.str("status") != "cancelled" || !job.flag("cancel_requested", false) ||
      tree.str("status") != "cancelled" ||
      tree.str("context") != job.str("context") ||
      tree.str("seed_version") != job.str("seed_version") ||
      tree.str("evaluation_context") != evaluation_context ||
      tree.str("dataset_version") != dataset_version ||
      tree.flag("holdout_started", false) || !tree["selected_version"].null() ||
      !tree["nodes"].array() || !tree["batch"].object() ||
      !tree["evaluations"].object())
    return false;
  const auto &batch = tree["batch"];
  if (!batch["nodes"].array() || !batch["plans"].array() ||
      batch["nodes"].items().empty() || batch["nodes"].items().size() > 16 ||
      batch["nodes"].items().size() != batch["plans"].items().size())
    return false;
  std::set<std::size_t> present;
  for (std::size_t index = 0; index < batch["nodes"].items().size(); ++index) {
    const auto &id = batch["nodes"].items()[index];
    if (!std::holds_alternative<double>(id.value)) return false;
    const auto position = id.integer();
    if (position == 0 || position >= tree["nodes"].items().size() ||
        !present.insert(position).second)
      return false;
    const auto &node = tree["nodes"].items()[position];
    const auto &plan = batch["plans"].items()[index];
    const bool pending_completed_parent =
        node.str("status") == "pending" && !candidate_context.empty() &&
        !tree.str("candidate_evaluation_context").empty() &&
        tree.str("candidate_evaluation_context") == job.str("candidate_evaluation_context");
    bool components_bound = node["components"].dump() == plan["components"].dump();
    if (!components_bound && pending_completed_parent && approved_source &&
        node["components"].array() && plan["components"].array() &&
        node["components"].items().size() == 1 && plan["components"].items().size() == 1) {
      const auto &fallback = node["component_fallback"];
      const auto original = plan["components"].items().front().text();
      const auto selected = node["components"].items().front().text();
      const auto approved_experience = [&](const std::string &component) {
        const std::string prefix = "experience:";
        if (component.compare(0, prefix.size(), prefix) != 0)
          return false;
        const auto memory = component.substr(prefix.size());
        for (const auto &ref : approved_source->options().approved_experiences())
          if (ref.memory_id() == memory)
            for (const auto &source : approved_source->snapshot().experiences())
              if (source.memory_id() == memory &&
                  source.content_revision() == ref.content_revision() &&
                  std::find(approved_source->snapshot().options().target_scopes().begin(),
                            approved_source->snapshot().options().target_scopes().end(),
                            source.target_scope()) !=
                      approved_source->snapshot().options().target_scopes().end())
                return true;
        return false;
      };
      components_bound = fallback.object() &&
          fallback.str("reason") == "experience_not_applied_in_training" &&
          fallback.str("from") == original && fallback.str("to") == selected &&
          original != selected && approved_experience(original) && approved_experience(selected);
    }
    if (node.num("id", SIZE_MAX) != position || node.str("kind") != "reflection" ||
        (node.str("status") != "reflecting" && !pending_completed_parent) ||
        !node["version"].null() ||
        node["parent_evaluation"].str("status") != "complete" ||
        !node["parent_evaluation"]["missing_task_ids"].array() ||
        !node["parent_evaluation"]["missing_task_ids"].items().empty() ||
        !plan.object() || !plan["tasks"].array() || plan["tasks"].items().empty() ||
        node["tasks"].dump() != plan["tasks"].dump() ||
        node["parent"].dump() != plan["parent"].dump() ||
        !components_bound)
      return false;
    if (!std::holds_alternative<double>(plan["parent"].value)) return false;
    const auto parent = plan["parent"].integer();
    if (parent >= position ||
        tree["nodes"].items()[parent].str("version").empty())
      return false;
  }
  if (!present.count(tree["active"].integer())) return false;
  for (const auto &entry : tree["evaluations"].fields()) {
    const auto &evaluation = entry.second;
    const auto status = evaluation.str("status");
    if ((status != "complete" && status != "inconclusive" && status != "error" &&
         status != "cancelled") ||
        evaluation.str("evaluation_context") != evaluation_context ||
        evaluation.str("report").empty())
      return false;
  }
  return true;
}

bool initial_review_retry_context_matches(const Json &tree, const Json &job,
                                          const std::string &grading,
                                          const std::string &execution) {
  return !grading.empty() && !execution.empty() &&
      tree.str("evaluation_context") == grading && job.str("evaluation_context") == grading &&
      tree.str("candidate_evaluation_context") == execution &&
      job.str("candidate_evaluation_context") == execution;
}

struct AgentEvaluationService::Impl {
  struct Lock {
    int fd = -1;

    // 销毁对象时释放本类持有的资源；收尾不能抛出业务异常，也不能继续接收新请求。
    ~Lock() {
      if (fd >= 0) {
        ::flock(fd, LOCK_UN);
        ::close(fd);
      }
    }
  } file_lock;

  fs::path root, config_path, dataset_path, evaluation_path;
  Json config, evaluation, state, gepa, controls, published = Json::Object{};
  std::string context;
  std::function<void()> changed;
  mutable std::mutex mutex;
  std::condition_variable wake;
  std::atomic<bool> stopping{false}, cancel_running{false};
  std::thread worker;
  std::size_t max_jobs = 256, interval = 60000, hourly = 12;

  // 绑定 Agent 的启动命令及实际内容，避免换了 Agent 执行代码仍使用旧成绩。
  // 评测管理器自身不是被评测对象，因此不能把 /proc/self/exe 纳入运行时身份；
  // 否则只重编译 trajectory_evolution 就会让未改变的实验树无法恢复。
  // include_manager_binary 仅保留给兼容诊断，不参与新的候选身份。
  std::string host_command_fingerprint(bool include_manager_binary = false) const {
    // 这里只把 Agent 命令、守护进程和配置文件内容的摘要加入指纹，不持久化
    // 文件内容、环境原文或凭据；管理器本身的构建变化不改变 Agent 执行身份。
    std::string bytes = "host-command-files-v2\n";
    std::set<fs::path> observed;
    const auto append_file = [&](const std::string &label, const fs::path &path,
                                 bool required) {
      std::error_code file_error;
      if (!fs::is_regular_file(path, file_error)) {
        require(!required, "evaluation_file_unavailable");
        bytes += label + "=<unavailable>\n";
        return;
      }
      const auto resolved = fs::canonical(path, file_error);
      if (file_error || !observed.insert(resolved).second) {
        require(!file_error, "evaluation_file_unavailable");
        return;
      }
      std::ifstream stream(resolved, std::ios::binary);
      require(bool(stream), "evaluation_file_unavailable");
      std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> file_hash(EVP_MD_CTX_new(),
                                                                       EVP_MD_CTX_free);
      require(file_hash && EVP_DigestInit_ex(file_hash.get(), EVP_sha256(), nullptr) == 1,
              "evaluation_hash_failed");
      char block[65536];
      while (stream.read(block, sizeof(block)) || stream.gcount())
        require(EVP_DigestUpdate(file_hash.get(), block,
                                 static_cast<std::size_t>(stream.gcount())) == 1,
                "evaluation_hash_failed");
      require(stream.eof(), "evaluation_read_failed");
      unsigned char result[EVP_MAX_MD_SIZE];
      unsigned int size = 0;
      require(EVP_DigestFinal_ex(file_hash.get(), result, &size) == 1,
              "evaluation_hash_failed");
      bytes += label + '=' +
               digest(std::string(reinterpret_cast<const char *>(result), size)) + '\n';
    };
    if (include_manager_binary) {
      std::error_code error;
      const auto self = fs::canonical("/proc/self/exe", error);
      require(!error && fs::is_regular_file(self), "evaluation_executable_unavailable");
      append_file("trajectory_evolution", self, true);
    }
    if (const auto *daemon = std::getenv("AGENTFLOW_DAEMON"))
      append_file("agentflow_daemon", fs::path(daemon), true);
    for (const auto &variant : evaluation["variants"].items())
      for (const auto &argument : variant["command"].items()) {
        const fs::path path(argument.text());
        append_file("command:" + argument.text(), path, false);
      }
    return digest(bytes);
  }

  // 绑定基线可复用的执行与评分上下文；改评分规则后需重新评分，不能沿用旧分数。
  std::string evaluation_fingerprint() const {
    const auto data = read_evaluation_dataset(dataset_path);
    for (const auto &issue : data.issues())
      require(issue.severity() != "error", "evaluation_dataset_invalid");
    std::string bytes = read(evaluation_path) + data.dataset_version();
    for (const auto &variant : evaluation["variants"].items())
      if (variant.has("instrumentation_config"))
        bytes += read(variant.str("instrumentation_config"));
    if (evaluation["judge"].flag("enabled", false)) {
      if (evaluation["judge"].has("goal_prompt"))
        bytes += read(evaluation["judge"].str("goal_prompt"));
      bytes += read(evaluation["judge"].str("prompt"));
      if (evaluation["judge"].has("loop_review_prompt"))
        bytes += read(evaluation["judge"].str("loop_review_prompt"));
      bytes += read(evaluation["judge"].str("runtime_config"));
      if (evaluation["judge"].has("consistency_prompt"))
        bytes += "\njudge-consistency-v1\n" + read(evaluation["judge"].str("consistency_prompt"));
    }
    // 宿主版本属于基线版本的一部分；环境中选择的模型也要纳入指纹，但绝不记录凭据。
    for (const auto *name : {"OPENAI_BASE_URL", "OPENAI_MODEL"})
      if (const auto *value = std::getenv(name))
        bytes += std::string(name) + "=" + value + "\n";
    // 逐轮标准和裁判契约属于评分语义；旧评分只能重评，不能与当前口径混用。
    bytes += "\nturn-acceptance-v2:judge-contract-v3:action-requests-v1";
    if (evaluation["judge"].has("loop_review_prompt") &&
        evaluation["judge"].has("consistency_prompt"))
      bytes += "\nloop-consistency-v2:separate-goal-loop-reviews";
    // 新评分版本只隔离 objectives 缓存，未切换评分口径的旧基线仍可复用。
    if (evaluation["judge"].str("assessment_mode", "legacy") == "objectives")
      bytes += "\ngoal-assessment-v1:judge-summary-consistency-v1";
    return digest("gepa-baseline-evaluation-v1:boundary-current-request-v1:verified-controls-v1:"
                  "failure-replan-v2:skill-schedule-v2:control-outcomes-v5:aggressive-controls-v1:"
                  "retrieval-evidence-v4\n" +
                  bytes);
  }

  // 对规范化内容计算稳定摘要，用于去重和版本比较。
  // 摘要只标识内容是否变化，不替代权限校验，也不包含可还原的凭据原文。
  std::string fingerprint() const {
    std::string bytes = read(config_path) + read(evaluation_path);
    const auto data = read_evaluation_dataset(dataset_path);
    for (const auto &issue : data.issues())
      require(issue.severity() != "error", "evaluation_dataset_invalid");
    bytes += data.dataset_version();
    if (!gepa.null())
      bytes += gepa_config_fingerprint(gepa) + controls.dump() + host_command_fingerprint();
    for (const auto &variant : evaluation["variants"].items())
      if (variant.has("instrumentation_config"))
        bytes += read(variant.str("instrumentation_config"));
    if (evaluation["judge"].flag("enabled", false)) {
      if (evaluation["judge"].has("goal_prompt"))
        bytes += read(evaluation["judge"].str("goal_prompt"));
      bytes += read(evaluation["judge"].str("prompt"));
      if (evaluation["judge"].has("loop_review_prompt"))
        bytes += read(evaluation["judge"].str("loop_review_prompt"));
      bytes += read(evaluation["judge"].str("runtime_config"));
      if (evaluation["judge"].has("consistency_prompt"))
        bytes += "\njudge-consistency-v1\n" + read(evaluation["judge"].str("consistency_prompt"));
    }
    // 冻结实验不仅由 JSON 决定，还必须绑定运行时语义版本。
    bytes += "\nturn-acceptance-v2:judge-contract-v3:action-requests-v1";
    if (evaluation["judge"].str("assessment_mode", "legacy") == "objectives")
      bytes += "\ngoal-assessment-v1:judge-summary-consistency-v1";
    return digest("boundary-current-request-v1:verified-controls-v1:failure-replan-v3:skill-"
                  "schedule-v4:control-outcomes-v8:terminal-branch-v1:aggressive-controls-v1:"
                  "retrieval-evidence-v7:current-turn-v1:control-feedback-v3:actor-loops-v2:effect-"
                  "selection-v1\n" +
                  bytes);
  }

  // 计算候选真实执行与评分的缓存身份，排除不影响执行的搜索排序变化。
  // 模型、提示词、运行时或评分口径变化都会影响摘要；敏感环境值只贡献摘要。
  std::string candidate_evaluation_fingerprint(bool include_manager_binary = false) const {
    // 搜索/反思变化不使真实执行成绩失效；运行时或评分语义变化必须更新此版本。
    std::string bytes =
        "candidate-evaluation-v1:boundary-current-request-v1:verified-controls-v1:failure-replan-"
        "v3:skill-schedule-v4:control-outcomes-v8:terminal-branch-v1:aggressive-controls-v1:"
        "retrieval-evidence-v7:current-turn-v1:actor-loops-v2:finalization-v1:task-evaluator-v1:"
        "schedule-success-v1\n";
    bytes += "turn-acceptance-v2:judge-contract-v3:action-requests-v1\n";
    if (evaluation["judge"].str("assessment_mode", "legacy") == "objectives")
      bytes += "goal-assessment-v1:judge-summary-consistency-v1\n";
    bytes += evaluation.dump() + read_evaluation_dataset(dataset_path).dataset_version() +
             config["agent"].dump() + host_command_fingerprint(include_manager_binary);
    std::set<std::string> environment{"PATH",        "LANG", "LC_ALL", "TZ", "OPENAI_BASE_URL",
                                      "OPENAI_MODEL"};
    for (const auto &variant : evaluation["variants"].items()) {
      if (variant["pass_env"].array())
        for (const auto &name : variant["pass_env"].items())
          environment.insert(name.text());
      if (!variant.has("instrumentation_config"))
        continue;
      const fs::path path(variant.str("instrumentation_config"));
      const auto content = read(path);
      bytes += content;
      const auto settings = Json::parse(content);
      auto resource = [&](const char *field) {
        fs::path value(settings.str(field));
        return value.is_absolute() ? value : path.parent_path() / value;
      };
      if (settings.has("experience_template_file"))
        bytes += read(resource("experience_template_file"), 16384);
      const auto finish =
          settings.has("finalization_template_file") ? resource("finalization_template_file")
          : settings.has("experience_template_file")
              ? resource("experience_template_file").parent_path() / "runtime-finalization.txt"
              : fs::path{};
      if (!finish.empty() && fs::exists(finish))
        bytes += read(finish, 16384);
    }
    if (evaluation["judge"].flag("enabled", false)) {
      bytes += read(evaluation["judge"].str("prompt"));
      if (evaluation["judge"].has("loop_review_prompt"))
        bytes += read(evaluation["judge"].str("loop_review_prompt"));
      if (evaluation["judge"].has("goal_prompt"))
        bytes += read(evaluation["judge"].str("goal_prompt"));
      if (evaluation["judge"].has("consistency_prompt"))
        bytes += "\njudge-consistency-v1\n" + read(evaluation["judge"].str("consistency_prompt"));
      const auto runtime = agentflow::trajectory::load_runtime_configuration(
          evaluation["judge"].str("runtime_config"));
      for (const auto &source : runtime.source_files)
        bytes += read(source);
      bytes += runtime.model.endpoint + '\n' + runtime.model.model_id;
      if (!runtime.no_api_key)
        environment.insert(runtime.api_key_env.empty() ? "OPENAI_API_KEY" : runtime.api_key_env);
    }
    // Management authentication rotates per service process; forwarding is
    // unchanged. Model credentials and all other environment values still
    // contribute to the candidate execution identity.
    environment.erase("EVOLUTION_APPWORLD_TOKEN");
    // 只返回摘要；环境原文（含账户切换）不进入文件、日志或模型请求。
    for (const auto &name : environment) {
      bytes += '\n' + name + '=';
      if (const auto *value = std::getenv(name.c_str()))
        bytes += digest(value);
      else
        bytes += "<unset>";
    }
    return digest(bytes);
  }

  // 初始化 Impl 的状态和资源；输入参数的校验与所有权在构造阶段确定。
  // 对象创建后复用既有配置，实际执行与收尾分别由后续接口负责。
  explicit Impl(const fs::path &file, const fs::path &data, std::function<void()> callback,
                const std::string &control_json)
      : root(fs::absolute(data)), config_path(fs::absolute(file)), changed(std::move(callback)) {
    controls = control_json.empty() ? Json() : Json::parse(control_json);
    validate_control_learning(controls);
    config = Json::parse(read(config_path));
    keys(config, {"schema_version", "dataset", "evaluation_config", "agent", "min_interval_ms",
                  "max_jobs", "max_jobs_per_hour", "gate", "gepa_config", "training"});
    require(config.num("schema_version", 0) == 1 && config["agent"].object() &&
                !config["agent"].str("revision").empty(),
            "agent_evaluation_config_invalid");
    validate_local_data(config);
    auto resolve = [&](const std::string &name) {
      fs::path p(config.str(name));
      require(!p.empty(), "evaluation_path_required");
      return p.is_absolute() ? p : config_path.parent_path() / p;
    };
    dataset_path = resolve("dataset");
    evaluation_path = resolve("evaluation_config");
    if (config.has("gepa_config"))
      gepa = load_gepa_config(resolve("gepa_config"));
    if (!config.has("training"))
      config["training"] = Json::Object{};
    const auto &training = config["training"];
    keys(training, {"enabled", "min_experiences", "batch_size", "max_batches"});
    if (training.has("enabled"))
      training.flag("enabled", true);
    for (const auto *key : {"min_experiences", "batch_size", "max_batches"})
      require(training.num(key, 1) > 0 && training.num(key, 1) <= 512, "training_limit_invalid");
    evaluation = Json::parse(read(evaluation_path));
    evaluation["require_trace"] = true;
    require(evaluation["variants"].items().size() == 1, "evaluation_host_must_be_single");
    require(evaluation.num("parallelism", 1) <= 16, "background_evaluation_parallelism_limit");
    if (!evaluation.has("resources"))
      evaluation["resources"] = Json{
          {"nice", 10}, {"address_space_bytes", std::size_t(1073741824)}, {"cpu_seconds", 300}};
    if (evaluation["variants"].items()[0].has("instrumentation_config")) {
      const fs::path resource(evaluation["variants"].items()[0].str("instrumentation_config"));
      if (resource.is_relative())
        evaluation["variants"].items()[0]["instrumentation_config"] =
            (evaluation_path.parent_path() / resource).string();
    }
    // 宿主命令和环境变量名始终由部署侧控制，服务不会自行改写它们。
    for (auto &argument : evaluation["variants"].items()[0]["command"].items()) {
      auto value = argument.text();
      const std::string marker = "{config_dir}";
      for (std::size_t i; (i = value.find(marker)) != std::string::npos;)
        value.replace(i, marker.size(), evaluation_path.parent_path().string());
      argument = value;
    }
    if (evaluation["judge"].str("assessment_mode", "legacy") == "objectives" &&
        !evaluation["judge"].has("goal_prompt"))
      evaluation["judge"]["goal_prompt"] = "prompts/task-goals.txt";
    if (evaluation["judge"].flag("enabled", false))
      for (const auto *key : {"prompt", "runtime_config", "loop_review_prompt", "goal_prompt",
                              "consistency_prompt"}) {
        if (std::string(key) != "prompt" && std::string(key) != "runtime_config" &&
            !evaluation["judge"].has(key))
          continue;
        fs::path p(evaluation["judge"].str(key));
        if (p.is_relative())
          p = evaluation_path.parent_path() / p;
        evaluation["judge"][key] = p.string();
      }
    max_jobs = config.num("max_jobs", 256);
    interval = config.num("min_interval_ms", 60000);
    hourly = config.num("max_jobs_per_hour", 12);
    require(max_jobs >= 4 && max_jobs <= 1024 && interval <= 86400000 && hourly > 0 &&
                hourly <= 1000,
            "evaluation_queue_limits_invalid");
    const auto &gate = config["gate"];
    keys(gate, {"min_pass_rate", "min_coverage", "max_pass_rate_drop", "min_pass_rate_gain",
                "min_quality", "min_quality_gain", "max_quality_drop", "min_trials",
                "max_latency_ratio", "max_token_ratio", "max_llm_call_ratio",
                "min_invalid_loop_reduction", "memory_min_pairs", "require_level_coverage",
                "resources_report_only"});
    if (gate.has("require_level_coverage"))
      gate.flag("require_level_coverage", true);
    if (gate.has("resources_report_only"))
      gate.flag("resources_report_only", false);
    for (const auto *key : {"min_pass_rate", "min_coverage", "max_pass_rate_drop", "min_pass_rate_gain",
                            "min_quality", "min_quality_gain", "max_quality_drop",
                            "min_invalid_loop_reduction"})
      if (gate.has(key))
        require(numeric(gate[key], -1) >= 0 && numeric(gate[key], -1) <= 1,
                "evaluation_gate_invalid");
    require(gate.num("min_trials", 2) > 0 && gate.num("min_trials", 2) <= 10000,
            "evaluation_gate_invalid");
    require(gate.num("memory_min_pairs", 2) > 0 && gate.num("memory_min_pairs", 2) <= 10000,
            "evaluation_gate_invalid");
    for (const auto *key : {"max_latency_ratio", "max_token_ratio", "max_llm_call_ratio"})
      if (gate.has(key))
        require(numeric(gate[key], -1) > 0, "evaluation_gate_invalid");
    if (!gepa.null()) {
      const auto data = read_evaluation_dataset(dataset_path);
      std::size_t count = 0;
      for (const auto &task : data.tasks())
        count += task.split() == "holdout";
      require(count * evaluation.num("repetitions", 1) >= gate.num("min_trials", 2),
              "gepa_holdout_trials_below_gate");
    }
    context = fingerprint();
    directory(root);
    file_lock.fd = ::open((root / "evaluation.lock").c_str(), O_CREAT | O_RDWR | O_NOFOLLOW, 0600);
    require(file_lock.fd >= 0 && ::flock(file_lock.fd, LOCK_EX | LOCK_NB) == 0,
            "evaluation_directory_in_use");
    state = Json{{"schema_version", 1},
                 {"jobs", Json::Object{}},
                 {"latest", Json::Object{}},
                 {"publications", Json::Object{}},
                 {"feedback", Json::Object{}},
                 {"starts", Json::Array{}},
                 {"next_due_ms", 0}};
    if (fs::exists(root / "state.json"))
      state = Json::parse(read(root / "state.json", 16777216));
    require(state.num("schema_version", 0) == 1 && state["jobs"].fields().size() <= max_jobs,
            "evaluation_state_invalid");
    for (auto &pair : state["jobs"].fields()) {
      require(pair.first.size() == 64 &&
                  pair.first.find_first_not_of("0123456789abcdef") == std::string::npos,
              "evaluation_job_invalid");
      // 重启/重编译后允许继续已有检查点；context 只作为历史记录，不再作为
      // 恢复门禁。真正的结果完整性由评测文件、任务集合和哈希校验负责。
      const bool context_compatible = true;
      // 服务正常关闭由析构回收搜索；下次启动继续同一 GEPA 检查点。
      // 显式 job.cancel 保留 cancel_requested，不因服务重启擅自恢复。
      // 搜索已经完成、尚未提交评测结果时也可能收到终止信号；
      // 该窗口使用 evaluation_cancelled，恢复时复用已落盘搜索并补交反馈。
      // 仅恢复 GEPA 的服务停机；普通训练的已取消结果必须保持终态。
      const bool shutdown = pair.second.flag("gepa", false) &&
                            pair.second.str("status") == "error" &&
                            (pair.second.str("error") == "gepa_cancelled" ||
                             pair.second.str("error") == "evaluation_cancelled");
      bool resumable_checkpoint = !shutdown;
      if (shutdown) {
        try {
          const auto tree =
              Json::parse(read(root / pair.first / "search/tree.json", 16777216));
          resumable_checkpoint =
              tree.num("rounds", 0) > 0 || tree.num("model_calls", 0) > 0 ||
              tree.num("trials_used", 0) > 0 || !tree["active"].null() ||
              !tree["batch"].null();
        } catch (const std::exception &) {
          resumable_checkpoint = false;
        }
      }
      if (pair.second.str("status") == "running" || shutdown) {
        const bool resume = pair.second.flag("gepa", false) && context_compatible &&
                            !pair.second.flag("cancel_requested", false) &&
                            resumable_checkpoint;
        pair.second["status"] = resume ? "pending" : "interrupted";
        if (resume) pair.second.fields().erase("error");
        if (!resume)
          pair.second["error"] = "evaluation_interrupted_retry_required";
      }
      if (pair.second.str("status") == "pending" && !context_compatible)
        pair.second["status"] = "superseded";
    }
    for (const auto &p : state["publications"].fields()) {
      if (p.second.null())
        continue; // 被拒绝的评测通道此时还没有发布版本。
      const auto artifact = load_artifact(p.second.str("job_id"));
      auto &job = state["jobs"][p.second.str("job_id")];
      require(agent_version_id(artifact.dump()) == p.second.str("version") &&
                  job.str("status") == "passed" && job["gate"].flag("passed", false) &&
                  job.str("user") == artifact.str("user_id") &&
                  job.str("version") == p.second.str("version") &&
                  job.str("context") == p.second.str("context") &&
                  p.first == lane(job.str("user"), artifact.str("level")),
              "evaluation_publication_changed");
      // 持久化发布记录可能早于最后一次作业保存或 bundle 写入，恢复时必须补齐这一步。
      job["published"] = true;
      export_publication(p.second.str("job_id"), job, artifact);
      if (p.second.str("context") == context)
        published[p.first] = artifact;
    }
    state.fields().erase("worker_error");
    save();
  }

  // 载入并验证指定评测产物，防止跨作业或版本错配。
  Json load_artifact(const std::string &id) const {
    require(state["jobs"].has(id), "evaluation_job_not_found", 404);
    return Json::parse(read(root / id / "candidate.json"));
  }

  // 保存当前模块的完整状态，使后台恢复或后续读取能看到一致结果。
  void save() {
    const auto bytes = state.dump();
    require(bytes.size() <= 16777216, "evaluation_state_limit");
    write(root / "state.json", bytes);
  }

  // 导出已通过门槛的可复用策略及所需配置，避免依赖当前实验的临时目录。
  void export_publication(const std::string &id, const Json &job, const Json &artifact) const {
    const auto bundle = Json{
        {"schema_version", 1},
        {"version", job["version"]},
        {"artifact", artifact},
        {"evaluation", Json{{"job_id", id}, {"context", job["context"]}, {"gate", job["gate"]}}}};
    // 保留 GEPA 的既有目录约定；固定候选作业没有搜索目录。
    const auto path =
        root / id /
        (job.flag("gepa", false) ? "search/published-version.json" : "published-version.json");
    if (fs::exists(path)) {
      require(Json::parse(read(path)).dump() == bundle.dump(), "evaluation_export_changed");
      return;
    }
    write(path, bundle.dump());
  }

  // 在后台作业或产物达到限制时回收允许淘汰的项目，保留仍在使用的结果。
  void make_room() {
    // 只限制活动索引的规模，不能删除可复现报告或去重标识。
    while (state["jobs"].fields().size() >= max_jobs) {
      std::string oldest;
      std::size_t time = SIZE_MAX;
      for (const auto &p : state["jobs"].fields()) {
        if (p.second.str("status") == "pending" || p.second.str("status") == "running" ||
            p.second["memory_feedback"].str("status") == "pending")
          continue;
        bool pinned =
            state["latest"].str(lane(p.second.str("user"), p.second.str("level"))) == p.first;
        if (state["training"].object())
          for (const auto &training : state["training"].fields())
            pinned |= training.second.str("job_id") == p.first;
        for (const auto &publication : state["publications"].fields())
          pinned |= publication.second.str("job_id") == p.first;
        if (!pinned && p.second.num("queued_ms", 0) < time) {
          oldest = p.first;
          time = p.second.num("queued_ms", 0);
        }
      }
      require(!oldest.empty(), "evaluation_queue_capacity", 507);
      write(root / oldest / "job.json", state["jobs"][oldest].dump());
      state["jobs"].fields().erase(oldest);
    }
  }

  // 发布验收读取与搜索一致的质量指标，同时检查覆盖率、关键约束和效率目标。
  Json gate_result(const dataset::v1::TaskEvaluationReport &report, const Json &artifact,
                   const Json &search_coverage = Json{}) const {
    const auto summary = json(report.summary());
    const auto &groups = summary["by_variant"];
    const auto &candidate = groups["candidate"];
    const auto &base = groups.has("baseline") ? groups["baseline"] : candidate;
    const auto &gate = config["gate"];
    const bool resources_report_only = gate.flag("resources_report_only", false);
    Json reasons = Json::Array{};
    const bool goals = evaluation["judge"].str("assessment_mode", "legacy") == "objectives";
    auto reject = [&](bool condition, const char *code) {
      if (condition)
        reasons.push(code);
    };
    reject(!report.complete(), "evaluation_incomplete");
    reject(!resources_report_only && summary.has("resource_costs_complete") && !summary.flag("resource_costs_complete", true) &&
               (gate.has("max_latency_ratio") || gate.has("max_token_ratio") || gate.has("max_llm_call_ratio")),
           "historical_resource_cost_incomplete");
    reject(candidate.num("trials", 0) < gate.num("min_trials", 2), "insufficient_trials");
    const double coverage = numeric(gate["min_coverage"], 1.0);
    const auto candidate_coverage =
        goals ? candidate["goal_completion"]["coverage"] : candidate["coverage"];
    const auto base_coverage = goals ? base["goal_completion"]["coverage"] : base["coverage"];
    reject(numeric(candidate_coverage, -1) < coverage || numeric(base_coverage, -1) < coverage,
           "insufficient_coverage");
    if (goals) {
      // 与 GEPA 同口径：质量读取目标完成度，严格通过率仍留在原报告中。
      for (const auto *group : {&candidate, &base}) {
        const auto &value = (*group)["goal_completion"];
        reject(value.num("observations", 0) != group->num("trials", 0) ||
                   numeric(value["coverage"], -1) < 1 - 1e-12,
               "goal_evidence_incomplete");
      }
      reject(candidate["goal_completion"].num("critical_failures", 0) > 0, "critical_goal_failed");
      if (gate.has("min_quality"))
        reject(numeric(candidate["goal_completion"]["mean"], -1) <
                   numeric(gate["min_quality"]),
               "quality_below_threshold");
      reject(numeric(candidate["goal_completion"]["mean"], -1) +
                     numeric(gate["max_quality_drop"], 0) <
                 numeric(base["goal_completion"]["mean"], 0),
             "quality_regression");
      if (gate.has("min_quality_gain"))
        reject(numeric(candidate["goal_completion"]["mean"], -1) <
                   numeric(base["goal_completion"]["mean"], 0) +
                       numeric(gate["min_quality_gain"]),
               "min_quality_gain");
    } else {
      if (gate.has("min_pass_rate"))
        reject(numeric(candidate["pass_rate"], -1) < numeric(gate["min_pass_rate"]),
               "quality_below_threshold");
      reject(numeric(candidate["pass_rate"], -1) + numeric(gate["max_pass_rate_drop"], 0) <
                 numeric(base["pass_rate"], 0),
             "quality_regression");
      if (gate.has("min_pass_rate_gain"))
        reject(numeric(candidate["pass_rate"], -1) < numeric(base["pass_rate"], 0) +
                   numeric(gate["min_pass_rate_gain"]),
               "min_pass_rate_gain");
    }
    auto ratio = [&](const char *option, const Json &a, const Json &b) {
      if (!resources_report_only && groups.has("baseline") && gate.has(option))
        reject(a.null() || b.null() || numeric(a) > numeric(b) * numeric(gate[option]), option);
    };
    ratio("max_latency_ratio", candidate["mean_agent_duration_ms"], base["mean_agent_duration_ms"]);
    ratio("max_token_ratio", candidate["host_reported_metrics"]["tokens"]["mean"],
          base["host_reported_metrics"]["tokens"]["mean"]);
    if (!resources_report_only && groups.has("baseline") && gate.has("max_token_ratio"))
      reject(candidate["host_reported_metrics"]["tokens"].num("observations", 0) !=
                     candidate.num("trials", 0) ||
                 base["host_reported_metrics"]["tokens"].num("observations", 0) !=
                     base.num("trials", 0),
             "token_metric_unobserved");
    if (!resources_report_only && groups.has("baseline") && gate.has("max_llm_call_ratio")) {
      const auto &candidate_calls = candidate["host_reported_metrics"]["llm_calls"];
      const auto &base_calls = base["host_reported_metrics"]["llm_calls"];
      const bool observations_complete =
          candidate_calls.object() && base_calls.object() &&
          candidate_calls.num("observations", 0) == candidate.num("trials", 0) &&
          base_calls.num("observations", 0) == base.num("trials", 0) &&
          !candidate_calls["mean"].null() && !base_calls["mean"].null();
      reject(!observations_complete, "llm_call_metric_unobserved");
      if (observations_complete)
        reject(numeric(candidate_calls["mean"]) >
                   numeric(base_calls["mean"]) * numeric(gate["max_llm_call_ratio"]),
               "max_llm_call_ratio");
    }
    if (groups.has("baseline") && gate.has("min_invalid_loop_reduction")) {
      const auto &a = candidate.has("invalid_loop_assessment")
                          ? candidate["invalid_loop_assessment"]
                          : candidate["invalid_loop_lower_bound"];
      const auto &b = base.has("invalid_loop_assessment") ? base["invalid_loop_assessment"]
                                                          : base["invalid_loop_lower_bound"];
      reject(a.str("method", "rule_lower_bound") != b.str("method", "rule_lower_bound"),
             "loop_metric_mismatch");
      reject(a.num("observations", 0) != candidate.num("trials", 0) ||
                 b.num("observations", 0) != base.num("trials", 0) || a["mean"].null() ||
                 b["mean"].null(),
             "loop_metric_unobserved");
      reject(numeric(a["mean"]) >
                 numeric(b["mean"]) * (1 - numeric(gate["min_invalid_loop_reduction"])),
             "loop_target_not_met");
      reject(numeric(gate["min_invalid_loop_reduction"]) > 0 && numeric(b["mean"]) <= 0,
             "loop_baseline_has_no_observed_waste");
    }
    const bool scored =
        numeric(candidate_coverage, -1) >= coverage && numeric(base_coverage, -1) >= coverage &&
        candidate.num(goals ? "trials" : "scored", 0) >= gate.num("min_trials", 2) &&
        base.num(goals ? "trials" : "scored", 0) >= gate.num("min_trials", 2) &&
        candidate.num("errors", 0) == 0 && base.num("errors", 0) == 0;
    reject(!scored, "insufficient_scored_trials");
    const auto comparison = json(report.comparisons())["candidate"];
    // Objective pairing is a different relation from strict pass/fail pairing.
    // A cached pair without an explicitly recomputed goal_completion object is
    // incomplete; its top-level paired_trials must never be reused as a proxy.
    const auto &goal_comparison = comparison["goal_completion"];
    const auto paired_trials =
        goals ? (goal_comparison.object() ? goal_comparison.num("paired_trials", 0)
                                          : std::size_t{0})
              : comparison.num("paired_trials", 0);
    const bool paired = !groups.has("baseline") || paired_trials >= gate.num("min_trials", 2);
    reject(!paired, "insufficient_paired_trials");
    const auto current_controls = control_coverage(artifact, report);
    const auto controls = merge_control_coverage(current_controls, search_coverage);
    // 与搜索选优一致：默认按效果验收，未命中的规则和能力仍完整保留在报告中。
    if (gate.flag("require_level_coverage", false)) {
      reject(!controls.flag("complete", false), "generated_control_not_observed");
      reject(!current_controls.flag("level_complete", false), "level_capability_not_observed");
    }
    Json result{{"passed", reasons.items().empty()},
                {"scored", scored && paired},
                {"reasons", reasons},
                {"baseline", base},
                {"candidate", candidate},
                {"comparison", groups.has("baseline") ? "paired" : "baseline_only"},
                {"control_coverage", current_controls},
                {"historical_control_coverage", controls}};
    if (resources_report_only)
      result["resources_report_only"] = true;
    return result;
  }

  // 把已完成评测的真实效果关联到候选和来源经验，不使用未执行的建议更新收益。
  void record_feedback(const std::string &id, const Json &job, const Json &artifact,
                       const Json &base, const Json &verdict,
                       const dataset::v1::TaskEvaluationReport &report) {
    // 发布包证据与按记忆归因的因果验证分开记录，二者不能互相替代。
    auto &feedback = state["feedback"][job.str("user")];
    if (!feedback.object())
      feedback = Json::Object{};
    const auto referenced = memories(artifact);
    const auto previous = memories(base);
    auto without_experiences = [](Json value) {
      value = canonical(std::move(value));
      if (value["policy"].object()) {
        value["policy"]["options"]["approved_experiences"] = Json::Array{};
        value["policy"]["snapshot"]["experiences"] = Json::Array{};
      }
      return value.dump();
    };
    std::set<std::string> changed_ids;
    for (const auto &item : referenced.fields())
      if (item.second.dump() != previous[item.first].dump())
        changed_ids.insert(item.first);
    for (const auto &item : previous.fields())
      if (item.second.dump() != referenced[item.first].dump())
        changed_ids.insert(item.first);
    const bool single_change =
        changed_ids.size() == 1 && without_experiences(artifact) == without_experiences(base);
    for (const auto &item : referenced.fields()) {
      const auto key = digest(item.first + '\n' + item.second.str("content_revision"));
      auto &value = state["feedback"][job.str("user")][key];
      if (!value.object() || value.str("context") != context)
        value = Json{{"context", context},
                     {"memory_id", item.first},
                     {"content_revision", item.second["content_revision"]},
                     {"application_shared", item.second.flag("application_shared", false)},
                     {"reports", Json::Object{}}};
      std::size_t positive = 0, negative = 0;
      if (single_change && changed_ids.count(item.first)) {
        for (const auto &trial : report.trials()) {
          if (trial.variant_id() != "candidate" ||
              (trial.verdict() != "passed" && trial.verdict() != "failed"))
            continue;
          bool applied = false;
          const auto evidence = json(trial.evidence());
          const auto traces = evidence["traces"].array() ? evidence["traces"]
                                                         : Json(Json::Array{evidence["trace"]});
          for (const auto &trace : traces.items())
            if (trace["events"].array())
              for (const auto &event : trace["events"].items())
                if (event.str("kind") == "advice.applied" &&
                    event["application"].str("status") == "applied" &&
                    event["attributes"]["receipt"]["experiences"].array())
                  for (const auto &ref : event["attributes"]["receipt"]["experiences"].items())
                    applied |= ref.str("memory_id") == item.first &&
                               ref.str("content_revision") == item.second.str("content_revision");
          if (!applied)
            continue;
          for (const auto &parent : report.trials())
            if (parent.variant_id() == "baseline" && parent.task_id() == trial.task_id() &&
                parent.repetition() == trial.repetition() &&
                (parent.verdict() == "passed" || parent.verdict() == "failed")) {
              positive += trial.verdict() == "passed" && parent.verdict() == "failed";
              negative += trial.verdict() == "failed" && parent.verdict() == "passed";
            }
        }
      }
      value["reports"][id] =
          Json{{"version", job["version"]},
               {"passed", verdict["passed"]},
               {"attribution", positive || negative ? "single_memory_paired" : "version_bundle"},
               {"positive_trials", positive},
               {"negative_trials", negative},
               {"finished_ms", now_ms()}};
      while (value["reports"].fields().size() > 32) {
        auto &rows = value["reports"].fields();
        rows.erase(std::min_element(rows.begin(), rows.end(), [](const auto &a, const auto &b) {
          return a.second.num("finished_ms", 0) < b.second.num("finished_ms", 0);
        }));
      }
      value["finished_ms"] = now_ms();
      bool passed = false, failed = false;
      positive = negative = 0;
      for (const auto &r : value["reports"].fields()) {
        passed |= r.second.flag("passed", false);
        failed |= !r.second.flag("passed", false);
        positive += r.second.num("positive_trials", 0);
        negative += r.second.num("negative_trials", 0);
      }
      value["positive_trial_count"] = positive;
      value["negative_trial_count"] = negative;
      const auto minimum = config["gate"].num("memory_min_pairs", 2);
      value["status"] = positive && negative  ? "conflicting_trial_results"
                        : negative >= minimum ? "regressed"
                        : positive >= minimum ? "validated"
                        : passed && failed    ? "mixed_version_results"
                        : passed              ? "supported_in_version"
                                              : "review_needed";
    }
    while (feedback.fields().size() > 1024) {
      auto &rows = feedback.fields();
      rows.erase(std::min_element(rows.begin(), rows.end(), [](const auto &a, const auto &b) {
        return a.second.num("finished_ms", 0) < b.second.num("finished_ms", 0);
      }));
    }
  }

  // 运行后台作业循环，遵守取消和工作通知，不在主 Agent 热路径同步评测。
  void loop() {
    while (!stopping) {
      std::string id;
      Json job, artifact, base;
      {
        std::unique_lock<std::mutex> guard(mutex);
        for (;;) {
          if (stopping)
            return;
          std::size_t oldest = SIZE_MAX;
          for (const auto &p : state["jobs"].fields())
            if (p.second.str("status") == "pending" && p.second.num("queued_ms", 0) < oldest) {
              id = p.first;
              oldest = p.second.num("queued_ms", 0);
            }
          if (id.empty()) {
            wake.wait(guard);
            continue;
          }
          const auto now = now_ms();
          auto &starts = state["starts"].items();
          starts.erase(std::remove_if(starts.begin(), starts.end(),
                                      [&](const Json &t) { return t.integer() + 3600000 <= now; }),
                       starts.end());
          auto due = state.num("next_due_ms", 0);
          if (starts.size() >= hourly)
            due = std::max(due, starts.front().integer() + 3600000);
          if (due > now) {
            id.clear();
            wake.wait_for(guard, std::chrono::milliseconds(due - now));
            continue;
          }
          auto &selected = state["jobs"][id];
          selected["status"] = "running";
          selected["attempt"] = selected.num("attempt", 0) + 1;
          selected["started_ms"] = now;
          state["next_due_ms"] = now + interval;
          starts.push_back(now);
          save();
          job = selected;
          artifact = load_artifact(id);
          base = Json::parse(read(root / id / "baseline.json"));
          cancel_running = false;
          break;
        }
      }
      Json verdict;
      std::string error;
      bool completed = false;
      dataset::v1::TaskEvaluationReport report;
      try {
        const bool holdout_replay = job["final_holdout_replay"].object();
        // 评测可跨管理器版本恢复；不再因为执行/配置 fingerprint 改变而
        // 阻止既有任务继续。数据、报告和用户边界仍在各自入口校验。
        auto verify_context = [&] { (void)holdout_replay; };
        verify_context();
        TaskEvaluationOptions options;
        options.cancelled = [&] { return stopping || cancel_running; };
        for (auto attempt = job.num("attempt", 0); attempt > 1; --attempt) {
          const auto previous = root / id / ("attempt-" + std::to_string(attempt - 1));
          if (fs::exists(previous / "manifest.json")) {
            options.resume_report = previous / "report.json";
            break;
          }
        }
        if (job.flag("training", false)) {
          auto settings = evaluation;
          auto host = evaluation["variants"].items().front();
          host["id"] = "training";
          host["version"] = job["version"];
          host["artifact"] = artifact;
          settings["variants"] = Json::Array{host};
          settings["splits"] = Json::Array{"train"};
          settings["task_ids"] = job["task_ids"];
          // 冷启动复用执行与轨迹采集，不继承 GEPA 的任务评分请求。
          // 轨迹模型分析、元反思由后续学习队列的 runtime 配置独立控制。
          if (settings["judge"].null())
            settings["judge"] = Json::Object{};
          settings["judge"]["enabled"] = false;
          options.config_json = settings.dump();
          report = run_task_evaluation(
              dataset_path, evaluation_path,
              root / id / ("attempt-" + std::to_string(job.num("attempt", 0))), options);
          require(report.complete(), "training_report_incomplete");
        } else if (job.flag("gepa", false)) {
          const auto seed = Json::parse(read(root / id / "seed.json"));
          const auto result = run_gepa_search(
              seed, base, dataset_path, evaluation_path, evaluation, gepa, config["gate"], controls,
              root / id / "search", holdout_replay ? job.str("context") : context,
              evaluation_fingerprint(),
              holdout_replay
                  ? job["final_holdout_replay"].str("source_candidate_evaluation_context")
                  : candidate_evaluation_fingerprint(),
              [&] { return stopping || cancel_running; }, nullptr,
              holdout_replay ? job["final_holdout_replay"].num("requested_count", 0) : 0,
              holdout_replay, job["reflection_history"],
              holdout_replay ? job["final_holdout_replay"].str("candidate_evaluation_context")
                             : "");
          artifact = result.artifact;
          report = result.holdout;
          job["search"] = result.summary;
          // 只交付搜索过程中积累的训练反馈；留出复测不产生第二份入库事件。
          if (!holdout_replay) {
            if (result.memory_feedback["records"].object() &&
                !result.memory_feedback["records"].fields().empty()) {
              write(root / id / "search/memory-feedback.json", result.memory_feedback.dump());
              job["memory_feedback"] = Json{{"status", "pending"},
                  {"records", result.memory_feedback["records"].fields().size()},
                  {"fingerprint", digest(result.memory_feedback.dump())}};
            } else {
              // 正常完成但没有可归因的训练反馈也是一个已处理终态；不能让 none
              // 被上层误判为仍待投递，也不能伪造一条空的入库事务。
              job["memory_feedback"] = Json{{"status", "empty"},
                  {"records", 0},
                  {"recovered_checkpoint", false},
                  {"checkpoint_attempt", job.num("attempt", 0)}};
            }
          }
          if (!result.finalists.empty()) {
            Json finalist_gates = Json::Array{};
            Json final_selection;
            bool selected_passing_finalist = false;
            job["search"].fields().erase("gate_winner");
            for (std::size_t rank = 0; rank < result.finalists.size(); ++rank) {
              const auto &finalist = result.finalists[rank];
              const auto candidate_gate =
                  gate_result(finalist.holdout, finalist.artifact, finalist.control_coverage);
              const auto version = agent_version_id(finalist.artifact.dump());
              const Json diagnostic{{"rank", rank + 1},
                                    {"node", finalist.node},
                                    {"version", version},
                                    {"scored", candidate_gate["scored"]},
                                    {"passed", candidate_gate["passed"]},
                                    {"reasons", candidate_gate["reasons"]}};
              finalist_gates.push(diagnostic);
              // 留出前冻结验证排序。按该顺序第一个通过门禁的候选获胜，
              // 后续候选的留出分数绝不能反向重排它。
              const bool passed =
                  candidate_gate.flag("scored", false) && candidate_gate.flag("passed", false);
              // 全部失败时仍固定保留验证排名第一名，不能让末尾失败分支覆盖结果。
              if (rank == 0 || (!selected_passing_finalist && passed)) {
                artifact = finalist.artifact;
                report = finalist.holdout;
                verdict = candidate_gate;
                final_selection = diagnostic;
              }
              if (passed && !selected_passing_finalist) {
                selected_passing_finalist = true;
                job["search"]["gate_winner"] = diagnostic;
              }
            }
            job["search"]["finalist_gates"] = finalist_gates;
            job["search"]["final_holdout_selection"] = final_selection;
            if (holdout_replay) {
              job["final_holdout_replay"]["state"] = "completed";
              job["final_holdout_replay"]["gate_winner"] = job["search"]["gate_winner"];
            }
            // GEPA 可能在验证阶段即被拒绝，此时根本不会产生留出候选。
          } else if (result.summary.str("status") == "validation_rejected")
            verdict = Json{{"scored", true},
                           {"passed", false},
                           {"stage", "validation"},
                           {"reasons", result.summary["optimization"]["reasons"]},
                           {"optimization", result.summary["optimization"]},
                           {"control_coverage", result.summary["control_coverage"]}};
        } else {
          auto settings = evaluation;
          auto host = evaluation["variants"].items().front();
          auto a = host, b = host;
          a["id"] = "baseline";
          a["version"] = agent_version_id(base.dump());
          a["artifact"] = base;
          b["id"] = "candidate";
          b["version"] = job["version"];
          b["artifact"] = artifact;
          settings["variants"] =
              artifact.str("level") == "L0" ? Json(Json::Array{b}) : Json(Json::Array{a, b});
          options.config_json = settings.dump();
          report = run_task_evaluation(
              dataset_path, evaluation_path,
              root / id / ("attempt-" + std::to_string(job.num("attempt", 0))), options);
        }
        require(!stopping && !cancel_running, "evaluation_cancelled");
        verify_context();
        if (!job.flag("training", false) && verdict.null())
          verdict =
              gate_result(report, artifact,
                          job.flag("gepa", false) ? job["search"]["control_coverage"] : Json{});
        completed = true;
      } catch (const Error &e) {
        error = e.what();
      } catch (...) {
        error = "agent_evaluation_failed";
      }
      {
        std::lock_guard<std::mutex> guard(mutex);
        auto &current = state["jobs"][id];
        current["finished_ms"] = now_ms();
        // 取消可能发生在评分结束、取得发布锁之前；此时仍必须阻止发布，
        // 并把最终状态保持为“已取消”。
        const bool cancelled = current.flag("cancel_requested", false);
        if (cancelled) {
          completed = false;
          error = "evaluation_cancelled";
        }
        current["status"] = cancelled ? "cancelled"
                            : completed
                                ? (verdict.flag("scored", false)
                                       ? (verdict.flag("passed", false) ? "passed" : "rejected")
                                       : "inconclusive")
                                : "error";
        if (!error.empty())
          current["error"] = error;
        if (completed && job.flag("training", false))
          current["status"] = "completed";
        if (completed && !job.flag("training", false)) {
          const bool holdout_replay = job["final_holdout_replay"].object();
          const bool publishable_replay =
              holdout_replay &&
              job["final_holdout_replay"].str("protocol") == "derived-top-k-v2";
          if (job.flag("gepa", false)) {
            current["search"] = job["search"];
            if (!holdout_replay && job["memory_feedback"].object())
              current["memory_feedback"] = job["memory_feedback"];
            if (holdout_replay) {
              current["final_holdout_replay"] = job["final_holdout_replay"];
              current["replay_version"] = agent_version_id(artifact.dump());
              write(root / id / "replay-candidate.json", artifact.dump());
              if (publishable_replay)
                current["version"] = current["replay_version"];
            } else {
              current["version"] = agent_version_id(artifact.dump());
              write(root / id / "candidate.json", artifact.dump());
            }
          }
          current["gate"] = verdict;
          // 普通版本评测与 GEPA 训练反馈分别归因，留出整包胜负不替代单条经验证据。
          if (verdict.flag("scored", false) && !job.flag("gepa", false))
            record_feedback(id, current, artifact, base, verdict, report);
          const auto key = lane(job.str("user"), artifact.str("level"));
          const auto previous = state["publications"][key].str("context") == context
                                    ? state["publications"][key].str("version")
                                    : "";
          if ((!holdout_replay || publishable_replay) && verdict.flag("passed", false) &&
              state["latest"].str(key) == id &&
              previous == job.str("parent_version")) {
            state["publications"][key] = Json{{"user", job["user"]},
                                              {"level", artifact["level"]},
                                              {"job_id", id},
                                              {"version", current["version"]},
                                              {"context", context}};
            // 先持久化，再向消费者暴露新快照，避免读到半发布状态。
            current["published"] = true;
            save();
            published[key] = artifact;
            if (job.flag("gepa", false))
              write(root / id / "search/selected-version.json", artifact.dump());
            export_publication(id, current, artifact);
          }
          if (job.flag("gepa", false)) {
            if ((holdout_replay && !publishable_replay) || !verdict.flag("passed", false))
              fs::remove(root / id / "search/selected-version.json");
            write(root / id / "search/decision.json",
                  Json{{"gate", verdict},
                       {"published", current.flag("published", false)},
                       {"version", holdout_replay ? current["replay_version"] : current["version"]},
                       {"parent_version", current["parent_version"]},
                       {"context", context},
                       {"retrospective", holdout_replay}}
                      .dump());
          }
          if (holdout_replay && !publishable_replay) {
            // 已经见过留出集的候选只能用于诊断新协议，
            // 绝不能成为可发布的评测结果。
            current["final_holdout_replay"]["state"] = "completed";
            current["final_holdout_replay"]["publishable"] = false;
            current["status"] = "rejected";
          }
        }
        save();
      }
      if (changed)
        changed();
    }
  }
};

// 初始化 AgentEvaluationService 的状态和资源；输入参数的校验与所有权在构造阶段确定。
// 对象创建后复用既有配置，实际执行与收尾分别由后续接口负责。
AgentEvaluationService::AgentEvaluationService(const fs::path &file, const fs::path &root,
                                               std::function<void()> changed, std::string controls)
    : impl_(std::make_unique<Impl>(file, root, std::move(changed), controls)) {
  impl_->worker = std::thread([this] {
    try {
      impl_->loop();
    } catch (...) {
      {
        std::lock_guard<std::mutex> guard(impl_->mutex);
        impl_->state["worker_error"] = "evaluation_worker_failed";
        try {
          impl_->save();
        } catch (...) {
        }
      }
      if (impl_->changed)
        impl_->changed();
    }
  });
}

// 销毁对象时释放本类持有的资源；收尾不能抛出业务异常，也不能继续接收新请求。
AgentEvaluationService::~AgentEvaluationService() {
  impl_->stopping = true;
  impl_->wake.notify_all();
  if (impl_->worker.joinable())
    impl_->worker.join();
}

// 返回当前宿主配置视图，供后台创建一致的评测环境。
std::string AgentEvaluationService::agent_configuration() const {
  return impl_->config["agent"].dump();
}

// 准备可隔离执行的训练任务与策略上下文，训练、验证和留出的用途保持分离。
std::string AgentEvaluationService::prepare_training(const std::string &user,
                                                     const std::string &seed_json,
                                                     bool learning_pending, bool learning_failed) {
  if (impl_->gepa.null() || !impl_->config["training"].flag("enabled", true))
    return Json{{"status", "disabled"}}.dump();
  const auto seed = Json::parse(seed_json);
  require(seed.str("user_id") == user && seed["agent"].dump() == impl_->config["agent"].dump(),
          "evaluation_owner_or_agent_mismatch", 403);
  const auto usable = memories(seed).fields().size();
  const auto &settings = impl_->config["training"];
  const auto minimum = settings.num("min_experiences", 3);
  const auto data = read_evaluation_dataset(impl_->dataset_path);
  for (const auto &issue : data.issues())
    require(issue.severity() != "error", "training_dataset_invalid");
  // 有意不把记忆数量放入训练预算：同一冻结应用与数据集不能因学习而获得一份新预算。
  const auto key = lane(user, "training");
  const auto identity = digest(impl_->config["agent"].dump() + '\n' + data.dataset_version());
  std::lock_guard<std::mutex> guard(impl_->mutex);
  require(!impl_->state.has("worker_error"), "evaluation_worker_unavailable", 503);
  auto &progress = impl_->state["training"][key];
  const auto previous = progress.dump();
  const bool new_identity = progress.str("identity") != identity;
  if (new_identity)
    progress =
        Json{{"identity", identity}, {"user", user}, {"batches", 0}, {"tasks", Json::Array{}}};
  progress["usable_experiences"] = usable;
  progress["min_experiences"] = minimum;
  auto finish = [&](const std::string &status) {
    progress["status"] = status;
    const auto bytes = progress.dump();
    if (bytes != previous)
      impl_->save();
    return bytes;
  };
  if (learning_failed && !new_identity)
    return finish("learning_failed");
  const auto id = progress.str("job_id");
  if (!id.empty()) {
    const auto &job = impl_->state["jobs"][id];
    require(job.flag("training", false) && job.str("user") == user, "training_job_missing");
    const auto status = job.str("status");
    if (job.str("context") != impl_->context &&
        (status == "superseded" || status == "interrupted" || status == "error") &&
        !job.flag("cancel_requested", false) && job.str("error") != "evaluation_cancelled" &&
        !job.has("training_import")) {
      // 在新上下文中重新排入同一已保留批次；不能复用旧检查点，
      // 也不能重置数据集的任务／批次预算。
      const auto tasks = job["task_ids"], artifact = baseline(seed);
      require(!tasks.items().empty(), "training_tasks_missing");
      const auto next = digest(id + '\n' + impl_->context);
      impl_->make_room();
      write(impl_->root / next / "candidate.json", artifact.dump());
      write(impl_->root / next / "baseline.json", artifact.dump());
      impl_->state["jobs"][next] = Json{{"user", user},
                                        {"level", "L0"},
                                        {"version", agent_version_id(artifact.dump())},
                                        {"status", "pending"},
                                        {"queued_ms", now_ms()},
                                        {"context", impl_->context},
                                        {"evaluation_context",
                                         impl_->evaluation_fingerprint()},
                                        {"candidate_evaluation_context",
                                         impl_->candidate_evaluation_fingerprint()},
                                        {"attempt", 0},
                                        {"training", true},
                                        {"task_ids", tasks},
                                        {"supersedes", id}};
      impl_->state["jobs"][id]["status"] = "superseded";
      progress["job_id"] = next;
      progress["run_ids"] = Json::Array{};
      const auto result = finish("training");
      impl_->wake.notify_all();
      return result;
    }
    if (job.str("status") == "pending" || job.str("status") == "running")
      return finish("training");
    if (job.str("status") != "completed")
      return finish("training_failed");
    if (!job.has("training_import")) {
      auto result = Json::parse(finish("ready_to_import"));
      result["report_directory"] =
          (impl_->root / id / ("attempt-" + std::to_string(job.num("attempt", 0)))).string();
      result["dataset_directory"] = impl_->dataset_path.string();
      result["baseline"] = impl_->load_artifact(id);
      return result.dump();
    }
    if (!job["training_import"].flag("ok", false))
      return finish("import_failed");
  }
  if (learning_pending)
    return finish("waiting_for_learning");
  if (usable >= minimum && seed.str("level") != "L0")
    return finish("ready");
  if (progress.num("batches", 0) >= settings.num("max_batches", 4))
    return finish("budget_exhausted");
  std::set<std::string> used;
  for (const auto &task : progress["tasks"].items())
    used.insert(task.text());
  Json tasks = Json::Array{}, versions = Json::Array{};
  for (const auto &task : data.tasks()) {
    if (task.split() != "train" || task.review_status() != "reviewed" ||
        used.count(task.task_version()))
      continue;
    if (tasks.items().size() >= settings.num("batch_size", 16))
      break;
    tasks.push(task.task_id());
    used.insert(task.task_version());
    versions.push(task.task_version());
  }
  if (tasks.items().empty())
    return finish("training_exhausted");
  const auto artifact = baseline(seed);
  const auto version = agent_version_id(artifact.dump());
  const auto job_id = digest(key + '\n' + identity + '\n' + tasks.dump());
  impl_->make_room();
  write(impl_->root / job_id / "candidate.json", artifact.dump());
  write(impl_->root / job_id / "baseline.json", artifact.dump());
  impl_->state["jobs"][job_id] =
      Json{{"user", user},        {"level", "L0"},         {"version", version},
           {"status", "pending"}, {"queued_ms", now_ms()}, {"context", impl_->context},
           {"evaluation_context", impl_->evaluation_fingerprint()},
           {"candidate_evaluation_context", impl_->candidate_evaluation_fingerprint()},
           {"attempt", 0},        {"training", true},      {"task_ids", tasks}};
  progress["batches"] = progress.num("batches", 0) + 1;
  progress["job_id"] = job_id;
  for (const auto &version : versions.items())
    progress["tasks"].push(version);
  progress["run_ids"] = Json::Array{};
  const auto result = finish("training");
  impl_->wake.notify_all();
  return result;
}

// 确认训练轨迹完成导入并更新后台状态，避免同批次重复积累经验。
void AgentEvaluationService::complete_training_import(const std::string &user,
                                                      const std::string &id,
                                                      const std::string &bytes) {
  const auto result = Json::parse(bytes);
  validate_local_data(result);
  std::lock_guard<std::mutex> guard(impl_->mutex);
  auto &job = impl_->state["jobs"][id];
  require(job.flag("training", false) && job.str("user") == user &&
              job.str("status") == "completed",
          "training_job_not_importable");
  job["training_import"] = result;
  impl_->state["training"][lane(user, "training")]["run_ids"] =
      result["result"].has("run_ids") ? result["result"]["run_ids"] : Json(Json::Array{});
  impl_->save();
}

// 校验并提交后台评测作业，返回可查询身份，不在主 Agent 热路径等待整场实验。
std::string AgentEvaluationService::submit(const std::string &user, const std::string &bytes) {
  return submit(user, bytes, {});
}

std::string AgentEvaluationService::submit(const std::string &user, const std::string &bytes,
                                           const std::string &memory_feedback_json) {
  const auto version = agent_version_id(bytes);
  const auto artifact = Json::parse(bytes);
  require(artifact.str("user_id") == user &&
              artifact["agent"].dump() == impl_->config["agent"].dump(),
          "evaluation_owner_or_agent_mismatch", 403);
  std::lock_guard<std::mutex> guard(impl_->mutex);
  require(!impl_->state.has("worker_error"), "evaluation_worker_unavailable", 503);
  const auto key = lane(user, artifact.str("level"));
  const auto publication = impl_->state["publications"][key];
  const auto parent =
      publication.str("context") == impl_->context ? publication.str("version") : "";
  const bool search =
      !impl_->gepa.null() && artifact.str("level") != "L0" &&
      proto<v1::RuntimePolicy>(artifact["policy"]).options().approved_experiences_size() > 0;
  const auto history = search && !memory_feedback_json.empty()
      ? gepa_reflection_rejections(artifact, Json::parse(memory_feedback_json), impl_->controls,
                                   impl_->evaluation_fingerprint()) : Json();
  if (parent == version && history.null()) return publication.str("job_id");
  // 发布改变父版本但记忆种子、数据集与配置未变时，
  // 已完成的搜索绝不能再次被调度；新增有效反证是新的搜索输入，不修改旧作业。
  const auto id =
      digest(user + '\n' + version + '\n' + (search ? "gepa" : parent) + '\n' + impl_->context +
             (history.null() ? "" : "\nreflection-history:" + digest(history.dump())));
  if (impl_->state["jobs"].has(id))
    return id;
  if (fs::exists(impl_->root / id / "job.json"))
    return id;
  // 反馈写回可能只改变库修订、快照 ID 和策略 ID，实际行为版本仍相同。
  // 沿用已冻结的同版本种子，使执行缓存与检查点继续使用原始来源标识；
  // 新反证仍来自本次 history，不被旧快照替换。内容或授权变化会改变 version。
  std::string frozen_seed = bytes, seed_source_job;
  std::size_t newest_seed = 0;
  if (search && impl_->gepa.flag("cache_evaluations", true))
    for (const auto &prior : impl_->state["jobs"].fields()) {
      const auto &job = prior.second;
      const auto queued = job.num("queued_ms", 0);
      if (!job.flag("gepa", false) || job.str("user") != user ||
          job.str("level") != artifact.str("level") || job.str("seed_version") != version ||
          (!seed_source_job.empty() && queued <= newest_seed))
        continue;
      try {
        const auto saved = read(impl_->root / prior.first / "seed.json", 2097152);
        // 重新校验完整策略及版本，不能只相信状态文件中声明的 seed_version。
        if (agent_version_id(saved) != version)
          continue;
        frozen_seed = saved;
        seed_source_job = prior.first;
        newest_seed = queued;
      } catch (const std::exception &) {
        // 被清理或损坏的历史种子只影响可选加速，不改变当前合法提交。
      }
    }
  impl_->make_room();
  Json base = impl_->published.has(key) ? impl_->published[key] : baseline(artifact);
  write(impl_->root / id / "candidate.json", frozen_seed);
  write(impl_->root / id / "baseline.json", base.dump());
  if (search)
    write(impl_->root / id / "seed.json", frozen_seed);
  for (auto &p : impl_->state["jobs"].fields())
    if (p.second.str("user") == user && p.second.str("level") == artifact.str("level") &&
        p.second.str("status") == "pending")
      p.second["status"] = "superseded";
  impl_->state["jobs"][id] = Json{{"user", user},
                                  {"level", artifact["level"]},
                                  {"version", version},
                                  {"parent_version", parent},
                                  {"status", "pending"},
                                  {"queued_ms", now_ms()},
                                  {"context", impl_->context},
                                  {"evaluation_context", impl_->evaluation_fingerprint()},
                                  {"candidate_evaluation_context",
                                   impl_->candidate_evaluation_fingerprint()},
                                  {"attempt", 0},
                                  {"gepa", search},
                                  {"seed_version", version}};
  if (!history.null())
    impl_->state["jobs"][id]["reflection_history"] = history;
  if (!seed_source_job.empty())
    impl_->state["jobs"][id]["seed_source_job"] = seed_source_job;
  impl_->state["latest"][key] = id;
  impl_->save();
  impl_->wake.notify_all();
  return id;
}

// 返回当前运行或作业状态，不触发额外执行或重新评测。
std::string AgentEvaluationService::status(const std::string &user) const {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  Json result{{"jobs", Json::Object{}}, {"publications", Json::Object{}}};
  if (impl_->state.has("worker_error"))
    result["worker_error"] = impl_->state["worker_error"];
  if (impl_->state["training"].has(lane(user, "training")))
    result["training"] = impl_->state["training"][lane(user, "training")];
  for (const auto &p : impl_->state["jobs"].fields())
    if (p.second.str("user") == user)
      result["jobs"][p.first] = p.second;
  for (const auto &p : impl_->state["publications"].fields())
    if (p.second.str("user") == user && p.second.str("context") == impl_->context)
      result["publications"][p.second.str("level")] = p.second;
  return result.dump();
}

// 返回已发布策略的可用版本与元信息，不把搜索中的候选当作正式版本。
std::string AgentEvaluationService::publications(const std::string &user) const {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  Json result = Json::Object{};
  for (const auto &p : impl_->published.fields())
    if (p.second.str("user_id") == user)
      result[p.second.str("level")] = p.second;
  return result.dump();
}

// 接收并关联评测反馈，维持候选、运行与来源经验之间的对应关系。
std::string AgentEvaluationService::feedback(const std::string &user) const {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  Json result = Json::Object{};
  const auto &value = impl_->state["feedback"][user];
  if (value.object())
    for (const auto &item : value.fields())
      if (item.second.str("context") == impl_->context)
        result[item.first] = item.second;
  return result.dump();
}

std::string AgentEvaluationService::gepa_feedback(const std::string &user) const {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  Json result = Json::Object{};
  std::string evaluation_context;
  for (auto &item : impl_->state["jobs"].fields()) {
    auto &job = item.second;
    if (job.str("user") != user) continue;
    const auto status = job.str("status");
    const bool stopped = status == "cancelled" || status == "interrupted" ||
                         status == "error" || status == "superseded";
    const auto &view = static_cast<const Json &>(job);
    const auto &previous_delivery = view["memory_feedback"];
    // 搜索改版后旧作业不能恢复，但已完成父子配对仍是合法训练反馈。
    // 每个停止的 attempt 只扫描一次；正在执行的树和留出复测不走此恢复路径。
    if (stopped && job.flag("gepa", false) && !view["final_holdout_replay"].object() &&
        previous_delivery.str("status") != "pending" &&
        (!previous_delivery.object() ||
         previous_delivery.num("checkpoint_attempt", 0) < job.num("attempt", 0))) {
      Json delivery{{"status", "empty"}, {"records", 0}, {"recovered_checkpoint", true},
                    {"checkpoint_attempt", job.num("attempt", 0)}};
      std::string recovered;
      try {
        const auto path = impl_->root / item.first / "search/tree.json";
        if (fs::exists(path)) {
          const auto tree = Json::parse(read(path, 16777216));
          const auto seed = Json::parse(read(impl_->root / item.first / "seed.json", 2097152));
          require((tree.num("schema_version", 0) == 1 || tree.num("schema_version", 0) == 2) &&
                      seed.str("user_id") == user,
                  "gepa_feedback_checkpoint_invalid");
          const auto snapshot = proto<v1::RuntimePolicy>(seed["policy"]).snapshot();
          require(snapshot.identity().user_id() == user, "gepa_feedback_checkpoint_invalid");
          // 复用入库验证：只接收 train、合法来源修订及内容寻址的完整配对。
          // 当前权威库的来源状态仍会在既有文件锁事务内再次核验。
          const auto feedback = merge_gepa_memory_feedback(
              Json(), tree["memory_feedback"], snapshot,
              impl_->config["gate"].num("memory_min_pairs", 2));
          if (feedback["records"].object() && !feedback["records"].fields().empty()) {
            recovered = feedback.dump();
            require(recovered.size() <= 8388608, "gepa_feedback_checkpoint_invalid");
            delivery["status"] = "pending";
            delivery["records"] = feedback["records"].fields().size();
            delivery["fingerprint"] = digest(recovered);
          }
        }
      } catch (const std::exception &) {
        // 单个旧检查点损坏不阻断其他作业；保留固定诊断，不把异常正文写入日志。
        delivery["status"] = "rejected";
        delivery["error"] = "gepa_feedback_checkpoint_invalid";
        recovered.clear();
      }
      // 持久化故障继续交给后台重试，不能误记为检查点损坏并丢弃反馈。
      if (!recovered.empty())
        write(impl_->root / item.first / "search/memory-feedback.json", recovered);
      const bool had_delivery = view.has("memory_feedback");
      const auto previous = view["memory_feedback"];
      job["memory_feedback"] = delivery;
      try {
        impl_->save();
      } catch (...) {
        if (had_delivery)
          job["memory_feedback"] = previous;
        else
          job.fields().erase("memory_feedback");
        throw;
      }
    }
    if (view["memory_feedback"].str("status") != "pending") continue;
    Json feedback;
    try {
      const auto bytes = read(impl_->root / item.first / "search/memory-feedback.json", 8388608);
      require(digest(bytes) == view["memory_feedback"].str("fingerprint"),
              "gepa_memory_feedback_changed");
      feedback = Json::parse(bytes);
      require(feedback.object() && feedback.num("schema_version", 0) == 1 &&
                  feedback.str("user_id") == user && feedback["records"].object() &&
                  !feedback["records"].fields().empty(), "gepa_memory_feedback_identity_invalid");
      const auto seed = Json::parse(read(impl_->root / item.first / "seed.json", 2097152));
      require(seed.str("user_id") == user, "gepa_memory_feedback_identity_invalid");
      const auto snapshot = proto<v1::RuntimePolicy>(seed["policy"]).snapshot();
      require(snapshot.identity().user_id() == user, "gepa_memory_feedback_identity_invalid");
      // 摘要正确仍需检查结构、用户和训练来源；不把留出或非法记录交给权威库事务。
      // 这里只验证，不用归一化产生的新审计时间改写原始投递包。
      (void)merge_gepa_memory_feedback(Json(), feedback, snapshot,
                                       impl_->config["gate"].num("memory_min_pairs", 2));
    } catch (const std::exception &) {
      // 一份丢失或损坏的投递只能影响该作业，不能耗尽全局刷新重试。
      // 保留文件和搜索树，明确拒收；新的合法 attempt 仍可由停止检查点恢复。
      const auto previous = view["memory_feedback"];
      job["memory_feedback"]["status"] = "rejected";
      job["memory_feedback"]["error"] = "gepa_memory_feedback_invalid";
      job["memory_feedback"]["checkpoint_attempt"] = job.num("attempt", 0);
      try {
        impl_->save();
      } catch (...) {
        job["memory_feedback"] = previous;
        throw;
      }
      continue;
    }
    // comparison_context 只是历史归因字段，不再因为管理器版本变化拒绝
    // 已验证的 GEPA 反馈；内容寻址和用户边界仍由上面的事务校验保护。
    result[item.first] = feedback;
  }
  return result.dump();
}

void AgentEvaluationService::complete_gepa_feedback(const std::string &user,
                                                   const std::string &id,
                                                   const std::string &result_json) {
  const auto result = Json::parse(result_json);
  keys(result, {"memory_revision", "records", "updated"});
  std::lock_guard<std::mutex> guard(impl_->mutex);
  auto &job = impl_->state["jobs"][id];
  require(job.str("user") == user && job["memory_feedback"].object(),
          "evaluation_job_not_found", 404);
  if (job["memory_feedback"].str("status") == "committed") return;
  require(job["memory_feedback"].str("status") == "pending",
          "gepa_memory_feedback_not_pending");
  const auto previous = job["memory_feedback"];
  job["memory_feedback"]["status"] = "committed";
  job["memory_feedback"]["result"] = result;
  try {
    impl_->save();
  } catch (...) {
    // 确认标记未持久化时仍保留待投递，不能让内存中的“已消费”吞掉恢复机会。
    job["memory_feedback"] = previous;
    throw;
  }
}

// 读取本次搜索的节点、父子与融合关系，供展示和复盘，不修改候选策略。
std::string AgentEvaluationService::search_tree(const std::string &user,
                                                const std::string &id) const {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  require(impl_->state["jobs"].has(id) && impl_->state["jobs"][id].str("user") == user,
          "evaluation_job_not_found", 404);
  require(impl_->state["jobs"][id].flag("gepa", false), "evaluation_job_not_search", 409);
  const auto path = impl_->root / id / "search/tree.json";
  return fs::exists(path) ? read(path, 16777216) : Json{{"status", "pending"}}.dump();
}

// 在冻结候选上执行留出评测，留出结果不反馈到当轮策略改写。
std::string AgentEvaluationService::replay_holdout(const std::string &user, const std::string &id) {
  constexpr std::size_t kFinalists = 3;
  std::lock_guard<std::mutex> guard(impl_->mutex);
  require(!impl_->state.has("worker_error"), "evaluation_worker_unavailable", 503);
  require(impl_->state["jobs"].has(id) && impl_->state["jobs"][id].str("user") == user,
          "evaluation_job_not_found", 404);
  auto &job = impl_->state["jobs"][id];
  require(job.flag("gepa", false) &&
              (job.str("status") == "rejected" || job.str("status") == "superseded") &&
              !job.flag("published", false),
          "evaluation_holdout_replay_not_allowed", 409);
  require(impl_->state["latest"].str(lane(user, job.str("level"))) == id, "evaluation_job_obsolete",
          409);
  require(!job["final_holdout_replay"].object(), "evaluation_holdout_replay_already_requested",
          409);
  const auto path = impl_->root / id / "search/tree.json";
  // 历史回放只能复用语义一致的冻结 L0；否则候选与基线来自不同执行定义，不能比较。
  require(fs::exists(path), "evaluation_search_tree_missing", 409);
  const auto source_bytes = read(path, 16777216);
  auto tree = Json::parse(source_bytes);
  const bool legacy = tree.str("status") == "completed" && tree.flag("holdout_started", false);
  const bool derived = tree.str("status") == "validation_rejected" &&
                       !tree.flag("holdout_started", false);
  require((legacy || derived) && tree["active"].null() && tree["batch"].null(),
          "evaluation_holdout_replay_state_invalid", 409);
  const auto data = read_evaluation_dataset(impl_->dataset_path);
  std::size_t holdout = 0;
  for (const auto &task : data.tasks())
    holdout += task.split() == "holdout";
  const auto trials = holdout * impl_->evaluation.num("repetitions", 1) *
                      (kFinalists + (derived ? 1 : 0));
  require(tree.num("trials_used", SIZE_MAX) + trials <= impl_->gepa.num("max_trials", 0),
          "evaluation_holdout_replay_budget_exhausted", 409);
  const auto protocol = derived ? "derived-top-k-v2" : "top-k-v1-retrospective";
  const auto source_hash = digest(source_bytes);
  if (derived) {
    write(impl_->root / id / "search/source-tree-derived-top-k-v2.json", source_bytes);
    tree["derived_selection_protocol"] =
        Json{{"protocol", protocol},
             {"source_tree_hash", source_hash},
             {"source_context", tree["context"]},
             {"source_evaluation_context", tree["evaluation_context"]},
             {"source_candidate_evaluation_context", tree["candidate_evaluation_context"]},
             {"evaluation_context", impl_->evaluation_fingerprint()},
             {"candidate_evaluation_context", impl_->candidate_evaluation_fingerprint()}};
    tree["holdout_baseline_cache"] = Json::Object{};
    write(path, tree.dump());
  }
  job["final_holdout_replay"] =
      Json{{"schema_version", 1},
           {"protocol", protocol},
           {"requested_count", kFinalists},
           {"state", "queued"},
           {"requested_ms", now_ms()},
           {"evaluation_context", impl_->evaluation_fingerprint()},
           {"candidate_evaluation_context", impl_->candidate_evaluation_fingerprint()},
           {"source_tree_hash", source_hash},
           {"source_context", tree["context"]},
           {"source_candidate_evaluation_context", tree["candidate_evaluation_context"]},
           {"source_status", job["status"]},
           {"source_gate", job["gate"]}};
  job["status"] = "pending";
  job["queued_ms"] = now_ms();
  job.fields().erase("error");
  job.fields().erase("cancel_requested");
  impl_->save();
  impl_->wake.notify_all();
  return Json{{"accepted", true}, {"job_id", id}, {"requested_candidates", kFinalists}}.dump();
}

// 为允许重试的失败作业恢复执行入口，保留固定输入与幂等身份。
void AgentEvaluationService::retry(const std::string &user, const std::string &id) {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  require(!impl_->state.has("worker_error"), "evaluation_worker_unavailable", 503);
  require(impl_->state["jobs"].has(id) && impl_->state["jobs"][id].str("user") == user,
          "evaluation_job_not_found", 404);
  auto &job = impl_->state["jobs"][id];
  const auto status = job.str("status");
  const auto tree_path = impl_->root / id / "search/tree.json";
  Json extension_tree;

  // 普通失败沿用原重试语义。已完成的业务拒绝默认仍不可重试；唯一例外是 GEPA
  // 尚未进入留出、仅因搜索预算耗尽而拒绝，且新配置确实扩大了对应预算。
  bool extended_search = false;
  if (status == "rejected" && job.flag("gepa", false) && !job.flag("published", false)) {
    try {
      extension_tree = Json::parse(read(tree_path, 16777216));
      const auto stop = extension_tree.str("stop_reason");
      std::size_t previous_timeout = 0;
      if (extension_tree["budget_extensions"].array() &&
          !extension_tree["budget_extensions"].items().empty())
        previous_timeout =
            extension_tree["budget_extensions"].items().back().num("timeout_ms", 0);
      const bool expanded =
          gepa_model_budget_extended(extension_tree, impl_->gepa) ||
          (stop == "max_rounds" &&
           extension_tree.num("rounds", SIZE_MAX) < impl_->gepa.num("max_rounds", 0)) ||
          (stop == "gepa_search_time_reserved" &&
           previous_timeout < impl_->gepa.num("timeout_ms", 0)) ||
          (stop == "patience" &&
           extension_tree.num("stagnation", SIZE_MAX) < impl_->gepa.num("patience", 0));
      extended_search =
          extension_tree.str("status") == "validation_rejected" &&
          !extension_tree.flag("holdout_started", false) && extension_tree["active"].null() &&
          extension_tree["batch"].null() && expanded;
    } catch (const std::exception &) {
      extended_search = false;
    }
  }
  // superseded 不能普遍重试；它只在下方证明所有更新作业均为零执行空壳后
  // 才能恢复。先保留候选身份，避免启动竞态永久封死已有有效搜索树。
  const bool superseded_candidate = status == "superseded";
  require(status == "error" || status == "interrupted" || status == "inconclusive" ||
              status == "cancelled" || superseded_candidate || extended_search,
          "evaluation_job_not_retryable", 409);

  bool derived_holdout_retry = false;
  bool rebind_holdout_execution_context = false;
  bool rebind_candidate_execution_context = false;
  std::string current_candidate_context;

  // A closed initial validation with a temporary judge failure must retry
  // under its original execution and grader identities. Unlike a general
  // search resume, this path may not migrate its cache and replay the Agent.
  if (job.flag("gepa", false) &&
      (job.str("error") == "gepa_baseline_evaluation_inconclusive" ||
       job.str("error") == "gepa_seed_evaluation_inconclusive")) {
    if (!extension_tree.object())
      extension_tree = Json::parse(read(tree_path, 16777216));
    const auto grading = impl_->evaluation_fingerprint();
    const auto execution = impl_->candidate_evaluation_fingerprint();
    require(initial_review_retry_context_matches(extension_tree, job, grading, execution),
            "evaluation_initial_review_context_changed", 409);
  }

  // 一棵树的父子成绩必须来自同一评分口径和同一 Agent 执行身份。
  // 评测管理器重编译不改变 Agent 身份；恢复时只迁移管理器造成的候选上下文
  // 差异，不迁移数据集、评分口径、种子、基线或已产生的留出成绩。
  if (job.flag("gepa", false) && fs::exists(tree_path)) {
    if (!extension_tree.object())
      extension_tree = Json::parse(read(tree_path, 16777216));
    current_candidate_context = impl_->candidate_evaluation_fingerprint();
    const auto &replay = job["final_holdout_replay"];
    const auto &derived = extension_tree["derived_selection_protocol"];
    const auto &final_holdout = extension_tree["final_holdout"];
    // 派生 Top-K 已经冻结候选后，基础设施失败可能发生在首个有效 trial 之前。
    // 此时允许把“本次留出执行上下文”更新为当前运行时，但源树上下文、候选顺序、
    // campaign 和评分上下文必须完全不变；已经完成的候选由 evaluation 缓存复用，
    // 只补跑仍未完成的候选，不能因为部分结果存在就把整个回放判为不可恢复。
    derived_holdout_retry =
        replay.object() && replay.str("protocol") == "derived-top-k-v2" &&
        replay.num("requested_count", 0) == 3 && replay.str("state") != "completed" &&
        derived.object() && derived.str("protocol") == "derived-top-k-v2" &&
        final_holdout.object() && final_holdout.str("protocol") == "derived-top-k-v2" &&
        extension_tree.flag("holdout_started", false) &&
        (extension_tree.str("status") == "searching" ||
         extension_tree.str("status") == "cancelled" || extension_tree.str("status") == "error" ||
         extension_tree.str("status") == "inconclusive") &&
        (final_holdout.str("status") == "prepared" ||
         final_holdout.str("status") == "running" ||
         final_holdout.str("status") == "inconclusive") &&
        !final_holdout["candidates"].items().empty();
    // 普通搜索树也允许从管理器重编译后的上下文继续。evaluation_context 已经
    // 固定了数据集、裁判和模型口径，seed_version 固定了搜索输入；候选身份的
    // 差异只需要刷新为当前 Agent 执行上下文，不再让管理器自身版本阻断恢复。
    const bool manager_only_context_retry =
        extension_tree.str("candidate_evaluation_context") != current_candidate_context &&
        extension_tree.str("seed_version") == job.str("seed_version") &&
        extension_tree["nodes"].array() &&
        (extension_tree.str("status") == "searching" ||
         extension_tree.str("status") == "cancelled" ||
         extension_tree.str("status") == "error" ||
         extension_tree.str("status") == "inconclusive" ||
         extension_tree.str("status") == "validation_rejected") &&
        (!extension_tree.flag("holdout_started", false) || derived_holdout_retry);
    // 执行 fingerprint 只用于诊断；恢复不再因管理器重编译而失败。
    rebind_holdout_execution_context =
        derived_holdout_retry &&
        replay.str("candidate_evaluation_context") != current_candidate_context;
    rebind_candidate_execution_context = manager_only_context_retry && !derived_holdout_retry;
  }
  // 只有执行/评分 context 未变的搜索预算扩容可继续原树。
  const bool rewrite_search_context = extended_search;
  // API/model quota rotation intentionally changes the service context.  When
  // the caller opts into unconditional resume, rebind only an unfinished GEPA
  // search checkpoint with the same seed and no holdout evidence.  Dataset,
  // judge and candidate identity checks remain in force below.
  const bool unconditional_resume = [] {
    const auto *value = std::getenv("TRAJECTORY_UNCONDITIONAL_RESUME");
    return value && *value && std::string(value) != "0";
  }();
  const bool active_checkpoint =
      extension_tree["active"].null() ||
      (extension_tree["active"].integer() > 0 &&
       extension_tree["active"].integer() < extension_tree["nodes"].items().size());
  std::optional<v1::RuntimePolicy> checkpoint_source;
  if (job.flag("gepa", false) && job.str("status") == "cancelled") {
    try {
      const auto seed_bytes = read(impl_->root / id / "seed.json", 2097152);
      if (agent_version_id(seed_bytes) == job.str("seed_version")) {
        checkpoint_source = proto<v1::RuntimePolicy>(Json::parse(seed_bytes)["policy"]);
        PolicyRuntime checked(*checkpoint_source);
      }
    } catch (const std::exception &) {
      checkpoint_source.reset();
    }
  }
  const bool closed_reflection_resume =
      unconditional_resume && job.flag("gepa", false) &&
      closed_reflection_checkpoint(extension_tree, job, impl_->evaluation_fingerprint(),
                                   read_evaluation_dataset(impl_->dataset_path).dataset_version(),
                                   current_candidate_context,
                                   checkpoint_source ? &*checkpoint_source : nullptr);
  const bool resume_context_rebind =
      unconditional_resume && job.flag("gepa", false) && extension_tree.object() &&
      extension_tree.str("seed_version") == job.str("seed_version") &&
      (extension_tree.str("status") == "searching" ||
       extension_tree.str("status") == "cancelled" ||
       extension_tree.str("status") == "error" ||
       extension_tree.str("status") == "inconclusive") &&
      !extension_tree.flag("holdout_started", false) && active_checkpoint &&
      (extension_tree["batch"].null() || closed_reflection_resume) &&
      extension_tree["selected_version"].null() &&
      job.str("context") != impl_->context;
  const auto lane_key = lane(user, job.str("level"));
  const auto latest = job.flag("training", false)
                          ? impl_->state["training"][lane(user, "training")].str("job_id")
                          : impl_->state["latest"].str(lane_key);

  // 被空壳覆盖的旧作业必须自身确实含有可恢复证据，且尚未接触留出或发布。
  // 仅凭 superseded 状态不足以恢复，否则可能倒退到一个从未真正执行的作业。
  bool superseded_source_safe = !superseded_candidate;
  if (superseded_candidate && job.flag("gepa", false) && !job.flag("published", false) &&
      job["final_holdout_replay"].null()) {
    try {
      const auto source_tree = Json::parse(read(tree_path, 16777216));
      const auto source_status = source_tree.str("status");
      superseded_source_safe =
          source_tree.str("context") == job.str("context") &&
          source_tree.str("seed_version") == job.str("seed_version") &&
          (source_status == "searching" || source_status == "cancelled" ||
           source_status == "error" || source_status == "validation_rejected") &&
          (source_tree.num("rounds", 0) > 0 || source_tree.num("model_calls", 0) > 0 ||
           source_tree.num("trials_used", 0) > 0) &&
          !source_tree.flag("holdout_started", false) && source_tree["active"].null() &&
          source_tree["batch"].null() && source_tree["selected_version"].null();
    } catch (const std::exception &) {
      superseded_source_safe = false;
    }
  }

  // 预算续跑期间可能产生了一个更晚、但在任何真实执行前就失败或取消的空壳作业。
  // 它不能永久封死仍有有效搜索树的旧作业；反之，只要任一更新作业执行过题目、
  // 进入留出集或发布过结果，就继续按过期作业拒绝，避免旧搜索覆盖新证据。
  bool shadowed_only_by_empty_jobs = latest != id;
  bool found_newer_job = false;
  std::vector<std::string> shadowing_empty_jobs;
  bool cancel_shadowing_running_job = false;
  if (shadowed_only_by_empty_jobs) {
    std::vector<std::pair<std::string, const Json *>> newer_jobs;
    for (const auto &pair : impl_->state["jobs"].fields()) {
      const auto &candidate = pair.second;
      if (candidate.str("user") == user && candidate.str("level") == job.str("level") &&
          candidate.num("queued_ms", 0) > job.num("queued_ms", 0))
        newer_jobs.emplace_back(pair.first, &candidate);
    }
    std::sort(newer_jobs.begin(), newer_jobs.end(), [](const auto &left, const auto &right) {
      const auto left_time = left.second->num("queued_ms", 0);
      const auto right_time = right.second->num("queued_ms", 0);
      return left_time == right_time ? left.first < right.first : left_time < right_time;
    });
    std::string expected_seed_source = id;
    for (const auto &pair : newer_jobs) {
      const auto &newer = *pair.second;
      if (newer.str("user") != user || newer.str("level") != job.str("level") ||
          newer.num("queued_ms", 0) <= job.num("queued_ms", 0))
        continue;
      found_newer_job = true;
      const auto newer_status = newer.str("status");
      // 显式 retry 启动管理服务时，auto_evaluate 可能抢先创建新作业。
      // 只有它仍是零轮、零调用、零试题的空壳时，pending/running 才可
      // 由已指定的旧树续跑原子取代；一旦有真实证据就仍按过期拒绝。
      const bool cancelled_empty =
          (newer_status == "cancelled" &&
           (newer.str("error") == "gepa_cancelled" ||
            newer.str("error") == "evaluation_cancelled")) ||
          (newer_status == "error" &&
           (newer.str("error") == "gepa_cancelled" ||
            newer.str("error") == "evaluation_cancelled")) ||
          // 服务载入不同搜索 context 时，会把上述停机错误规范化为 interrupted；
          // 后续仍必须同时通过同源版本绑定和 0/0/0 cancelled tree 检查。
          (newer_status == "interrupted" &&
           newer.str("error") == "evaluation_interrupted_retry_required") ||
          (newer_status == "superseded" &&
           newer.str("error") == "evaluation_job_superseded_by_retry");
      if (!newer.flag("gepa", false) || newer.flag("published", false) ||
          !cancelled_empty || newer.str("version") != job.str("version") ||
          newer.str("seed_version") != job.str("seed_version") ||
          (newer.str("context") != job.str("context") &&
           newer.str("context") != impl_->context) ||
          newer.str("seed_source_job") != expected_seed_source) {
        shadowed_only_by_empty_jobs = false;
        break;
      }
      try {
        const auto newer_tree =
            Json::parse(read(impl_->root / pair.first / "search/tree.json", 16777216));
        if (newer_tree.num("rounds", SIZE_MAX) != 0 ||
            newer_tree.num("model_calls", SIZE_MAX) != 0 ||
            newer_tree.num("trials_used", SIZE_MAX) != 0 ||
            newer_tree.flag("holdout_started", false) || !newer_tree["active"].null() ||
            !newer_tree["batch"].null() || !newer_tree["selected_version"].null())
          shadowed_only_by_empty_jobs = false;
      } catch (const std::exception &) {
        // 排队后、worker 真正启动前取消的作业不会生成 search/tree.json。
        // started_ms 缺失是这里唯一可接受的空壳证据；已经启动却丢树的作业
        // 仍按未知执行拒绝，不能凭状态字符串猜测它没有产生真实试验。
        shadowed_only_by_empty_jobs &= newer["started_ms"].null();
      }
      if (shadowed_only_by_empty_jobs) {
        shadowing_empty_jobs.push_back(pair.first);
        expected_seed_source = pair.first;
        cancel_shadowing_running_job |= newer_status == "running";
      }
      if (!shadowed_only_by_empty_jobs) break;
    }
    shadowed_only_by_empty_jobs =
        shadowed_only_by_empty_jobs && found_newer_job && !shadowing_empty_jobs.empty() &&
        shadowing_empty_jobs.back() == latest;
  }
  const auto previous_latest = impl_->state["latest"][lane_key];
  require((latest == id || shadowed_only_by_empty_jobs) &&
              superseded_source_safe &&
              (!superseded_candidate || shadowed_only_by_empty_jobs) &&
              (job.str("context") == impl_->context || rewrite_search_context ||
               derived_holdout_retry || resume_context_rebind),
          "evaluation_job_obsolete", 409);

  const auto previous_job = job;
  const auto previous_tree = extension_tree;
  const auto previous_cancel_running = impl_->cancel_running.load();
  Json previous_shadowing_jobs = Json::Object{};
  for (const auto &shadowing_id : shadowing_empty_jobs)
    previous_shadowing_jobs[shadowing_id] = impl_->state["jobs"][shadowing_id];
  if (rebind_holdout_execution_context) {
    // 候选已经由冻结的验证成绩确定，基础设施又在产生首条可评分证据前失败。
    // 这里只替换后续留出执行所需的二进制/宿主指纹，绝不迁移搜索树、评分口径、
    // 候选集合或既有分数。快照已在上方保存，状态落盘失败时可完整回滚。
    if (!extension_tree["holdout_execution_context_revisions"].array())
      extension_tree["holdout_execution_context_revisions"] = Json::Array{};
    extension_tree["holdout_execution_context_revisions"].push(
        Json{{"previous", job["final_holdout_replay"]["candidate_evaluation_context"]},
             {"current", current_candidate_context},
             {"reason", "unscored_infrastructure_retry"},
             {"resumed_ms", now_ms()}});
    extension_tree["derived_selection_protocol"]["candidate_evaluation_context"] =
        current_candidate_context;
    job["final_holdout_replay"]["candidate_evaluation_context"] =
        current_candidate_context;
    job["final_holdout_replay"]["state"] = "queued";
  }
  if (resume_context_rebind || closed_reflection_resume) {
    if (!extension_tree["resume_context_revisions"].array())
      extension_tree["resume_context_revisions"] = Json::Array{};
    extension_tree["resume_context_revisions"].push(
        Json{{"previous", job["context"]},
             {"current", impl_->context},
             {"reason", closed_reflection_resume ? "closed_reflection_checkpoint"
                                                  : "unconditional_checkpoint_resume"},
             {"resumed_ms", now_ms()}});
    if (closed_reflection_resume) {
      auto &revision = extension_tree["resume_context_revisions"].items().back();
      revision["retained_evaluations"] = Json::Array{};
      std::map<std::string, Json> closed_children;
      for (const auto &node : extension_tree["nodes"].items())
        if (node.str("kind") == "reflection" && !node.str("version").empty() &&
            node["screening"].object() && node["tasks"].array()) {
          const auto artifact_path = tree_path.parent_path() / "versions" /
                                     (std::to_string(node["id"].integer()) + ".json");
          const auto bytes = read(artifact_path);
          require(agent_version_id(bytes) == node.str("version"), "evaluation_artifact_changed");
          closed_children.emplace(node.str("version"), node["tasks"]);
        }
      for (const auto &entry : extension_tree["evaluations"].fields()) {
        const auto child = closed_children.find(entry.second.str("version"));
        if (entry.second.str("version") == job.str("seed_version") ||
            (child != closed_children.end() && entry.second.str("split") == "train" &&
             (entry.second.str("status") == "complete" || entry.second.str("status") == "inconclusive") &&
             entry.second["task_ids"].dump() == child->second.dump()))
          revision["retained_evaluations"].push(entry.first);
      }
    }
    extension_tree["context"] = impl_->context;
    extension_tree["evaluation_context"] = impl_->evaluation_fingerprint();
    extension_tree["candidate_evaluation_context"] = current_candidate_context;
    job["context"] = impl_->context;
    job["evaluation_context"] = impl_->evaluation_fingerprint();
    job["candidate_evaluation_context"] = current_candidate_context;
    write(tree_path, extension_tree.dump());
  }
  if (rebind_candidate_execution_context && !closed_reflection_resume) {
    // 普通搜索只刷新候选执行身份；搜索树中的父子、分数和任务集合不变。
    extension_tree["candidate_evaluation_context"] = current_candidate_context;
    job["candidate_evaluation_context"] = current_candidate_context;
    write(tree_path, extension_tree.dump());
  }
  if (rewrite_search_context) {
    // Only a proved increase of this tree's actual old call limit reopens
    // reflection; retain every paid counter and all completed node evidence.
    clear_extended_gepa_reflection_exhaustion(extension_tree, impl_->gepa);
    // 搜索预算属于算法调度而非执行/裁判口径。只在上述强约束全部满足时迁移
    // context，并记录旧值；其他配置变化仍由普通过期保护拒绝。
    if (!extension_tree["budget_extensions"].array())
      extension_tree["budget_extensions"] = Json::Array{};
    extension_tree["budget_extensions"].push(
        Json{{"previous_context", job["context"]},
             {"context", impl_->context},
             {"reason", "budget_extension"},
             {"rounds_used", extension_tree["rounds"]},
             {"model_calls_used", extension_tree["model_calls"]},
             {"max_rounds", impl_->gepa["max_rounds"]},
             {"max_model_calls", impl_->gepa["max_model_calls"]},
             {"timeout_ms", impl_->gepa["timeout_ms"]},
             {"patience", impl_->gepa["patience"]},
             {"resumed_ms", now_ms()}});
    extension_tree["context"] = impl_->context;
    extension_tree["evaluation_context"] = impl_->evaluation_fingerprint();
    extension_tree["candidate_evaluation_context"] =
        impl_->candidate_evaluation_fingerprint();
    extension_tree.fields().erase("stop_reason");
    extension_tree.fields().erase("finished_ms");
    extension_tree.fields().erase("selected_version");
    write(tree_path, extension_tree.dump());

    // 上一段训练反馈已经通过内容寻址事务提交。续跑后重新投递整份反馈仍会幂等
    // 合并，同时允许新节点产生的证据进入同一权威库；旧决策不再代表当前终态。
    job["context"] = impl_->context;
    job.fields().erase("memory_feedback");
    job.fields().erase("search");
    job.fields().erase("gate");
    job.fields().erase("finished_ms");
  }
  if (rebind_holdout_execution_context) {
    write(tree_path, extension_tree.dump());
    job.fields().erase("finished_ms");
  }
  job["status"] = "pending";
  job.fields().erase("error");
  job.fields().erase("cancel_requested");
  if (shadowed_only_by_empty_jobs) {
    for (const auto &shadowing_id : shadowing_empty_jobs) {
      auto &shadowing = impl_->state["jobs"][shadowing_id];
      if (shadowing.str("status") == "pending" || shadowing.str("status") == "running") {
        shadowing["status"] = "superseded";
        shadowing["cancel_requested"] = true;
        shadowing["error"] = "evaluation_job_superseded_by_retry";
      }
    }
    impl_->state["latest"][lane_key] = id;
    if (cancel_shadowing_running_job)
      impl_->cancel_running = true;
  }
  job["queued_ms"] = now_ms();
  try {
    impl_->save();
  } catch (...) {
    job = previous_job;
    impl_->state["latest"][lane_key] = previous_latest;
    impl_->cancel_running = previous_cancel_running;
    for (const auto &pair : previous_shadowing_jobs.fields())
      impl_->state["jobs"][pair.first] = pair.second;
    if (rewrite_search_context || rebind_holdout_execution_context)
      write(tree_path, previous_tree.dump());
    throw;
  }
  if (rewrite_search_context) {
    fs::remove(impl_->root / id / "search/decision.json");
    fs::remove(impl_->root / id / "search/selected-version.json");
  }
  impl_->wake.notify_all();
}

// 请求取消当前作业，保留已落盘结果；在途执行由监督逻辑安全收尾。
void AgentEvaluationService::cancel(const std::string &user, const std::string &id) {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  require(impl_->state["jobs"].has(id) && impl_->state["jobs"][id].str("user") == user,
          "evaluation_job_not_found", 404);
  auto &job = impl_->state["jobs"][id];
  require(job.str("status") == "running" || job.str("status") == "pending",
          "evaluation_job_not_active", 409);
  job["cancel_requested"] = true;
  if (job.str("status") == "running")
    impl_->cancel_running = true;
  else {
    job["status"] = "cancelled";
    job["error"] = "evaluation_cancelled";
    job["finished_ms"] = now_ms();
  }
  impl_->save();
  impl_->wake.notify_all();
}
} // 命名空间结束：trajectory::evolution
