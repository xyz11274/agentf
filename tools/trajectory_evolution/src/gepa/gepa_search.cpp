// GEPA 搜索主流程：生成候选、选择父节点、融合分支并调度训练和验证。
// 候选是持久化策略版本，不直接修改宿主代码；实际效果由统一评测器测量。
// 缓存身份必须包含数据集、运行版本和评分语义，不能把旧评分与新评分直接混用。
// 留出数据用于最终比较，不作为反思输入或下一代候选的训练证据。

// GEPA 搜索负责候选生成、评测调度与节点选择；宿主执行交给统一评测器。
#include "gepa_search.h"
#include "gepa_strategy.h"
#include "control_generation.h"
#include "failure_ledger.h"
#include "holdout_baseline_cache.h"
#include "schedule_evidence.h"
#include "gepa_feedback.h"
#include "shared_memory.h"
#include "evaluation_contracts.h"
#include "turn_contract.h"
#include "goal_assessment.h"
#include "evidence_fields.h"
#include "code_projection.h"
#include "local_support.h"
#include "model_json.h"
#include "agentflow/trajectory/runtime_config.h"
#include "trajectory_evolution/agent_evaluation.h"

#include <chrono>
#include <cctype>
#include <cmath>
#include <fstream>
#include <limits>
#include <random>
#include <set>
#include <sstream>
#include <tuple>
#include <openssl/sha.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <google/protobuf/util/json_util.h>
#include <google/protobuf/util/message_differencer.h>

namespace trajectory::evolution::integration {
namespace {
namespace fs = std::filesystem;
namespace legacy = agentflow::trajectory;
using Report = dataset::v1::TaskEvaluationReport;
using Trial = dataset::v1::EvaluationTrial;

// Preserve the original error while keeping incomplete full-pool work out of
// the legacy single-draft repair path. Completed drafts remain repairable.
struct FullPoolProposalIncomplete : Error {
  using Error::Error;
};

// 计算稳定内容摘要，供请求标识、缓存或版本比较使用。
std::string hash(const std::string &bytes) {
  unsigned char out[SHA256_DIGEST_LENGTH];
  require(SHA256(reinterpret_cast<const unsigned char *>(bytes.data()), bytes.size(), out),
          "gepa_hash_failed");
  std::string result;
  for (auto c : out) {
    result += "0123456789abcdef"[c >> 4];
    result += "0123456789abcdef"[c & 15];
  }
  return result;
}

// 读取当前时间，供本模块预算、状态或持久化标记使用。
std::size_t now() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

// 校验输入或产物路径，防止越界与错误目录复用。
void check_path(const fs::path &path) {
  fs::path prefix;
  for (const auto &part : fs::absolute(path)) {
    prefix /= part;
    require(!fs::is_symlink(prefix), "gepa_symlink_rejected");
  }
}

// 按调用方给出的大小上限读取文件；读取失败向上层报告，不能用空数据冒充成功。
std::string read(const fs::path &path, std::size_t limit = 4194304) {
  check_path(path);
  std::ifstream stream(path, std::ios::binary | std::ios::ate);
  const auto size = stream ? stream.tellg() : std::streampos(-1);
  if (!stream || size < 0 || static_cast<std::uintmax_t>(size) > limit) {
    // 只记录相对 GEPA 根目录的路径和大小，帮助恢复定位，不记录文件内容或凭据。
    std::cerr << "[gepa] file_unavailable path=" << path.string()
              << " size=" << (size < 0 ? -1 : static_cast<long long>(size))
              << " limit=" << limit << '\n';
    throw Error("gepa_file_unavailable");
  }
  std::string bytes(static_cast<std::size_t>(size), '\0');
  stream.seekg(0);
  require(bytes.empty() || bool(stream.read(bytes.data(), bytes.size())), "gepa_read_failed");
  return bytes;
}

// 按照本模块的输出约定写入内容；调用方负责提供已验证的目标和数据。
void write(const fs::path &path, const std::string &bytes) {
  check_path(path);
  fs::create_directories(path.parent_path());
  require(::chmod(path.parent_path().c_str(), 0700) == 0, "gepa_permission_failed");
  std::string pending = path.string() + ".pending.XXXXXX";
  int fd = ::mkstemp(pending.data());
  require(fd >= 0, "gepa_write_failed");
  try {
    for (std::size_t offset = 0; offset < bytes.size();) {
      const auto n = ::write(fd, bytes.data() + offset, bytes.size() - offset);
      require(n > 0, "gepa_write_failed");
      offset += n;
    }
    require(::fsync(fd) == 0, "gepa_write_failed");
    ::close(fd);
    fd = -1;
    require(::rename(pending.c_str(), path.c_str()) == 0, "gepa_write_failed");
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

// 读取数值并验证类型，避免字符串、空值被当成有效测量结果。
double number(const Json &value, double fallback = 0) {
  return std::holds_alternative<double>(value.value) ? std::get<double>(value.value) : fallback;
}

// 检查或截取当前值至允许范围，维持请求和输出预算。
std::size_t bound(const Json &c, const char *key, std::size_t fallback, std::size_t low,
                  std::size_t high) {
  const auto n = c.num(key, fallback);
  require(n >= low && n <= high, "gepa_limit_invalid");
  return n;
}

// 检查单条试验是否具备可比较成绩。批次级完整性与逐题可用性必须分开：
// 一条暂态失败可以触发定点恢复，但不能抹掉同批其他已经完成的题目。
bool scored_trial(const Trial &trial) {
  // execution_status=failed 表示真实执行未完成，不能因裁判或硬检查
  // 恰好给出 passed 就把它当成可比成绩。业务失败应由完成执行的
  // verdict/assessment 表达，runner 与环境失败则保持未决并定点重跑。
  if (trial.execution_status() != "completed" || !trial.error_code().empty() ||
      trial.verdict() == "error")
    return false;
  const auto evidence = json(trial.evidence());
  if (evidence["assessment"].object()) {
    try {
      assessment_quality(evidence, trial.verdict() == "passed");
      return true;
    } catch (const Error &) {
      return false;
    }
  }
  return trial.verdict() == "passed" || trial.verdict() == "failed";
}

// 检查整份报告是否具备完整成绩；缓存和最终发布仍要求所有试验都可评分。
bool scored(const Report &report) {
  return report.complete() && report.trials_size() &&
         std::all_of(report.trials().begin(), report.trials().end(), scored_trial);
}

// 在保留具体文本的同时限制单字段和整包大小，避免反思上下文失控。
std::string clip(std::string value, std::size_t limit) {
  if (value.size() <= limit)
    return value;
  while (limit && (static_cast<unsigned char>(value[limit]) & 0xc0) == 0x80)
    --limit;
  value.resize(limit);
  return value + " [truncated]";
}

// 裁判常先列已满足项、最后指出缺失项；只保留前缀会把失败原因截掉。
// 固定字节预算内保留两端，并标记中间省略，不把片段伪装成完整评审。
std::string diagnostic_excerpt(const std::string &value, std::size_t limit) {
  if (value.size() <= limit)
    return value;
  const std::string omitted = " ...[middle omitted]... ";
  if (limit <= omitted.size()) {
    auto end = limit;
    while (end && (static_cast<unsigned char>(value[end]) & 0xc0) == 0x80)
      --end;
    return value.substr(0, end);
  }
  auto head = (limit - omitted.size()) / 3;
  auto tail = value.size() - (limit - omitted.size() - head);
  while (head && (static_cast<unsigned char>(value[head]) & 0xc0) == 0x80)
    --head;
  while (tail < value.size() && (static_cast<unsigned char>(value[tail]) & 0xc0) == 0x80)
    ++tail;
  return value.substr(0, head) + omitted + value.substr(tail);
}

// 明确提出经验更新却未改变任何行为时，仅提供一次有界纠正机会。
// 合法空提案保持弃权，不为了凑节点强行制造修改。
Json noop_experience_repair_packet(const Json &input, const Json &draft, std::size_t limit) {
  const auto &selected = input["selected_components"];
  if (!selected.array() || selected.items().size() != 1 ||
      selected.items().front().text().rfind("experience:", 0) != 0)
    return Json();
  const bool attempted = draft["experience"].object() ||
                         (draft["experiences"].array() && !draft["experiences"].items().empty());
  if (!attempted)
    return Json();
  auto repair = input;
  repair.fields().erase("parent_version");
  repair["phase"] = "repair_experience_no_behavior_change";
  repair["error"] = "experience_update_matches_parent";
  repair["attempted_experiences"] = draft.has("experiences")
                                       ? draft["experiences"]
                                       : Json(Json::Array{draft["experience"]});
  // 当前正文、实际条件和应用点已经在原包内；不复制整份父策略或理由文字。
  while (repair.dump().size() > limit && repair["feedback"].items().size() > 2)
    repair["feedback"].items().pop_back();
  if (repair.dump().size() > limit)
    repair.fields().erase("training_batch_summary");
  if (repair.dump().size() > limit)
    return Json();
  return repair;
}

// 有界反思保留实际失败、策略作用轮次及其前一轮，同时保留最新用户修订。
// 编号和省略数明确可见，不能把筛选后的片段误认为完整、连续会话。
Json feedback_turn_context(const dataset::v1::EvaluationTask &task, const Json &evidence,
                           std::size_t limit = 8, std::size_t text_limit = 1024) {
  const auto standards = turn_expectations(task);
  const std::size_t count = task.followup_inputs_size() + 1;
  std::set<std::size_t> selected;
  auto add = [&](std::size_t index) {
    if (index < count && selected.size() < limit)
      selected.insert(index);
  };
  add(count - 1);
  if (evidence["turn_evaluation"].array())
    for (const auto &row : evidence["turn_evaluation"].items())
      if (row.str("verdict") != "passed" && row.num("turn", 0)) {
        const auto index = row.num("turn", 0) - 1;
        add(index);
        if (index)
          add(index - 1);
      }
  const auto traces = turn_traces(evidence);
  for (std::size_t i = 0; i < traces.items().size(); ++i)
    if (traces.items()[i]["events"].array())
      for (const auto &event : traces.items()[i]["events"].items())
        if (event.str("kind") == "advice.applied" &&
            event["application"].str("status") == "applied") {
          add(i);
          if (i)
            add(i - 1);
          break;
        }
  add(0);
  for (std::size_t i = count; i > 0; --i)
    add(i - 1);
  Json rows = Json::Array{};
  for (const auto index : selected) {
    Json row{
        {"turn", index + 1},
        {"input", clip(index ? task.followup_inputs(index - 1) : task.task_input(), text_limit)}};
    if (!standards.null())
      row["expected_behavior"] =
          clip(standards.items()[index].str("expected_behavior"), text_limit);
    if (evidence["turn_evaluation"].array())
      for (const auto &grade : evidence["turn_evaluation"].items())
        if (grade.num("turn", 0) == index + 1) {
          row["verdict"] = grade["verdict"];
          row["reason"] = clip(grade.str("reason"), text_limit);
        }
    rows.push(std::move(row));
  }
  return Json{
      {"turns", rows},
      {"total_turns", count},
      {"omitted_turns", count - selected.size()},
      {"selection", "failed_applied_latest_context; omitted turns are unknown, not absent"}};
}

// 提取受限长度的证据片段供反思使用，完整原文仍保留在轨迹中。
Json excerpt(const Json &value, unsigned depth = 0) {
  if (value.string())
    return clip(value.text(), 2048);
  if (depth >= 12)
    return "[depth limit]";
  if (value.array()) {
    Json result = Json::Array{};
    for (const auto &item : value.items()) {
      if (result.items().size() == 24) {
        result.push("[items truncated]");
        break;
      }
      result.push(excerpt(item, depth + 1));
    }
    return result;
  }
  if (value.object()) {
    Json result = Json::Object{};
    for (const auto &item : value.fields()) {
      if (result.fields().size() == 32) {
        result["_truncated"] = true;
        break;
      }
      result[item.first] = excerpt(item.second, depth + 1);
    }
    return result;
  }
  return value;
}

// 对反思片段进行有界精简，只影响模型视图，不修改原始证据。
Json compact_excerpt(const Json &value, unsigned depth = 0) {
  if (value.string())
    return clip(value.text(), 512);
  if (depth >= 8)
    return "[depth limit]";
  if (value.array()) {
    Json result = Json::Array{};
    for (const auto &item : value.items()) {
      if (result.items().size() == 8) {
        result.push("[items truncated]");
        break;
      }
      result.push(compact_excerpt(item, depth + 1));
    }
    return result;
  }
  if (value.object()) {
    Json result = Json::Object{};
    for (const auto &item : value.fields()) {
      if (result.fields().size() == 16) {
        result["_truncated"] = true;
        break;
      }
      result[item.first] = compact_excerpt(item.second, depth + 1);
    }
    return result;
  }
  return value;
}

// 解析模型返回的单个对象并验证结构；格式正确不等于业务判断正确。
Json parse_model_object(const std::string &response, bool *recovered = nullptr) {
  if (recovered)
    *recovered = false;
  const auto body = legacy::unwrap_model_json(response);
  try {
    auto value = Json::parse(body);
    require(value.object(), "invalid_json");
    return value;
  } catch (const std::exception &) {
    // 部分 OpenAI 兼容服务即使指定 response_format=json_object，仍会在有效 JSON
    // 对象前后附带简短说明。这里只接受唯一一个括号平衡且可解析的对象；多个对象
    // 仍视为歧义，不能猜测哪一个才是模型结果。
  }
  Json only;
  std::size_t count = 0, start = 0, depth = 0;
  bool quoted = false, escaped = false;
  for (std::size_t i = 0; i < body.size(); ++i) {
    const char c = body[i];
    if (depth && quoted) {
      if (escaped)
        escaped = false;
      else if (c == '\\')
        escaped = true;
      else if (c == '"')
        quoted = false;
      continue;
    }
    if (depth && c == '"') {
      quoted = true;
      continue;
    }
    if (c == '{') {
      if (!depth)
        start = i;
      ++depth;
    } else if (c == '}' && depth && !--depth) {
      try {
        auto candidate = Json::parse(body.substr(start, i - start + 1));
        if (candidate.object()) {
          only = std::move(candidate);
          ++count;
        }
      } catch (const std::exception &) {
      }
    }
  }
  require(count == 1, "invalid_json");
  if (recovered)
    *recovered = true;
  return only;
}

// 收集候选关联的全部来源经验，用于反思与版本溯源。
Json all_source_experiences(const Json &artifact) {
  Json result = Json::Array{};
  const auto policy = proto<v1::RuntimePolicy>(artifact["policy"]);
  for (const auto &item : policy.snapshot().experiences()) {
    if (!item.control_target_type().empty())
      continue;
    bool approved = false;
    for (const auto &ref : policy.options().approved_experiences())
      approved |=
          ref.memory_id() == item.memory_id() && ref.content_revision() == item.content_revision();
    if (!approved)
      continue;
    Json value{{"memory_id", item.memory_id()},
               {"scope", item.target_scope()},
               {"condition", item.trigger_pattern()},
               {"content", item.content()}};
    for (const auto &rewrite : policy.options().experience_overrides())
      if (rewrite.source_experience().memory_id() == item.memory_id()) {
        value["content"] = rewrite.content();
        if (!rewrite.trigger_pattern().empty())
          value["condition"] = value["trigger_pattern"] = rewrite.trigger_pattern();
        if (!rewrite.observed_operations().empty()) {
          value["observed_operations"] = Json::Array{};
          for (const auto &operation : rewrite.observed_operations())
            value["observed_operations"].push(operation);
        }
      }
    for (const auto &disabled : policy.options().disabled_injections())
      if (disabled.memory_id() == item.memory_id() &&
          disabled.content_revision() == item.content_revision()) {
        value["content"] = nullptr;
        value.fields().erase("trigger_pattern");
        value.fields().erase("observed_operations");
      }
    result.push(value);
  }
  return result;
}

// 公共关系固定 PublishedExperience 的版本；批准和应用回执固定 ShadowExperience
// 的版本。两种 hash 不可直接比较，只有内容与双边批准均有效时才合并搜索轴。
std::map<std::pair<std::string, std::string>, std::string>
canonical_experience_aliases(const v1::RuntimePolicy &policy) {
  std::map<std::pair<std::string, std::string>, std::string> result;
  std::map<std::string, const v1::ShadowExperience *> entries;
  std::set<std::pair<std::string, std::string>> approved;
  for (const auto &item : policy.snapshot().experiences())
    entries[item.memory_id()] = &item;
  for (const auto &ref : policy.options().approved_experiences())
    approved.emplace(ref.memory_id(), ref.content_revision());
  for (const auto &relation : policy.snapshot().cross_relations()) {
    const auto own = entries.find(relation.private_memory_id()),
               shared = entries.find(relation.shared_memory_id());
    if (relation.relation() != "duplicate" || own == entries.end() || shared == entries.end() ||
        own->second->application_shared() || !shared->second->application_shared() ||
        !own->second->control_target_type().empty() ||
        !shared->second->control_target_type().empty() ||
        own->second->target_scope() != shared->second->target_scope() ||
        own->second->content_revision() != relation.private_content_revision() ||
        !approved.count({own->first, own->second->content_revision()}) ||
        !approved.count({shared->first, shared->second->content_revision()}))
      continue;
    v1::PublishedExperience published;
    published.set_memory_id(shared->first);
    published.set_target_scope(shared->second->target_scope());
    published.set_trigger_pattern(shared->second->trigger_pattern());
    published.set_content(shared->second->content());
    if (shared_content_revision(published) == relation.shared_content_revision())
      result.emplace(std::make_pair(shared->first, shared->second->content_revision()), own->first);
  }
  return result;
}

// 选择当前节点相关的经验来源，避免每次反思发送全库内容。
Json source_experiences(const Json &artifact) {
  auto result = all_source_experiences(artifact);
  const auto aliases = canonical_experience_aliases(proto<v1::RuntimePolicy>(artifact["policy"]));
  std::set<std::string> shadowed_shared;
  for (const auto &alias : aliases)
    shadowed_shared.insert(alias.first.first);
  Json filtered = Json::Array{};
  for (const auto &item : result.items())
    if (!shadowed_shared.count(item.str("memory_id")))
      filtered.push(item);
  return filtered;
}

// 仅含内容的组件保持旧搜索所使用的线缆格式，确保历史树能够继续回放。
Json experience_component(const Json &source) {
  if (!source.has("trigger_pattern") && !source.has("observed_operations"))
    return source["content"];
  Json result{{"content", source["content"]}};
  for (const auto *field : {"trigger_pattern", "observed_operations"})
    if (source.has(field))
      result[field] = source[field];
  return result;
}

// 提取经验的可注入内容与适用条件，剥离仅用于管理的字段。
std::string experience_content(const Json &value) {
  return value.null() ? std::string{} : value.object() ? value.str("content") : value.text();
}

// 从宿主声明和真实执行证据整理可用目标，避免生成不存在的技能名称。
Json observed_target_options(const std::string &scope, const std::string &operation,
                             const Json &arguments) {
  Json observed{{"action_rules", Json::Array{Json{{"rule_id", "$operation-budget"},
                                                  {"target_scope", scope},
                                                  {"operation", operation},
                                                  {"max_attempts", 16}}}}};
  if (arguments.object())
    for (const auto &argument : arguments.fields()) {
      if (observed["action_rules"].items().size() >= 32)
        break;
      const auto &value = argument.second;
      if (!identifier(argument.first) || value.null() || value.object() || value.array() ||
          (value.string() && (value.text().empty() || value.text().size() > 1024)))
        continue;
      observed["action_rules"].push(Json{{"rule_id", argument.first},
                                         {"target_scope", scope},
                                         {"operation", operation},
                                         {"failure_key_arguments", Json::Array{argument.first}}});
    }
  return observed;
}

// 仅从本次真实动作结束回执提取工具动作失败。参数校验可能在工具执行前
// 拒绝动作，因此明确保留 executed=false；模型输入/输出转述的旧错误不计数。
Json observed_tool_failures(const Json &runs) {
  Json result{{"status", runs.array() && !runs.items().empty() ? "scanned" : "unavailable"},
              {"count", 0}, {"executed_count", 0}, {"examples", Json::Array{}}};
  if (!runs.array())
    return result;
  for (const auto &run : runs.items()) {
    if (!run["actions"].array())
      continue;
    for (const auto &action : run["actions"].items()) {
      if (action.str("kind") != "action.end" || action.str("semantic") == "model" ||
          action.str("operation").empty() || action.str("operation") == "model_call" ||
          !action.flag("observed_failure", false))
        continue;
      auto code = action["error"].str("code");
      if (code.empty())
        code = action.str("terminal_error_code_candidate");
      if (code.empty())
        continue;
      result["count"] = result.num("count", 0) + 1;
      result["executed_count"] =
          result.num("executed_count", 0) + action.flag("executed", false);
      if (result["examples"].items().size() >= 4)
        continue;
      Json example{{"run_id", clip(run.str("run_id"), 80)},
                   {"event_id", clip(action.str("event_id"), 48)},
                   {"operation", clip(action.str("operation"), 64)},
                   {"error_code", clip(code, 64)},
                   {"executed", action.flag("executed", false)}};
      if (action.str("operation") == "run_skills") {
        const auto &input = action["input"];
        example["skills_argument"] =
            !input.object() || !input.has("skills") ? "missing"
            : input["skills"].array() && input["skills"].items().empty() ? "empty_array"
                                                                           : "other";
      }
      result["examples"].push(std::move(example));
    }
  }
  result["omitted"] = result.num("count", 0) - result["examples"].items().size();
  return result;
}

// 反思与选优共用已验证的评分结果。缺测保留缺测，不能把原始循环轮数或模型完成分
// 当作正式无效循环或有效质量；覆盖标记明确说明哪些数字实际存在。
Json reflection_scored_facts(const Json &score) {
  Json result = Json::Object{};
  for (const auto *field : {"quality", "strict_pass_rate", "invalid_loops", "tokens", "duration_ms"})
    if (score.has(field) && !score[field].null())
      result[field] = score[field];
  if (result.has("strict_pass_rate"))
    result["strict_passed"] = result.num("strict_pass_rate", 0) == 1;
  if (result.has("invalid_loops"))
    result["invalid_loop_count"] = result["invalid_loops"];
  result["metric_coverage"] = Json{{"effective_quality", result.has("quality")},
                                     {"strict_passed", result.has("strict_passed")},
                                     {"invalid_loop_count", result.has("invalid_loop_count")}};
  return result;
}

void bind_reflection_scored_facts(Json &trial, const Json &facts) {
  for (const auto &field : facts.fields())
    if (field.first != "tokens" && field.first != "duration_ms")
      trial["metrics"][field.first] = field.second;
  if (trial["goal_completion"].object()) {
    if (facts.has("quality"))
      trial["goal_completion"]["effective_quality"] = facts["quality"];
    if (facts.has("strict_passed"))
      trial["goal_completion"]["strict_passed"] = facts["strict_passed"];
  }
}

void collect_source_memory_ids(const Json &value, std::set<std::string> &ids) {
  if (value.object()) {
    for (const auto &field : value.fields()) {
      if (field.first == "memory_id" && field.second.string())
        ids.insert(field.second.text());
      collect_source_memory_ids(field.second, ids);
    }
  } else if (value.array()) {
    for (const auto &item : value.items())
      collect_source_memory_ids(item, ids);
  }
}

Json compact_unassociated_source_bodies(Json input, std::set<std::string> associated) {
  collect_source_memory_ids(input["components"], associated);
  collect_source_memory_ids(input["controls"], associated);
  for (auto &source : input["experiences"].items())
    if (!associated.count(source.str("memory_id"))) {
      source.fields().erase("content");
      source.fields().erase("original_content");
      source.fields().erase("observed_problem");
    }
  return input;
}

// 整理单题的真实执行反馈，保留可归因的失败和策略应用证据。
Json feedback_trial(const dataset::v1::EvaluationTrial &trial, const Json &controls) {
  // 只转换需要的检查项，避免为取得 checks 再序列化一遍整条长轨迹。
  const auto evidence = code_projection_view(json(trial.evidence()));
  Json checks = trial.checks_size() ? Json(Json::Array{}) : Json();
  for (const auto &check : trial.checks())
    checks.push(json(check));
  Json result{{"verdict", trial.verdict()},
              {"checks", excerpt(checks)},
              {"metrics", json(trial.metrics())},
              {"execution_status", trial.execution_status()}};
  // 反思需要知道究竟哪些目标未完成；只传短诊断，不把完整逐批裁判输入再次回放。
  if (evidence["assessment"].object()) {
    auto &summary = result["goal_completion"];
    for (const auto *field : {"quality", "quality_upper", "coverage", "critical_failures"})
      summary[field] = evidence["assessment"][field];
    summary["issues"] = Json::Array{};
    std::size_t omitted = 0;
    for (const auto &goal : evidence["assessment"]["goals"].items()) {
      if ((goal.str("status") == "passed" || goal.str("status") == "not_applicable") &&
          !goal.flag("critical_failure", false))
        continue;
      if (summary["issues"].items().size() == 8) {
        ++omitted;
        continue;
      }
      Json row;
      for (const auto *field : {"id", "turn", "due_turn", "status", "critical_failure"})
        if (goal.has(field))
          row[field] = goal[field];
      row["reason"] = diagnostic_excerpt(goal.str("reason"), 512);
      summary["issues"].push(row);
    }
    summary["omitted_issues"] = omitted;
  }
  auto &view = result["evidence"];
  for (const auto *field : {"output", "state", "artifacts", "loop_review"})
    if (evidence.has(field))
      view[field] = excerpt(evidence[field]);
  const auto traces = evidence.has("traces")  ? evidence["traces"]
                      : evidence.has("trace") ? Json(Json::Array{evidence["trace"]})
                                              : Json(Json::Array{});
  const auto failure_code = [](const Json &event) {
    auto code = event["attributes"].str("observed_error_code");
    if (code.empty())
      code = structured_error_code(event["output"]);
    if (code.empty() && event.has("error"))
      code = structured_error_code(event["error"]);
    return code;
  };
  for (const auto &trace : traces.items()) {
    Json run{{"run_id", trace["identity"]["run_id"]}, {"actions", Json::Array{}}};
    std::map<std::string, Json> starts, step_starts, action_controls, loop_controls,
        experience_controls;
    Json target_ledger = Json::Object{}, target_attempts = Json::Object{};
    // 错误码只配置本地探针，不提前写入失败账本。机会仍须按事件顺序，等失败
    // 落定后再看到同执行、同操作、完整参数相同的真实查询；首次失败不是机会。
    Json terminal_codes = Json::Array{};
    std::set<std::string> seen_codes;
    for (const auto &event : trace["events"].items()) {
      if (event.str("kind") != "action.end" ||
          !event["attributes"].flag("executed", false) ||
          !event["attributes"].flag("observed_failure", event.str("status") == "failed") ||
          event["error"].str("retryability") == "retryable")
        continue;
      const auto code = failure_code(event);
      if (!code.empty() && seen_codes.insert(code).second)
        terminal_codes.push(code);
    }
    // 探针独立使用同一账本算法，不挤占原有预算/跨执行目标账本的容量。
    Json terminal_ledger = Json::Object{}, terminal_attempts = Json::Object{};
    const auto observe_terminal = [&](const std::string &id, const std::string &scope,
                                      const std::string &operation, const Json &arguments,
                                      const std::string &execution) {
      Json observed{{"action_rules", Json::Array{}}};
      if (!terminal_codes.items().empty())
        observed["action_rules"].push(Json{{"rule_id", "$terminal-observation"},
                                           {"target_scope", scope}, {"operation", operation},
                                           {"max_failed_attempts", 1},
                                           {"terminal_error_codes", terminal_codes}});
      const auto entries =
          target_failure_snapshot(observed, scope, operation, arguments, terminal_ledger, execution);
      terminal_attempts[id] = Json{{"entries", entries}, {"options", observed}, {"scope", scope},
                                   {"operation", operation}, {"execution_id", execution}};
      return entries;
    };
    std::map<std::string, std::string> terminal_sources;
    for (const auto &event : trace["events"].items()) {
      if (event.str("kind") == "action.start" &&
          event["attributes"].str("type") == "schedule.action" &&
          !event["attributes"].str("target_scope").empty()) {
        const auto scope = event["attributes"].str("target_scope"),
                   operation = event.str("operation");
        const auto observed = observed_target_options(scope, operation, event["input"]);
        target_attempts[event.str("action_id")] =
            Json{{"options", observed},
                 {"scope", scope},
                 {"operation", operation},
                 {"execution_id", event["action_id"]},
                 {"entries", target_failure_snapshot(observed, scope, operation, event["input"],
                                                     target_ledger, event.str("action_id"))}};
        observe_terminal(event.str("action_id"), scope, operation, event["input"],
                         event.str("action_id"));
      }
      if (event.str("kind") == "action.end") {
        const auto &frame = static_cast<const Json &>(target_attempts)[event.str("action_id")];
        settle_target_event(target_ledger, frame, frame["options"], event);
        const auto &terminal_frame =
            static_cast<const Json &>(terminal_attempts)[event.str("action_id")];
        const bool retryable_failure =
            event["attributes"].flag("observed_failure", event.str("status") == "failed") &&
            event["error"].str("retryability") == "retryable";
        // 显式可重试错误不是终态事实，不能在成功清零后重新抬高专用终态账本，
        // 否则下一次查询会错误复活旧失败 source；成功事件仍须正常结算并清零。
        if (!retryable_failure)
          settle_target_event(terminal_ledger, terminal_frame, terminal_frame["options"], event);
        const auto key = terminal_frame["entries"]["$terminal-observation"].str("key");
        if (!key.empty() && event["attributes"].flag("executed", false) &&
            !event["attributes"].flag("delegates_schedule", false) &&
            (event.str("status") == "completed" || event.str("status") == "failed") &&
            event["attributes"].flag("observed_failure", event.str("status") == "failed") &&
            event["error"].str("retryability") != "retryable" &&
            seen_codes.count(failure_code(event)) &&
            terminal_ledger["entries"][key].num("count", 0))
          terminal_sources[key] = event.str("action_id");
      }
      if (event.str("kind") == "action.start")
        starts[event.str("action_id")] = event;
      if (event.str("kind") == "step.start")
        step_starts[event.str("step_id")] = event;
      if (event.str("kind") == "advice.applied" &&
          event["attributes"]["receipt"].str("boundary") == "llm")
        experience_controls[event.str("step_id")]["application_status"] =
            event["application"]["status"];
      const auto &decision = event["attributes"]["decision"];
      const auto &query = decision["query"];
      if (event["attributes"].str("type") != "policy.query")
        continue;
      if (query.str("boundary") == "llm" && !event.str("step_id").empty()) {
        Json advice{{"target_scope", query["advice_query"]["target_scope"]},
                    {"memory_ids", Json::Array{}}};
        if (decision["experiences"].array())
          for (const auto &source : decision["experiences"].items())
            advice["memory_ids"].push(source["memory_id"]);
        if (query.has("observed_operations"))
          advice["observed_operations"] = query["observed_operations"];
        experience_controls[event.str("step_id")] = advice;
      }
      if (query.str("boundary") == "schedule" && query["actions"].array()) {
        auto schedule_query = proto<v1::RuntimeQuery>(query);
        const auto start = step_starts.find(event.str("step_id"));
        if (start != step_starts.end())
          project_schedule_evidence(schedule_query, start->second["input"]);
        Json plan{{"query_event_id", event["event_id"]},
                  {"target_scope", query["advice_query"]["target_scope"]},
                  {"actions", Json::Array{}},
                  {"effective_plan", decision["schedule"]}};
        // 文本条件匹配 Run 输入，不是某个技能的派发 prompt；给出真实匹配文本。
        plan["task_text"] = clip(query["advice_query"].str("task"), 2048);
        const auto schedule_input = json(schedule_query);
        for (const auto &action : schedule_input["actions"].items()) {
          Json item{{"action_id", action["action_id"]}, {"operation", action["operation"]}};
          if (action.has("after"))
            item["after"] = action["after"];
          if (action.has("arguments"))
            item["arguments"] = action["arguments"];
          plan["actions"].push(item);
        }
        run["schedule_plans"].push(plan);
      }
      if (query.str("boundary") == "loop" && !event.str("step_id").empty()) {
        const auto &progress = decision["progress"];
        const auto &previous = query["previous"];
        const auto fingerprint = query.str("state_fingerprint");
        const bool counted = !query.flag("pending_actions", false) && !fingerprint.empty() &&
                             progress.str("fingerprint") == fingerprint &&
                             progress["iteration"].dump() == query["iteration"].dump();
        // 在重规划重置计数器前暴露运行时计数。模型评审的浪费是另一类信号；
        // 缺失或关闭的计数器必须保持未知，不能补成零。
        Json control{{"query_event_id", event["event_id"]},
                     {"target_scope", query["advice_query"]["target_scope"]},
                     {"observed_no_progress",
                      counted ? Json(std::max({query.num("repeated_action_steps", 0),
                                               query.num("repeated_failure_steps", 0),
                                               previous.str("fingerprint") == fingerprint
                                                   ? previous.num("no_progress", 0) + 1
                                                   : std::size_t(0)}))
                              : Json()},
                     {"pending_actions", query.flag("pending_actions", false)},
                     {"proposed_decision", progress["decision"]}};
        control["failed_actions"] = query.num("failed_actions", 0);
        control["repeated_failure_steps"] = query.num("repeated_failure_steps", 0);
        if (query.has("error_codes"))
          control["error_codes"] = query["error_codes"];
        if (query.has("rejected_actions"))
          control["rejected_actions"] = query["rejected_actions"];
        if (query.has("can_finalize"))
          control["can_finalize"] = query["can_finalize"];
        // Protobuf JSON 会将 uint64 的 iteration 表示为十进制字符串。
        control["iteration"] = static_cast<std::size_t>(proto<v1::RuntimeQuery>(query).iteration());
        if (progress.has("rule_id"))
          control["rule_id"] = progress["rule_id"];
        loop_controls[event.str("step_id")] = std::move(control);
      }
      if (query.str("boundary") != "actions" || !query["actions"].array())
        continue;
      for (const auto &planned : query["actions"].items()) {
        if (planned.str("action_id").empty())
          continue;
        Json control{{"query_event_id", event["event_id"]},
                     {"target_scope", query["advice_query"]["target_scope"]}};
        if (planned.has("schema_required_arguments"))
          control["schema_required_arguments"] = planned["schema_required_arguments"];
        // 必须在展示裁剪前判断参数是否存在，避免真实参数被截断后误判为缺失。
        const auto &configured = controls["actions"][planned.str("operation")];
        const auto &required =
            configured.array() ? configured : planned["schema_required_arguments"];
        control["missing_required_arguments"] = Json::Array{};
        if (required.array())
          for (const auto &name : required.items()) {
            const auto &value = planned["arguments"][name.text()];
            if (value.null() || (value.string() && value.text().empty()) ||
                (value.array() && value.items().empty()) ||
                (value.object() && value.fields().empty()))
              control["missing_required_arguments"].push(name);
          }
        if (planned.has("prior_failures"))
          control["prior_failures"] = planned["prior_failures"];
        const auto observed =
            observed_target_options(query["advice_query"].str("target_scope"),
                                    planned.str("operation"), planned["arguments"]);
        const auto prior = target_failure_snapshot(
            observed, query["advice_query"].str("target_scope"), planned.str("operation"),
            planned["arguments"], target_ledger, query.str("execution_id"));
        target_attempts[planned.str("action_id")] =
            Json{{"entries", prior},
                 {"options", observed},
                 {"scope", query["advice_query"]["target_scope"]},
                 {"operation", planned["operation"]},
                 {"execution_id", query["execution_id"]}};
        control["prior_operation_rounds"] = prior["$operation-budget"].num("count", 0);
        for (const auto &attempt : prior.fields())
          if (attempt.first != "$operation-budget" && attempt.second.num("count", 0))
            control["failure_key_candidates"].push(
                Json{{"argument", attempt.first},
                     {"prior_failures", attempt.second["count"]},
                     {"cross_execution",
                      !attempt.second.str("execution_id").empty() &&
                          attempt.second.str("execution_id") != query.str("execution_id")}});
        if (planned.has("target_failures"))
          control["target_failures"] = planned["target_failures"];
        if (decision["actions"].array())
          for (const auto &outcome : decision["actions"].items())
            if (outcome.str("action_id") == planned.str("action_id"))
              control["decision"] = outcome["decision"];
        const auto terminal_snapshot = observe_terminal(
            planned.str("action_id"), query["advice_query"].str("target_scope"),
            planned.str("operation"), planned["arguments"], query.str("execution_id"));
        const auto &terminal = terminal_snapshot["$terminal-observation"];
        const auto source = terminal_sources.find(terminal.str("key"));
        if (control.str("decision") == "allow" && terminal.num("count", 0) &&
            source != terminal_sources.end() && action_controls.count(source->second))
          action_controls[source->second]["terminal_retry_query_event_id"] = event["event_id"];
        action_controls[planned.str("action_id")] = std::move(control);
      }
    }
    for (const auto &event : trace["events"].items()) {
      const auto kind = event.str("kind");
      if (kind != "action.end" && kind != "run.start" && kind != "run.end" &&
          kind != "loop.decision")
        continue;
      const auto found = starts.find(event.str("action_id"));
      const auto start = found == starts.end() ? Json(Json::Object{}) : found->second;
      Json action = Json::Object{};
      for (const auto *field : {"event_id", "kind", "actor_id", "execution_id", "loop_id",
                                "iteration", "status", "error", "decision", "reason", "outcome"})
        if (event.has(field))
          action[field] = excerpt(event[field]);
      const auto operation = event.str("operation", start.str("operation"));
      if (!operation.empty())
        action["operation"] = operation;
      if (event["attributes"].has("executed"))
        action["executed"] = event["attributes"]["executed"];
      const bool model = event["attributes"].str("semantic") == "model" ||
                         start["attributes"].str("semantic") == "model";
      if (kind == "action.end") {
        action["semantic"] =
            event["attributes"].str("semantic", start["attributes"].str("semantic"));
        // 失败事实必须来自结构化执行回执。completed/not_found 等成功空结果保持
        // false，不能再由输出文本或后续反思猜成失败。
        action["observed_failure"] =
            event["attributes"].flag("observed_failure", event.str("status") == "failed");
        const auto step = step_starts.find(event.str("step_id"));
        if (start["attributes"]["extensions"].flag("guard_only", false) ||
            (step != step_starts.end() &&
             step->second["attributes"]["extensions"].flag("guard_only", false)))
          action["guard_only"] = true;
        const auto control = action_controls.find(event.str("action_id"));
        action["action_control"] = control == action_controls.end() ? Json() : control->second;
        if (!model &&
            event["attributes"].flag("observed_failure", event.str("status") == "failed")) {
          // 旧轨迹可能只在 ErrorInfo 保存失败码；补齐显式证据，不凭结果文本猜终态。
          // 已有观测码和结果结构码仍优先，避免反思生成与运行时语义相反的规则。
          const auto code = failure_code(event);
          if (!code.empty())
            action["terminal_error_code_candidate"] = code;
        }
        if (model) {
          const auto advice = experience_controls.find(event.str("step_id"));
          if (advice != experience_controls.end())
            action["experience_query"] = advice->second;
        }
      }
      if (kind == "loop.decision") {
        const auto control = loop_controls.find(event.str("step_id"));
        action["loop_control"] = control == loop_controls.end() ? Json() : control->second;
      }
      // 反思只使用任务/评分标准、真实决策和工具结果。重放的模型消息历史及供应商
      // 封装不会产生新动作，因此不应占用反思上下文。
      if (!model) {
        if (event.has("input"))
          action["input"] = excerpt(event["input"]);
        else if (start.has("input"))
          action["input"] = excerpt(start["input"]);
      }
      if (event.has("output")) {
        const auto &output = event["output"];
        action["output"] = excerpt(output);
        if (model && output["choices"].array()) {
          action["output"] = Json::Array{};
          for (const auto &choice : output["choices"].items()) {
            Json message = Json::Object{};
            for (const auto *field : {"content", "tool_calls", "function_call", "refusal"})
              if (choice["message"].has(field))
                message[field] = excerpt(choice["message"][field]);
            if (choice.has("finish_reason"))
              message["finish_reason"] = choice["finish_reason"];
            action["output"].push(message);
          }
        }
      }
      run["actions"].push(action);
    }
    view["runs"].push(run);
  }
  result["observed_tool_failures"] = observed_tool_failures(view["runs"]);
  return result;
}

// 归因只认已应用回执及其固定版本，不把查询、相邻失败或公共库近似文本当成使用证明。
Json selected_experience_applications(const dataset::v1::EvaluationTrial &trial,
                                      const Json &selected, const Json &artifact) {
  Json result{{"status", "not_observed"}, {"count", 0}, {"points", Json::Array{}}};
  const auto policy = proto<v1::RuntimePolicy>(artifact["policy"]);
  const auto aliases = canonical_experience_aliases(policy);
  std::map<std::string, std::string> revisions;
  for (const auto &source : policy.snapshot().experiences())
    for (const auto &approved : policy.options().approved_experiences())
      if (source.memory_id() == approved.memory_id() &&
          source.content_revision() == approved.content_revision())
        revisions[source.memory_id()] = source.content_revision();
  const auto evidence = json(trial.evidence());
  const auto traces =
      evidence["traces"].array() ? evidence["traces"] : Json(Json::Array{evidence["trace"]});
  std::size_t count = 0;
  for (const auto &trace : traces.items()) {
    // 单题采集/模型失败可能没有 events；保留未观测状态，不让该题阻断整轮反思。
    if (!trace["events"].array())
      continue;
    // 同一条经验在下一次真实执行中是否仍能命中，取决于查询当时真正可见的任务、
    // 状态和已执行操作。只给反思模型一个 receipt_event_id，会迫使它凭自然语言猜测
    // trigger_pattern，生成“多次返回”“已经完成”等运行时无法直接观测的条件。
    // 这里按 policy/query/decision 三元组关联原查询，并只保留有界摘录；这些字段本来
    // 就属于本轮已授权训练轨迹，不引入验证集、留出集或新的宿主依赖。
    std::map<std::string, Json> queries;
    const auto query_key = [](const Json &value) {
      return value.str("policy_id") + '\n' + value.str("query_id") + '\n' +
             value.str("decision_id");
    };
    if (trace["events"].array())
      for (const auto &event : trace["events"].items()) {
        if (event["attributes"].str("type") != "policy.query")
          continue;
        const auto &decision = event["attributes"]["decision"];
        const auto &query = decision["query"];
        if (query.str("boundary") != "llm" || !query["advice_query"].object())
          continue;
        auto identity = query["advice_query"];
        identity["policy_id"] = decision["policy_id"];
        Json view{{"target_scope", query["advice_query"]["target_scope"]},
                  {"task_excerpt", clip(query["advice_query"].str("task"), 512)},
                  {"state_excerpt", clip(query["advice_query"].str("state"), 1024)}};
        if (query["observed_operations"].array())
          view["observed_operations"] = query["observed_operations"];
        queries[query_key(identity)] = std::move(view);
      }
    for (const auto &event : trace["events"].items()) {
      const auto &receipt = event["attributes"]["receipt"];
      if (event.str("kind") != "advice.applied" ||
          event["application"].str("status") != "applied" || receipt.str("boundary") != "llm" ||
          !receipt["experiences"].array() || trace["identity"].str("run_id").empty() ||
          event.str("event_id").empty())
        continue;
      for (const auto &source : receipt["experiences"].items()) {
        const auto actual = source.str("memory_id"), revision = source.str("content_revision");
        const auto known = revisions.find(actual);
        if (known == revisions.end() || known->second != revision)
          continue;
        const auto alias = aliases.find({actual, revision});
        const auto canonical = alias == aliases.end() ? actual : alias->second;
        if (std::none_of(selected.items().begin(), selected.items().end(),
                         [&](const auto &component) {
                           return component.text() == "experience:" + canonical;
                         }))
          continue;
        ++count;
        if (result["points"].items().size() == 8)
          continue;
        Json point{{"run_id", trace["identity"]["run_id"]},
                   {"receipt_event_id", event["event_id"]},
                   {"memory_id", canonical},
                   {"content_revision", revisions.at(canonical)}};
        if (canonical != actual) {
          point["receipt_memory_id"] = actual;
          point["receipt_content_revision"] = revision;
        }
        for (const auto *field : {"actor_id", "execution_id"})
          if (event.has(field))
            point[field] = event[field];
        const auto query = queries.find(query_key(event["application"]));
        if (query != queries.end())
          for (const auto &field : query->second.fields())
            point[field.first] = field.second;
        result["points"].push(point);
      }
    }
  }
  result["count"] = count;
  if (count)
    result["status"] = "observed";
  if (count > result["points"].items().size())
    result["truncated"] = true;
  return result;
}

// 完整 trial 超出反思预算时，仍保留带回执的 L1 证据；相邻动作只提供上下文，
// 不能被错误地当作因果归因。
Json compact_experience_feedback_trial(const dataset::v1::EvaluationTrial &trial, const Json &full,
                                       const Json &selected, std::size_t byte_limit,
                                       const Json &artifact = Json()) {
  Json summary{{"verdict", full["verdict"]},
               {"checks", compact_excerpt(full["checks"])},
               {"output", compact_excerpt(json(trial.evidence())["output"])},
               {"metrics", full["metrics"]},
               {"observed_tool_failures", full["observed_tool_failures"]},
               {"evidence_truncated", true}};
  const auto evidence = code_projection_view(json(trial.evidence()));
  const auto traces = evidence.has("traces")  ? evidence["traces"]
                      : evidence.has("trace") ? Json(Json::Array{evidence["trace"]})
                                              : Json(Json::Array{});
  if (!full["evidence"]["runs"].array())
    return summary;
  std::map<std::pair<std::string, std::string>, std::string> canonical;
  if (artifact["policy"].object())
    canonical = canonical_experience_aliases(proto<v1::RuntimePolicy>(artifact["policy"]));
  auto selected_memory = [&](const Json &source) {
    auto id = source.str("memory_id");
    const auto alias = canonical.find({id, source.str("content_revision")});
    if (alias != canonical.end())
      id = alias->second;
    for (const auto &component : selected.items())
      if (component.text() == "experience:" + id)
        return id;
    return std::string{};
  };

  struct Point {
    std::size_t run, action;
    Json query, receipt;
  };

  std::vector<Point> points;
  std::vector<std::map<std::string, Json>> events;
  std::vector<std::map<std::string, std::size_t>> positions;
  std::vector<std::map<std::string, std::size_t>> query_positions;
  const auto &runs = full["evidence"]["runs"].items();
  for (std::size_t r = 0; r < traces.items().size() && r < runs.size(); ++r) {
    std::map<std::string, Json> by_id, queries, receipts;
    positions.emplace_back();
    query_positions.emplace_back();
    for (const auto &event : traces.items()[r]["events"].items()) {
      positions.back()[event.str("event_id")] = positions.back().size();
      by_id[event.str("event_id")] = event;
      if (event["attributes"].str("type") == "policy.query" &&
          event["attributes"]["decision"]["query"].str("boundary") == "llm") {
        queries[event.str("step_id")] = event;
        query_positions.back()[event.str("step_id")] = positions.back().at(event.str("event_id"));
      }
      if (event.str("kind") == "advice.applied" &&
          event["application"].str("status") == "applied" &&
          event["attributes"]["receipt"].str("boundary") == "llm")
        receipts[event.str("step_id")] = event;
    }
    events.push_back(std::move(by_id));
    for (std::size_t a = 0; a < runs[r]["actions"].items().size(); ++a) {
      const auto &action = runs[r]["actions"].items()[a];
      if (action.str("semantic") != "model")
        continue;
      const auto found = events.back().find(action.str("event_id"));
      if (found == events.back().end())
        continue;
      const auto step = found->second.str("step_id");
      const auto q = queries.find(step), receipt = receipts.find(step);
      if (q == queries.end() || receipt == receipts.end())
        continue;
      bool relevant = false;
      const auto &used = receipt->second["attributes"]["receipt"]["experiences"];
      if (used.array())
        for (const auto &source : used.items())
          relevant |= !selected_memory(source).empty();
      if (relevant)
        points.push_back({r, a, q->second, receipt->second});
    }
  }
  if (points.empty())
    return summary;
  Json base{{"verdict", full["verdict"]},
            {"metrics", full["metrics"]},
            {"execution_status", full["execution_status"]},
            {"observed_tool_failures", full["observed_tool_failures"]},
            {"evidence_truncated", true}};
  std::map<std::size_t, std::map<std::size_t, Json>> kept;
  auto render = [&] {
    auto result = base;
    result["evidence"]["runs"] = Json::Array{};
    for (const auto &run : kept) {
      Json view{{"run_id", runs[run.first]["run_id"]}, {"actions", Json::Array{}}};
      for (const auto &action : run.second)
        view["actions"].push(action.second);
      result["evidence"]["runs"].push(view);
    }
    return result;
  };
  auto insert = [&](std::size_t r, std::size_t a, const Json &action) {
    if (kept[r].count(a))
      return true;
    kept[r][a] = action;
    if (render().dump().size() <= byte_limit)
      return true;
    kept[r].erase(a);
    if (kept[r].empty())
      kept.erase(r);
    return false;
  };
  // 对每个 run/source 优先保留最近一次应用，再按时间回溯更早的观测点。
  std::vector<Point> ordered, rest;
  std::set<std::pair<std::size_t, std::string>> represented;
  for (auto it = points.rbegin(); it != points.rend(); ++it) {
    bool first = false;
    for (const auto &source : it->receipt["attributes"]["receipt"]["experiences"].items()) {
      const auto id = selected_memory(source);
      if (!id.empty())
        first |= represented.emplace(it->run, id).second;
    }
    (first ? ordered : rest).push_back(*it);
  }
  ordered.insert(ordered.end(), rest.begin(), rest.end());
  std::vector<Point> retained;
  for (const auto &point : ordered) {
    if (retained.size() == 8)
      break;
    const auto &source = runs[point.run]["actions"].items()[point.action];
    auto action = compact_excerpt(source);
    const auto &query = point.query["attributes"]["decision"]["query"];
    action["experience_query"] = source["experience_query"];
    auto &view = action["experience_query"];
    view["query_event_id"] = point.query["event_id"];
    view["receipt_event_id"] = point.receipt["event_id"];
    view["applied_memory_ids"] = Json::Array{};
    for (const auto &ref : point.receipt["attributes"]["receipt"]["experiences"].items())
      view["applied_memory_ids"].push(ref["memory_id"]);
    view["state_excerpt"] = clip(query["advice_query"].str("state"), 512);
    if (!insert(point.run, point.action, action)) {
      view["state_excerpt"] = clip(query["advice_query"].str("state"), 128);
      action.fields().erase("output");
      if (!insert(point.run, point.action, action))
        continue;
    }
    retained.push_back(point);
  }
  for (const auto &point : retained) {
    const auto &actions = runs[point.run]["actions"].items();
    const auto &model = events[point.run].at(actions[point.action].str("event_id"));
    auto same_branch = [&](const Json &event) {
      if (model.str("actor_id").empty() || model.str("execution_id").empty() ||
          event.str("actor_id") != model.str("actor_id") ||
          event.str("execution_id") != model.str("execution_id"))
        return false;
      const auto &a = model["attributes"]["extensions"]["point_reference"];
      const auto &b = event["attributes"]["extensions"]["point_reference"];
      return a.object() == b.object() &&
             (!a.object() || a.str("parent", "root") == b.str("parent", "root"));
    };
    std::size_t next_query = SIZE_MAX;
    for (std::size_t i = point.action + 1; i < actions.size(); ++i) {
      const auto event = events[point.run].find(actions[i].str("event_id"));
      if (actions[i].str("semantic") != "model" || event == events[point.run].end() ||
          !same_branch(event->second))
        continue;
      const auto q = query_positions[point.run].find(event->second.str("step_id"));
      if (q != query_positions[point.run].end())
        next_query = q->second;
      break;
    }
    for (const int direction : {-1, 1}) {
      std::size_t count = 0;
      for (int i = static_cast<int>(point.action) + direction;
           i >= 0 && i < static_cast<int>(actions.size()); i += direction) {
        const auto &nearby = actions[i];
        const auto event = events[point.run].find(nearby.str("event_id"));
        if (event == events[point.run].end() || !same_branch(event->second))
          continue;
        if (nearby.str("semantic") == "model")
          break;
        if (nearby.str("semantic") != "action")
          continue;
        if (direction > 0 && positions[point.run].at(nearby.str("event_id")) >= next_query)
          break;
        if (direction < 0) {
          const auto &observed =
              point.query["attributes"]["decision"]["query"]["observed_operations"];
          if (positions[point.run].at(nearby.str("event_id")) >=
                  positions[point.run].at(point.query.str("event_id")) ||
              !nearby.flag("executed", false) || !observed.array() ||
              std::none_of(observed.items().begin(), observed.items().end(),
                           [&](const auto &op) { return op.text() == nearby.str("operation"); }))
            continue;
        }
        // 委派 ACK 不一定含业务结果。只沿本次动作的直接因果链补齐批次结果，
        // 不借用时间相邻的兄弟批次，也不把批次结束误当作业务成功。
        const auto &delegate = event->second;
        const auto &parent = delegate["attributes"]["extensions"]["point_reference"];
        if (direction < 0 && delegate["attributes"].flag("delegates_schedule", false) &&
            !parent.str("key").empty() && delegate["caused_by"].array()) {
          std::size_t result_count = 0;
          for (const auto &cause : delegate["caused_by"].items()) {
            const auto batch = events[point.run].find(cause.text());
            if (batch == events[point.run].end())
              continue;
            const auto &end = batch->second;
            const auto &ref = end["attributes"]["extensions"]["point_reference"];
            if (end.str("kind") != "step.end" || end.str("step_id").empty() ||
                end.str("actor_id") != delegate.str("actor_id") ||
                end.str("execution_id") != delegate.str("execution_id") ||
                ref.str("parent") != parent.str("key") || ref.str("run") != parent.str("run") ||
                positions[point.run].at(end.str("event_id")) >=
                    positions[point.run].at(delegate.str("event_id")) ||
                !end["caused_by"].array())
              continue;
            for (const auto &result_id : end["caused_by"].items()) {
              if (result_count == 4)
                break;
              const auto found = events[point.run].find(result_id.text());
              if (found == events[point.run].end())
                continue;
              const auto &result = found->second;
              if (result.str("kind") != "action.end" ||
                  result["attributes"].str("type") != "schedule.action" ||
                  result.str("step_id") != end.str("step_id") ||
                  result.str("actor_id") != end.str("actor_id") ||
                  result.str("execution_id") != end.str("execution_id") ||
                  !result["attributes"].flag("executed", false) ||
                  (result.str("status") != "completed" && result.str("status") != "failed") ||
                  positions[point.run].at(result.str("event_id")) >=
                      positions[point.run].at(end.str("event_id")))
                continue;
              const auto row =
                  std::find_if(actions.begin(), actions.end(), [&](const auto &action) {
                    return action.str("event_id") == result.str("event_id");
                  });
              if (row == actions.end())
                continue;
              auto linked = compact_excerpt(*row);
              linked["dispatch_result_of"] = delegate["event_id"];
              linked["schedule_end_event_id"] = end["event_id"];
              if (insert(point.run, static_cast<std::size_t>(row - actions.begin()), linked))
                ++result_count;
            }
          }
        }
        auto context = compact_excerpt(nearby);
        context["temporal_context_only"] = true;
        if (!insert(point.run, static_cast<std::size_t>(i), context) || ++count == 2)
          break;
      }
    }
  }
  if (kept.empty())
    return summary;
  auto result = render();
  for (const auto *field : {"checks", "output"}) {
    auto candidate = result;
    candidate[field] = summary[field];
    if (candidate.dump().size() <= byte_limit)
      result = std::move(candidate);
  }
  return result;
}

// 关联动作边界的规则命中、宿主应用与实际结果，区分查询和执行。
bool action_control_evidence(const Json &action) {
  const auto &control = action["action_control"];
  if (action.str("kind") != "action.end" || !control.object() || control.str("decision") != "allow")
    return false;
  if (control.num("prior_failures", 0) >= 2 &&
      (action.flag("guard_only", false) || action.flag("executed", true)))
    return true;
  // 普通重复工具调用可能分别完成不同工作，不能凭次数占满机会窗口。
  // 总尝试预算只优先考虑重复派发检查点；具体循环证据仍保留给 L3。
  if (control.num("prior_operation_rounds", 0) > 0 && action.flag("guard_only", false))
    return true;
  if (action.has("terminal_error_code_candidate") && action.flag("executed", false) &&
      !control.str("terminal_retry_query_event_id").empty())
    return true;
  if (action.flag("executed", true) && control["failure_key_candidates"].array())
    for (const auto &target : control["failure_key_candidates"].items())
      if (target.flag("cross_execution", false))
        return true;
  return action.flag("executed", true) && control["missing_required_arguments"].array() &&
         !control["missing_required_arguments"].items().empty();
}

// 根据运行和执行实例生成循环轮次身份，不能只用轮号关联并行任务。
std::string iteration_identity(const Json &run, const Json &action) {
  if (run.str("run_id").empty() || action.str("execution_id").empty() ||
      action.str("loop_id").empty() || !action.has("iteration"))
    return {};
  // 与 task_evaluation.cpp::loop_inventory 对齐：Protobuf 规范化后，数值形式和
  // 十进制字符串形式的 uint64 都表示同一次迭代。
  const auto event = proto<v1::TraceEvent>(Json{{"iteration", action["iteration"]}});
  if (!event.iteration())
    return {};
  const auto iteration = json(event)["iteration"];
  return hash(Json(Json::Array{run["run_id"], action["execution_id"], action["loop_id"], iteration})
                  .dump());
}

// 只从完整原始轨迹确认末轮是一次纯文本交付；工具工作、缺失结束和未知格式不作推断。
bool answer_only_iteration(const Json &raw, const std::string &run_id, const std::string &execution,
                           const std::string &loop, std::size_t iteration) {
  const auto traces = raw["traces"].array() ? raw["traces"] : Json(Json::Array{raw["trace"]});
  for (const auto &trace : traces.items()) {
    if (trace["identity"].str("run_id") != run_id || !trace["events"].array())
      continue;
    std::set<std::string> starts, ends;
    bool completed = false;
    for (const auto &event : trace["events"].items()) {
      if (event.str("kind") == "run.end")
        completed = event["outcome"].str("execution_status", event.str("status")) == "completed";
      if (event.str("execution_id") != execution || event.str("loop_id") != loop ||
          event["iteration"].null())
        continue;
      if (proto<v1::TraceEvent>(Json{{"iteration", event["iteration"]}}).iteration() != iteration)
        continue;
      const auto kind = event.str("kind");
      const auto &attributes = event["attributes"];
      if (kind == "step.start" && attributes.str("type") != "boundary.control" &&
          attributes.str("semantic") != "model")
        return false;
      if (kind != "action.start" && kind != "action.end")
        continue;
      if (attributes.str("semantic") != "model" || event.str("action_id").empty())
        return false;
      if (kind == "action.start") {
        if (!starts.insert(event.str("action_id")).second)
          return false;
        continue;
      }
      if (event.str("status") != "completed" || !attributes.flag("executed", false) ||
          !ends.insert(event.str("action_id")).second)
        return false;
      const auto &choices = event["output"]["choices"];
      if (!choices.array() || choices.items().size() != 1)
        return false;
      const auto &choice = choices.items().front();
      const auto &message = choice["message"];
      if (choice.str("finish_reason") != "stop" || !message["content"].string() ||
          message.str("content").empty() || !message["function_call"].null() ||
          (!message["tool_calls"].null() &&
           (!message["tool_calls"].array() || !message["tool_calls"].items().empty())))
        return false;
    }
    return completed && starts.size() == 1 && starts == ends;
  }
  return false;
}

// 在截断前将完整的语义迭代评审绑定到精简结构行。被评审的后缀只描述这次训练观测；
// 后续评测仍须判断由此得到的 actor 级上限能否泛化。
void bind_iteration_assessments(Json &trial, const Json &raw_evidence) {
  auto &evidence = trial["evidence"];
  // feedback_trial 有意只摘录展示字段；授权必须改用评测器验证过的完整原始评审。
  const auto &review = raw_evidence["loop_review"];
  if (review.str("status") != "complete" || !review["iteration_reviews"].array() ||
      review["observed_iterations"].null() || !evidence["runs"].array())
    return;
  std::map<std::string, Json> assessments;
  for (const auto &item : review["iteration_reviews"].items()) {
    if (!item.object())
      return;
    const auto id = item.str("iteration_id"), decision = item.str("decision"),
               reason = item.str("reason");
    if (id.empty() || (decision != "keep" && decision != "waste") || reason.empty() ||
        !assessments
             .emplace(
                 id,
                 Json{{"iteration_id", id}, {"decision", decision}, {"reason", clip(reason, 512)}})
             .second)
      return;
  }
  if (assessments.size() != static_cast<std::size_t>(review.num("observed_iterations", 0)))
    return;

  struct Bound {
    Json *action;
    std::string id;
    std::string group;
    std::size_t iteration;
    bool loop;
  };

  std::vector<Bound> bound;
  std::set<std::string> matched;
  std::map<std::string, std::map<std::size_t, std::string>> decisions;
  std::map<std::string, std::vector<std::size_t>> loops;
  for (auto &run : evidence["runs"].items())
    for (auto &action : run["actions"].items()) {
      const auto id = iteration_identity(run, action);
      if (id.empty())
        continue;
      const auto found = assessments.find(id);
      const bool loop = action.str("kind") == "loop.decision" && action["loop_control"].object();
      const auto iteration = static_cast<std::size_t>(
          proto<v1::TraceEvent>(Json{{"iteration", action["iteration"]}}).iteration());
      bound.push_back(Bound{&action, id,
                            run.str("run_id") + "\n" + action.str("execution_id") + "\n" +
                                action.str("loop_id"),
                            iteration, loop});
      const auto current_index = bound.size() - 1;
      auto &current = bound[current_index];
      if (found != assessments.end()) {
        matched.insert(id);
        if (iteration) {
          const auto inserted =
              decisions[current.group].emplace(iteration, found->second.str("decision"));
          if (!inserted.second && inserted.first->second != found->second.str("decision"))
            return;
        }
      }
      if (loop && iteration)
        loops[current.group].push_back(current_index);
    }
  // 过期、截断或采用不同规范化方式的评审，不能授权上限，也不能意外标注到另一执行上。
  if (matched.size() != assessments.size())
    return;

  std::map<std::string, std::size_t> waste_after;
  std::map<std::string, std::size_t> final_answers;
  for (const auto &group : decisions) {
    if (group.second.empty())
      continue;
    auto first = group.second.rbegin();
    if (first->second == "keep" && group.second.size() > 1) {
      const auto indices = loops.find(group.first);
      if (indices == loops.end() || indices->second.empty())
        continue;
      const auto &sample = *bound[indices->second.front()].action;
      const auto run_end = group.first.find('\n');
      if (!answer_only_iteration(raw_evidence, group.first.substr(0, run_end),
                                 sample.str("execution_id"), sample.str("loop_id"), first->first))
        continue;
      final_answers[group.first] = first->first;
      ++first;
    }
    if (first->second != "waste")
      continue;
    std::size_t first_waste = first->first;
    for (++first; first != group.second.rend() && first->second == "waste"; ++first)
      first_waste = first->first;
    if (!first_waste)
      continue;
    const auto cap = first_waste - 1;
    bool complete_suffix = true;
    for (const auto index : loops[group.first]) {
      const auto &row = bound[index];
      if (row.iteration <= cap)
        continue;
      if (final_answers.count(group.first) && row.iteration == final_answers[group.first])
        continue;
      const auto assessment = assessments.find(row.id);
      complete_suffix &=
          assessment != assessments.end() && assessment->second.str("decision") == "waste";
    }
    if (complete_suffix)
      waste_after[group.first] = cap;
  }
  for (auto &item : bound) {
    const auto assessment = assessments.find(item.id);
    if (assessment != assessments.end())
      (*item.action)["iteration_assessment"] = assessment->second;
    const bool finalizing = final_answers.count(item.group);
    if (item.loop && waste_after.count(item.group) &&
        (!finalizing || (item.iteration < final_answers[item.group] &&
                         (*item.action)["loop_control"].flag("can_finalize", false)))) {
      (*item.action)["loop_control"]["reviewed_waste_after"] = waste_after[item.group];
      if (finalizing)
        (*item.action)["loop_control"]["require_finalization"] = true;
    }
  }
  // 同名 actor 的另一次执行可能仍有必要工作；在展示裁剪前保留完整反例下界。
  // 有收尾能力时仅豁免已验证的末尾纯答复，不能把工具工作一并省掉。
  std::map<std::pair<std::string, std::string>, std::pair<std::size_t, std::size_t>> required;
  std::map<std::string, std::set<std::pair<std::string, std::string>>> scopes;
  std::set<std::string> finalizable;
  for (const auto &item : bound)
    if (item.loop) {
      const auto &control = (*item.action)["loop_control"];
      const auto key = std::make_pair(control.str("target_scope"), item.action->str("actor_id"));
      if (!key.first.empty() && !key.second.empty())
        scopes[item.group].insert(key);
      if (control.flag("can_finalize", false))
        finalizable.insert(item.group);
    }
  for (auto &item : bound) {
    const auto known_scope = scopes.find(item.group);
    if (known_scope == scopes.end())
      continue;
    const bool answer =
        final_answers.count(item.group) && final_answers[item.group] == item.iteration;
    if (answer) {
      (*item.action)["reviewed_final_answer"] = true;
      if (item.loop)
        (*item.action)["loop_control"]["reviewed_final_answer"] = true;
    }
    const auto assessment = assessments.find(item.id);
    if (assessment == assessments.end() || assessment->second.str("decision") != "keep")
      continue;
    // 某轮缺少 loop 打点时，仍沿同 run/execution/loop 的确切归属保护已执行工作。
    for (const auto &key : known_scope->second) {
      if (!item.action->str("actor_id").empty() && item.action->str("actor_id") != key.second)
        continue;
      auto &floor = required[key];
      floor.first = std::max(floor.first, item.iteration);
      if (finalizable.count(item.group) && !answer)
        floor.second = std::max(floor.second, item.iteration);
    }
  }
  evidence["loop_requirements"] = Json::Array{};
  for (const auto &entry : required)
    for (const bool finish : {false, true})
      evidence["loop_requirements"].push(
          Json{{"target_scope", entry.first.first},
               {"actor_id", entry.first.second},
               {"require_finalization", finish},
               {"required_iteration_floor", finish ? entry.second.second : entry.second.first}});
  for (auto &item : bound)
    if (item.loop) {
      auto &control = (*item.action)["loop_control"];
      const auto found = required.find({control.str("target_scope"), item.action->str("actor_id")});
      if (found != required.end())
        control["required_iteration_floor"] = control.flag("require_finalization", false)
                                                  ? found->second.second
                                                  : found->second.first;
    }
  // keep 评审属于整轮：仅有一个明确逻辑动作时才向该动作归因。
  // 混合动作轮保留未知；按真实 action_id 去重，不按操作名称合并并发分支。
  std::map<std::string, const Json *> raw_actions;
  const auto index_trace = [&](const Json &trace) {
    if (trace["events"].array())
      for (const auto &event : trace["events"].items())
        if (event.str("kind") == "action.end" && !event.str("event_id").empty())
          raw_actions[trace["identity"].str("run_id") + '\n' + event.str("event_id")] = &event;
  };
  if (raw_evidence["traces"].array())
    for (const auto &trace : raw_evidence["traces"].items())
      index_trace(trace);
  else
    index_trace(raw_evidence["trace"]);
  // 一个派发实例有独立的前置检查与完成回执，不能把它们算成两个业务动作。
  // 仅使用组件生成的批次/实例引用关联；不同 run、迭代、作用域或同名兄弟均不合并。
  // 缺少身份或存在歧义时仍保留未知，不从操作名、参数或时间先后猜测对应关系。
  std::map<std::pair<std::string, std::string>, std::vector<const Json *>> scheduled_actions;
  for (const auto &item : bound) {
    const auto raw = raw_actions.find(item.group.substr(0, item.group.find('\n')) + '\n' +
                                      item.action->str("event_id"));
    if (raw == raw_actions.end())
      continue;
    const auto &event = *raw->second;
    const auto &attributes = event["attributes"];
    const auto step = event.str("step_id"), logical = attributes.str("logical_action_id");
    if (attributes.str("type") != "schedule.action" || !attributes.flag("executed", false) ||
        event.str("action_id").empty() || step.rfind("step-", 0) != 0 || step.size() <= 5 ||
        logical.empty() || attributes.str("target_scope").empty())
      continue;
    scheduled_actions[{item.id, step.substr(5) + ":guard:" + logical}].push_back(raw->second);
  }
  std::map<std::string, std::set<std::string>> logical_actions;
  std::set<const Json *> delegated;
  std::set<std::string> independent_delivery;
  std::size_t action_ordinal = 0;
  for (const auto &item : bound) {
    auto &action = *item.action;
    action.fields().erase("reviewed_required_action");
    if (action.str("kind") != "action.end")
      continue;
    const auto raw = raw_actions.find(item.group.substr(0, item.group.find('\n')) + '\n' +
                                      action.str("event_id"));
    if (action.str("semantic") == "model") {
      if (raw != raw_actions.end() && (*raw->second)["output"]["choices"].array())
        for (const auto &choice : (*raw->second)["output"]["choices"].items()) {
          const auto &content = choice["message"]["content"];
          if (choice.str("finish_reason") == "stop" &&
              (!content.string() || !content.text().empty()))
            independent_delivery.insert(item.id);
        }
      continue;
    }
    if (raw != raw_actions.end() &&
        (*raw->second)["attributes"].flag("delegates_schedule", false)) {
      delegated.insert(item.action);
      continue;
    }
    auto action_id = raw == raw_actions.end() ? std::string{} : raw->second->str("action_id");
    if (raw != raw_actions.end() && action.flag("guard_only", false) &&
        !action.flag("executed", true)) {
      const auto reference =
          (*raw->second)["attributes"]["extensions"]["point_reference"].str("key");
      const auto paired = scheduled_actions.find({item.id, reference});
      if (paired != scheduled_actions.end() && paired->second.size() == 1) {
        const auto &settled = *paired->second.front();
        if (settled.str("actor_id") == action.str("actor_id") &&
            settled.str("operation") == action.str("operation") &&
            settled["attributes"].str("target_scope") == action["action_control"].str("target_scope"))
          action_id = settled.str("action_id");
      }
    }
    logical_actions[item.id].insert(action_id.empty() ? "event:" + action.str("event_id") + ":" +
                                                            std::to_string(action_ordinal++)
                                                      : "action:" + action_id);
  }
  for (const auto &item : bound)
    if (item.action->str("kind") == "action.end" && item.action->str("semantic") != "model" &&
        !delegated.count(item.action) && logical_actions[item.id].size() == 1 &&
        !independent_delivery.count(item.id) &&
        (*item.action)["iteration_assessment"].str("decision") == "keep")
      (*item.action)["reviewed_required_action"] = true;
  // 操作预算作用于同 scope/operation；必要的后续调用不能被另一条轨迹的冗余后缀覆盖。
  // 取各自账本计数的最大下界，不把独立 Run 或并发分支的次数相加。
  std::map<std::pair<std::string, std::string>, std::size_t> action_required;
  for (const auto &item : bound) {
    const auto &action = *item.action;
    const auto &control = action["action_control"];
    if (action.str("kind") != "action.end" || action.str("semantic") == "model" ||
        !action.flag("reviewed_required_action", false) || control.str("decision") != "allow" ||
        !control.has("prior_operation_rounds") || control.str("target_scope").empty() ||
        action.str("operation").empty())
      continue;
    auto &floor = action_required[{control.str("target_scope"), action.str("operation")}];
    floor = std::max(floor, control.num("prior_operation_rounds", 0) + 1);
  }
  evidence["action_requirements"] = Json::Array{};
  for (const auto &entry : action_required)
    evidence["action_requirements"].push(Json{{"target_scope", entry.first.first},
                                              {"operation", entry.first.second},
                                              {"required_attempt_floor", entry.second}});
  // 在裁剪前定位更早的冗余派发。时间后缀还须满足计数边界：
  // 并发 keep/waste 可能具有相同 prior，不能用同一预算误伤必要分支。
  for (auto &run : evidence["runs"].items()) {
    std::map<std::pair<std::string, std::string>, std::vector<Json *>> guards;
    for (auto &action : run["actions"].items()) {
      const auto &control = static_cast<const Json &>(action)["action_control"];
      if (action.str("kind") == "action.end" && action.flag("guard_only", false) &&
          !action.flag("executed", true) && control.object() &&
          control.str("decision") == "allow" && !control.str("target_scope").empty() &&
          !action.str("operation").empty())
        guards[{control.str("target_scope"), action.str("operation")}].push_back(&action);
    }
    for (const auto &group : guards)
      for (std::size_t i = 0; i < group.second.size(); ++i) {
        auto &candidate = *group.second[i];
        const auto prior = candidate["action_control"].num("prior_operation_rounds", 0);
        if (!prior || candidate["iteration_assessment"].str("decision") != "waste")
          continue;
        bool safe = true;
        for (std::size_t j = 0; j < group.second.size(); ++j) {
          const auto &other = *group.second[j];
          const auto decision = other["iteration_assessment"].str("decision");
          safe &= (decision == "keep" || decision == "waste") &&
                  other["action_control"].has("prior_operation_rounds") &&
                  (j < i || decision == "waste") &&
                  (decision != "keep" ||
                   other["action_control"].num("prior_operation_rounds", 0) < prior);
        }
        if (safe) {
          candidate["action_control"]["reviewed_budget_prior"] = prior;
          break;
        }
      }
  }
}

// 合并动作相关的执行要求，保留不同来源中的强制约束。
void merge_action_requirements(Json &target, const Json &source) {
  if (!source.array())
    return;
  if (!target.array())
    target = Json::Array{};
  for (const auto &row : source.items()) {
    auto found =
        std::find_if(target.items().begin(), target.items().end(), [&](const auto &previous) {
          return previous.str("target_scope") == row.str("target_scope") &&
                 previous.str("operation") == row.str("operation");
        });
    if (found == target.items().end())
      target.push(row);
    else
      (*found)["required_attempt_floor"] =
          std::max(found->num("required_attempt_floor", 0), row.num("required_attempt_floor", 0));
  }
}

// 计算有依据的最少执行尝试数，避免提前阻止必要的第一次尝试。
std::size_t required_attempt_floor(const Json &input, const Json &binding) {
  std::size_t floor = 1;
  const auto merge = [&](const Json &rows) {
    if (rows.array())
      for (const auto &row : rows.items())
        if (row.str("target_scope") == binding.str("target_scope") &&
            row.str("operation") == binding.str("operation"))
          floor = std::max(floor, row.num("required_attempt_floor", 0));
  };
  merge(input["controls"]["action_requirements"]);
  if (input["feedback"].array())
    for (const auto &row : input["feedback"].items()) {
      const auto &evidence = row["trial"]["evidence"];
      merge(evidence["action_requirements"]);
      if (evidence["runs"].array())
        for (const auto &run : evidence["runs"].items())
          if (run["actions"].array())
            for (const auto &action : run["actions"].items()) {
              const auto &control = action["action_control"];
              if (action.str("kind") == "action.end" && action.str("semantic") != "model" &&
                  action.flag("reviewed_required_action", false) &&
                  control.str("decision") == "allow" && control.has("prior_operation_rounds") &&
                  control.str("target_scope") == binding.str("target_scope") &&
                  action.str("operation") == binding.str("operation"))
                floor = std::max(floor, control.num("prior_operation_rounds", 0) + 1);
            }
    }
  return floor;
}

// 合并循环收敛要求，避免多条经验互相覆盖必要的完成条件。
void merge_loop_requirements(Json &target, const Json &source) {
  if (!source.array())
    return;
  if (!target.array())
    target = Json::Array{};
  for (const auto &row : source.items()) {
    auto found =
        std::find_if(target.items().begin(), target.items().end(), [&](const auto &previous) {
          return previous.str("target_scope") == row.str("target_scope") &&
                 previous.str("actor_id") == row.str("actor_id") &&
                 previous.flag("require_finalization", false) ==
                     row.flag("require_finalization", false);
        });
    if (found == target.items().end())
      target.push(row);
    else
      (*found)["required_iteration_floor"] = std::max(found->num("required_iteration_floor", 0),
                                                      row.num("required_iteration_floor", 0));
  }
}

// 根据任务约束确定最低循环需求，提前停止不能破坏必要的业务步骤。
std::size_t required_iteration_floor(const Json &input, const Json &control,
                                     const std::string &actor, std::size_t fallback = 0) {
  auto floor = control.num("required_iteration_floor", fallback);
  const auto &requirements = input["controls"]["loop_requirements"];
  if (requirements.array())
    for (const auto &row : requirements.items())
      if (row.str("target_scope") == control.str("target_scope") && row.str("actor_id") == actor &&
          row.flag("require_finalization", false) == control.flag("require_finalization", false))
        floor = std::max(floor, row.num("required_iteration_floor", 0));
  return floor;
}

// 同一分支的重试护栏与恢复预算需要一起优化；不能把并发兄弟的失败串起来。
Json recovery_components(const Json &selected, const Json &input, const Json &available) {
  if (selected.items().size() != 1 || !available.has("action_rules") ||
      !available.has("loop_rules") ||
      (selected.items().front().text() != "action_rules" &&
       selected.items().front().text() != "loop_rules"))
    return selected;
  for (const auto &feedback : input["feedback"].items()) {
    const auto &runs = feedback["trial"]["evidence"]["runs"];
    if (!runs.array())
      continue;
    for (const auto &run : runs.items()) {
      if (!run["actions"].array())
        continue;
      for (const auto &action : run["actions"].items()) {
        if (!action_control_evidence(action) || action.str("execution_id").empty() ||
            (!action.has("terminal_error_code_candidate") &&
             action["action_control"].num("prior_failures", 0) < 2))
          continue;
        for (const auto &step : run["actions"].items()) {
          const auto &loop = step["loop_control"];
          if (!loop.object() || loop.flag("pending_actions", false) ||
              step.str("execution_id") != action.str("execution_id") ||
              step.str("actor_id") != action.str("actor_id") ||
              loop.str("target_scope") != action["action_control"].str("target_scope"))
            continue;
          if (loop.str("proposed_decision") == "replan" || loop.num("repeated_failure_steps", 0))
            return Json::Array{"action_rules", "loop_rules"};
        }
      }
    }
  }
  return selected;
}

// 保守模式不允许无条件截停。只有已绑定收尾约束、宿主确实支持收尾的 actor
// 才能研究轮数上限；是否有浪费后缀及是否截掉必要工作由后续完整证据继续验证。
// 摘录优先级、候选绑定和机会路由共用此条件，避免某处放行、另一处静默过滤。
bool iteration_cap_scope_available(const Json &controls, const Json &action) {
  const auto &loop = action["loop_control"];
  return controls.flag("aggressive_controls", false) ||
         (!action.str("actor_id").empty() && loop.flag("require_finalization", false) &&
          loop.flag("can_finalize", false));
}

// 提取当前控制规则真正介入的执行片段，避免用未触发规则的随机收益驱动改写。
Json control_feedback_trial(const dataset::v1::EvaluationTrial &trial, const Json &selected,
                            const Json &controls) {
  auto full = feedback_trial(trial, controls);
  bind_iteration_assessments(full, json(trial.evidence()));
  auto chosen = [&](const char *component) {
    return std::any_of(selected.items().begin(), selected.items().end(),
                       [&](const auto &item) { return item.text() == component; });
  };
  const bool actions = chosen("action_rules"), loops = chosen("loop_rules"),
             schedules = chosen("schedule_rules");
  Json checks = Json::Array{};
  if (!full["checks"].null())
    for (const auto &check : full["checks"].items()) {
      if (checks.items().size() == 8)
        break;
      Json item = Json::Object{};
      for (const auto *field : {"check_id", "status", "attempts"})
        if (check.has(field))
          item[field] = check[field];
      if (check.has("reason"))
        item["reason"] = clip(check.str("reason"), 512);
      checks.push(item);
    }
  Json result{{"verdict", full["verdict"]}, {"checks", checks},
              {"metrics", full["metrics"]}, {"execution_status", full["execution_status"]},
              {"observed_tool_failures", full["observed_tool_failures"]},
              {"evidence_truncated", true}, {"control_evidence_preserved", true}};
  if (full.has("goal_completion"))
    result["goal_completion"] = full["goal_completion"];
  if (!trial.error_code().empty())
    result["error_code"] = trial.error_code();
  auto &evidence = result["evidence"];
  if (full["evidence"].has("loop_review")) {
    if (loops)
      evidence["loop_review"] = compact_excerpt(full["evidence"]["loop_review"]);
    else {
      // 动作或编排改动也以减少无效循环为目标；保留真实评审数字，不回放长评审。
      const auto &review = full["evidence"]["loop_review"];
      for (const auto *field : {"status", "count", "observed_iterations", "keep_count", "waste_count"})
        if (review.has(field))
          evidence["loop_review"][field] = review[field];
    }
  }
  evidence["runs"] = Json::Array{};
  // 评测异常可能没有检查项或轨迹；保留异常，不伪造可执行控制证据。
  if (full["evidence"]["runs"].null()) {
    result["control_evidence_preserved"] = false;
    return result;
  }
  for (const auto &run : full["evidence"]["runs"].items()) {
    Json view{{"run_id", run["run_id"]}, {"actions", Json::Array{}}};
    if (schedules && run["schedule_plans"].array()) {
      view["schedule_plans"] = Json::Array{};
      std::vector<std::pair<std::size_t, Json>> preferred, other, kept;
      std::size_t ordinal = 0;
      for (const auto &plan : run["schedule_plans"].items()) {
        std::set<std::string> instances;
        for (const auto &action : plan["actions"].items())
          if (!action.str("operation").empty() && !action.str("action_id").empty())
            instances.insert(action.str("action_id"));
        auto compact = compact_excerpt(plan);
        // 实例 ID 和宿主依赖本身已是不含内容的视图；裁剪该数组可能隐藏唯一的
        // 生产者/消费者配对。
        compact["actions"] = plan["actions"];
        (instances.size() > 1 ? preferred : other).emplace_back(ordinal++, std::move(compact));
      }
      for (const auto *rows : {&preferred, &other})
        for (const auto &plan : *rows) {
          if (kept.size() == 8)
            break;
          kept.push_back(plan);
        }
      std::sort(kept.begin(), kept.end(),
                [](const auto &a, const auto &b) { return a.first < b.first; });
      for (const auto &plan : kept)
        view["schedule_plans"].push(plan.second);
    }
    std::vector<std::pair<std::size_t, Json>> dispatches, opportunities, primary, secondary;
    std::size_t ordinal = 0;
    for (const auto &action : run["actions"].items()) {
      const auto order = ordinal++;
      bool keep = false, important = false, actionable = false;
      if (actions && action.str("kind") == "action.end" && action["action_control"].object())
        keep = true;
      if (actions && action_control_evidence(action))
        actionable = true;
      if (loops && action.str("kind") == "loop.decision" && action["loop_control"].object()) {
        keep = true;
        const auto &control = action["loop_control"];
        important =
            (!control["observed_no_progress"].null() && control.num("observed_no_progress", 0)) ||
            control.num("failed_actions", 0) || control.num("repeated_failure_steps", 0) ||
            control.str("proposed_decision", "continue") != "continue";
        const auto reviewed_cap =
            std::max({controls.num("min_iterations", 2), control.num("reviewed_waste_after", 0),
                      control.num("required_iteration_floor", 0)});
        actionable = important || (iteration_cap_scope_available(controls, action) &&
                                   control.has("reviewed_waste_after") &&
                                   control.num("iteration", 0) > reviewed_cap);
      }
      if ((loops || schedules) && action.str("kind") == "action.end" &&
          action.str("semantic") != "model" && !action.str("operation").empty()) {
        keep = true;
        important |= action.str("status") == "failed" || action.has("error");
      }
      if (!keep)
        continue;
      Json item = Json::Object{};
      for (const auto *field : {"event_id",
                                "kind",
                                "actor_id",
                                "execution_id",
                                "loop_id",
                                "iteration",
                                "status",
                                "error",
                                "decision",
                                "reason",
                                "outcome",
                                "operation",
                                "executed",
                                "observed_failure",
                                "semantic",
                                "guard_only",
                                "input",
                                "output",
                                "action_control",
                                "loop_control",
                                "iteration_assessment",
                                "reviewed_required_action",
                                "terminal_error_code_candidate"})
        if (action.has(field))
          item[field] = compact_excerpt(action[field]);
      // 选择器和模型都会使用这类小型类型化证据；不能把被省略的条目改写成“参数或
      // 谓词不存在”，否则会改变可执行含义。
      if (action["action_control"].object())
        item["action_control"] = action["action_control"];
      if (action["loop_control"].object())
        item["loop_control"] = action["loop_control"];
      // 单独编排只使用批次中的类型化身份/依赖；其他层的计数与逐轮评语不应挤掉
      // 第二个任务的编排证据。工具内容只作语义背景，保留截断标记，不据预览推断缺参。
      if (schedules && !actions && !loops) {
        item.fields().erase("action_control");
        item.fields().erase("loop_control");
        item.fields().erase("iteration_assessment");
        for (const auto *field : {"input", "output"})
          if (item.has(field) && item[field].dump().size() > 512)
            item[field] = clip(item[field].dump(), 512);
      }
      // 上层重复派发不能被同一子分支的大量重试挤出窗口；总长度上限不变。
      const bool redispatch = actions && actionable && action.flag("guard_only", false) &&
                              action["action_control"].num("prior_operation_rounds", 0) > 0;
      (redispatch   ? dispatches
       : actionable ? opportunities
       : important  ? primary
                    : secondary)
          .emplace_back(order, std::move(item));
    }
    std::vector<std::pair<std::size_t, Json>> chosen_actions;
    for (const auto *rows : {&dispatches, &opportunities, &primary, &secondary})
      for (const auto &action : *rows) {
        if (chosen_actions.size() == 16)
          break;
        chosen_actions.push_back(action);
      }
    std::sort(chosen_actions.begin(), chosen_actions.end(),
              [](const auto &a, const auto &b) { return a.first < b.first; });
    for (const auto &action : chosen_actions)
      view["actions"].push(action.second);
    if (view["actions"].items().size() || view["schedule_plans"].array())
      evidence["runs"].push(view);
  }
  return result;
}

// 压缩重复的运行上下文；已按机会优先级选出的动作不能再次按时间截断，
// 否则容易丢掉最后的冗余后缀。typed 控制字段、必要步骤和真实依赖完整保留。
Json compact_control_reflection_trial(Json full, const Json &selected) {
  (void)selected;
  if (!full["evidence"]["runs"].array())
    return full;
  static const char *const fields[] = {
      "event_id", "kind", "actor_id", "execution_id", "loop_id", "iteration", "status",
      "operation", "executed", "observed_failure", "semantic", "guard_only", "input",
      "iteration_assessment", "reviewed_required_action", "terminal_error_code_candidate",
      "action_control", "loop_control"};
  for (auto &run : full["evidence"]["runs"].items()) {
    if (!run["actions"].array())
      continue;
    for (auto &action : run["actions"].items()) {
      Json item = Json::Object{};
      for (const auto *field : fields)
        if (action.has(field))
          item[field] = action[field];
      // 输出仅供解释；谓词和参数缺失的判断始终来自上面的完整 typed 字段。
      if (action.has("output")) {
        item["output"] = compact_excerpt(action["output"]);
        if (item["output"].dump().size() > 512)
          item["output"] = Json{{"preview", clip(action["output"].dump(), 512)},
                                {"_truncated", true}};
      }
      if (item["input"].object())
        for (auto &field : item["input"].fields())
          // action 是 repeat_failed_operation 的真实谓词；其他长参数仅作背景。
          if (field.first != "action" && field.second.dump().size() > 256)
            field.second = Json{{"preview", clip(field.second.dump(), 256)},
                                {"_truncated", true}};
      if (item["iteration_assessment"].object())
        for (auto &field : item["iteration_assessment"].fields())
          if (field.first != "decision" && field.second.string() &&
              field.second.text().size() > 256)
            field.second = clip(field.second.text(), 256);
      action = std::move(item);
    }
  }
  full["evidence_compacted_for_reflection"] = true;
  return full;
}

// 反思请求的详细轨迹按可执行机会跨 Run 选取，避免单个 Run 的时间顺序
// 吞掉后续真正可修改的边界。完整的 16 条控制证据仍留在调用方的原视图中。
Json compact_control_reflection_detail(Json full, const Json &selected,
                                       std::size_t max_actions) {
  if (!full["evidence"]["runs"].array())
    return full;
  const auto selected_component = [&](const char *name) {
    return std::any_of(selected.items().begin(), selected.items().end(),
                       [&](const auto &value) { return value.text() == name; });
  };
  const bool action_rules = selected_component("action_rules");
  const bool loop_rules = selected_component("loop_rules");
  const bool schedule_rules = selected_component("schedule_rules");
  struct Choice {
    std::size_t run, index;
    int priority;
    std::string identity;
  };
  std::vector<Choice> choices;
  std::map<std::string, std::pair<std::size_t, std::size_t>> highest_keep;
  std::size_t total_actions = 0;
  const auto &runs = full["evidence"]["runs"].items();
  for (std::size_t run = 0; run < runs.size(); ++run) {
    const auto &actions = runs[run]["actions"];
    if (!actions.array())
      continue;
    total_actions += actions.items().size();
    for (std::size_t index = 0; index < actions.items().size(); ++index) {
      const auto &action = actions.items()[index];
      const auto &control = action["loop_control"];
      if (!loop_rules || action.str("kind") != "loop.decision" || !control.object() ||
          action["iteration_assessment"].str("decision") != "keep")
        continue;
      const auto key = control.str("target_scope") + "\n" + action.str("actor_id");
      const auto old = highest_keep.find(key);
      if (old == highest_keep.end() ||
          runs[old->second.first]["actions"].items()[old->second.second]["loop_control"].num(
              "iteration", 0) < control.num("iteration", 0))
        highest_keep[key] = {run, index};
    }
  }
  for (std::size_t run = 0; run < runs.size(); ++run) {
    const auto &actions = runs[run]["actions"];
    if (!actions.array())
      continue;
    for (std::size_t index = 0; index < actions.items().size(); ++index) {
      const auto &action = actions.items()[index];
      const auto &control = action["action_control"];
      const auto &loop = action["loop_control"];
      int priority = 0;
      std::string identity;
      if (action_rules && action.str("kind") == "action.end" && control.object()) {
        identity = "action\n" + control.str("target_scope") + "\n" + action.str("operation");
        const auto rounds = control.num("prior_operation_rounds", 0);
        if (action.flag("guard_only", false) && !action.flag("executed", true) && rounds &&
            control.has("reviewed_budget_prior") &&
            control.num("reviewed_budget_prior", 0) == rounds)
          priority = 160;
        else if (action.has("terminal_error_code_candidate") &&
                 !control.str("terminal_retry_query_event_id").empty())
          priority = 150;
        else if (action_control_evidence(action))
          priority = 130;
        else if (action.str("status") == "failed" || action.has("error"))
          priority = 75;
        else
          priority = 20;
      }
      if (loop_rules && action.str("kind") == "loop.decision" && loop.object()) {
        identity = "loop\n" + loop.str("target_scope") + "\n" + action.str("actor_id");
        // 中性决策仍需留一条可观察上下文；它不能凭此获得可编辑证据。
        priority = std::max(priority, 10);
        const auto iteration = loop.num("iteration", 0);
        const auto floor = std::max(loop.num("reviewed_waste_after", 0),
                                    loop.num("required_iteration_floor", 0));
        if (!loop.flag("pending_actions", false) && loop.has("reviewed_waste_after") &&
            iteration > floor &&
            (loop.str("proposed_decision", "continue") == "continue" ||
             loop.str("proposed_decision") == "replan"))
          priority = std::max(priority, 155);
        else if (loop.num("observed_no_progress", 0) || loop.num("failed_actions", 0) ||
                 loop.num("repeated_failure_steps", 0))
          priority = std::max(priority, 125);
        const auto keep = highest_keep.find(loop.str("target_scope") + "\n" +
                                            action.str("actor_id"));
        if (keep != highest_keep.end() && keep->second == std::make_pair(run, index))
          priority = std::max(priority, 120);
        if (loop.str("proposed_decision", "continue") != "continue")
          priority = std::max(priority, 105);
      }
      if (schedule_rules && action.str("kind") == "action.end" &&
          !action.str("operation").empty()) {
        identity = "schedule\n" + action.str("execution_id") + "\n" +
                   action.str("operation");
        priority = std::max(priority, action.str("status") == "failed" ? 80 : 45);
      }
      if (priority)
        choices.push_back({run, index, priority, std::move(identity)});
    }
  }
  std::stable_sort(choices.begin(), choices.end(), [](const auto &a, const auto &b) {
    return a.priority > b.priority;
  });
  std::set<std::pair<std::size_t, std::size_t>> kept;
  std::set<std::string> represented;
  // 先覆盖不同控制对象，再用余量补同一对象的收尾/keep 对照。
  for (unsigned pass = 0; pass < 2; ++pass)
    for (const auto &choice : choices) {
      if (kept.size() >= max_actions)
        break;
      if (pass == 0 && represented.count(choice.identity))
        continue;
      if (kept.insert({choice.run, choice.index}).second)
        represented.insert(choice.identity);
    }
  std::size_t kept_actions = 0;
  Json detailed_runs = Json::Array{};
  for (std::size_t run = 0; run < runs.size(); ++run) {
    Json view = runs[run];
    const auto &actions = runs[run]["actions"];
    if (actions.array()) {
      view["actions"] = Json::Array{};
      for (std::size_t index = 0; index < actions.items().size(); ++index)
        if (kept.count({run, index}))
          view["actions"].push(actions.items()[index]);
      kept_actions += view["actions"].items().size();
      if (view["actions"].items().size() < actions.items().size()) {
        view["actions_truncated"] = true;
        view["actions_omitted"] = actions.items().size() - view["actions"].items().size();
      }
    }
    if ((view["actions"].array() && !view["actions"].items().empty()) ||
        (view["schedule_plans"].array() && !view["schedule_plans"].items().empty()))
      detailed_runs.push(std::move(view));
  }
  full["evidence"]["runs"] = std::move(detailed_runs);
  full["reflection_actions_available"] = total_actions;
  full["reflection_actions_selected"] = kept_actions;
  full["reflection_actions_truncated"] = kept_actions < total_actions;
  return full;
}

// 一次提案可能改写多条规则；pool 只要求至少一条新增规则真实应用，
// 其余未命中规则明确留在诊断里，最终覆盖门禁仍独立检查完整策略。
Json new_control_observation(const Json &coverage, const Json &inherited) {
  Json applied = Json::Array{}, unobserved = Json::Array{};
  for (const auto &rule : coverage["rules"].fields())
    if (!inherited["rules"].has(rule.first)) {
      if (rule.second.num("applied", 0))
        applied.push(rule.first);
      else
        unobserved.push(rule.first);
    }
  return Json{{"observed", !applied.items().empty() || unobserved.items().empty()},
              {"applied_new_rules", applied}, {"unobserved_new_rules", unobserved}};
}

bool control_changes_observed(const Json &coverage, const Json &inherited) {
  return new_control_observation(coverage, inherited).flag("observed", false);
}

// 只保留本轮获选组件能够造成的边界差分。候选即使继承了其他层的策略，
// 也不能把那些层的随机命中或历史收益归因给本轮修改。
Json component_applied_changes(const Json &selected, const Json &changes) {
  if (!selected.array() || selected.items().empty() || !changes.object())
    return changes;
  std::set<std::string> boundaries;
  for (const auto &component : selected.items()) {
    const auto name = component.text();
    if (name.rfind("experience:", 0) == 0)
      boundaries.insert("llm");
    else if (name == "action_rules")
      boundaries.insert("actions");
    else if (name == "schedule_rules")
      boundaries.insert("schedule");
    else if (name == "loop_rules")
      boundaries.insert("loop");
  }
  Json result = Json::Object{};
  for (const auto &task : changes.fields())
    for (const auto &boundary : task.second.fields())
      if (boundaries.count(boundary.first) && number(boundary.second, 0) > 0)
        result[task.first][boundary.first] = boundary.second;
  return result;
}

// 增量草案只在搜索入口展开；索引始终指向父数组，未提及的独立规则原样保留。
// 旧 controls 仍表示完整替换，编译、导出和宿主策略协议不增加新字段。
Json expand_control_updates(const Json &draft, const Json &before, const Json &selected) {
  if (!draft.has("control_updates"))
    return draft;
  require(!draft.has("controls") && draft["control_updates"].array(), "gepa_draft_invalid");
  std::set<std::string> allowed;
  for (const auto &component : selected.items())
    allowed.insert(component.text());
  std::map<std::string, std::map<std::size_t, Json>> replacements;
  std::map<std::string, Json::Array> additions;
  std::set<std::string> touched, added;
  for (const auto &change : draft["control_updates"].items()) {
    keys(change, {"component", "op", "index", "value"});
    require(change.object() && change["component"].string() && change["op"].string(),
            "gepa_draft_invalid");
    const auto component = change.str("component"), operation = change.str("op");
    require((component == "action_rules" || component == "loop_rules" ||
             component == "schedule_rules") &&
                allowed.count(component) && before.has(component) && before[component].array(),
            "gepa_component_not_authorized");
    require(operation == "add" || operation == "set" || operation == "remove",
            "gepa_draft_invalid");
    require(operation == "remove" ? !change.has("value") : change["value"].object(),
            "gepa_draft_invalid");
    // 空父数组没有可供删除的旧规则。模型偶尔会把 editable_bindings 中的候选边
    // 误认为已经存在的父规则，并对 index=0 连续输出 remove。该操作在语义上只能
    // 是无改动；本地丢弃这个结构合法但不可能执行的删除，避免再为同一事实调用一次
    // 修复模型。add 仍按正常路径保留，因此空组件依然可以产生第一条新规则。
    if (operation == "remove" && before[component].items().empty()) {
      require(std::holds_alternative<double>(change["index"].value), "gepa_draft_invalid");
      const auto index = number(change["index"]);
      require(std::isfinite(index) && index >= 0 && std::floor(index) == index,
              "gepa_draft_invalid");
      continue;
    }
    touched.insert(component);
    if (operation == "add") {
      require(!change.has("index") &&
                  added.insert(component + "\n" + change["value"].dump()).second,
              "gepa_draft_invalid");
      additions[component].push_back(change["value"]);
    } else {
      require(std::holds_alternative<double>(change["index"].value), "gepa_draft_invalid");
      const auto index = number(change["index"]);
      require(std::isfinite(index) && index >= 0 && std::floor(index) == index &&
                  index < before[component].items().size(),
              "gepa_draft_invalid");
      require(replacements[component]
                  .emplace(static_cast<std::size_t>(index),
                           operation == "remove" ? Json() : change["value"])
                  .second,
              "gepa_draft_invalid");
    }
  }
  auto result = draft;
  result.fields().erase("control_updates");
  for (const auto &component : touched) {
    auto &output = result["controls"][component];
    output = Json::Array{};
    const auto &changes = replacements[component];
    const auto &original = before[component].items();
    for (std::size_t index = 0; index < original.size(); ++index) {
      const auto changed = changes.find(index);
      if (changed == changes.end())
        output.push(original[index]);
      else if (!changed->second.null())
        output.push(changed->second);
    }
    for (const auto &value : additions[component])
      output.push(value);
  }
  // 空更新也是明确的弃权，沿用既有空 experiences 的无改动路径。
  if (touched.empty() && !result.has("experience") && !result.has("experiences"))
    result["experiences"] = Json::Array{};
  return result;
}

// A duplicate must be repaired explicitly; never turn add into an implicit replacement.
Json loop_scope_repair_context(const Json &before, const Json &snapshot) {
  Json rules = Json::Array{};
  for (std::size_t index = 0; index < before["loop_rules"].items().size(); ++index) {
    const auto &rule = before["loop_rules"].items()[index];
    Json entry{{"index", index}, {"memory_id", rule.str("memory_id")},
               {"actor_id", rule.str("actor_id")}, {"error_codes", rule["error_codes"]}};
    for (const auto &source : snapshot["experiences"].items())
      if (source.str("memory_id") == rule.str("memory_id")) {
        entry["target_scope"] = source.str("target_scope");
        break;
      }
    rules.push(std::move(entry));
  }
  return Json{{"component", "loop_rules"}, {"existing_rules", rules},
              {"uniqueness", "approved source target_scope and exact actor_id"},
              {"resolution", "Use explicit set at the existing parent index only when justified, "
                             "preserving necessary recovery conditions; otherwise abstain. "
                             "Add does not replace an existing rule."}};
}

Json action_rule_repair_context(const Json &before, const Json &snapshot) {
  Json rules = Json::Array{};
  for (std::size_t index = 0; index < before["action_rules"].items().size(); ++index) {
    auto entry = before["action_rules"].items()[index];
    entry["index"] = index;
    for (const auto &source : snapshot["experiences"].items())
      if (source.str("memory_id") == entry.str("memory_id")) {
        entry["target_scope"] = source.str("target_scope");
        break;
      }
    rules.push(std::move(entry));
  }
  return Json{{"component", "action_rules"}, {"existing_rules", rules},
              {"resolution", "An add does not replace the existing parent rule. Use explicit set "
                             "at the parent index only when justified, preserving necessary "
                             "recovery conditions; otherwise abstain."}};
}

// 请求只展示本轮可改的组件；权限仍由原始配置和候选编译校验决定。
void restrict_reflection_components(Json &packet, const Json &selected) {
  std::set<std::string> allowed;
  for (const auto &item : selected.items())
    allowed.insert(item.text());
  packet["selected_components"] = selected;
  const bool controls = allowed.count("action_rules") || allowed.count("loop_rules") ||
                        allowed.count("schedule_rules");
  if (controls && packet["experiences"].array() && packet["components"].object()) {
    const auto &values = packet["components"].fields();
    for (auto &source : packet["experiences"].items()) {
      const auto key = "experience:" + source.str("memory_id");
      const auto found = values.find(key);
      if (allowed.count(key) || found == values.end())
        continue;
      // 控制子请求裁掉经验组件前保留当前正文；历史正文不能复活已停用项。
      if (found->second.null())
        source.fields().erase("content");
      else
        source["content"] = experience_content(found->second);
    }
  }
  if (packet["components"].object()) {
    auto &values = packet["components"].fields();
    for (auto it = values.begin(); it != values.end();)
      if (!allowed.count(it->first))
        it = values.erase(it);
      else
        ++it;
  }
  for (const auto *category : {"action", "loop", "schedule"}) {
    const std::string prefix(category);
    if (allowed.count(prefix + "_rules"))
      continue;
    if (packet["controls"].object()) {
      packet["controls"].fields().erase(prefix + "_primitives");
      packet["controls"].fields().erase("base_" + prefix + "_rules");
      packet["controls"].fields().erase(prefix + "_conditions");
      packet["controls"].fields().erase(prefix + "_requirements");
      if (packet["controls"]["allowed_update_operations"].object())
        packet["controls"]["allowed_update_operations"].fields().erase(prefix + "_rules");
    }
  }
  if (!controls) {
    packet.fields().erase("controls");
    if (packet["experiences"].array()) {
      auto &sources = packet["experiences"].items();
      sources.erase(std::remove_if(sources.begin(), sources.end(),
                                   [&](const auto &item) {
                                     return !allowed.count("experience:" + item.str("memory_id"));
                                   }),
                    sources.end());
    }
  }
}

// 外置提示词按本次获选组件加载；未使用分节标记的自定义模板保持原样。
// 这只减少无关说明，授权仍由 mutate/resolve 校验，不能用提示词替代执行权限。
std::string reflection_prompt_for_components(const std::string &source, const Json &selected) {
  if (source.find("<!-- gepa:") == std::string::npos)
    return source;
  std::set<std::string> active{"common"};
  if (selected.array())
    for (const auto &item : selected.items()) {
      if (item.text().rfind("experience:", 0) == 0)
        active.insert("experience");
      else if (item.text() == "action_rules" || item.text() == "loop_rules" ||
               item.text() == "schedule_rules") {
        active.insert(item.text());
        active.insert("controls");
      }
    }
  const std::set<std::string> known{"common", "experience", "controls", "action_rules",
                                     "loop_rules", "schedule_rules"};
  std::istringstream stream(source);
  std::string line, result;
  bool keep = true;
  while (std::getline(stream, line)) {
    if (line.rfind("<!-- gepa:", 0) == 0) {
      if (!line.empty() && line.back() == '\r') line.pop_back();
      require(line.size() > 14 && line.substr(line.size() - 4) == " -->",
              "gepa_prompt_section_invalid");
      const auto names = line.substr(10, line.size() - 14);
      require(!names.empty() && names.back() != ',', "gepa_prompt_section_invalid");
      std::istringstream sections(names);
      std::string section;
      keep = false;
      while (std::getline(sections, section, ',')) {
        require(known.count(section), "gepa_prompt_section_invalid");
        keep |= active.count(section) != 0;
      }
    } else if (keep)
      result += line + '\n';
  }
  require(!result.empty(), "gepa_prompt_section_invalid");
  return result;
}

// 生成控制绑定的稳定身份，便于规则去重与执行关联。
std::string control_binding_id(const Json &input, Json binding) {
  auto packet = input;
  if (input["controls"].object()) {
    packet["controls"].fields().erase("editable_bindings");
    if (input["controls"]["action_primitives"].array())
      for (auto &primitive : packet["controls"]["action_primitives"].items())
        if (primitive.object())
          primitive.fields().erase("observed_bindings");
  }
  binding.fields().erase("binding_id");
  return "control:" + hash(hash(packet.dump()) + '\0' + binding.dump()).substr(0, 24);
}

// 数值范围来自当前观察与父规则交集；仅说明预算，不替代来源或组件授权。
Json control_binding_budget(const Json &prepared, const Json &binding) {
  const bool action = binding.str("component") == "action_rules" &&
                      binding.str("primitive_id") == "operation_budget";
  const bool loop = binding.str("component") == "loop_rules" &&
                    binding.str("primitive_id") == "bounded_iteration_cap";
  if (!action && !loop)
    return Json();
  const auto field = action ? "max_attempts" : "max_iterations";
  const auto minimum = action ? required_attempt_floor(prepared, binding)
                              : std::max({prepared["controls"]["limits"].num("min_iterations", 2),
                                          binding.num("reviewed_waste_after", 0),
                                          binding.num("required_iteration_floor", 0)});
  const auto observed =
      action ? binding.num("prior_operation_rounds", 0) : binding.num("observed_iteration", 0);
  auto maximum = std::min<std::size_t>(action ? 16 : 10000, action     ? observed
                                                            : observed ? observed - 1
                                                                       : 0);
  std::size_t matched = 0, existing_maximum = 0;
  const Json *current = nullptr;
  const auto &rules = prepared["components"][binding.str("component")];
  if (rules.array())
    for (const auto &rule : rules.items()) {
      if ((action &&
           (rule.str("operation") != binding.str("operation") || !rule.num("max_attempts", 0))) ||
          (loop && rule.str("actor_id") != binding.str("actor_id")))
        continue;
      std::size_t sources = 0;
      std::string scope;
      for (const auto *values :
           {&prepared["experiences"], &prepared["controls"]["control_sources"]})
        if (values->array())
          for (const auto &source : values->items())
            if (source.str("memory_id") == rule.str("memory_id")) {
              ++sources;
              scope = source.str("scope");
            }
      if (sources == 1 && scope == binding.str("target_scope")) {
        ++matched;
        existing_maximum = rule.num(field, 0);
        current = &rule;
      }
    }
  if (matched == 1)
    maximum = std::min(maximum, existing_maximum);
  Json result{{"field", field}, {"min", minimum}, {"max", maximum}};
  if (loop) {
    // current 是父策略现值，min/max 是允许的新值；未知条件不伪装成无条件预算。
    if (!matched) {
      std::size_t exact = 0, generic = 0;
      if (prepared["controls"]["base_loop_rules"].array())
        for (const auto &rule : prepared["controls"]["base_loop_rules"].items()) {
          if (rule.str("target_scope") != binding.str("target_scope") ||
              (!rule.str("actor_id").empty() && rule.str("actor_id") != binding.str("actor_id")))
            continue;
          if (rule.str("actor_id").empty()) {
            ++generic;
            if (!exact)
              current = &rule;
          } else {
            ++exact;
            current = &rule;
          }
        }
      if (exact != 1 && (exact || generic != 1))
        current = nullptr;
      // 通用生成规则可能覆盖通用基础规则，但 resolver 不从该来源继承；不猜其生效条件。
      if (current && current->str("actor_id").empty() && rules.array())
        for (const auto &rule : rules.items()) {
          if (!rule.str("actor_id").empty())
            continue;
          for (const auto *sources :
               {&prepared["experiences"], &prepared["controls"]["control_sources"]})
            if (sources->array())
              for (const auto &source : sources->items())
                if (source.str("memory_id") == rule.str("memory_id") &&
                    source.str("scope") == binding.str("target_scope"))
                  current = nullptr;
        }
    }
    if (current && matched <= 1 && current->num(field, 0) &&
        !current->flag("after_action_rejection", false) &&
        !current->flag("require_finalization", false) &&
        (!(*current)["error_codes"].array() || (*current)["error_codes"].items().empty()) &&
        (!current->has("memory_id") || current->str("match_mode") == "predicate"))
      result["current"] = (*current)[field];
  }
  return result;
}

// 收紧恢复预算必须有同一执行分支的结构化条件；语义上的 waste 不能替代错误码或拒绝回执。
// 这里只描述实际观测，不替模型生成规则。按条件去重并保留最多四项，避免重复轮次挤占输入。
Json recovery_primitive_bindings(const Json &input) {
  Json result = Json::Array{};
  if (!input["feedback"].array())
    return result;
  std::set<std::string> seen;
  for (std::size_t row = 0; row < input["feedback"].items().size(); ++row) {
    const auto &runs = input["feedback"].items()[row]["trial"]["evidence"]["runs"];
    if (!runs.array())
      continue;
    for (std::size_t run = 0; run < runs.items().size(); ++run) {
      const auto &actions = runs.items()[run]["actions"];
      if (!actions.array())
        continue;
      for (std::size_t index = 0; index < actions.items().size(); ++index) {
        const auto &action = actions.items()[index];
        const auto &control = action["loop_control"];
        if (action.str("kind") != "loop.decision" || !control.object() ||
            control.flag("pending_actions", false) || action.str("actor_id").empty() ||
            control.str("target_scope").empty())
          continue;
        Json binding{{"actor_id", action["actor_id"]},
                     {"target_scope", control["target_scope"]}};
        if (control["error_codes"].array()) {
          Json codes = Json::Array{};
          std::set<std::string> unique;
          for (const auto &code : control["error_codes"].items())
            if (code.string() && !code.text().empty() && unique.insert(code.text()).second) {
              codes.push(code);
              if (codes.items().size() == 8)
                break;
            }
          if (!codes.items().empty())
            binding["error_codes"] = std::move(codes);
        }
        if (control.num("rejected_actions", 0))
          binding["after_action_rejection"] = true;
        if ((!binding.has("error_codes") && !binding.has("after_action_rejection")) ||
            !seen.insert(binding.dump()).second)
          continue;
        binding["evidence_ref"] = "/feedback/" + std::to_string(row) + "/trial/evidence/runs/" +
                                  std::to_string(run) + "/actions/" + std::to_string(index);
        result.push(std::move(binding));
        if (result.items().size() == 4)
          return result;
      }
    }
  }
  return result;
}

// 将当前训练包中的真实控制机会投影为 request-local typed binding；不增加原语或运行时授权。
Json observed_operation_bindings(const Json &input, std::size_t byte_limit) {
  if (!input["controls"].object())
    return input;
  auto packet = input;
  packet["controls"].fields().erase("editable_bindings");
  Json *budget = nullptr;
  if (input["controls"]["action_primitives"].array())
    for (auto &primitive : packet["controls"]["action_primitives"].items()) {
      if (!primitive.object())
        continue;
      primitive.fields().erase("observed_bindings");
      if (primitive.str("primitive_id") == "operation_budget" && !budget)
        budget = &primitive;
    }
  const auto selected = [&](const char *component) {
    return input["selected_components"].array() &&
           std::any_of(input["selected_components"].items().begin(),
                       input["selected_components"].items().end(),
                       [&](const auto &item) { return item.string() && item.text() == component; });
  };
  // 静态原语说明只代表宿主能力；本次反馈没有恢复条件时不向模型开放这一收紧方向。
  // 每次重新组包都重建证据位置，防止裁剪或纠错后仍引用上一份输入中的条件。
  if (selected("loop_rules") && packet["controls"]["loop_primitives"].array()) {
    const auto recovery = recovery_primitive_bindings(input);
    auto &primitives = packet["controls"]["loop_primitives"].items();
    primitives.erase(std::remove_if(primitives.begin(), primitives.end(), [&](auto &primitive) {
      if (primitive.str("primitive_id") != "bounded_recovery_budget")
        return false;
      primitive.fields().erase("observed_bindings");
      if (recovery.items().empty())
        return true;
      primitive["observed_bindings"] = recovery;
      return false;
    }), primitives.end());
  }
  bool iteration_cap = false;
  if (input["controls"]["loop_primitives"].array())
    for (const auto &primitive : input["controls"]["loop_primitives"].items())
      iteration_cap |=
          primitive.object() && primitive.str("primitive_id") == "bounded_iteration_cap";

  std::vector<Json> repeat_failure_candidates, action_candidates, legacy_action_candidates,
      loop_candidates, schedule_candidates;
  if (selected("action_rules") && input["feedback"].array()) {
    std::set<std::string> seen;
    for (std::size_t row = 0; row < input["feedback"].items().size(); ++row) {
      const auto &feedback = input["feedback"].items()[row];
      const auto &runs = feedback["trial"]["evidence"]["runs"];
      if (feedback.str("task_id").empty() || !runs.array())
        continue;
      for (std::size_t run = 0; run < runs.items().size(); ++run) {
        const auto &actions = runs.items()[run]["actions"];
        if (!actions.array())
          continue;
        for (std::size_t index = 0; index < actions.items().size(); ++index) {
          const auto &action = actions.items()[index];
          const auto &control = action["action_control"];
          const auto operation = action.str("operation");
          const auto business_action = action["input"].str("action");
          // repeat_failed_operation 只从同一动作已经两次失败后、当前尝试也确实
          // 失败的结构化事实生成。成功空查询（如 not_found）不会进入候选。
          if (action.str("kind") != "action.end" || action.str("semantic") == "model" ||
              !action.flag("executed", false) || !action.flag("observed_failure", false) ||
              !control.object() || control.str("decision") != "allow" ||
              control.num("prior_failures", 0) < 2 || control.str("target_scope").empty() ||
              operation.empty() || operation == "*" || !identifier(operation) ||
              business_action.empty() || business_action.size() > 256)
            continue;
          Json binding{{"task_id", feedback["task_id"]},
                       {"operation", operation},
                       {"arguments", Json{{"action", business_action}}},
                       {"observed_failure", true},
                       {"prior_failures", control["prior_failures"]},
                       {"target_scope", control["target_scope"]},
                       {"evidence_ref", "/feedback/" + std::to_string(row) +
                                            "/trial/evidence/runs/" + std::to_string(run) +
                                            "/actions/" + std::to_string(index)}};
          const auto identity = binding.dump();
          if (seen.insert(identity).second)
            repeat_failure_candidates.push_back(std::move(binding));
        }
      }
    }
    std::stable_sort(repeat_failure_candidates.begin(), repeat_failure_candidates.end(),
                     [](const auto &a, const auto &b) {
                       return a.num("prior_failures", 0) > b.num("prior_failures", 0);
                     });
  }
  if (selected("schedule_rules") && input["feedback"].array()) {
    std::set<std::string> seen;
    // 同批并行并不说明应该串行化。只有明确要求顺序，或观察到一个动作的
    // 输出路径确实成为另一动作的输入路径，才开放新增的顺序边。
    const auto resource_dependency = [](const Json &producer, const Json &consumer) {
      const auto &outputs = producer["arguments"];
      const auto &inputs = consumer["arguments"];
      if (!outputs.object() || !inputs.object())
        return false;
      for (const auto &output : outputs.fields()) {
        if (output.first != "output_path" && output.first != "output_file" &&
            output.first != "artifact_path" && output.first != "save_path")
          continue;
        if (!output.second.string() || output.second.text().empty())
          continue;
        for (const auto &input : inputs.fields())
          if ((input.first == "input_path" || input.first == "input_file" ||
               input.first == "source_path") &&
              input.second.string() && input.second.text() == output.second.text())
            return true;
      }
      return false;
    };
    for (std::size_t row = 0; row < input["feedback"].items().size(); ++row) {
      const auto &feedback = input["feedback"].items()[row];
      const auto &runs = feedback["trial"]["evidence"]["runs"];
      if (!runs.array())
        continue;
      for (std::size_t run = 0; run < runs.items().size(); ++run) {
        const auto &plans = runs.items()[run]["schedule_plans"];
        if (!plans.array())
          continue;
        for (std::size_t plan = 0; plan < plans.items().size(); ++plan) {
          const auto &observed_plan = plans.items()[plan];
          const auto &contract = observed_plan.has("schedule_contract")
                                     ? observed_plan["schedule_contract"]
                                     : feedback["schedule_contract"];
          if (!contract.flag("order_editable", contract.flag("ordering_required", false)) ||
              contract.flag("result_conditional", false))
            continue;
          const auto &actions = observed_plan["actions"];
          if (!actions.array() || actions.items().size() < 2 ||
              observed_plan.str("target_scope").empty())
            continue;
          // 沿既有前置边查祖先：同向已受约束无需重复加边，反向加边会形成环。
          const auto depends_on = [&](const std::string &start, const std::string &target) {
            std::vector<std::string> pending{start};
            std::set<std::string> visited;
            while (!pending.empty()) {
              const auto id = pending.back();
              pending.pop_back();
              if (!visited.insert(id).second)
                continue;
              for (const auto &action : actions.items())
                if (action.str("action_id") == id && action["after"].array())
                  for (const auto &edge : action["after"].items()) {
                    if (edge.str("action_id") == target)
                      return true;
                    pending.push_back(edge.str("action_id"));
                  }
            }
            return false;
          };
          for (std::size_t after_index = 1; after_index < actions.items().size();
               ++after_index)
            // 无顺序契约时只沿真实资源生产→消费方向提出候选。
            for (unsigned reverse = 0;
                 reverse < (contract.flag("ordering_required", false) ? 1u : 2u); ++reverse) {
            const auto &before = actions.items()[after_index - (reverse ? 0 : 1)];
            const auto &after = actions.items()[after_index - (reverse ? 1 : 0)];
            if (before.str("action_id").empty() || after.str("action_id").empty() ||
                before.str("operation").empty() || after.str("operation").empty() ||
                before.str("action_id") == after.str("action_id") ||
                before.str("operation") == after.str("operation"))
              continue;
            if (!contract.flag("ordering_required", false) &&
                !resource_dependency(before, after))
              continue;
            if (depends_on(after.str("action_id"), before.str("action_id")) ||
                depends_on(before.str("action_id"), after.str("action_id")))
              continue;
            Json binding{
                {"target_scope", observed_plan["target_scope"]},
                {"before_operation", before["operation"]},
                {"after_operation", after["operation"]},
                {"require_success", contract.flag("require_success", false)},
                {"task_text", observed_plan.str("task_text", feedback.str("task_input"))},
                {"evidence_ref", "/feedback/" + std::to_string(row) +
                                     "/trial/evidence/runs/" + std::to_string(run) +
                                     "/schedule_plans/" + std::to_string(plan)}};
            const auto identity = binding.dump();
            if (seen.insert(identity).second)
              schedule_candidates.push_back(std::move(binding));
          }
        }
      }
    }
  }
  if (budget && selected("action_rules") && input["feedback"].array()) {
    std::map<std::pair<std::string, std::string>, Json> observed, reviewed;
    for (std::size_t row = 0; row < input["feedback"].items().size(); ++row) {
      const auto &runs = input["feedback"].items()[row]["trial"]["evidence"]["runs"];
      if (!runs.array())
        continue;
      for (std::size_t run = 0; run < runs.items().size(); ++run) {
        const auto &actions = runs.items()[run]["actions"];
        if (!actions.array())
          continue;
        for (std::size_t index = 0; index < actions.items().size(); ++index) {
          const auto &action = actions.items()[index];
          const auto &control = action["action_control"];
          if (action.str("kind") != "action.end" || action.str("semantic") == "model" ||
              !std::holds_alternative<bool>(action["guard_only"].value) ||
              !action["guard_only"].boolean() ||
              !std::holds_alternative<bool>(action["executed"].value) ||
              action["executed"].boolean() || !control.object() ||
              control.str("decision") != "allow" || control.str("query_event_id").empty() ||
              control.str("target_scope").empty() || action.str("operation").empty() ||
              !control.num("prior_operation_rounds", 0))
            continue;
          const auto key = std::make_pair(control.str("target_scope"), action.str("operation"));
          const auto rounds = control.num("prior_operation_rounds", 0);
          const Json binding{{"operation", key.second},
                             {"target_scope", key.first},
                             {"prior_operation_rounds", rounds},
                             {"evidence_ref", "/feedback/" + std::to_string(row) +
                                                  "/trial/evidence/runs/" + std::to_string(run) +
                                                  "/actions/" + std::to_string(index)}};
          if (control.has("reviewed_budget_prior") &&
              control.num("reviewed_budget_prior", 0) == rounds &&
              rounds >= required_attempt_floor(input, binding) &&
              (!reviewed.count(key) || reviewed[key].num("prior_operation_rounds", 0) > rounds)) {
            reviewed[key] = binding;
            reviewed[key]["reviewed_redundant_suffix"] = true;
          }
          const auto previous = observed.find(key);
          if (previous != observed.end() &&
              previous->second.num("prior_operation_rounds", 0) >= rounds)
            continue;
          // 保留某个真实观察的最大值和原位置，不把不同 Run 的轮数相加。
          observed[key] = binding;
        }
      }
    }
    for (const auto &entry : observed) {
      legacy_action_candidates.push_back(entry.second);
      action_candidates.push_back(reviewed.count(entry.first) ? reviewed[entry.first]
                                                              : entry.second);
    }
    std::stable_sort(
        action_candidates.begin(), action_candidates.end(), [](const auto &a, const auto &b) {
          if (a.flag("reviewed_redundant_suffix", false) !=
              b.flag("reviewed_redundant_suffix", false))
            return a.flag("reviewed_redundant_suffix", false);
          return a.num("prior_operation_rounds", 0) > b.num("prior_operation_rounds", 0);
        });
    std::stable_sort(legacy_action_candidates.begin(), legacy_action_candidates.end(),
                     [](const auto &a, const auto &b) {
                       return a.num("prior_operation_rounds", 0) >
                              b.num("prior_operation_rounds", 0);
                     });
  }

  if (iteration_cap && selected("loop_rules") && input["feedback"].array()) {
    std::map<std::pair<std::string, std::string>, std::size_t> highest_keep;
    for (const auto &feedback : input["feedback"].items()) {
      const auto &runs = feedback["trial"]["evidence"]["runs"];
      if (!runs.array())
        continue;
      for (const auto &run : runs.items()) {
        if (!run["actions"].array())
          continue;
        for (const auto &action : run["actions"].items()) {
          const auto &control = action["loop_control"];
          if (action.str("kind") != "loop.decision" || !control.object() ||
              control.str("target_scope").empty() || action.str("actor_id").empty() ||
              action["iteration_assessment"].str("decision") != "keep")
            continue;
          auto &value = highest_keep[{control.str("target_scope"), action.str("actor_id")}];
          value = std::max(value, control.num("iteration", 0));
        }
      }
    }
    std::map<std::pair<std::string, std::string>, Json> observed;
    const auto minimum = input["controls"]["limits"].num("min_iterations", 2);
    for (std::size_t row = 0; row < input["feedback"].items().size(); ++row) {
      const auto &runs = input["feedback"].items()[row]["trial"]["evidence"]["runs"];
      if (!runs.array())
        continue;
      for (std::size_t run = 0; run < runs.items().size(); ++run) {
        const auto &actions = runs.items()[run]["actions"];
        if (!actions.array())
          continue;
        for (std::size_t index = 0; index < actions.items().size(); ++index) {
          const auto &action = actions.items()[index];
          const auto &control = action["loop_control"];
          const auto proposed = control.str("proposed_decision", "continue");
          if (action.str("kind") != "loop.decision" || !control.object() ||
              control.flag("pending_actions", false) ||
              !iteration_cap_scope_available(input["controls"], action) ||
              (proposed != "continue" && proposed != "replan") ||
              !control.has("reviewed_waste_after") || control.str("target_scope").empty() ||
              action.str("actor_id").empty())
            continue;
          const auto iteration = control.num("iteration", 0);
          const auto waste_after = control.num("reviewed_waste_after", 0);
          const auto key = std::make_pair(control.str("target_scope"), action.str("actor_id"));
          const auto floor =
              required_iteration_floor(input, control, action.str("actor_id"), highest_keep[key]);
          if (iteration <= std::max({minimum, waste_after, floor}))
            continue;
          const auto previous = observed.find(key);
          if (previous != observed.end() &&
              previous->second.num("observed_iteration", 0) >= iteration)
            continue;
          observed[key] =
              Json{{"actor_id", key.second},
                   {"target_scope", key.first},
                   {"observed_iteration", iteration},
                   {"reviewed_waste_after", waste_after},
                   {"required_iteration_floor", floor},
                   {"highest_reviewed_keep_iteration", highest_keep[key]},
                   {"evidence_ref", "/feedback/" + std::to_string(row) + "/trial/evidence/runs/" +
                                        std::to_string(run) + "/actions/" + std::to_string(index)}};
          if (control.flag("require_finalization", false))
            observed[key]["require_finalization"] = true;
        }
      }
    }
    for (const auto &entry : observed)
      loop_candidates.push_back(entry.second);
    std::stable_sort(loop_candidates.begin(), loop_candidates.end(),
                     [](const auto &a, const auto &b) {
                       return a.num("observed_iteration", 0) > b.num("observed_iteration", 0);
                     });
  }

  auto &editable = packet["controls"]["editable_bindings"];
  editable = Json::Array{};
  const auto add_editable = [&](Json binding, const char *component, const char *primitive) {
    binding["component"] = component;
    binding["primitive_id"] = primitive;
    const auto binding_budget = control_binding_budget(packet, binding);
    if (binding_budget.object()) {
      binding["budget"] = binding_budget;
      if (binding["budget"].num("min", 0) > binding["budget"].num("max", 0))
        return false;
    }
    binding["binding_id"] = control_binding_id(packet, binding);
    editable.push(std::move(binding));
    if (packet.dump().size() > byte_limit)
      editable.items().pop_back();
    return true;
  };
  bool action_budget_feasible = false;
  for (std::size_t index = 0; editable.items().size() < 6 &&
                              (index < repeat_failure_candidates.size() ||
                               index < schedule_candidates.size() ||
                               index < action_candidates.size() ||
                               index < loop_candidates.size());
       ++index) {
    if (editable.items().size() < 6 && index < repeat_failure_candidates.size())
      add_editable(repeat_failure_candidates[index], "action_rules", "repeat_failed_operation");
    if (editable.items().size() < 6 && index < schedule_candidates.size())
      add_editable(schedule_candidates[index], "schedule_rules", "order_operations");
    if (editable.items().size() < 6 && index < action_candidates.size())
      action_budget_feasible |=
          add_editable(action_candidates[index], "action_rules", "operation_budget");
    if (editable.items().size() < 6 && index < loop_candidates.size())
      add_editable(loop_candidates[index], "loop_rules", "bounded_iteration_cap");
  }
  const bool action_budget_editable =
      editable.array() &&
      std::any_of(editable.items().begin(), editable.items().end(), [](const auto &binding) {
        return binding.str("component") == "action_rules" &&
               binding.str("primitive_id") == "operation_budget";
      });
  const bool loop_budget_editable =
      editable.array() &&
      std::any_of(editable.items().begin(), editable.items().end(), [](const auto &binding) {
        return binding.str("component") == "loop_rules" &&
               binding.str("primitive_id") == "bounded_iteration_cap";
      });
  const bool has_editable_bindings = !editable.items().empty();
  if (!has_editable_bindings)
    packet["controls"].fields().erase("editable_bindings");
  // 只有真正生成 typed binding 时才展示兼容视图。若预算上下界无法形成合法
  // binding，继续展示 operation_budget 会诱导模型引用不存在的绑定。
  if (budget && action_budget_editable && !legacy_action_candidates.empty()) {
    auto &bindings = (*budget)["observed_bindings"];
    bindings = Json::Array{};
    for (const auto &binding : legacy_action_candidates) {
      bindings.push(binding);
      if (packet.dump().size() > byte_limit)
        bindings.items().pop_back();
      if (bindings.items().size() == 4)
        break;
    }
    if (bindings.items().empty())
      budget->fields().erase("observed_bindings");
  }
  if (budget && selected("action_rules") && !action_budget_feasible) {
    auto &primitives = packet["controls"]["action_primitives"].items();
    primitives.erase(std::remove_if(primitives.begin(), primitives.end(), [](const auto &value) {
                       return value.str("primitive_id") == "operation_budget";
                     }),
                     primitives.end());
    // binding_id 必须基于最终发送包生成；删除不可用原语后同步重算，避免其他
    // 组件的合法绑定被误判为 stale_binding_id。
    if (has_editable_bindings)
      for (auto &binding : packet["controls"]["editable_bindings"].items())
        binding["binding_id"] = control_binding_id(packet, binding);
  }
  // 轮次上限和动作次数一样只能通过本次 typed binding 修改。若完整必要工作下界、
  // 父规则或输入预算使 binding 不可用，不再向模型展示一个本请求无法执行的原语；
  // no-progress、失败恢复等独立 loop 能力仍原样保留。
  if (iteration_cap && selected("loop_rules") && !loop_budget_editable) {
    auto &primitives = packet["controls"]["loop_primitives"].items();
    primitives.erase(std::remove_if(primitives.begin(), primitives.end(), [](const auto &value) {
                       return value.str("primitive_id") == "bounded_iteration_cap";
                     }),
                     primitives.end());
    // binding_id 覆盖整个最终请求包；即使保留下来的是 action/schedule binding，
    // 删除 loop 原语后也必须同步更新，避免合法更新被误判为 stale_binding_id。
    if (has_editable_bindings)
      for (auto &binding : packet["controls"]["editable_bindings"].items())
        binding["binding_id"] = control_binding_id(packet, binding);
  }
  return packet;
}

// 组包时按最终请求计费，而不是只量尚未添加绑定的正文。绑定、兼容视图和一次纠错
// 的诊断都占同一个输入预算；预先量出开销，避免最后静默删掉真正可修改的控制位置。
// 此处只计算字节数，不裁剪原轨迹，也不增加宿主授权或远程模型调用。
std::size_t reflection_input_bytes(const Json &input) {
  auto prepared = observed_operation_bindings(input, std::numeric_limits<std::size_t>::max());
  auto maximum = prepared.dump().size();
  if (!prepared["controls"].object())
    return maximum;
  auto repair = prepared;
  repair.fields().erase("parent_version");
  repair["phase"] = "repair_component_selection";
  repair["error"] = "control_failure_replan_not_authorized";
  maximum = std::max(maximum, repair.dump().size());
  if (prepared["controls"]["editable_bindings"].array()) {
    repair["phase"] = "repair_control_binding";
    repair["error"] = "control_binding_invalid";
    for (const auto &binding : prepared["controls"]["editable_bindings"].items()) {
      // 采用解析器最长的固定诊断码；只按数值越界预留会漏掉字段错误等更长诊断。
      repair["binding_error"] = Json{{"reason", "base_loop_missing_or_ambiguous"},
                                     {"update_index", 5}, {"budget", binding["budget"]}};
      maximum = std::max(maximum, repair.dump().size());
    }
  }
  return maximum;
}

// Review the selected synthesis against one fact row per training task and a real
// failure/success pair. The reduce stage has already read every map proposal;
// sending all of them again duplicates tens of KB of task facts and can prevent
// the counterexample review from running at all.
// Select relevant real contrast cases without dropping the full-pool outcome ledger.
Json select_full_pool_review_cases(const Json &feedback, const Json &support) {
  Json result = Json::Object{};
  std::set<std::string> supported;
  if (support["task_ids"].array())
    for (const auto &id : support["task_ids"].items())
      supported.insert(id.text());
  std::tuple<std::size_t, std::size_t, std::size_t> failure_score{}, success_score{};
  for (const auto &row : feedback.items()) {
    const auto &trial = row["trial"];
    std::size_t related = 0, needed = 0, longest = 0, matched = 0;
    if (trial["evidence"]["runs"].array())
      for (const auto &run : trial["evidence"]["runs"].items())
        if (run["actions"].array())
          for (const auto &action : run["actions"].items()) {
        const bool actor = support["actors"].has(action.str("actor_id"));
        if (actor) ++related;
        const bool keep = action.flag("reviewed_required_action", false) ||
                          action["iteration_assessment"].str("decision") == "keep";
        const bool final_answer = action.flag("reviewed_final_answer", false) ||
                                   action["loop_control"].flag("reviewed_final_answer", false);
        if (actor && keep && !final_answer) {
          ++needed;
          longest = std::max(longest, static_cast<std::size_t>(
              action.has("iteration")
                  ? proto<v1::TraceEvent>(Json{{"iteration", action["iteration"]}}).iteration()
                  : 0));
        }
        if (support["events"].array())
          for (const auto &event : support["events"].items())
            if (event.str("task_id") == row.str("task_id") &&
              (event.str("event_id") == action.str("event_id") ||
               event.str("event_id") == action["action_control"].str("query_event_id") ||
               event.str("event_id") == action["loop_control"].str("query_event_id")))
            ++matched;
      }
    if (trial.str("verdict") == "passed") {
      const auto score = std::make_tuple(std::size_t(needed > 0), longest, related);
      if (!result["success"].object() || score > success_score) {
        result["success"] = row;
        success_score = score;
      }
    } else {
      const auto score = std::make_tuple(supported.count(row.str("task_id")), matched, related);
      if (!result["failure"].object() || score > failure_score) {
        result["failure"] = row;
        failure_score = score;
      }
    }
  }
  return result;
}

// Retain the successful contrast's actual longest necessary action after the
// normal two-action display compaction. A later pure answer is not a substitute.
bool full_pool_records_abstain(const Json &records) {
  return records.array() && !records.items().empty() &&
      std::any_of(records.items().begin(), records.items().end(), [](const Json &record) {
        return record.flag("valid", true) && record["candidate_values"].object();
      }) &&
      std::all_of(records.items().begin(), records.items().end(), [](const Json &record) {
        return !record.flag("valid", true) || (record["candidate_values"].object() &&
               record["candidate_values"].fields().empty());
      });
}

Json coupling_packet_or_single(const Json &single, const std::function<Json()> &prepare) {
  try {
    return prepare();
  } catch (const Error &error) {
    if (std::string(error.what()) != "gepa_feedback_budget_too_small")
      throw;
    return single;
  }
}

bool coupling_budget_before_maps(const std::string &code, std::size_t calls_before,
                                std::size_t calls_after, const Json &stages) {
  return code == "gepa_feedback_budget_too_small" && calls_before == calls_after &&
         (stages.null() || (stages.array() && stages.items().empty()));
}

Json compact_full_pool_reduction(Json packet) {
  // Maps have already passed source authorization. Retain their actual sources and
  // the selected parent's sources, instead of repeating the entire approved corpus
  // at every reduce level. The complete task outcomes and distinct facts stay intact.
  std::set<std::string> needed;
  std::function<void(const Json &)> sources = [&](const Json &value) {
    if (value.object()) {
      if (!value.str("memory_id").empty()) needed.insert(value.str("memory_id"));
      for (const auto &field : value.fields()) sources(field.second);
    } else if (value.array()) {
      for (const auto &item : value.items()) sources(item);
    }
  };
  sources(packet["components"]);
  for (const auto &component : packet["selected_components"].items())
    if (component.text().rfind("experience:", 0) == 0)
      needed.insert(component.text().substr(11));
  for (auto &record : packet["intermediate_proposals"].items()) {
    sources(record["candidate_values"]);
    if (record["task_facts"].array()) {
      std::set<std::string> seen;
      Json facts = Json::Array{};
      for (const auto &fact : record["task_facts"].items())
        if (seen.insert(fact.dump()).second) facts.push(fact);
      record["task_facts"] = std::move(facts);
    }
  }
  if (packet["experiences"].array() && !needed.empty()) {
    Json retained = Json::Array{};
    for (const auto &source : packet["experiences"].items())
      if (needed.count(source.str("memory_id"))) retained.push(source);
    packet["experiences"] = std::move(retained);
  }
  return packet;
}

Json compact_full_pool_review_case(Json row, const Json &selected, const Json &support) {
  const auto original = row["trial"];
  row["trial"] = compact_control_reflection_detail(original, selected, 2);
  if (original.str("verdict") != "passed")
    return row;
  Json required, required_run;
  std::size_t longest = 0;
  if (original["evidence"]["runs"].array())
    for (const auto &run : original["evidence"]["runs"].items())
      if (run["actions"].array())
        for (const auto &action : run["actions"].items()) {
      const bool keep = action.flag("reviewed_required_action", false) ||
                        action["iteration_assessment"].str("decision") == "keep";
      if (!support["actors"].has(action.str("actor_id")) || !keep ||
          action.flag("reviewed_final_answer", false) ||
          action["loop_control"].flag("reviewed_final_answer", false))
        continue;
      const auto iteration = action.has("iteration")
          ? proto<v1::TraceEvent>(Json{{"iteration", action["iteration"]}}).iteration() : 0;
      if (required.null() || iteration > longest) {
        required = action;
        required_run = run["run_id"];
        longest = iteration;
      }
    }
  if (!required.object())
    return row;
  auto &runs = row["trial"]["evidence"]["runs"];
  for (auto &run : runs.items())
    if (run["run_id"].dump() == required_run.dump()) {
      for (const auto &action : run["actions"].items())
        if (action.dump() == required.dump())
          return row;
      run["actions"].push(required);
      row["trial"]["reflection_actions_selected"] =
          row["trial"].num("reflection_actions_selected", 0) + 1;
      return row;
    }
  runs.push(Json{{"run_id", required_run}, {"actions", Json::Array{required}}});
  row["trial"]["reflection_actions_selected"] =
      row["trial"].num("reflection_actions_selected", 0) + 1;
  return row;
}

Json compact_full_pool_review(Json review, const Json &selection,
                              std::size_t input_limit) {
  Json chosen = Json::Array{};
  std::set<std::string> selected_ids;
  if (selection["selected_proposals"].object())
    for (const auto &entry : selection["selected_proposals"].fields())
      if (entry.second.string() && !entry.second.text().empty())
        selected_ids.insert(entry.second.text());
  if (review["intermediate_proposals"].array())
    for (const auto &proposal : review["intermediate_proposals"].items())
      if (selected_ids.count(proposal.str("id"))) {
        auto visible = proposal;
        // The per-task facts are already represented exactly once below.
        visible.fields().erase("task_facts");
        chosen.push(std::move(visible));
      }
  // Keep one evidenced alternative when it fits, so a final review can change its mind.
  Json alternative;
  if (review["intermediate_proposals"].array())
    for (const auto &proposal : review["intermediate_proposals"].items())
      if (!selected_ids.count(proposal.str("id")) &&
          proposal["candidate_values"].object() && !proposal["candidate_values"].fields().empty() &&
          proposal["actual_support"].object() &&
          (alternative.null() || proposal["actual_support"].num("different_tasks", 0) >
                                     alternative["actual_support"].num("different_tasks", 0)))
        alternative = proposal;
  if (alternative.object()) {
    alternative.fields().erase("task_facts");
    review["alternative_proposal_id"] = alternative["id"];
    chosen.push(std::move(alternative));
  }
  review["intermediate_proposals"] = std::move(chosen);
  review.fields().erase("training_batch_summary"); // case_outcomes is the same 32-task ledger.
  if (review["experiences"].array()) {
    Json relevant = Json::Array{};
    for (const auto &experience : review["experiences"].items()) {
      const auto key = "experience:" + experience.str("memory_id");
      if (review["components"].has(key))
        relevant.push(experience);
    }
    review["experiences"] = std::move(relevant);
  }
  // Keep the actual application count and memory IDs for all 32 tasks. The
  // paired raw feedback retains the full receipt/query evidence for causal review.
  if (review["case_outcomes"].array())
    for (auto &outcome : review["case_outcomes"].items()) {
      // The formal ledger exposes these facts at the top level. Keep a differing
      // raw metric, but do not serialize the identical formal value twice per task.
      if (outcome["metrics"].object())
        for (const auto *field : {"quality", "strict_pass_rate", "strict_passed",
                                  "invalid_loops", "invalid_loop_count", "metric_coverage"})
          if (outcome.has(field) && outcome["metrics"].has(field) &&
              outcome[field].dump() == outcome["metrics"][field].dump())
            outcome["metrics"].fields().erase(field);
      auto &applications = outcome["selected_experience_applications"];
      if (!applications.object())
        continue;
      Json compact{{"status", applications["status"]}, {"count", applications["count"]}};
      std::set<std::string> ids;
      if (applications["points"].array())
        for (const auto &point : applications["points"].items())
          if (!point.str("memory_id").empty())
            ids.insert(point.str("memory_id"));
      compact["memory_ids"] = Json::Array{};
      for (const auto &id : ids)
        compact["memory_ids"].push(id);
      applications = std::move(compact);
    }
  // Preserve every task, goal status, real pair, and the controls' necessary
  // action/loop lower bounds. Only shorten diagnostic prose if the byte envelope
  // still needs room; never delete those facts to hide an over-budget request.
  if (reflection_input_bytes(review) > input_limit && review["case_outcomes"].array())
    for (auto &outcome : review["case_outcomes"].items())
      if (outcome["goal_completion"]["issues"].array())
        for (auto &issue : outcome["goal_completion"]["issues"].items())
          issue["reason"] = diagnostic_excerpt(issue.str("reason"), 128);
  if (reflection_input_bytes(review) > input_limit)
    review.fields().erase("prior_search_rejections");
  // Each real contrast case can carry many applied-memory receipts with the
  // same long query context. Keep every receipt and its causal identifiers;
  // shorten only diagnostic excerpts, not task goals, action facts or bounds.
  if (reflection_input_bytes(review) > input_limit && review["feedback"].array())
    for (auto &row : review["feedback"].items()) {
      auto &applications = row["selected_experience_applications"];
      if (!applications["points"].array())
        continue;
      bool shortened = false;
      for (auto &point : applications["points"].items())
        for (const auto *field : {"task_excerpt", "state_excerpt"})
          if (point[field].string()) {
            const auto before = point[field].text();
            point[field] = diagnostic_excerpt(before, 128);
            shortened |= point[field].text() != before;
          }
      if (shortened)
        applications["context_excerpts_truncated"] = true;
    }
  if (reflection_input_bytes(review) > input_limit && review["alternative_proposal_id"].string()) {
    const auto alternative_id = review.str("alternative_proposal_id");
    auto &proposals = review["intermediate_proposals"].items();
    proposals.erase(std::remove_if(proposals.begin(), proposals.end(), [&](const auto &proposal) {
                      return proposal.str("id") == alternative_id;
                    }), proposals.end());
    review.fields().erase("alternative_proposal_id");
  }
  require(reflection_input_bytes(review) <= input_limit,
          "gepa_feedback_budget_too_small");
  return review;
}

// 累计每个反思阶段真实送入模型的题目与字节数。reduce/review 通常不带原始
// feedback，不能覆盖 map 阶段的覆盖题数；重复阶段键只合并题目集合，但保留每次
// 实际请求的字节消耗，便于断点续跑审计。
void record_reflection_coverage(Json &node, const std::string &suffix,
                                const Json &prepared) {
  if (!node["reflection_inputs"].object())
    node["reflection_inputs"] = Json::Object{};
  const auto key = suffix.empty() ? std::string("single") : suffix;
  auto &stage = node["reflection_inputs"][key];
  if (!stage.object())
    stage = Json{{"phase", prepared.str("phase", key == "single" ? "single" : "reflection")},
                 {"task_ids", Json::Array{}},
                 {"input_bytes", 0},
                 {"attempts", 0}};
  std::set<std::string> stage_tasks;
  if (stage["task_ids"].array())
    for (const auto &task : stage["task_ids"].items())
      if (task.string() && !task.text().empty())
        stage_tasks.insert(task.text());
  if (prepared["feedback"].array())
    for (const auto &row : prepared["feedback"].items())
      if (row["task_id"].string() && !row.str("task_id").empty())
        stage_tasks.insert(row.str("task_id"));
  stage["phase"] = prepared.str("phase", stage.str("phase", "reflection"));
  stage["task_ids"] = Json::Array{};
  for (const auto &task : stage_tasks)
    stage["task_ids"].push(task);
  stage["input_bytes"] = stage.num("input_bytes", 0) + prepared.dump().size();
  stage["attempts"] = stage.num("attempts", 0) + 1;

  std::set<std::string> all_tasks;
  std::size_t total_bytes = 0;
  for (const auto &entry : node["reflection_inputs"].fields()) {
    total_bytes += entry.second.num("input_bytes", 0);
    if (entry.second["task_ids"].array())
      for (const auto &task : entry.second["task_ids"].items())
        if (task.string() && !task.text().empty())
          all_tasks.insert(task.text());
  }
  node["reflection_feedback_count"] = all_tasks.size();
  node["reflection_input_bytes"] = total_bytes;
}

// 为 map 结果保留可归因的逐题事实，避免 reduce 只能看到一段自由文本理由。
// 字段仅从当前训练 group.feedback 提取；不合成父子质量差，也不读取 val/holdout。
Json reflection_task_facts(const Json &feedback_rows, const Json &selected) {
  Json facts = Json::Array{};
  if (!feedback_rows.array())
    return facts;
  const auto chosen = [&](const char *name) {
    return std::any_of(selected.items().begin(), selected.items().end(),
                       [&](const auto &item) { return item.text() == name; });
  };
  for (const auto &row : feedback_rows.items()) {
    const auto &trial = row["trial"];
    const auto &metrics = trial["metrics"];
    Json fact{{"task_id", clip(row.str("task_id"), 120)},
              {"verdict", trial["verdict"]},
              {"execution_status", trial["execution_status"]}};
    if (!trial.str("error_code").empty())
      fact["error_code"] = clip(trial.str("error_code"), 64);
    if (trial["observed_tool_failures"].object()) {
      const auto &failures = trial["observed_tool_failures"];
      if (failures.str("status") == "scanned")
        fact["observed_tool_failure_count"] = failures["count"];
      else
        fact["observed_tool_failure_status"] = "unavailable";
    }
    for (const auto *name : {"quality", "strict_pass_rate", "strict_passed", "invalid_loop_count",
                             "metric_coverage", "tokens", "llm_calls"})
      if (metrics.has(name))
        fact[name] = metrics[name];
    if (metrics.has("invalid_loops"))
      fact["invalid_loops"] = metrics["invalid_loops"];
    else if (trial["evidence"]["loop_review"].str("status") == "complete" &&
             trial["evidence"]["loop_review"].has("count"))
      fact["invalid_loops"] = trial["evidence"]["loop_review"]["count"];
    else if (trial["evidence"]["trajectory"]["summary"].has("invalid_loop_lower_bound"))
      fact["invalid_loops_lower_bound"] =
          trial["evidence"]["trajectory"]["summary"]["invalid_loop_lower_bound"];

    if (trial["goal_completion"].object()) {
      Json goal = Json::Object{};
      for (const auto *name : {"quality", "effective_quality", "strict_passed", "coverage", "critical_failures"})
        if (trial["goal_completion"].has(name))
          goal[name] = trial["goal_completion"][name];
      std::vector<const Json *> issues;
      if (trial["goal_completion"]["issues"].array())
        for (const auto &issue : trial["goal_completion"]["issues"].items())
          issues.push_back(&issue);
      std::stable_sort(issues.begin(), issues.end(), [](const auto *left, const auto *right) {
        const bool left_critical = left->flag("critical_failure", false);
        const bool right_critical = right->flag("critical_failure", false);
        if (left_critical != right_critical)
          return left_critical;
        return (left->str("status") != "passed") > (right->str("status") != "passed");
      });
      goal["issues"] = Json::Array{};
      const auto kept = std::min<std::size_t>(2, issues.size());
      for (std::size_t i = 0; i < kept; ++i) {
        const auto &issue = *issues[i];
        Json item = Json::Object{};
        for (const auto *name : {"id", "turn", "status", "critical_failure"})
          if (issue.has(name))
            item[name] = issue[name];
        if (!issue.str("reason").empty())
          item["reason"] = diagnostic_excerpt(issue.str("reason"), 96);
        goal["issues"].push(std::move(item));
      }
      if (issues.size() > kept)
        goal["omitted_issues"] = issues.size() - kept;
      fact["goal"] = std::move(goal);
    }

    std::size_t action_queries = 0, action_executed = 0, action_failures = 0;
    std::size_t loop_queries = 0, loop_rejected = 0, loop_failed = 0, loop_no_progress = 0;
    if (trial["evidence"]["runs"].array())
      for (const auto &run : trial["evidence"]["runs"].items())
        if (run["actions"].array())
          for (const auto &action : run["actions"].items()) {
            if (chosen("action_rules") && action["action_control"].object()) {
              ++action_queries;
              action_executed += action.flag("executed", false);
              action_failures += action.flag("observed_failure", false);
            }
            if (chosen("loop_rules") && action["loop_control"].object()) {
              ++loop_queries;
              loop_rejected += static_cast<std::size_t>(
                  number(action["loop_control"]["rejected_actions"]));
              loop_failed += static_cast<std::size_t>(
                  number(action["loop_control"]["failed_actions"]));
              loop_no_progress += static_cast<std::size_t>(
                  number(action["loop_control"]["observed_no_progress"]));
            }
          }
    if (action_queries || loop_queries)
      fact["control_evidence"] = Json{
          {"action_queries", action_queries}, {"action_executed", action_executed},
          {"action_failures", action_failures}, {"loop_queries", loop_queries},
          {"loop_rejected_actions", loop_rejected}, {"loop_failed_actions", loop_failed},
          {"loop_no_progress", loop_no_progress}};
    if (row["selected_experience_applications"].object()) {
      const auto &application = row["selected_experience_applications"];
      Json summary{{"count", application.num("count", 0)}};
      if (application.has("status"))
        summary["status"] = application["status"];
      std::set<std::string> memory_ids;
      if (application["points"].array())
        for (const auto &point : application["points"].items())
          if (!point.str("memory_id").empty())
            memory_ids.insert(point.str("memory_id"));
      if (!memory_ids.empty()) {
        summary["memory_ids"] = Json::Array{};
        for (const auto &id : memory_ids)
          summary["memory_ids"].push(clip(id, 48));
      }
      fact["experience_applications"] = std::move(summary);
    }

    // 单题事实有严格字节上限，32 题最多约 16 KiB；先压缩自由理由和回执 ID，
    // 再删除次要诊断字段，绝不删除 task_id/verdict/loop-loss 这些审计锚点。
    if (fact.dump().size() > 512 && fact.has("strict_passed") &&
        (fact.num("strict_pass_rate", -1) == 0 || fact.num("strict_pass_rate", -1) == 1) &&
        fact.flag("strict_passed", false) == (fact.num("strict_pass_rate", -1) == 1))
      fact.fields().erase("strict_pass_rate");
    if (fact.dump().size() > 512 && fact.has("invalid_loop_count") &&
        fact["invalid_loops"].dump() == fact["invalid_loop_count"].dump())
      fact.fields().erase("invalid_loops");
    if (fact.dump().size() > 512 && fact["goal"]["issues"].array())
      for (auto &issue : fact["goal"]["issues"].items())
        if (issue.has("reason"))
          issue["reason"] = diagnostic_excerpt(issue.str("reason"), 32);
    if (fact.dump().size() > 512 && fact["experience_applications"].has("memory_ids"))
      fact["experience_applications"].fields().erase("memory_ids");
    if (fact.dump().size() > 512 && fact["experience_applications"].object())
      fact["experience_applications"].fields().erase("status");
    if (fact.dump().size() > 512 && fact["goal"]["issues"].array() &&
        fact["goal"]["issues"].items().size() > 1)
      fact["goal"]["issues"].items().resize(1);
    if (fact.dump().size() > 512 && fact["goal"].object()) {
      fact["goal"].fields().erase("quality");
      fact["goal"].fields().erase("coverage");
    }
    if (fact.dump().size() > 512) {
      fact.fields().erase("tokens");
      fact.fields().erase("llm_calls");
    }
    if (fact.dump().size() > 512 && fact["control_evidence"].object()) {
      fact["control_evidence"].fields().erase("action_queries");
      fact["control_evidence"].fields().erase("action_executed");
      fact["control_evidence"].fields().erase("action_failures");
    }
    if (fact.dump().size() > 512 && fact["goal"]["issues"].array())
      for (auto &issue : fact["goal"]["issues"].items())
        if (issue.has("reason"))
          issue["reason"] = diagnostic_excerpt(issue.str("reason"), 12);
    if (fact.dump().size() > 512)
      fact.fields().erase("goal");
    if (fact.dump().size() > 512)
      fact["task_id"] = clip(fact.str("task_id"), 64);
    require(fact.dump().size() <= 512, "gepa_task_diagnostic_budget_too_small");
    facts.push(std::move(fact));
  }
  return facts;
}

// 应用经过授权的控制绑定更新，校验字段和动作仍由当前宿主支持。
Json resolve_control_binding_updates(const Json &draft, const Json &prepared,
                                     Json *diagnostic = nullptr, Json *provenance = nullptr) {
  if (diagnostic)
    *diagnostic = Json();
  if (provenance)
    *provenance = Json::Array{};
  if (!draft.has("control_binding_updates"))
    return draft;
  std::size_t update_index = 0;
  Json budget;
  const auto check = [&](bool valid, const char *reason) {
    if (valid)
      return;
    if (diagnostic) {
      *diagnostic = Json{{"reason", reason}, {"update_index", update_index}};
      if (budget.object())
        (*diagnostic)["budget"] = budget;
    }
    throw Error("control_binding_invalid");
  };
  check(!draft.has("control_updates") && !draft.has("controls"), "mixed_protocol");
  check(draft["control_binding_updates"].array(), "updates_not_array");
  const auto &bindings = prepared["controls"]["editable_bindings"];
  // 空更新表示合法弃权，不需要凭空创建可编辑对象；非空修改仍必须绑定本次输入。
  check(draft["control_binding_updates"].items().empty() || bindings.array(),
        "bindings_unavailable");
  Json result = draft;
  result.fields().erase("control_binding_updates");
  result["control_updates"] = Json::Array{};
  std::set<std::string> used;
  const auto scope = [&](const std::string &memory_id) {
    std::string value;
    std::size_t matches = 0;
    if (prepared["experiences"].array())
      for (const auto &source : prepared["experiences"].items())
        if (source.str("memory_id") == memory_id) {
          value = source.str("scope");
          ++matches;
        }
    if (prepared["controls"]["control_sources"].array())
      for (const auto &source : prepared["controls"]["control_sources"].items())
        if (source.str("memory_id") == memory_id) {
          value = source.str("scope");
          ++matches;
        }
    check(matches == 1 && !value.empty(), "source_missing_or_ambiguous");
    return value;
  };
  const auto target_type = [&](const std::string &memory_id) {
    std::string value;
    std::size_t matches = 0;
    if (prepared["controls"]["control_sources"].array())
      for (const auto &source : prepared["controls"]["control_sources"].items())
        if (source.str("memory_id") == memory_id) {
          value = source.str("target_type");
          ++matches;
        }
    check(matches == 1 && !value.empty(), "source_type_missing_or_ambiguous");
    return value;
  };
  const auto whole = [&](const Json &value, const char *field) {
    check(std::holds_alternative<double>(value[field].value), "budget_not_integer");
    const auto numeric = std::get<double>(value[field].value);
    check(std::isfinite(numeric) && std::floor(numeric) == numeric, "budget_not_integer");
    check(numeric >= 0 && numeric <= 10000, "budget_out_of_range");
    return static_cast<std::size_t>(numeric);
  };
  for (const auto &update : draft["control_binding_updates"].items()) {
    budget = Json();
    check(update.object(), "update_not_object");
    const std::set<std::string> allowed{"binding_id", "memory_id", "max_attempts", "max_iterations",
                                        "task_terms", "exclude_terms", "reason"};
    for (const auto &field : update.fields())
      check(allowed.count(field.first), "unknown_update_field");
    check(update["binding_id"].string() && update["memory_id"].string() &&
              update["reason"].string() && !update.str("binding_id").empty() &&
              !update.str("memory_id").empty() && !update.str("reason").empty(),
          "required_field_invalid");
    check(used.insert(update.str("binding_id")).second, "duplicate_binding_update");
    const Json *binding = nullptr;
    for (const auto &candidate : bindings.items())
      if (candidate.str("binding_id") == update.str("binding_id")) {
        check(!binding, "binding_ambiguous");
        binding = &candidate;
      }
    check(binding != nullptr, "unknown_binding_id");
    check(control_binding_id(prepared, *binding) == binding->str("binding_id"), "stale_binding_id");
    check(scope(update.str("memory_id")) == binding->str("target_scope"), "source_scope_mismatch");
    const auto component = binding->str("component"), primitive = binding->str("primitive_id");
    check((component == "action_rules" || component == "loop_rules" ||
           component == "schedule_rules") &&
              prepared["components"][component].array(),
          "component_unavailable");
    budget = control_binding_budget(prepared, *binding);
    Json value, change{{"component", component}};
    std::size_t matched = 0, matched_index = 0;
    if (component == "action_rules" && primitive == "repeat_failed_operation") {
      check(!update.has("max_attempts") && !update.has("max_iterations"),
            "budget_field_mismatch");
      check(!binding->str("task_id").empty() && !binding->str("evidence_ref").empty() &&
                !binding->str("operation").empty() &&
                (*binding)["arguments"].object(),
            "binding_target_missing");
      check(binding->flag("observed_failure", false) &&
                binding->num("prior_failures", 0) >= 2 &&
                !(*binding)["arguments"].str("action").empty(),
            "failure_evidence_invalid");
      for (std::size_t index = 0; index < prepared["components"][component].items().size();
           ++index) {
        const auto &rule = prepared["components"][component].items()[index];
        const bool repeat = rule.str("operation") == binding->str("operation") &&
                            rule.num("max_failed_attempts", 0) >= 2 &&
                            !rule.num("max_attempts", 0) &&
                            (!rule["failure_key_arguments"].array() ||
                             rule["failure_key_arguments"].items().empty()) &&
                            (!rule["terminal_error_codes"].array() ||
                             rule["terminal_error_codes"].items().empty());
        if (repeat && scope(rule.str("memory_id")) == binding->str("target_scope")) {
          ++matched;
          matched_index = index;
        }
      }
      value =
          matched == 1 ? prepared["components"][component].items()[matched_index] : Json::Object{};
      value["memory_id"] = update.str("memory_id");
      value["primitive_id"] = primitive;
      value["operation"] = binding->str("operation");
      value["required_arguments"] = Json::Array{};
      value["max_failed_attempts"] = 2;
      value["reason"] = update.str("reason");
      if (provenance) {
        auto cited = *binding;
        cited["memory_id"] = update["memory_id"];
        provenance->push(std::move(cited));
      }
    } else if (component == "action_rules" && primitive == "operation_budget") {
      check(update.has("max_attempts") && !update.has("max_iterations"), "budget_field_mismatch");
      check(!binding->str("operation").empty(), "binding_target_missing");
      const auto attempts = whole(update, "max_attempts");
      check(attempts >= budget.num("min", 1) && attempts <= budget.num("max", 0),
            "budget_out_of_range");
      for (std::size_t index = 0; index < prepared["components"][component].items().size();
           ++index) {
        const auto &rule = prepared["components"][component].items()[index];
        if (rule.str("operation") == binding->str("operation") && rule.num("max_attempts", 0) &&
            scope(rule.str("memory_id")) == binding->str("target_scope")) {
          ++matched;
          matched_index = index;
        }
      }
      check(matched != 1 ||
                attempts <=
                    prepared["components"][component].items()[matched_index].num("max_attempts", 0),
            "budget_out_of_range");
      value =
          matched == 1 ? prepared["components"][component].items()[matched_index] : Json::Object{};
      value["memory_id"] = update.str("memory_id");
      value["primitive_id"] = primitive;
      value["operation"] = binding->str("operation");
      value["max_attempts"] = attempts;
      if (!value["attempt_key_arguments"].array())
        value["attempt_key_arguments"] = Json::Array{};
      value["reason"] = update.str("reason");
    } else if (component == "loop_rules" && primitive == "bounded_iteration_cap") {
      check(update.has("max_iterations") && !update.has("max_attempts"), "budget_field_mismatch");
      check(!binding->str("actor_id").empty() && binding->has("reviewed_waste_after"),
            "binding_target_missing");
      const auto iterations = whole(update, "max_iterations");
      check(iterations >= budget.num("min", 0) && iterations <= budget.num("max", 0) &&
                iterations < binding->num("observed_iteration", 0),
            "budget_out_of_range");
      for (std::size_t index = 0; index < prepared["components"][component].items().size();
           ++index) {
        const auto &rule = prepared["components"][component].items()[index];
        if (rule.str("actor_id") == binding->str("actor_id") &&
            scope(rule.str("memory_id")) == binding->str("target_scope")) {
          ++matched;
          matched_index = index;
        }
      }
      const Json *base = nullptr;
      std::size_t exact = 0, generic = 0;
      if (prepared["controls"]["base_loop_rules"].array())
        for (const auto &rule : prepared["controls"]["base_loop_rules"].items()) {
          if (rule.str("target_scope") != binding->str("target_scope") ||
              (!rule.str("actor_id").empty() && rule.str("actor_id") != binding->str("actor_id")))
            continue;
          if (rule.str("actor_id").empty()) {
            ++generic;
            if (!exact)
              base = &rule;
          } else {
            ++exact;
            base = &rule;
          }
        }
      check(exact == 1 || (!exact && generic == 1), "base_loop_missing_or_ambiguous");
      check(matched != 1 ||
                iterations <= prepared["components"][component].items()[matched_index].num(
                                  "max_iterations", 0),
            "budget_out_of_range");
      value =
          matched == 1 ? prepared["components"][component].items()[matched_index] : Json::Object{};
      value["memory_id"] = update.str("memory_id");
      value["actor_id"] = binding->str("actor_id");
      value["max_iterations"] = iterations;
      value["reason"] = update.str("reason");
      if (matched == 0) {
        value["max_no_progress"] = base->num("max_no_progress", 0);
        value["max_replans"] = base->num("max_replans", 0);
        value["replan_on_failure"] = base->flag("replan_on_failure", false);
        for (const auto *field : {"error_codes", "after_action_rejection", "require_finalization"})
          if (base->has(field))
            value[field] = (*base)[field];
      }
      if (binding->flag("require_finalization", false))
        value["require_finalization"] = true;
    } else if (component == "schedule_rules" && primitive == "order_operations") {
      check(!update.has("max_attempts") && !update.has("max_iterations"),
            "budget_field_mismatch");
      check(!binding->str("before_operation").empty() &&
                !binding->str("after_operation").empty() &&
                !binding->str("task_text").empty(),
            "binding_target_missing");
      check(target_type(update.str("memory_id")) ==
                "CANDIDATE_TARGET_TYPE_SKILL_ORCHESTRATION",
            "source_type_mismatch");
      check(update["task_terms"].array() && !update["task_terms"].items().empty() &&
                update["task_terms"].items().size() <= 8,
            "schedule_terms_invalid");
      auto lower = [](std::string text) {
        std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) {
          return static_cast<char>(c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c);
        });
        return text;
      };
      const auto task = lower(binding->str("task_text"));
      Json terms = Json::Array{}, excludes = Json::Array{};
      std::set<std::string> unique;
      for (const auto &term : update["task_terms"].items()) {
        check(term.string() && !term.text().empty() && term.text().size() <= 256 &&
                  task.find(lower(term.text())) != std::string::npos &&
                  unique.insert(term.text()).second,
              "schedule_terms_invalid");
        terms.push(term);
      }
      unique.clear();
      if (update.has("exclude_terms")) {
        check(update["exclude_terms"].array() && update["exclude_terms"].items().size() <= 8,
              "schedule_terms_invalid");
        for (const auto &term : update["exclude_terms"].items()) {
          check(term.string() && !term.text().empty() && term.text().size() <= 256 &&
                    unique.insert(term.text()).second,
                "schedule_terms_invalid");
          excludes.push(term);
        }
      }
      for (std::size_t index = 0; index < prepared["components"][component].items().size();
           ++index) {
        const auto &rule = prepared["components"][component].items()[index];
        if (rule.str("before_operation") == binding->str("before_operation") &&
            rule.str("after_operation") == binding->str("after_operation") &&
            rule["task_terms"].dump() == terms.dump() &&
            rule["exclude_terms"].dump() == excludes.dump() &&
            scope(rule.str("memory_id")) == binding->str("target_scope")) {
          ++matched;
          matched_index = index;
        }
      }
      value =
          matched == 1 ? prepared["components"][component].items()[matched_index] : Json::Object{};
      value["memory_id"] = update.str("memory_id");
      value["before_operation"] = binding->str("before_operation");
      value["after_operation"] = binding->str("after_operation");
      value["require_success"] = binding->flag("require_success", false);
      value["task_terms"] = std::move(terms);
      if (!excludes.items().empty())
        value["exclude_terms"] = std::move(excludes);
      else
        value.fields().erase("exclude_terms");
      value["reason"] = update.str("reason");
    } else
      check(false, "unsupported_primitive");
    check(matched <= 1, "existing_rules_ambiguous");
    change["op"] = matched == 1 ? "set" : "add";
    if (matched == 1)
      change["index"] = matched_index;
    change["value"] = std::move(value);
    result["control_updates"].push(std::move(change));
    ++update_index;
  }
  return result;
}

// 模型不能用旧的 concrete 草稿绕开 typed binding 中已经核对的执行对象。
// 次数/轮次预算必须使用合并过必要工作下界的绑定；存在同题、同批 schedule 绑定时，
// 编排规则也必须引用该绑定，不能自由填写操作名或拼接不同任务的触发词。
// 已发布旧版本的加载路径不经过此函数，因此兼容性保持不变。
void require_typed_budget_updates(const Json &draft, const Json &prepared,
                                  Json *diagnostic = nullptr) {
  if (diagnostic)
    *diagnostic = Json();
  if (!draft["control_updates"].array())
    return;
  const auto memory_scope = [&](const std::string &memory_id) {
    std::string scope;
    for (const auto *sources : {&prepared["experiences"],
                                &prepared["controls"]["control_sources"]})
      if (sources->array())
        for (const auto &source : sources->items())
          if (source.str("memory_id") == memory_id && scope.empty())
            scope = source.str("scope");
    return scope;
  };
  std::size_t update_index = 0;
  for (const auto &update : draft["control_updates"].items()) {
    if (!update.object() || update.str("op") == "remove" || !update["value"].object()) {
      ++update_index;
      continue;
    }
    const auto component = update.str("component");
    const auto &value = update["value"];
    bool schedule_binding = false;
    if (component == "schedule_rules" && prepared["controls"]["editable_bindings"].array())
      for (const auto &binding : prepared["controls"]["editable_bindings"].items())
        schedule_binding |= binding.str("component") == "schedule_rules" &&
                            binding.str("primitive_id") == "order_operations";
    if (schedule_binding) {
      if (diagnostic)
        *diagnostic =
            Json{{"reason", "typed_binding_required"}, {"update_index", update_index}};
      throw Error("control_binding_invalid");
    }
    const auto empty_array = [](const Json &item) {
      return item.null() || (item.array() && item.items().empty());
    };
    const bool repeat_failure =
        component == "action_rules" &&
        (value.str("primitive_id") == "repeat_failed_operation" ||
         (value.num("max_failed_attempts", 0) >= 2 && !value.num("max_attempts", 0) &&
          empty_array(value["required_arguments"]) &&
          empty_array(value["failure_key_arguments"]) &&
          empty_array(value["terminal_error_codes"]))) &&
        !value.str("operation").empty() && value.str("operation") != "*";
    if (repeat_failure) {
      const auto scope = memory_scope(value.str("memory_id"));
      bool matching_failure_binding = false;
      if (prepared["controls"]["editable_bindings"].array())
        for (const auto &binding : prepared["controls"]["editable_bindings"].items())
          matching_failure_binding |=
              binding.str("component") == "action_rules" &&
              binding.str("primitive_id") == "repeat_failed_operation" && !scope.empty() &&
              binding.str("target_scope") == scope &&
              binding.str("operation") == value.str("operation");
      if (diagnostic)
        *diagnostic = Json{{"reason", matching_failure_binding ? "typed_binding_required"
                                                               : "failure_binding_unavailable"},
                           {"update_index", update_index}};
      throw Error("control_binding_invalid");
    }
    bool budget_change = false;
    bool matching_typed_budget = false;
    if (component == "action_rules" && value.num("max_attempts", 0)) {
      std::size_t current = 0;
      const auto scope = memory_scope(value.str("memory_id"));
      if (prepared["components"][component].array())
        for (const auto &rule : prepared["components"][component].items())
          if (rule.str("operation") == value.str("operation") &&
              (scope.empty() || memory_scope(rule.str("memory_id")) == scope))
            current = std::max(current, rule.num("max_attempts", 0));
      budget_change = current != value.num("max_attempts", 0);
    }
    if (component == "loop_rules" && value.num("max_iterations", 0)) {
      std::size_t current = 0;
      const auto scope = memory_scope(value.str("memory_id"));
      if (prepared["components"][component].array())
        for (const auto &rule : prepared["components"][component].items())
          if (rule.str("actor_id") == value.str("actor_id") &&
              (scope.empty() || memory_scope(rule.str("memory_id")) == scope))
            current = std::max(current, rule.num("max_iterations", 0));
      if (prepared["controls"]["base_loop_rules"].array())
        for (const auto &rule : prepared["controls"]["base_loop_rules"].items())
          if ((scope.empty() || rule.str("target_scope") == scope) &&
              (rule.str("actor_id").empty() ||
               rule.str("actor_id") == value.str("actor_id")))
            current = std::max(current, rule.num("max_iterations", 0));
      budget_change = current != value.num("max_iterations", 0);
      // bounded_iteration_cap 只约束它实际绑定的 scope + actor。其他组件（甚至
      // 其他 actor）的 editable_binding 不能把普通的、带收尾保护的 L3 收敛规则
      // 误判成“绕过 typed budget”。若本题确有同目标的已评审浪费后缀，则必须走
      // request-local binding；否则仍交给 control_generation 的父预算、最小轮次、
      // actor/收尾能力校验，不能借 aggressive_controls 生成无作用域的任意截断。
      if (prepared["controls"]["editable_bindings"].array())
        for (const auto &binding : prepared["controls"]["editable_bindings"].items())
          matching_typed_budget |=
              binding.str("component") == "loop_rules" &&
              binding.str("primitive_id") == "bounded_iteration_cap" && !scope.empty() &&
              binding.str("target_scope") == scope &&
              binding.str("actor_id") == value.str("actor_id");
    }
    if (!budget_change) {
      ++update_index;
      continue;
    }
    bool available = false;
    if (prepared["controls"]["editable_bindings"].array())
      for (const auto &binding : prepared["controls"]["editable_bindings"].items())
        available |= binding.str("component") == component;
    if (component == "loop_rules") {
      if (!matching_typed_budget && !value.str("actor_id").empty() &&
          value.flag("require_finalization", false)) {
        ++update_index;
        continue;
      }
      available = matching_typed_budget;
    }
    if (diagnostic)
      *diagnostic = Json{{"reason", available ? "typed_binding_required"
                                                : "budget_binding_unavailable"},
                         {"update_index", update_index}};
    throw Error("control_binding_invalid");
  }
}

// 来源只表示已有批准关联，不把训练评审的新假设冒记为记忆正文中的结论。
Json control_probe_source(const Json &parent, const Json &prepared, const Json &binding,
                          const Report &report) {
  const auto policy = proto<v1::RuntimePolicy>(parent["policy"]);
  std::map<std::string, std::string> approved;
  for (const auto &source : policy.snapshot().experiences()) {
    if (source.target_scope() != binding.str("target_scope"))
      continue;
    for (const auto &ref : policy.options().approved_experiences())
      if (ref.memory_id() == source.memory_id() &&
          ref.content_revision() == source.content_revision())
        approved[source.memory_id()] = source.content_revision();
  }
  const auto eligible = [&](const std::string &id) {
    std::size_t matches = 0;
    for (const auto *sources : {&prepared["experiences"], &prepared["controls"]["control_sources"]})
      if (sources->array())
        for (const auto &source : sources->items())
          matches +=
              source.str("memory_id") == id && source.str("scope") == binding.str("target_scope");
    return approved.count(id) && matches == 1;
  };
  const auto &rules = prepared["components"][binding.str("component")];
  if (rules.array())
    for (const auto &rule : rules.items()) {
      const auto field = binding.str("component") == "action_rules" ? "operation" : "actor_id";
      if (!binding.str(field).empty() && rule.str(field) == binding.str(field) &&
          eligible(rule.str("memory_id")))
        return Json{{"memory_id", rule["memory_id"]}, {"association", "existing_control"}};
    }
  if (!prepared["feedback"].array())
    return Json();
  for (std::size_t row = 0; row < prepared["feedback"].items().size(); ++row) {
    const auto &feedback = prepared["feedback"].items()[row];
    const auto &runs = feedback["trial"]["evidence"]["runs"];
    if (!runs.array())
      continue;
    for (std::size_t run_index = 0; run_index < runs.items().size(); ++run_index) {
      const auto &run = runs.items()[run_index];
      const auto &actions = run["actions"];
      if (!actions.array())
        continue;
      for (std::size_t action_index = 0; action_index < actions.items().size(); ++action_index) {
        if (binding.str("evidence_ref") != "/feedback/" + std::to_string(row) +
                                               "/trial/evidence/runs/" + std::to_string(run_index) +
                                               "/actions/" + std::to_string(action_index))
          continue;
        const auto &anchor = actions.items()[action_index];
        const auto iteration = iteration_identity(run, anchor);
        if (iteration.empty() || anchor.str("actor_id").empty() || anchor.str("event_id").empty())
          return Json();
        for (const auto &trial : report.trials()) {
          if (trial.variant_id() != "candidate" || trial.task_id() != feedback.str("task_id") ||
              trial.execution_status() != "completed")
            continue;
          const auto evidence = json(trial.evidence());
          const auto traces = evidence["traces"].array() ? evidence["traces"]
                                                         : Json(Json::Array{evidence["trace"]});
          for (const auto &trace : traces.items()) {
            if (trace["identity"].str("run_id") != run.str("run_id") || !trace["events"].array())
              continue;
            const auto same_round = [&](const Json &event) {
              return event.str("actor_id") == anchor.str("actor_id") &&
                     iteration_identity(run, event) == iteration;
            };
            bool found_anchor = false;
            std::map<std::string, Json> models, queries;
            for (const auto &event : trace["events"].items()) {
              if (!same_round(event))
                continue;
              found_anchor |= event.str("event_id") == anchor.str("event_id") &&
                              event.str("kind") == anchor.str("kind");
              if (!event.str("step_id").empty() && event.str("kind") == "action.end" &&
                  event["attributes"].str("semantic") == "model" &&
                  event["attributes"].flag("executed", false))
                models[event.str("step_id")] = event;
              const auto &query = event["attributes"]["decision"]["query"];
              if (!event.str("step_id").empty() && !query["advice_query"].str("query_id").empty() &&
                  event["attributes"].str("type") == "policy.query" &&
                  query.str("boundary") == "llm" &&
                  query["advice_query"].str("target_scope") == binding.str("target_scope"))
                queries[event.str("step_id")] = query;
            }
            if (!found_anchor)
              continue;
            for (const auto &event : trace["events"].items()) {
              const auto step = event.str("step_id");
              if (!same_round(event) || event.str("kind") != "advice.applied" ||
                  event["application"].str("status") != "applied" || !models.count(step) ||
                  !queries.count(step) ||
                  event["application"].str("query_id") !=
                      queries.at(step)["advice_query"].str("query_id"))
                continue;
              const auto &receipt = event["attributes"]["receipt"];
              if (receipt.str("boundary") != "llm" || !receipt["experiences"].array())
                continue;
              for (const auto &source : receipt["experiences"].items())
                if (eligible(source.str("memory_id")) &&
                    source.str("content_revision") == approved.at(source.str("memory_id")))
                  return Json{{"memory_id", source["memory_id"]},
                              {"association", "same_iteration_application"},
                              {"application_event_id", event["event_id"]},
                              {"model_event_id", models.at(step)["event_id"]}};
            }
          }
        }
      }
    }
  }
  return Json();
}

// 只保留固定错误与有界数字码；响应正文、模型标识和自由文本不能进入诊断树。
Json reflection_call_diagnostic(const legacy::RawModelResponse &response, std::size_t duration_ms) {
  static const std::set<std::string> fixed{"",
                                           "missing_record",
                                           "request_too_large",
                                           "response_too_large",
                                           "client_initialization_failed",
                                           "header_allocation_failed",
                                           "output_token_limit_reached",
                                           "invalid_api_response",
                                           "transport_exhausted"};
  const auto numeric_code = [&](const std::string &prefix, unsigned low, unsigned high) {
    const auto &code = response.error_code;
    if (code.compare(0, prefix.size(), prefix) != 0 || code.size() <= prefix.size() ||
        code.size() > prefix.size() + 3 || code[prefix.size()] == '0')
      return false;
    unsigned value = 0;
    for (std::size_t i = prefix.size(); i < code.size(); ++i) {
      if (code[i] < '0' || code[i] > '9')
        return false;
      value = value * 10 + static_cast<unsigned>(code[i] - '0');
    }
    return value >= low && value <= high;
  };
  const bool known = fixed.count(response.error_code) || numeric_code("http_", 100, 599) ||
                     numeric_code("transport_", 1, 999);
  return Json{{"transport_ok", response.transport_ok},
              {"error_code", known ? response.error_code : "unknown_adapter_error"},
              {"attempts", std::size_t(response.attempts)},
              {"duration_ms", duration_ms},
              {"decoded_body_bytes", response.body.size()}};
}

struct Search {
  fs::path dataset_path, evaluation_path, root;
  Json seed, baseline, evaluation, config, gate, controls, tree;
  Json reflection_history;
  mutable std::map<std::string, Json> reviewed_feedback_cache;
  std::string context, evaluation_context, candidate_evaluation_context;
  std::function<bool()> cancelled;
  std::shared_ptr<legacy::ModelAdapter> model;
  dataset::v1::EvaluationDataset dataset;
  std::vector<std::string> train, val, holdout;
  std::size_t reserve = 0;
  std::size_t final_holdout_top_k_override = 0;
  bool final_holdout_replay = false;
  std::string final_holdout_execution_context;
  std::size_t final_holdout_reserve = 0;

  Json reviewed_feedback_trial(const dataset::v1::EvaluationTrial &trial) const {
    const auto key = trial.SerializeAsString();
    const auto found = reviewed_feedback_cache.find(key);
    if (found != reviewed_feedback_cache.end())
      return found->second;
    auto view = feedback_trial(trial, controls);
    bind_iteration_assessments(view, json(trial.evidence()));
    reviewed_feedback_cache.emplace(key, view);
    return view;
  }

  Json trigger_only_failure_evidence(const Json &parent, const Report &report,
                                     const Json &selected) const {
    Json result = Json::Array{};
    if (selected.items().size() != 1 ||
        selected.items().front().text().rfind("experience:", 0) != 0)
      return result;
    const auto id = selected.items().front().text().substr(11);
    const auto policy = proto<v1::RuntimePolicy>(parent["policy"]);
    Json source;
    const auto approved_sources = source_experiences(parent);
    for (const auto &item : approved_sources.items())
      if (item.str("memory_id") == id && !item["content"].null())
        source = item;
    if (source.null())
      return result;
    std::string profile = source.str("content") + "\n" + source.str("condition");
    for (const auto &item : policy.snapshot().experiences())
      if (item.memory_id() == id)
        profile += "\n" + item.observed_problem();
    const auto symbol = [&](const std::string &text) {
      for (auto at = profile.find(text); at != std::string::npos; at = profile.find(text, at + 1))
        if ((at == 0 || !std::isalnum(static_cast<unsigned char>(profile[at - 1]))) &&
            (at + text.size() == profile.size() ||
             !std::isalnum(static_cast<unsigned char>(profile[at + text.size()]))))
          return true;
      return false;
    };
    for (const auto &trial : report.trials()) {
      if (trial.variant_id() != "candidate" || trial.execution_status() != "completed")
        continue;
      const auto task = std::find_if(dataset.tasks().begin(), dataset.tasks().end(),
                                    [&](const auto &value) { return value.task_id() == trial.task_id(); });
      if (task == dataset.tasks().end() || task->split() != "train")
        continue;
      const auto evidence = json(trial.evidence());
      const auto traces = evidence["traces"].array() ? evidence["traces"]
                          : Json(Json::Array{evidence["trace"]});
      std::map<std::string, std::string> agents;
      for (const auto &trace : traces.items())
        agents[trace["identity"].str("run_id")] = trace["identity"].str("agent_id");
      const auto view = reviewed_feedback_trial(trial);
      for (const auto &run : view["evidence"]["runs"].items()) {
        const auto agent = agents[run.str("run_id")];
        if (agent.empty())
          continue;
        for (const auto &action : run["actions"].items()) {
          if (action.str("kind") != "action.end" || action.str("status") != "failed" ||
              !action.flag("observed_failure", false))
            continue;
          const auto code = action.str("terminal_error_code_candidate");
          if (code.empty())
            continue;
          const auto actor = action.str("actor_id");
          const auto scope = "agent:" + agent + "/actor:" +
                             ((actor.empty() || actor == "unspecified") ? "default" : actor);
          if (scope != source.str("scope"))
            continue;
          bool related = symbol(code);
          if (source["observed_operations"].array())
            for (const auto &operation : source["observed_operations"].items())
              related |= operation.text() == action.str("operation") &&
                         operation.text() != "appworld.execute" && operation.text() != "*";
          // These are literal public protocol/parameter symbols, not topic inference.
          related |= code == "appworld_auth_401" && symbol("401");
          related |= code == "appworld_validation_422" && symbol("422");
          related |= code == "appworld_missing_access_token" &&
                     source.str("content").find("access_token") != std::string::npos;
          related |= code == "appworld_type_error_positional" && symbol("TypeError") &&
                     symbol("positional");
          if (!related)
            continue;
          result.push(Json{{"task_id", trial.task_id()}, {"run_id", run["run_id"]},
                           {"event_id", action["event_id"]}, {"error_code", code},
                           {"memory_id", id}, {"target_scope", scope}});
        }
      }
    }
    return result;
  }

  // 先持久化搜索状态，再推进外部工作；异常恢复依赖这份节点与预算快照。
  void save() {
    const auto bytes = tree.dump();
    require(bytes.size() <= 16777216, "gepa_tree_limit");
    write(root / "tree.json", bytes);
  }

  // 搜索取消属于控制信号，不得被当成候选业务失败或触发额外反思。
  void check() const {
    require(!cancelled || !cancelled(), "gepa_cancelled");
    require(now() < deadline(), "gepa_time_budget_exhausted");
  }

  // 计算当前阶段可使用的截止时间，不能通过重试无限刷新总预算。
  std::size_t deadline() const {
    if (final_holdout_replay && tree["final_holdout"].num("replay_deadline_ms", 0))
      return tree["final_holdout"].num("replay_deadline_ms", 0);
    return tree.num("started_ms", 0) + config.num("timeout_ms", 1800000) + tree.num("paused_ms", 0);
  }

  // 恢复已消耗的搜索时间，重启实验不重置原有预算。
  void resume_clock(std::size_t resumed_at) {
    if (tree.str("status") != "cancelled" || tree.str("error") != "gepa_cancelled")
      return;
    const auto finished = tree.num("finished_ms", 0);
    // 仅扣除明确取消后的停机时间；保留原始时间戳，不能复活已耗尽的执行预算。
    if (!finished || finished < tree.num("started_ms", 0) || finished >= deadline() ||
        resumed_at < finished)
      return;
    tree["paused_ms"] = tree.num("paused_ms", 0) + resumed_at - finished;
    tree.fields().erase("finished_ms"); // 同一取消检查点只能扣除一次。
  }

  // 时间估算按并发“波次”归一化：补跑 1 题与满批 16 题都至少需要一波。
  // 不能把一波耗时除以补跑题数，再按串行速度外推完整留出集。
  std::size_t evaluation_waves(std::size_t trials) const {
    const auto parallelism = std::max<std::size_t>(1, evaluation.num("parallelism", 1));
    return trials / parallelism + (trials % parallelism != 0);
  }

  // 只用于未来耗时估计，不改变真实执行数、调用数或截止时间。
  std::size_t estimated_evaluation_ms(std::size_t trials) const {
    const auto waves = tree.num("timed_waves", 0);
    return waves ? tree.num("evaluation_wall_ms", 0) * evaluation_waves(trials) / waves : 0;
  }

  // 完成一次有新增 Agent 执行的评测后采样；纯缓存、纯补评分不用于推断 Agent 吞吐。
  // 混合补跑的整批耗时保留在该波内，包含评分和持久化开销，估计保持保守。
  void record_evaluation_time(std::size_t elapsed, std::size_t trials) {
    if (!trials)
      return;
    if (!tree.has("timed_waves")) {
      // 旧计时缺少批次占用信息，无法可靠恢复波数。仅重建估计器，不退还真实预算。
      tree["evaluation_wall_ms"] = 0;
      tree["timed_trials"] = 0;
      tree["timed_waves"] = 0;
    }
    tree["evaluation_wall_ms"] = tree.num("evaluation_wall_ms", 0) + elapsed;
    tree["timed_trials"] = tree.num("timed_trials", 0) + trials;
    tree["timed_waves"] = tree.num("timed_waves", 0) + evaluation_waves(trials);
  }

  // 保留冻结留出阶段需要的时间预算，避免搜索耗尽全部时间。
  std::size_t holdout_time() const {
    const auto budget = config.num("timeout_ms", 1800000);
    if (config.num("holdout_reserve_ms", 0))
      return config.num("holdout_reserve_ms", 0);
    // 使用观测到的每波墙钟时间，保留原有 1.5 倍安全系数和至少 20% 余量。
    // 满并发批次已经计入并行收益，不能再额外除一次并行数。
    const auto estimate = estimated_evaluation_ms(reserve) * 3 / 2;
    return std::min(budget - 1, std::max(budget / 5, estimate));
  }

  // 在搜索边界检查取消、时间和调用预算，保证长实验可受控结束。
  void search_check(std::size_t trials = 0) const {
    check();
    const auto estimate = estimated_evaluation_ms(trials);
    require(now() + estimate < deadline() - holdout_time(), "gepa_search_time_reserved");
  }

  // 解析当前搜索节点的评测产物，并验证其属于预期运行。
  Json artifact(std::size_t index) const {
    const auto value = Json::parse(read(root / "versions" / (std::to_string(index) + ".json")));
    require(agent_version_id(value.dump()) == tree["nodes"].items().at(index).str("version"),
            "gepa_version_changed");
    return value;
  }

  // 读取或生成当前留出任务集合的固定基线，缓存需通过版本与完整性检查。
  HoldoutBaselineCache holdout_baselines() const {
    Json versions = Json::Object{};
    for (const auto &task : dataset.tasks())
      if (task.split() == "holdout")
        versions[task.task_id()] = task.task_version();
    const Json identity{
        {"schema_version", 1},
        {"evaluation_context", evaluation_context},
        {"split", "holdout"},
        {"baseline_version", agent_version_id(baseline.dump())},
        {"dataset_version", dataset.dataset_version()},
        {"task_versions", versions},
        {"repetitions", evaluation.num("repetitions", 1)},
        // 缺失 judge 时只能读取空值，不能借由非 const operator[] 把 null 写回
        // 冻结配置；否则同一次评测的 retry 会得到不同 identity。
        {"loop_review", static_cast<const Json &>(evaluation)["judge"].has("loop_review_prompt")},
        {"evaluator_version", static_cast<const Json &>(evaluation)["judge"].str("assessment_mode", "legacy") == "objectives"
                                  ? "task-evaluator-v2-goals"
                                  : "task-evaluator-v1"}};
    HoldoutBaselineCache cache{root.parent_path().parent_path(),
            identity,
            tree["baseline_cache"].str("key"),
            evaluation.num("max_artifact_bytes", 16777216),
            read,
            write,
            hash};
    if (config.has("holdout_baseline_result_file")) {
      cache.external_requested = true;
      cache.external_source = external_holdout_source(identity);
      cache.external_loader = [this, identity](const Json &source) {
        return load_external_holdout(source, identity);
      };
    }
    return cache;
  }

  // External L0 is a reference to a completed native execution, not another
  // execution or a rewritten source report. The result owns attempt accounting.
  Json external_holdout_source(const Json &identity) const {
    const fs::path result_path(config.str("holdout_baseline_result_file"));
    if (!fs::exists(result_path))
      return Json();
    const auto result = Json::parse(read(result_path, 16777216));
    if (result.str("status") != "complete")
      return Json();
    const fs::path report_path(result.str("source_report"));
    require(report_path.is_absolute() && report_path.filename() == "report.json",
            "gepa_external_baseline_source_invalid");
    const auto manifest_bytes = read(report_path.parent_path() / "manifest.json", 16777216);
    const auto manifest = Json::parse(manifest_bytes);
    require(manifest.flag("complete", false) && !manifest["files"].str("report.json").empty(),
            "gepa_external_baseline_source_incomplete");
    bool costs_complete = result["metrics"].flag("tokens_complete", false);
    for (const auto &item : result["items"].items())
      costs_complete &= item.flag("execution_costs_complete", item.flag("tokens_complete", false));
    fs::path source_evaluation(config.str("holdout_baseline_evaluation_file"));
    // A pending source can later finish through native model-only regrading.
    // Resolve its actual retained grading configuration when it becomes
    // complete; the report's judge fingerprint is still checked below.
    auto folder = report_path.parent_path();
    const auto source_stage = result_path.parent_path().lexically_normal();
    require(!report_path.lexically_relative(source_stage).empty() &&
                *report_path.lexically_relative(source_stage).begin() != "..",
            "gepa_external_baseline_source_invalid");
    for (unsigned depth = 0; depth < 4 && folder != folder.parent_path(); ++depth) {
      const auto grading = folder.parent_path() / (folder.filename().string() + ".regrade.config.json");
      if (fs::is_regular_file(grading)) { source_evaluation = grading; break; }
      if (folder == source_stage) break;
      folder = folder.parent_path();
    }
    return Json{{"kind", "verified_external_baseline.v1"}, {"identity", identity},
                {"report", report_path.lexically_normal().string()},
                {"result_file", result_path.string()},
                {"source_dataset", config["holdout_baseline_dataset"]},
                {"source_evaluation_file", source_evaluation.string()},
                {"source_variant", "candidate"}, {"display_variant", "baseline"},
                {"resource_costs_complete", costs_complete},
                {"comparison_timing", "historical_baseline"}};
  }

  Report load_external_holdout(const Json &source, const Json &identity) const {
    const auto current = external_holdout_source(identity);
    require(current.object() && source.dump() == current.dump(),
            "gepa_external_baseline_source_changed");
    const auto result = Json::parse(read(source.str("result_file"), 16777216));
    require(result.str("host_version") == baseline["agent"].str("revision"),
            "gepa_external_baseline_host_mismatch");
    const auto source_dataset = read_evaluation_dataset(source.str("source_dataset"));
    std::map<std::string, const dataset::v1::EvaluationTask *> original_tasks, current_tasks;
    for (const auto &task : source_dataset.tasks()) {
      require(task.split() == "holdout" && task.review_status() == "reviewed" &&
                  original_tasks.emplace(task.task_id(), &task).second,
              "gepa_external_baseline_task_mismatch");
    }
    for (const auto &task : dataset.tasks())
      if (task.split() == "holdout") current_tasks.emplace(task.task_id(), &task);
    require(original_tasks.size() == current_tasks.size() && !current_tasks.empty(),
            "gepa_external_baseline_task_mismatch");
    auto task_body = [](const dataset::v1::EvaluationTask &task) {
      auto value = json(task);
      value.fields().erase("task_version");
      if (value["provenance"].object()) value["provenance"].fields().erase("source_fingerprints");
      return value.dump();
    };
    for (const auto &item : current_tasks)
      require(original_tasks.count(item.first) &&
                  task_body(*item.second) == task_body(*original_tasks.at(item.first)),
              "gepa_external_baseline_task_mismatch");
    const fs::path source_config_path(source.str("source_evaluation_file"));
    auto original_config = Json::parse(read(source_config_path, 16777216));
    auto current_config = evaluation;
    require(original_config["variants"].array() && original_config["variants"].items().size() == 1 &&
                current_config["variants"].array() && current_config["variants"].items().size() == 1,
            "gepa_external_baseline_host_mismatch");
    auto original_variant = original_config["variants"].items()[0];
    auto current_variant = current_config["variants"].items()[0];
    require(original_variant.str("version") == baseline["agent"].str("revision") ||
                original_variant.str("version") == identity.str("baseline_version"),
            "gepa_external_baseline_host_mismatch");
    if (original_variant.has("artifact"))
      require(agent_version_id(original_variant["artifact"].dump()) == identity.str("baseline_version"),
              "gepa_external_baseline_artifact_mismatch");
    for (const auto *key : {"id", "version", "artifact", "instrumentation_config"}) {
      original_variant.fields().erase(key); current_variant.fields().erase(key);
    }
    require(original_variant.dump() == current_variant.dump(), "gepa_external_baseline_host_mismatch");
    // These fields describe batch scope, report storage or judge call allowance;
    // per-task budgets, host arguments/options and grading semantics stay exact.
    for (const auto *key : {"task_ids", "splits", "variants", "max_trials", "max_report_bytes",
                            "max_jsonl_bytes", "max_markdown_bytes", "artifact_retention", "judge"}) {
      original_config.fields().erase(key); current_config.fields().erase(key);
    }
    const bool grading_parallelism_changed =
        original_config["parallelism"].dump() != current_config["parallelism"].dump();
    original_config.fields().erase("parallelism");
    current_config.fields().erase("parallelism");
    require(original_config.dump() == current_config.dump(), "gepa_external_baseline_execution_mismatch");
    const auto expected_judge = task_evaluation_judge_fingerprint(evaluation_path, evaluation.dump());
    require(task_evaluation_judge_fingerprint(source_config_path,
                read(source_config_path, 16777216)) == expected_judge,
            "gepa_external_baseline_judge_mismatch");
    const fs::path path(source.str("report"));
    auto bytes = read(path, kMaxEvaluationReportBytes);
    const auto manifest = Json::parse(read(path.parent_path() / "manifest.json", 16777216));
    require(hash(bytes) == manifest["files"].str("report.json"),
            "gepa_external_baseline_source_changed");
    Report report;
    require(google::protobuf::util::JsonStringToMessage(bytes, &report).ok(),
            "gepa_external_baseline_report_invalid");
    bytes.clear(); bytes.shrink_to_fit();
    for (const auto &file : manifest["files"].fields()) {
      require(!file.first.empty() && fs::path(file.first).filename() == file.first &&
                  file.first != "." && file.first != "..", "gepa_external_baseline_manifest_invalid");
      if (file.first != "report.json")
        require(hash(read(path.parent_path() / file.first, kMaxEvaluationReportBytes)) == file.second.text(),
                "gepa_external_baseline_source_changed");
    }
    const auto settings = json(report.configuration());
    if (grading_parallelism_changed) {
      // A model-only regrade changes judge worker capacity, not the saved Agent
      // execution. Accept that difference only after authenticating its report
      // and the native no-execution accounting/parent binding. An ordinary
      // capture with different execution parallelism remains incompatible.
      const auto summary = json(report.summary());
      const auto &execution = settings["execution_source"];
      const auto &parent = summary["resume_source"];
      const auto &manifest_parent = manifest["summary"]["resume_source"];
      const auto planned = summary.num("planned_trials", 0);
      bool ordinary_saved_resume = false;
      if (!settings.has("model_only_regrade") && !settings.has("execution_source") &&
          !settings.has("rechecked_configured_checks") &&
          !parent.str("run_id").empty() && parent.str("run_id") != report.run_id() &&
          !parent.str("report_sha256").empty() && parent.dump() == manifest_parent.dump() &&
          planned == report.trials_size() && planned > 0 &&
          summary.has("not_started_trials") && summary.num("not_started_trials", 1) == 0 &&
          summary.num("reused_trials", 0) + summary.num("regraded_trials", 0) == planned) {
        // Ordinary native resume has no direct-regrade marker. Do not infer
        // execution reuse from absent fields: authenticate its retained parent,
        // then compare every original execution independently of judge results.
        std::set<fs::path> folders{path.parent_path().parent_path(),
                                  fs::path(source.str("result_file")).parent_path()};
        const auto roots = folders;
        for (const auto &folder : roots)
          for (const auto &entry : fs::directory_iterator(folder)) {
            require(folders.size() < 512, "gepa_external_baseline_execution_mismatch");
            if (entry.is_directory() && !entry.is_symlink()) folders.insert(entry.path());
          }
        fs::path saved_parent;
        for (const auto &folder : folders) {
          const auto parent_manifest = folder / "manifest.json";
          if (!fs::is_regular_file(parent_manifest)) continue;
          const auto value = Json::parse(read(parent_manifest, 16777216));
          if (value.str("run_id") != parent.str("run_id")) continue;
          require(saved_parent.empty() && value.flag("complete", false) &&
                      value["files"].str("report.json") == parent.str("report_sha256"),
                  "gepa_external_baseline_execution_mismatch");
          saved_parent = folder / "report.json";
        }
        require(!saved_parent.empty(), "gepa_external_baseline_execution_mismatch");
        auto parent_bytes = read(saved_parent, kMaxEvaluationReportBytes);
        require(hash(parent_bytes) == parent.str("report_sha256"),
                "gepa_external_baseline_source_changed");
        Report previous;
        require(google::protobuf::util::JsonStringToMessage(parent_bytes, &previous).ok() &&
                    previous.complete() && previous.run_id() == parent.str("run_id") &&
                    previous.dataset_version() == report.dataset_version() &&
                    previous.trials_size() == report.trials_size(),
                "gepa_external_baseline_execution_mismatch");
        parent_bytes.clear(); parent_bytes.shrink_to_fit();
        std::map<std::string, const Trial *> saved;
        for (const auto &trial : previous.trials())
          require(saved.emplace(trial.trial_id(), &trial).second,
                  "gepa_external_baseline_execution_mismatch");
        for (const auto &trial : report.trials()) {
          const auto found = saved.find(trial.trial_id());
          require(found != saved.end(), "gepa_external_baseline_execution_mismatch");
          const auto &before = *found->second;
          require(before.task_id() == trial.task_id() && before.task_version() == trial.task_version() &&
                      before.variant_id() == trial.variant_id() && before.repetition() == trial.repetition() &&
                      before.execution_status() == "completed" && trial.execution_status() == "completed" &&
                      before.duration_ms() == trial.duration_ms() &&
                      google::protobuf::util::MessageDifferencer::Equals(before.metrics(), trial.metrics()),
                  "gepa_external_baseline_execution_mismatch");
          const auto &old_fields = before.evidence().fields();
          const auto &new_fields = trial.evidence().fields();
          bool trace_bound = false;
          for (const auto *field : {"execution_run_id", "loaded_version", "trace", "traces", "runs",
                                    "output", "artifacts", "official_evidence_dir"}) {
            const auto old_value = old_fields.find(field), new_value = new_fields.find(field);
            require((old_value == old_fields.end()) == (new_value == new_fields.end()),
                    "gepa_external_baseline_execution_mismatch");
            if (old_value == old_fields.end()) continue;
            require(google::protobuf::util::MessageDifferencer::Equals(old_value->second, new_value->second),
                    "gepa_external_baseline_execution_mismatch");
            trace_bound |= std::string(field) == "trace" || std::string(field) == "traces" ||
                           std::string(field) == "runs";
          }
          require(trace_bound, "gepa_external_baseline_execution_mismatch");
        }
        ordinary_saved_resume = true;
      }
      require(ordinary_saved_resume || (settings.flag("model_only_regrade", false) &&
                  !settings.flag("rechecked_configured_checks", false) &&
                  !execution.str("run_id").empty() && execution.str("run_id") != report.run_id() &&
                  !execution.str("config_fingerprint").empty() &&
                  !execution.str("report_sha256").empty() &&
                  execution.str("run_id") == parent.str("run_id") &&
                  execution.str("report_sha256") == parent.str("report_sha256") &&
                  parent.dump() == manifest_parent.dump() &&
                  planned == report.trials_size() && planned > 0 &&
                  summary.has("not_started_trials") && summary.num("not_started_trials", 1) == 0 &&
                  summary.num("reused_trials", 0) + summary.num("regraded_trials", 0) == planned),
              "gepa_external_baseline_execution_mismatch");
    }
    require(report.complete() && report.schema_version() == 1 && report.report_kind() == "task_evaluation.v1" &&
                report.dataset_version() == source_dataset.dataset_version() &&
                manifest.str("dataset_version") == source_dataset.dataset_version() &&
                report.run_id() == manifest.str("run_id") &&
                report.evaluator_version() == identity.str("evaluator_version") &&
                settings.str("judge_fingerprint") == expected_judge &&
                settings["variants"].items().size() == 1 &&
                settings["variants"].items()[0].str("id") == "candidate" &&
                settings["splits"].dump() == Json(Json::Array{"holdout"}).dump() &&
                settings.num("repetitions", 0) == identity.num("repetitions", 1),
            "gepa_external_baseline_report_invalid");
    std::set<std::pair<std::string, unsigned>> seen;
    std::set<std::string> trials, runs, result_ids;
    for (const auto &item : result["items"].items())
      require(result_ids.insert(item.str("task_id")).second, "gepa_external_baseline_result_invalid");
    for (auto &trial : *report.mutable_trials()) {
      auto evidence = json(trial.evidence());
      const auto run = evidence.str("execution_run_id", evidence["trace"]["identity"].str("run_id"));
      const auto &trace = evidence["trace"];
      bool bound = false;
      for (const auto &event : trace["events"].items())
        if (event.str("kind") == "run.start" &&
            event["attributes"].str("agent_version_id") == identity.str("baseline_version") &&
            event["attributes"].str("evaluation_trial_id") == trial.trial_id()) bound = true;
      require(original_tasks.count(trial.task_id()) && trial.variant_id() == "candidate" &&
                  trial.split() == "holdout" && trial.task_version() == original_tasks.at(trial.task_id())->task_version() &&
                  trial.repetition() > 0 && trial.repetition() <= identity.num("repetitions", 1) &&
                  seen.emplace(trial.task_id(), trial.repetition()).second && trials.insert(trial.trial_id()).second &&
                  !run.empty() && runs.insert(run).second && result_ids.count(trial.task_id()) &&
                  evidence.str("loaded_version") == identity.str("baseline_version") &&
                  trace["identity"].str("run_id") == run && bound &&
                  (trial.execution_status() == "completed" || trial.execution_status() == "failed") &&
                  (trial.verdict() == "passed" || trial.verdict() == "failed") &&
                  (trial.error_code().empty() || trial.execution_status() == "failed") &&
                  (!identity.flag("loop_review", false) || evidence["loop_review"].str("status") == "complete"),
              "gepa_external_baseline_trial_mismatch");
      if (evidence["assessment"].object()) assessment_quality(evidence, trial.verdict() == "passed");
      const Json reference{{"report", source["report"]}, {"trial_id", trial.trial_id()},
          {"run_id", run}, {"variant_id", trial.variant_id()}, {"task_version", trial.task_version()},
          {"full_execution_evidence", "original_source"}};
      // Keep native score/cost values byte-for-byte in protobuf. JSON parsing
      // above is only validation; rebuilding these values can round a double.
      const std::set<std::string> retained{"assessment", "loop_review", "metrics", "output", "loaded_version",
          "execution_run_id", "execution_status", "artifacts"};
      auto *fields = trial.mutable_evidence()->mutable_fields();
      for (auto it = fields->begin(); it != fields->end();)
        if (!retained.count(it->first)) it = fields->erase(it); else ++it;
      (*fields)["source_reference"].mutable_struct_value()->CopyFrom(proto<google::protobuf::Struct>(reference));
      trial.set_variant_id("baseline"); trial.set_task_version(current_tasks.at(trial.task_id())->task_version());
    }
    require(seen.size() == current_tasks.size() * identity.num("repetitions", 1) &&
                result_ids.size() == current_tasks.size(), "gepa_external_baseline_trials_incomplete");
    auto summary = json(report.summary());
    const auto base = summary["by_variant"]["candidate"];
    const auto observations = report.evaluator_version() == "task-evaluator-v2-goals"
        ? base["goal_completion"].num("observations", 0) : base.num("scored", 0);
    require(base.num("trials", 0) == seen.size() && observations == seen.size(),
            "gepa_external_baseline_summary_invalid");
    auto *summary_fields = report.mutable_summary()->mutable_fields();
    auto original_base = summary_fields->at("by_variant").struct_value().fields().at("candidate");
    auto *variants = (*summary_fields)["by_variant"].mutable_struct_value()->mutable_fields();
    variants->clear(); (*variants)["baseline"] = std::move(original_base);
    (*summary_fields)["baseline_source"].mutable_struct_value()->CopyFrom(proto<google::protobuf::Struct>(source));
    (*summary_fields)["resource_costs_complete"].set_bool_value(source.flag("resource_costs_complete", false));
    report.set_dataset_version(dataset.dataset_version());
    return report;
  }

  // 生成可跨节点复用的评测身份，绑定数据、策略及宿主执行口径。
  Json shared_evaluation_identity(const Json &settings, bool final) const {
    const auto &splits = settings["splits"];
    if (final || candidate_evaluation_context.empty() || !config.flag("cache_evaluations", true) ||
        !splits.array() || splits.items().size() != 1 ||
        (splits.items()[0].text() != "train" && splits.items()[0].text() != "val"))
      return Json();
    return Json{{"schema_version", 1},
                {"execution_context", candidate_evaluation_context},
                {"dataset_version", dataset.dataset_version()},
                {"settings_hash", hash(settings.dump())}};
  }

  // 缓存命中必须同时满足任务身份、版本和评分完整性，不能只检查报告文件存在。
  bool shared_report_complete(const Report &report, const Json &settings,
                              bool native_cached = false) const {
    if (!scored(report) || (report.report_kind() != "task_evaluation.v1" &&
                           (!native_cached || report.report_kind() != "gepa_cached_evaluation.v1")) ||
        report.dataset_version() != dataset.dataset_version() || !settings["task_ids"].array() ||
        !settings["variants"].array() || settings["variants"].items().size() != 1 ||
        scores(report).fields().size() != settings["task_ids"].items().size())
      return false;
    const auto &variant = settings["variants"].items()[0];
    const auto repetitions = settings.num("repetitions", 1);
    std::set<std::pair<std::string, std::size_t>> seen;
    for (const auto &trial : report.trials()) {
      if (trial.variant_id() != variant.str("id") ||
          trial.split() != settings["splits"].items()[0].text() || !trial.error_code().empty() ||
          (trial.execution_status() != "completed" && trial.execution_status() != "failed") ||
          !trial.repetition() || trial.repetition() > repetitions ||
          !seen.emplace(trial.task_id(), trial.repetition()).second ||
          std::none_of(settings["task_ids"].items().begin(), settings["task_ids"].items().end(),
                       [&](const auto &id) { return id.text() == trial.task_id(); }))
        return false;
      const auto evidence = evidence_fields(trial.evidence(), {"loaded_version", "loop_review"});
      if (evidence.str("loaded_version") != variant.str("version") ||
          (settings["judge"].has("loop_review_prompt") &&
           evidence["loop_review"].str("status") != "complete"))
        return false;
    }
    return seen.size() == settings["task_ids"].items().size() * repetitions;
  }

  // GEPA 的成本目标来自宿主遥测；缺失值不能补成零。这里仅判断当前 trial
  // 是否缺少本轮目标要求的观测，供逐题缓存和定点重跑共同使用。
  bool resources_report_only() const {
    return gate.flag("resources_report_only", false);
  }

  Json effective_objective() const {
    auto objective = config["objective"];
    if (resources_report_only()) {
      objective["token_weight"] = 0;
      objective["latency_weight"] = 0;
    }
    return objective;
  }

  bool required_token_metric_missing(const Trial &trial) const {
    return !resources_report_only() && number(config["objective"]["token_weight"], 0.05) &&
           json(trial.metrics())["tokens"].null() &&
           config.str("unknown_token_policy", "reject") != "worst_case";
  }

  bool required_objective_metrics_missing(const Trial &trial) const {
    const auto evidence = json(trial.evidence());
    const auto loops = evidence.has("loop_review")
                           ? (evidence["loop_review"].str("status") == "complete"
                                  ? evidence["loop_review"]["count"]
                                  : Json())
                           : evidence["trajectory"]["summary"]["invalid_loop_lower_bound"];
    const auto &objective = config["objective"];
    const bool need_loops = number(objective["invalid_loop_weight"], 0.2) ||
                            gate.has("min_invalid_loop_reduction");
    return (need_loops && loops.null()) || required_token_metric_missing(trial);
  }

  // 统计可续跑的执行记录；整批未完成不妨碍复用已完成题，但也不代表评分完成。
  // 真正恢复时仍由评测器复核配置、任务版本和交付物摘要，未知评分只进入重评。
  std::size_t shared_report_reusable(const Report &report, const Json &settings) const {
    if (report.schema_version() != 1 || report.report_kind() != "task_evaluation.v1" ||
        report.dataset_version() != dataset.dataset_version() || !settings["task_ids"].array() ||
        !settings["variants"].array() || settings["variants"].items().size() != 1)
      return 0;
    const auto &variant = settings["variants"].items()[0];
    std::set<std::pair<std::string, std::size_t>> seen;
    std::set<std::string> trial_ids;
    std::size_t reusable = 0;
    for (const auto &trial : report.trials()) {
      if (trial.variant_id() != variant.str("id") ||
          trial.split() != settings["splits"].items()[0].text() || trial.trial_id().empty() ||
          !trial_ids.insert(trial.trial_id()).second || !trial.repetition() ||
          trial.repetition() > settings.num("repetitions", 1) ||
          !seen.emplace(trial.task_id(), trial.repetition()).second ||
          std::none_of(settings["task_ids"].items().begin(), settings["task_ids"].items().end(),
                       [&](const auto &id) { return id.text() == trial.task_id(); }))
        return 0;
      if (trial.execution_status() != "completed")
        continue;
      const auto evidence = evidence_fields(trial.evidence(), {"loaded_version", "loop_review"});
      if (evidence.str("loaded_version") != variant.str("version"))
        return 0;
      const bool reviewed = !settings["judge"].has("loop_review_prompt") ||
                            evidence["loop_review"].str("status") == "complete";
      const bool judge_error = trial.error_code() == "evaluation_judge_failed" &&
          std::any_of(trial.checks().begin(), trial.checks().end(), [](const auto &check) {
            return check.check_id() == "model" && check.status() == "error";
          });
      const bool judge_uncertain = trial.execution_status() == "completed" &&
          trial.error_code().empty() &&
          (trial.verdict() == "uncertain" || incomplete_goal_assessment(trial.evidence())) &&
          std::any_of(trial.checks().begin(), trial.checks().end(), [](const auto &check) {
            return check.check_id() == "model" && check.status() != "error";
          });
      // token 用量来自宿主执行，缺失时重评无法补齐；loop review 则由
      // 裁判产生，裁判失败/未决的已完成执行应保留并只重评，不重跑 Agent。
      if (required_token_metric_missing(trial) ||
          ((!judge_error && !judge_uncertain) && required_objective_metrics_missing(trial)))
        continue;
      reusable += judge_error || judge_uncertain ||
                  (reviewed && trial.error_code().empty() &&
                   (trial.verdict() == "passed" || trial.verdict() == "failed"));
    }
    return reusable;
  }

  // 载入通过身份校验的完整报告或部分检查点；部分报告只能续跑，不能直接当作完成。
  fs::path shared_evaluation_report(const Json &settings, bool final) const {
    const auto identity = shared_evaluation_identity(settings, final);
    if (identity.null())
      return {};
    // 与后台评测 worker 共用 evaluation.lock；此索引只保存摘要和根目录内的报告引用。
    const auto shared_root = root.parent_path().parent_path();
    try {
      const auto entry = Json::parse(
          read(shared_root / "candidate-cache" / (hash(identity.dump()) + ".json"), 8192));
      if (entry["identity"].dump() != identity.dump())
        return {};
      const fs::path relative(entry.str("report"));
      if (relative.empty() || relative.is_absolute() || relative.filename() != "report.json")
        return {};
      for (const auto &part : relative)
        if (part == ".." || part == ".")
          return {};
      const auto path = shared_root / relative;
      const auto bytes = read(path, kMaxEvaluationReportBytes),
                 manifest_bytes = read(path.parent_path() / "manifest.json", 16777216);
      if (hash(bytes) != entry.str("report_hash") ||
          hash(manifest_bytes) != entry.str("manifest_hash"))
        return {};
      const auto report = proto<Report>(Json::parse(bytes));
      const auto manifest = Json::parse(manifest_bytes);
      if (manifest.flag("complete", false) != report.complete() ||
          manifest.str("run_id") != report.run_id() ||
          manifest["files"].str("report.json") != entry.str("report_hash") ||
          !shared_report_reusable(report, settings))
        return {};
      return path;
    } catch (const std::exception &) {
      return {};
    } // 过期、清理或损坏的缓存按 miss，不终止搜索。
  }

  // 登记同条件检查点；取消批次也保留已完成题，不让较差检查点覆盖完整结果。
  void remember_shared_evaluation(const Json &settings, bool final, const fs::path &path,
                                  const Report &report) const {
    const auto identity = shared_evaluation_identity(settings, final);
    const auto reusable = identity.null() ? 0 : shared_report_reusable(report, settings);
    if (!reusable)
      return;
    try {
      const auto bytes = read(path, kMaxEvaluationReportBytes),
                 manifest_bytes = read(path.parent_path() / "manifest.json", 16777216);
      const auto manifest = Json::parse(manifest_bytes);
      if (manifest.flag("complete", false) != report.complete() ||
          manifest.str("run_id") != report.run_id() ||
          manifest["files"].str("report.json") != hash(bytes))
        return;
      if (!shared_report_complete(report, settings)) {
        const auto previous = shared_evaluation_report(settings, final);
        if (!previous.empty() && previous != path) {
          const auto saved = proto<Report>(Json::parse(read(previous, kMaxEvaluationReportBytes)));
          if (shared_report_complete(saved, settings) ||
              shared_report_reusable(saved, settings) > reusable)
            return;
        }
      }
      const auto shared_root = root.parent_path().parent_path();
      const auto relative = fs::absolute(path).lexically_normal().lexically_relative(
          fs::absolute(shared_root).lexically_normal());
      if (relative.empty() || relative.is_absolute())
        return;
      for (const auto &part : relative)
        if (part == ".." || part == ".")
          return;
      write(shared_root / "candidate-cache" / (hash(identity.dump()) + ".json"),
            Json{{"identity", identity},
                 {"report", relative.string()},
                 {"report_hash", hash(bytes)},
                 {"manifest_hash", hash(manifest_bytes)}}
                .dump());
    } catch (const std::exception &) {
    } // 可选加速失败不改变评测结果或发布结论。
  }

  // 统一调度一次候选评测；已确认的缓存可复用，未知或不完整结果不能伪装成完成。
  Report evaluate(const Json &candidate, const std::vector<std::string> &ids,
                  const std::string &split, bool final = false, bool shared_reuse = true,
                  bool final_candidate_only = false, const std::string &final_campaign = {}) {
    Json settings = evaluation, selected = Json::Array{};
    for (const auto &id : ids)
      selected.push(id);
    settings["splits"] = Json::Array{split};
    settings["task_ids"] = selected;
    // 留出批次通常远大于搜索小批次；只覆盖存储容量，不改变执行或评分口径。
    // 搜索/选择验证沿用原配置，因此提高留出容量不会让有效基线缓存失效。
    if (final && config.num("holdout_max_report_bytes", 0))
      settings["max_report_bytes"] = config["holdout_max_report_bytes"];
    auto host = evaluation["variants"].items().front(), a = host, b = host;
    a["id"] = "baseline";
    a["artifact"] = baseline;
    a["version"] = agent_version_id(baseline.dump());
    b["id"] = "candidate";
    b["artifact"] = candidate;
    b["version"] = agent_version_id(candidate.dump());
    settings["variants"] =
        final && !final_candidate_only ? Json(Json::Array{a, b}) : Json(Json::Array{b});
    // evaluation entry 的键和持久化元数据都显式绑定评分与真实执行
    // context。不依赖上层 context 的间接包含关系，避免旧版缺字段的 entry
    // 在重编译后被逐题拼回新父子对比。
    const auto execution_context = final && final_holdout_replay &&
                                           !final_holdout_execution_context.empty()
                                       ? final_holdout_execution_context
                                       : candidate_evaluation_context;
    const auto evaluation_key = [&](const Json &frozen_settings) {
      return hash("gepa-evaluation-entry-v2\n" + evaluation_context + '\n' +
                  execution_context + '\n' + context + '\n' +
                  frozen_settings.dump() +
                  (final_campaign.empty()
                       ? ""
                       : "\nfinal_holdout_campaign=" + final_campaign));
    };
    const auto key = evaluation_key(settings);
    auto cache = static_cast<const Json &>(tree)["evaluations"][key];
    // 键本身已包含真实执行 context，但恢复边界仍显式校验持久化元数据。
    // 这样即使旧版本、人工迁移或损坏索引把报告放到了新键下，也不会把
    // 不同宿主二进制产生的 attempt 当作本轮检查点继续使用。
    if (cache.object() &&
        (cache.str("evaluation_context") != evaluation_context ||
         cache.str("candidate_evaluation_context") != execution_context))
      cache = Json::Object{};
    if (cache.str("status") == "complete" &&
        cache.str("evaluation_context") == evaluation_context &&
        cache.str("candidate_evaluation_context") == execution_context &&
        (final || config.flag("cache_evaluations", true))) {
      const auto bytes =
          read(root / "evaluations" / key / ("attempt-" + std::to_string(cache.num("attempt", 0))) /
                   "report.json",
               kMaxEvaluationReportBytes);
      require(hash(bytes) == cache.str("report_hash"), "gepa_report_changed");
      const auto report = proto<Report>(Json::parse(bytes));
      // 较早的 complete 条目可能缺少后来启用的目标指标，不能据此盲目复用。
      if (final || scores(report).fields().size() == ids.size()) {
        remember_shared_evaluation(settings, final,
                                   root / "evaluations" / key /
                                       ("attempt-" + std::to_string(cache.num("attempt", 0))) /
                                       "report.json",
                                   report);
        return report;
      }
    }
    if (final && !final_candidate_only && split == "holdout" && config.has("holdout_baseline_result_file") &&
        tree["holdout_baseline_cache"].str("status") != "hit") {
      tree["holdout_baseline_cache"] = holdout_baselines().lookup();
      save();
      require(tree["holdout_baseline_cache"].str("status") == "hit",
              "gepa_external_baseline_pending_or_invalid");
    }
    if (final && !final_candidate_only && split == "holdout" &&
        tree["holdout_baseline_cache"].str("status") == "hit") {
      const auto fixed = tree["holdout_baseline_cache"]["source"];
      const auto baseline_cache = holdout_baselines();
      // 搜索期间已锁定的源若损坏，不隐式更换另一份 L0，也不偷偷重跑基线。
      try {
        baseline_cache.load(fixed);
      } catch (const std::exception &) {
        tree["holdout_baseline_cache"]["status"] = "invalid";
        tree["holdout_baseline_cache"]["reason"] = "locked_source_changed";
        save();
        throw Error("gepa_holdout_baseline_changed");
      }
      // 管理器重编译只会改变旧版 evaluation entry 的执行摘要，不会改变
      // 候选策略、题目集合或评分口径。优先迁移已有完整配对报告，避免把
      // 已完成的 Node 当成新候选再次调用 Agent；固定 L0 仍由上面的完整
      // baseline cache 校验保护，损坏或版本不一致时不会迁移。
      for (const auto &previous_entry : static_cast<const Json &>(tree)["evaluations"].fields()) {
        const auto &previous = previous_entry.second;
        if (previous.str("status") != "complete" || previous.str("split") != "holdout" ||
            previous.str("evaluation_context") != evaluation_context ||
            previous.str("version") != b.str("version") ||
            previous.str("baseline_version") != a.str("version") ||
            previous["task_ids"].dump() != selected.dump() ||
            previous.str("candidate_evaluation_context") == execution_context ||
            previous.str("report").empty())
          continue;
        const auto previous_report_path = root / previous.str("report");
        try {
          // 历史留出报告可能达到数百 MB。迁移时只校验旧 JSON 哈希，
          // 再从同目录 protobuf 直接恢复，避免把 JSON 解析树和报告对象
          // 同时留在内存中，缓存迁移不应再次造成一次大规模评测级别的开销。
          const auto previous_json_hash = hash(read(previous_report_path, kMaxEvaluationReportBytes));
          require(previous_json_hash == previous.str("report_hash"), "gepa_report_changed");
          const auto previous_manifest =
              Json::parse(read(previous_report_path.parent_path() / "manifest.json", 16777216));
          Report previous_report;
          {
            const auto previous_pb =
                read(previous_report_path.parent_path() / "report.pb", kMaxEvaluationReportBytes);
            require(previous_manifest["files"].str("report.pb") == hash(previous_pb) &&
                        previous_report.ParseFromString(previous_pb),
                    "gepa_report_changed");
          }
          const auto previous_config = json(previous_report.configuration());
          // 旧版在直接执行双 variant 评测时会保留 task_evaluation.v1，
          // 新版合并器才写 gepa_cached_pair.v1；两者只要都通过下面的
          // baseline/candidate 版本与题目校验，都代表同一份完整配对结果。
          require(previous_report.complete() &&
                      (previous_report.report_kind() == "gepa_cached_pair.v1" ||
                       previous_report.report_kind() == "task_evaluation.v1") &&
                      previous_config["splits"].dump() ==
                          Json(Json::Array{"holdout"}).dump() &&
                      previous_config["variants"].array() &&
                      previous_config["variants"].items().size() == 2 &&
                      previous_config["variants"].items()[0].str("id") == "baseline" &&
                      previous_config["variants"].items()[0].str("version") ==
                          a.str("version") &&
                      previous_config["variants"].items()[1].str("id") == "candidate" &&
                      previous_config["variants"].items()[1].str("version") == b.str("version"),
                  "gepa_report_changed");
          auto migrated = previous;
          migrated["candidate_evaluation_context"] = execution_context;
          migrated["baseline_cache"] = "hit";
          migrated["comparison_timing"] = "historical_baseline";
          migrated["baseline_source"] = fixed;
          tree["evaluations"][key] = migrated;
          save();
          return previous_report;
        } catch (const std::exception &error) {
          // 旧报告损坏或不完整时继续正常候选评测，不把坏缓存当成成功。
          // 记录稳定错误码，便于恢复失败时区分“缓存不可用”和“真的需要重跑”；
          // 不记录本地路径、报告内容或凭据。
          std::cerr << "[gepa] historical_report_migration_skipped code="
                    << error.what() << '\n';
        }
      }
      const auto fresh = evaluate(candidate, ids, split, true, false, true, final_campaign);
      auto candidate_settings = settings;
      candidate_settings["variants"] = Json::Array{b};
      const auto candidate_key = evaluation_key(candidate_settings);
      const auto &candidate_cache = static_cast<const Json &>(tree)["evaluations"][candidate_key];
      const auto directory = fs::path("evaluations") / key /
                             ("attempt-" + std::to_string(cache.num("attempt", 0) + 1));
      const auto joined = baseline_cache.combine(
          fixed, fresh, root / candidate_cache.str("report"), root / directory, key,
          std::min<std::size_t>(settings.num("max_report_bytes", 33554432),
                                kMaxEvaluationReportBytes));
      cache["attempt"] = cache.num("attempt", 0) + 1;
      cache["status"] = scored(joined) ? "complete" : "inconclusive";
      cache["version"] = b["version"];
      cache["baseline_version"] = a["version"];
      cache["evaluation_context"] = evaluation_context;
      cache["candidate_evaluation_context"] = execution_context;
      cache["split"] = split;
      cache["task_ids"] = selected;
      cache["trials_reserved"] = 0;
      cache["baseline_cache"] = "hit";
      cache["comparison_timing"] = "historical_baseline";
      cache["baseline_source"] = fixed;
      cache["report"] = (directory / "report.json").string();
      cache["report_hash"] = hash(read(root / directory / "report.json", kMaxEvaluationReportBytes));
      tree["evaluations"][key] = cache;
      save();
      return joined;
    }
    check();
    fs::path retained_checkpoint;
    if (!final && config.flag("cache_evaluations", true)) {
      std::map<std::string, std::vector<dataset::v1::EvaluationTrial>> reused;
      Json sources = Json::Array{}, source_refs = Json::Array{};
      Report joined;
      for (const auto &entry : static_cast<const Json &>(tree)["evaluations"].fields()) {
        const auto &previous = entry.second;
        // Only explicitly retained parent entries from a validated, cancelled
        // reflection checkpoint may cross an infrastructure context revision.
        // Their original metadata stays intact; new executions use the current
        // context. Report hashes and per-task health checks below still apply.
        bool retained_parent = false;
        const auto &revisions = static_cast<const Json &>(tree)["resume_context_revisions"];
        if (revisions.array() && !revisions.items().empty()) {
          const auto &last = revisions.items().back();
          if (last.str("reason") == "closed_reflection_checkpoint" &&
              last.str("current") == context && last["retained_evaluations"].array())
            for (const auto &retained : last["retained_evaluations"].items())
              retained_parent |= retained.text() == entry.first;
        }
        if ((previous.str("status") != "complete" && previous.str("status") != "inconclusive") ||
            previous.str("split") != split || previous.str("version") != b.str("version") ||
            previous.str("evaluation_context") != evaluation_context ||
            (previous.str("candidate_evaluation_context") != candidate_evaluation_context &&
             !retained_parent) ||
            previous.has("baseline_version"))
          continue;
        bool overlaps = false;
        for (const auto &id : previous["task_ids"].items())
          overlaps |=
              std::find(ids.begin(), ids.end(), id.text()) != ids.end() && !reused.count(id.text());
        if (!overlaps)
          continue;
        const auto source_report = fs::path("evaluations") / entry.first /
                                   ("attempt-" + std::to_string(previous.num("attempt", 0))) /
                                   "report.json";
        const auto bytes = read(root / source_report, kMaxEvaluationReportBytes);
        require(hash(bytes) == previous.str("report_hash"), "gepa_report_changed");
        const auto report = proto<Report>(Json::parse(bytes));
        if (previous.str("status") == "complete")
          require(scored(report), "gepa_report_changed");
        if (!report.complete())
          continue;
        if (retained_parent) {
          std::set<std::string> report_tasks;
          std::set<std::tuple<std::string, std::string, unsigned>> trial_keys;
          bool complete_execution = report.dataset_version() == dataset.dataset_version() &&
              static_cast<std::size_t>(report.trials_size()) ==
                  ids.size() * evaluation.num("repetitions", 1);
          bool judge_failed = false;
          for (const auto &trial : report.trials()) {
            report_tasks.insert(trial.task_id());
            complete_execution &= trial.variant_id() == "candidate" &&
                trial.execution_status() == "completed" &&
                std::find(ids.begin(), ids.end(), trial.task_id()) != ids.end() &&
                trial_keys.emplace(trial.task_id(), trial.variant_id(), trial.repetition()).second &&
                evidence_fields(trial.evidence(), {"loaded_version"}).str("loaded_version") ==
                    b.str("version");
            judge_failed |= trial.error_code() == "evaluation_judge_failed" ||
                incomplete_goal_assessment(trial.evidence()) ||
                (trial.verdict() != "passed" && trial.verdict() != "failed");
          }
          // A closed child resumes its original complete execution checkpoint.
          // Never turn its unsettled judge rows into fresh Agent executions.
          if (complete_execution && report_tasks.size() == ids.size() && judge_failed) {
            retained_checkpoint = root / source_report;
            reused.clear();
            break;
          }
        }
        // 精确检查点可仅重评失败判定而无需重跑 Agent；拆成新子集会丢失该检查点。
        if (entry.first == key &&
            std::any_of(report.trials().begin(), report.trials().end(), [](const auto &trial) {
              return trial.execution_status() == "completed" &&
                     (trial.error_code() == "evaluation_judge_failed" ||
                      incomplete_goal_assessment(trial.evidence()) ||
                      (trial.verdict() != "passed" && trial.verdict() != "failed"));
            })) {
          reused.clear();
          break;
        }
        std::map<std::string, std::vector<dataset::v1::EvaluationTrial>> grouped;
        for (const auto &trial : report.trials())
          if (trial.variant_id() == "candidate" &&
              std::find(ids.begin(), ids.end(), trial.task_id()) != ids.end() &&
              !reused.count(trial.task_id()))
            grouped[trial.task_id()].push_back(trial);
        bool used = false;
        for (auto &item : grouped) {
          std::set<unsigned> repetitions;
          for (const auto &trial : item.second)
            repetitions.insert(trial.repetition());
          if (item.second.size() == evaluation.num("repetitions", 1) &&
              repetitions.size() == item.second.size()) {
            // 同组其他 trial 失败不使本任务失效；只复用其完整重复且全部目标指标仍可观测的结果。
            Report task_report;
            task_report.set_complete(true);
            for (const auto &trial : item.second)
              *task_report.add_trials() = trial;
            if (scores(task_report).fields().size() != 1)
              continue;
            reused.emplace(item.first, std::move(item.second));
            used = true;
          }
        }
        if (used) {
          sources.push(entry.first);
          // 同 key 的重试必须引用更早 attempt，绝不能引用正在生成的自身报告。
          source_refs.push(Json{{"key", entry.first},
                                {"report", source_report.string()},
                                {"report_hash", previous["report_hash"]}});
          if (joined.schema_version() == 0)
            joined = report;
        }
      }
      if (!reused.empty()) {
        std::vector<std::string> missing;
        for (const auto &id : ids)
          if (!reused.count(id))
            missing.push_back(id);
        Report fresh;
        bool complete = true;
        if (!missing.empty()) {
          fresh = evaluate(candidate, missing, split);
          complete = fresh.complete();
        }
        const auto reused_count = reused.size() * evaluation.num("repetitions", 1);
        for (const auto &trial : fresh.trials())
          reused[trial.task_id()].push_back(trial);
        if (!missing.empty()) {
          auto subset = settings;
          subset["task_ids"] = Json::Array{};
          for (const auto &id : missing)
            subset["task_ids"].push(id);
          const auto fresh_key = evaluation_key(subset);
          const auto &added = static_cast<const Json &>(tree)["evaluations"][fresh_key];
          sources.push(fresh_key);
          source_refs.push(Json{{"key", fresh_key},
                                {"report", added["report"]},
                                {"report_hash", added["report_hash"]}});
        }
        joined.clear_trials();
        for (const auto &id : ids)
          for (const auto &trial : reused[id])
            *joined.add_trials() = trial;
        complete &= static_cast<std::size_t>(joined.trials_size()) ==
                    ids.size() * evaluation.num("repetitions", 1);
        joined.set_report_kind("gepa_cached_evaluation.v1");
        joined.set_run_id("gepa-cache-" + key);
        joined.set_config_fingerprint(key);
        joined.set_complete(complete);
        joined.clear_comparisons();
        auto configuration = json(joined.configuration());
        configuration["task_ids"] = selected;
        *joined.mutable_configuration() = proto<google::protobuf::Struct>(configuration);
        *joined.mutable_summary() =
            proto<google::protobuf::Struct>(Json{{"trials", std::size_t(joined.trials_size())},
                                                 {"reused_trials", reused_count},
                                                 {"source_reports", sources},
                                                 {"source_report_refs", source_refs}});
        cache["attempt"] = cache.num("attempt", 0) + 1;
        cache["status"] =
            scores(joined).fields().size() == ids.size() ? "complete" : "inconclusive";
        cache["version"] = b["version"];
        cache["evaluation_context"] = evaluation_context;
        cache["candidate_evaluation_context"] = candidate_evaluation_context;
        cache["split"] = split;
        cache["task_ids"] = selected;
        cache["trials_reserved"] = 0;
        cache["source_reports"] = sources;
        cache["source_report_refs"] = source_refs;
        const auto directory =
            fs::path("evaluations") / key / ("attempt-" + std::to_string(cache.num("attempt", 0)));
        const auto bytes = json(joined).dump();
        write(root / directory / "report.json", bytes);
        write(root / directory / "report.pb", joined.SerializeAsString());
        cache["report_hash"] = hash(bytes);
        cache["report"] = (directory / "report.json").string();
        tree["evaluations"][key] = cache;
        save();
        return joined;
      }
    }
    TaskEvaluationOptions options;
    options.config_json = settings.dump();
    options.resume_report = retained_checkpoint;
    const auto previous_attempts = cache.num("attempt", 0);
    if (options.resume_report.empty() && previous_attempts > 0 &&
        (final || config.flag("cache_evaluations", true))) {
      // tree 会在启动外部评测前先持久化新 attempt。若服务随后优雅停机，
      // 最新目录可能还没有 manifest；必须向前找最近的可验证检查点，不能因一个
      // 空 attempt 重跑已完成题。这里只做文件级完整性筛选；数据集、任务版本、
      // 策略版本、裁判指纹及 trial 唯一性仍由统一评测器在执行前强校验。
      for (auto attempt = previous_attempts; attempt > 0; --attempt) {
        const auto previous =
            root / "evaluations" / key / ("attempt-" + std::to_string(attempt));
        try {
          if (!fs::exists(previous / "manifest.json") ||
              !fs::exists(previous / "report.json"))
            continue;
          const auto bytes = read(previous / "report.json", kMaxEvaluationReportBytes);
          const auto manifest = Json::parse(read(previous / "manifest.json", 16777216));
          const auto report = proto<Report>(Json::parse(bytes));
          if (manifest["files"].str("report.json") != hash(bytes) ||
              manifest.str("run_id") != report.run_id())
            continue;
          // 先拒绝明显身份冲突，避免坏的新报告遮住更早的有效检查点。
          // 完整评分指纹仍由 task evaluator 用当前冻结配置再校验一次。
          if (report.schema_version() != 1 || report.report_kind() != "task_evaluation.v1" ||
              report.dataset_version() != dataset.dataset_version())
            continue;
          std::set<std::string> allowed_tasks, trial_ids;
          std::map<std::string, std::string> task_versions, variant_versions;
          std::set<std::tuple<std::string, std::string, std::size_t>> seen;
          for (const auto &id : settings["task_ids"].items())
            allowed_tasks.insert(id.text());
          for (const auto &task : dataset.tasks())
            if (allowed_tasks.count(task.task_id()))
              task_versions[task.task_id()] = task.task_version();
          for (const auto &variant : settings["variants"].items())
            variant_versions[variant.str("id")] = variant.str("version");
          bool valid = task_versions.size() == allowed_tasks.size(), reusable = false;
          for (const auto &trial : report.trials()) {
            const auto identity =
                std::make_tuple(trial.task_id(), trial.variant_id(), trial.repetition());
            const auto evidence = evidence_fields(trial.evidence(), {"loaded_version"});
            valid &= allowed_tasks.count(trial.task_id()) &&
                     variant_versions.count(trial.variant_id()) && !trial.trial_id().empty() &&
                     trial.split() == split && trial.repetition() > 0 &&
                     trial.repetition() <= settings.num("repetitions", 1) &&
                     task_versions[trial.task_id()] == trial.task_version() &&
                     evidence.str("loaded_version") == variant_versions[trial.variant_id()] &&
                     trial_ids.insert(trial.trial_id()).second && seen.insert(identity).second;
            reusable |= valid && trial.execution_status() == "completed" &&
                        (trial.error_code() == "evaluation_judge_failed" ||
                         (trial.error_code().empty() &&
                          (trial.verdict() == "passed" || trial.verdict() == "failed" ||
                           trial.verdict() == "uncertain")));
            if (!valid)
              break;
          }
          if (!valid || !reusable)
            continue;
          options.resume_report = previous / "report.json";
          break;
        } catch (const std::exception &) {
          // 单个旧 attempt 不完整或已损坏时继续回溯；绝不把它当成可用成绩。
        }
      }
    }
    fs::path shared_source;
    // 共享报告也可以作为混合恢复源；缺失目标遥测的 trial 会在下方逐条排除，
    // 不再用一个整批布尔标记关闭所有完整执行的恢复。
    if (shared_reuse && options.resume_report.empty()) {
      shared_source = shared_evaluation_report(settings, final);
      if (!shared_source.empty()) {
        options.resume_report = shared_source;
        cache["shared_cache"] = "hit";
      }
    }
    const auto planned_cost =
        ids.size() * evaluation.num("repetitions", 1) * (final && !final_candidate_only ? 2 : 1);
    auto execution_cost = planned_cost;
    if (!options.resume_report.empty()) {
      const auto previous = proto<Report>(Json::parse(read(options.resume_report, kMaxEvaluationReportBytes)));
      std::size_t reusable = 0;
      for (const auto &trial : previous.trials()) {
        if (trial.execution_status() != "completed")
          continue;
        const auto evidence = evidence_fields(trial.evidence(), {"loop_review"});
        const bool reviewed =
            !static_cast<const Json &>(evaluation)["judge"].has("loop_review_prompt") ||
                              evidence["loop_review"].str("status") == "complete";
        const bool judge_error =
            trial.error_code() == "evaluation_judge_failed" &&
            std::any_of(trial.checks().begin(), trial.checks().end(), [](const auto &check) {
              return check.check_id() == "model" && check.status() == "error";
            });
        // token 等宿主遥测无法通过裁判补评生成。即使同一个 trial 同时存在
        // 裁判错误，也必须重跑该 trial；其余裁判错误仍只重评旧执行。
        const bool rerun_missing_metric = required_token_metric_missing(trial);
        if (rerun_missing_metric)
          options.resume_rerun_trial_ids.insert(trial.trial_id());
        // 裁判未知的恢复仅消费裁判调用，不重复占用真实 Agent 执行次数。
        const bool judge_uncertain =
            trial.execution_status() == "completed" && trial.error_code().empty() &&
            (trial.verdict() == "uncertain" ||
             incomplete_goal_assessment(trial.evidence())) &&
            std::any_of(trial.checks().begin(), trial.checks().end(), [](const auto &check) {
              return check.check_id() == "model" && check.status() != "error";
            });
        reusable += !rerun_missing_metric &&
                    (judge_error || judge_uncertain ||
                     (reviewed && trial.error_code().empty() &&
                      (trial.verdict() == "passed" || trial.verdict() == "failed")));
      }
      execution_cost -= std::min(execution_cost, reusable);
    }
    // trials_used 是保守搜索预算，不是“已完成题数”。同一 evaluation key
    // 的 retry 只填补原计划，已预留的任务不再计费；但如果首次只尝试复用
    // 共享检查点而预留为 0，后续发现其产物损坏必须真实重跑时，仍要补足预算。
    // 新字段保留同 key 累计预留；旧 tree 若曾为该 key 预留过正数，按整个
    // 冻结计划已受预算保护迁移，避免升级后立即重复扣费。
    const auto previously_reserved = cache.has("trials_budget_reserved")
                                         ? cache.num("trials_budget_reserved", 0)
                                         : previous_attempts > 0 && cache.num("trials_reserved", 0)
                                               ? planned_cost
                                               : std::size_t{0};
    const auto budget_cost = execution_cost > previously_reserved
                                 ? execution_cost - previously_reserved
                                 : std::size_t{0};
    if (!final)
      search_check(execution_cost);
    require(tree.num("trials_used", 0) + budget_cost +
                    (final ? final_holdout_reserve : reserve) <=
                config.num("max_trials", 400),
            "gepa_trial_budget_exhausted");
    tree["trials_used"] = tree.num("trials_used", 0) + budget_cost;
    cache["attempt"] = cache.num("attempt", 0) + 1;
    cache["status"] = "running";
    cache["trials_reserved"] = budget_cost;
    cache["trials_budget_reserved"] = previously_reserved + budget_cost;
    cache["version"] = b["version"];
    cache["evaluation_context"] = evaluation_context;
    cache["candidate_evaluation_context"] = execution_context;
    cache["split"] = split;
    cache["task_ids"] = selected;
    if (final && !final_candidate_only)
      cache["baseline_version"] = a["version"];
    tree["evaluations"][key] = cache;
    save();
    const auto cutoff = final ? deadline() : deadline() - holdout_time();
    options.cancelled = [&] { return (cancelled && cancelled()) || now() >= cutoff; };
    const auto directory =
        root / "evaluations" / key / ("attempt-" + std::to_string(cache.num("attempt", 0)));
    const auto evaluation_started = now();
    Report report;
    try {
      report = run_task_evaluation(dataset_path, evaluation_path, directory, options);
    } catch (const std::exception &exc) {
      const std::string code = exc.what();
      const bool safe_code = !code.empty() && code.size() <= 128 &&
          std::all_of(code.begin(), code.end(), [](unsigned char ch) {
            return std::isalnum(ch) || ch == '_' || ch == '-' || ch == '.';
          });
      cache["error_code"] = safe_code ? code : "non_identifier_exception";
      std::cerr << "[gepa] evaluation_exception code=" << cache.str("error_code") << '\n';
      const bool stale = code.rfind("evaluation_checkpoint_", 0) == 0 ||
                         code.rfind("evaluation_artifact_", 0) == 0 ||
                         code == "evaluation_file_not_regular" ||
                         code == "evaluation_file_size_invalid" || code == "evaluation_read_failed";
      cache["status"] = "error";
      if (!shared_source.empty() && stale)
        cache["shared_cache"] = "invalid";
      tree["evaluations"][key] = cache;
      save();
      if (!shared_source.empty() && stale) {
        // 完整恢复校验发现已清理/变化的产物时，另开正常 attempt；不把坏缓存变成实验失败。
        const auto identity = shared_evaluation_identity(settings, final);
        try {
          write(root.parent_path().parent_path() / "candidate-cache" /
                    (hash(identity.dump()) + ".json"),
                "{}");
        } catch (const std::exception &) {
        }
        return evaluate(candidate, ids, split, final, false, final_candidate_only, final_campaign);
      }
      throw;
    }
    cache["status"] = scored(report) && (final || scores(report).fields().size() == ids.size())
                          ? "complete"
                          : "inconclusive";
    cache["report_hash"] = hash(read(directory / "report.json", kMaxEvaluationReportBytes));
    cache["report"] =
        (fs::path("evaluations") / key / directory.filename() / "report.json").string();
    tree["evaluations"][key] = cache;
    if (report.complete())
      record_evaluation_time(now() - evaluation_started, execution_cost);
    save();
    remember_shared_evaluation(settings, final, directory / "report.json", report);
    if (final && !final_candidate_only && split == "holdout" &&
        config.flag("cache_evaluations", true) && scored(report)) {
      try {
        holdout_baselines().remember(directory / "report.json", report);
        tree["holdout_baseline_cache"] = holdout_baselines().lookup();
        require(tree["holdout_baseline_cache"].str("status") == "hit",
                "gepa_holdout_baseline_changed");
        save();
      } catch (const std::exception &) {
        tree["holdout_baseline_cache"]["status"] = "invalid";
        tree["holdout_baseline_cache"]["reason"] = "source_validation_failed";
        save();
      }
    }
    // Intermediate protobuf reports duplicate the JSON cache byte-for-byte in
    // storage cost.  The in-memory report above has already supplied scoring,
    // feedback and recovery decisions; keep the protobuf only for final
    // holdout/baseline evidence.  A missing PB therefore never invalidates a
    // resumable search checkpoint.
    // artifact_retention belongs to the evaluation config copied into
    // ``evaluation`` above, not to the GEPA search config.  Reading the latter
    // silently disabled the lightweight mode and retained a full protobuf
    // beside every intermediate JSON report.
    const auto &retention = static_cast<const Json &>(evaluation)["artifact_retention"];
    if (!final && retention.object() && retention.flag("drop_intermediate_pb", false)) {
      std::error_code remove_error;
      fs::remove(directory / "report.pb", remove_error);
    }
    check();
    if (!final && now() >= cutoff)
      throw Error("gepa_search_time_reserved");
    for (const auto &trial : report.trials())
      for (const auto &check : trial.checks())
        if (check.check_id() == "model" && check.status() == "error" &&
            check.reason() == "evaluation_judge_http_429")
          throw Error("gepa_model_rate_limited");
    // 对暂态判定或 runner 失败最多恢复一次。检查点会保留成功 trial，故孤立 runner
    // 失败只会单独重放，不应扩大为整批重跑。
    if (!cache.flag("automatic_recovery_used", false) &&
        (final || config.flag("cache_evaluations", true)) &&
        fs::exists(directory / "manifest.json") &&
        tree.num("trials_used", 0) + (final ? final_holdout_reserve : reserve) <=
            config.num("max_trials", 400)) {
      const std::set<std::string> transient{"evaluation_judge_transport_6",
                                            "evaluation_judge_transport_7",
                                            "evaluation_judge_transport_18",
                                            "evaluation_judge_transport_28",
                                            "evaluation_judge_transport_52",
                                            "evaluation_judge_transport_55",
                                            "evaluation_judge_transport_56",
                                            "evaluation_judge_http_408",
                                            "evaluation_judge_http_500",
                                            "evaluation_judge_http_502",
                                            "evaluation_judge_http_503",
                                            "evaluation_judge_http_504",
                                            "invalid_json"};
      bool regrade = false, rerun_incomplete_metrics = false;
      for (const auto &trial : report.trials()) {
        const std::set<std::string> runner{"runner_nonzero_exit",    "runner_wait_failed",
                                           "runner_poll_failed",     "runner_read_failed",
                                           "runner_cleanup_timeout", "runner_timeout"};
        if (trial.execution_status() != "completed") {
          if (runner.count(trial.error_code())) {
            regrade = true;
            continue;
          }
          return report;
        }
        if (required_objective_metrics_missing(trial)) {
          regrade = true;
          rerun_incomplete_metrics |= required_token_metric_missing(trial);
          continue;
        }
        if (trial.error_code().empty()) {
          // 证据预览可能遗漏关键内容。最多一次重评完整执行证据，仍未知就保持未决；
          // 不能反复投票到通过，也不能通过重跑 Agent 换一份更有利的答案。
          if ((trial.verdict() == "uncertain" ||
               incomplete_goal_assessment(trial.evidence())) &&
              std::any_of(trial.checks().begin(), trial.checks().end(), [](const auto &check) {
                return check.check_id() == "model" && check.status() != "error";
              }))
            regrade = true;
          continue;
        }
        if (trial.error_code() != "evaluation_judge_failed")
          return report;
        const auto failed =
            std::find_if(trial.checks().begin(), trial.checks().end(), [](const auto &check) {
              return check.check_id() == "model" && check.status() == "error";
            });
        const auto invalid_loop_review =
            failed != trial.checks().end() &&
            failed->reason().rfind("evaluation_loop_review_invalid:", 0) == 0;
        // 业务裁判返回非法明细、矛盾结论或核验格式错误时，只补评该 trial 的旧轨迹。
        // 不把一次格式失误扩散成整批父子策略不可比较，也不重跑真实 Agent。
        const auto invalid_verdicts =
            failed != trial.checks().end() &&
            (failed->reason().rfind("evaluation_turn_verdict_invalid", 0) == 0 ||
             failed->reason().rfind("evaluation_goal_verdict_invalid", 0) == 0 ||
             failed->reason().rfind("evaluation_judge_summary_conflict", 0) == 0 ||
             failed->reason() == "evaluation_judge_reason_conflict" ||
             failed->reason() == "evaluation_consistency_response_invalid");
        if (failed == trial.checks().end() ||
            (!transient.count(failed->reason()) && !invalid_loop_review && !invalid_verdicts))
          return report;
        regrade = true;
      }
      if (regrade) {
        // 人工取消/恢复不消耗这一次自动恢复额度。缺遥测时不能把原 trial
        // 作为已完成检查点恢复；递归入口会先复用同批其他完整题，仅重跑缺失题。
        cache["automatic_recovery_used"] = true;
        if (rerun_incomplete_metrics)
          cache["rerun_incomplete_metrics"] = true;
        tree["evaluations"][key] = cache;
        save();
        return evaluate(candidate, ids, split, final, shared_reuse, final_candidate_only,
                        final_campaign);
      }
    }
    return report;
  }

  // 父评测的基础设施未决不能立即消耗一个策略提案。沿用同一父版本和题目，
  // 在 evaluate 自身的恢复之后最多再补一次；计数先落盘，重启不重新获得额度。
  // 完整题仍由 evaluate 的逐题缓存复用，缺用量只能重跑，不能补零或只换裁判。
  Report evaluate_parent(Json &node, const Json &parent, const std::vector<std::string> &tasks) {
    require(node["parent_evaluation"].str("status") != "inconclusive",
            "gepa_parent_evaluation_inconclusive");
    auto report = evaluate(parent, tasks, "train");
    if (scores(report).fields().size() != tasks.size()) {
      bool recoverable = false, permanent = false;
      const std::set<std::string> runner{"runner_nonzero_exit", "runner_wait_failed",
          "runner_poll_failed", "runner_read_failed", "runner_cleanup_timeout", "runner_timeout"};
      for (const auto &trial : report.trials()) {
        if (trial.variant_id() != "candidate" ||
            (scored_trial(trial) && !required_objective_metrics_missing(trial)))
          continue;
        const bool missing_metrics = trial.execution_status() == "completed" &&
            (trial.error_code().empty() || trial.error_code() == "evaluation_judge_failed") &&
            required_objective_metrics_missing(trial);
        recoverable |= missing_metrics || runner.count(trial.error_code());
        permanent |= !missing_metrics && !runner.count(trial.error_code());
      }
      if (recoverable && !permanent && node.num("parent_recovery_attempts", 0) == 0) {
        node["parent_recovery_attempts"] = 1;
        node["parent_evaluation"]["status"] = "recovering";
        save();
        report = evaluate(parent, tasks, "train");
      }
    }
    const bool complete = scores(report).fields().size() == tasks.size();
    node["parent_evaluation"]["status"] = complete ? "complete" : "inconclusive";
    node["parent_evaluation"]["missing_task_ids"] = Json::Array{};
    if (!complete) {
      // 从预计集合开始，报告完全漏掉的题也必须显示；重复试验中任一失败仍保留该题。
      std::set<std::string> missing(tasks.begin(), tasks.end()), invalid;
      for (const auto &trial : report.trials())
        if (trial.variant_id() == "candidate") {
          if (scored_trial(trial) && !required_objective_metrics_missing(trial))
            missing.erase(trial.task_id());
          else
            invalid.insert(trial.task_id());
        }
      for (const auto &id : tasks)
        if (invalid.count(id))
          missing.insert(id);
      for (const auto &id : missing)
        node["parent_evaluation"]["missing_task_ids"].push(id);
    }
    save();
    require(complete, "gepa_parent_evaluation_inconclusive");
    return report;
  }

  // 把逐题证据转换为搜索目标；完成度、严格通过率和成本分别保存，未知不补零。
  Json scores(const Report &report) const {
    Json result = Json::Object{};
    if (!report.complete())
      return result;
    std::set<std::string> incomplete;
    for (const auto &trial : report.trials()) {
      if (trial.variant_id() != "candidate")
        continue;
      if (incomplete.count(trial.task_id()))
        continue;
      if (!scored_trial(trial) || required_objective_metrics_missing(trial)) {
        result.fields().erase(trial.task_id());
        incomplete.insert(trial.task_id());
        continue;
      }
      const auto metrics = json(trial.metrics()), evidence = json(trial.evidence());
      bool passed = trial.verdict() == "passed";
      double quality = assessment_quality(evidence, passed);
      // 可复用已测量的基线执行，但比较候选质量前必须将当前确定性契约应用到保存的轨迹上。
      for (const auto &task : dataset.tasks())
        if (task.task_id() == trial.task_id()) {
          const bool contract_ok =
              schedule_contract_check(json(task.attributes()), evidence).str("status") != "failed";
          passed &= contract_ok;
          if (!contract_ok)
            quality = 0;
          break;
        }
      // 配置中的确定性硬检查仍优先；不能因模型给了部分完成分而抵消真实违规。
      for (const auto &check : trial.checks())
        if (check.check_id() != "model" && check.status() == "failed")
          quality = 0;
      const auto loops = evidence.has("loop_review")
                             ? (evidence["loop_review"].str("status") == "complete"
                                    ? evidence["loop_review"]["count"]
                                    : Json())
                             : evidence["trajectory"]["summary"]["invalid_loop_lower_bound"];
      const auto objective = effective_objective();
      const auto lw = number(objective["invalid_loop_weight"], 0.2);
      const auto tw = number(objective["token_weight"], 0.05);
      const auto dw = number(objective["latency_weight"], 0);
      const bool need_loops = lw || gate.has("min_invalid_loop_reduction");
      const bool unknown_tokens = metrics["tokens"].null();
      auto penalty = [](double x, double scale) { return x / (scale + x); };
      // 此有界代价的上确界为 1。显式保守策略据此排序，绝不伪造未测得的 token 数。
      const auto token_cost = unknown_tokens ? 1.0
                                             : penalty(number(metrics["tokens"]),
                                                       number(objective["token_scale"], 10000));
      const double fitness =
          quality - lw * penalty(number(loops), number(objective["invalid_loop_scale"], 1)) -
          tw * token_cost -
          dw * penalty(trial.duration_ms(), number(objective["latency_scale_ms"], 60000));
      auto &row = result[trial.task_id()];
      row["fitness"] = number(row["fitness"]) + fitness;
      row["quality"] = number(row["quality"]) + quality;
      row["strict_pass_rate"] = number(row["strict_pass_rate"]) + (passed ? 1 : 0);
      if (evidence["assessment"].object() &&
          evidence["assessment"].has("critical_failures"))
        row["critical_failures"] = number(row["critical_failures"]) +
                                   evidence["assessment"].num("critical_failures", 0);
      row["duration_ms"] = number(row["duration_ms"]) + trial.duration_ms();
      if (!unknown_tokens) {
        row["tokens"] = number(row["tokens"]) + number(metrics["tokens"]);
        row["token_observations"] = row.num("token_observations", 0) + 1;
      }
      if (!metrics["llm_calls"].null()) {
        row["llm_calls"] = number(row["llm_calls"]) + number(metrics["llm_calls"]);
        row["llm_call_observations"] = row.num("llm_call_observations", 0) + 1;
      }
      if (need_loops) {
        const auto method = evidence.has("loop_review") ? "model_review" : "rule_lower_bound";
        if (row.has("loop_method") && row.str("loop_method") != method) {
          result.fields().erase(trial.task_id());
          incomplete.insert(trial.task_id());
          continue;
        }
        row["loop_method"] = method;
        row["invalid_loops"] = number(row["invalid_loops"]) + number(loops);
      }
      if (lw)
        row["loop_efficiency"] = number(row["loop_efficiency"]) + 1 -
                                 penalty(number(loops), number(objective["invalid_loop_scale"], 1));
      if (tw)
        row["token_efficiency"] = number(row["token_efficiency"]) + 1 - token_cost;
      if (tw && unknown_tokens)
        row["unknown_token_trials"] = row.num("unknown_token_trials", 0) + 1;
      if (dw)
        row["latency_efficiency"] =
            number(row["latency_efficiency"]) + 1 -
            penalty(trial.duration_ms(), number(objective["latency_scale_ms"], 60000));
      row["repetitions"] = row.num("repetitions", 0) + 1;
    }
    // 搜索比较必须覆盖整批任务；逐题缓存复用会先构造单题报告调用本函数，
    // 因此完整题仍可独立复用，而任一未决题不会让残缺批次参与父子选择。
    if (!incomplete.empty())
      return Json::Object{};
    for (auto &item : result.fields()) {
      item.second["fitness"] = number(item.second["fitness"]) / item.second.num("repetitions", 1);
      item.second["quality"] = number(item.second["quality"]) / item.second.num("repetitions", 1);
      item.second["strict_pass_rate"] =
          number(item.second["strict_pass_rate"]) / item.second.num("repetitions", 1);
      if (item.second.has("critical_failures"))
        item.second["critical_failures"] =
            number(item.second["critical_failures"]) / item.second.num("repetitions", 1);
      for (const auto *field : {"invalid_loops", "loop_efficiency", "token_efficiency",
                                "latency_efficiency", "duration_ms"})
        if (item.second.has(field))
          item.second[field] = number(item.second[field]) / item.second.num("repetitions", 1);
      if (item.second.has("tokens"))
        item.second["tokens"] =
            number(item.second["tokens"]) / item.second.num("token_observations", 1);
      if (item.second.has("llm_calls"))
        item.second["llm_calls"] =
            number(item.second["llm_calls"]) / item.second.num("llm_call_observations", 1);
    }
    return result;
  }

  // 计算非空指标集合的均值，避免将缺失观测当作零计入。
  static double mean(const Json &values, const char *field) {
    double sum = 0;
    for (const auto &item : values.fields())
      sum += number(item.second[field]);
    return values.fields().empty() ? -1e9 : sum / values.fields().size();
  }

  // 判断当前候选距离发布要求还有哪些缺口；搜索排序与发布门槛不是同一概念。
  Json optimization_status(const Json &candidate) const {
    const auto &base = static_cast<const Json &>(tree)["baseline_validation"];
    Json reasons = Json::Array{}, hard = Json::Array{}, criteria = Json::Object{};
    double gap = 0;
    auto reject = [&](bool condition, const char *code, double distance, bool hard_constraint) {
      if (!condition)
        return;
      reasons.push(code);
      if (hard_constraint)
        hard.push(code);
      gap += std::max(0.0, distance);
    };
    bool paired = candidate.object() && base.object() && !candidate.fields().empty() &&
                  candidate.fields().size() == base.fields().size();
    if (paired)
      for (const auto &item : base.fields())
        paired &= candidate.has(item.first);
    reject(!paired, "validation_pair_incomplete", 1, true);
    const auto quality = paired ? mean(candidate, "quality") : -1;
    const auto base_quality = paired ? mean(base, "quality") : -1;
    const bool goals = static_cast<const Json &>(evaluation)["judge"].str(
                           "assessment_mode", "legacy") == "objectives";
    const auto *minimum_quality_key = goals ? "min_quality" : "min_pass_rate";
    const bool has_minimum_quality = gate.has(minimum_quality_key);
    const auto minimum_quality = has_minimum_quality ? number(gate[minimum_quality_key]) : 0;
    const auto allowed_drop = number(gate[goals ? "max_quality_drop" : "max_pass_rate_drop"], 0);
    criteria["quality"] = quality;
    criteria["baseline_quality"] = base_quality;
    if (has_minimum_quality)
      criteria["minimum_quality"] = minimum_quality;
    criteria["maximum_quality_drop"] = allowed_drop;
    reject(paired && has_minimum_quality && quality < minimum_quality - 1e-12,
           "quality_below_threshold",
           (minimum_quality - quality) / std::max(0.1, minimum_quality), true);
    reject(paired && quality + allowed_drop < base_quality - 1e-12, "quality_regression",
           (base_quality - allowed_drop - quality) / std::max(0.1, base_quality), true);
    const auto *minimum_gain_key = goals ? "min_quality_gain" : "min_pass_rate_gain";
    if (gate.has(minimum_gain_key))
      reject(paired && quality < base_quality + number(gate[minimum_gain_key]),
             minimum_gain_key,
             paired ? (base_quality + number(gate[minimum_gain_key]) - quality) /
                          std::max(0.1, base_quality + number(gate[minimum_gain_key]))
                    : 1,
             true);
    if (!resources_report_only() && gate.has("max_latency_ratio") && paired) {
      const auto latency = mean(candidate, "duration_ms"), base_latency = mean(base, "duration_ms");
      const auto ratio = number(gate["max_latency_ratio"], 1);
      criteria["duration_ms"] = latency;
      criteria["baseline_duration_ms"] = base_latency;
      criteria["maximum_latency_ratio"] = ratio;
      reject(base_latency <= 0 || latency < 0 || latency > base_latency * ratio,
             "max_latency_ratio",
             base_latency > 0 && ratio > 0 ? latency / (base_latency * ratio) - 1 : 1, true);
    }
    if (!resources_report_only() && gate.has("max_token_ratio") && paired) {
      bool observed = true;
      for (const auto &item : base.fields())
        observed &= item.second.has("tokens") &&
                    item.second.num("token_observations", 0) == item.second.num("repetitions", 0) &&
                    candidate[item.first].has("tokens") &&
                    candidate[item.first].num("token_observations", 0) ==
                        candidate[item.first].num("repetitions", 0);
      const auto tokens = observed ? mean(candidate, "tokens") : -1;
      const auto base_tokens = observed ? mean(base, "tokens") : -1;
      const auto ratio = number(gate["max_token_ratio"], 1);
      criteria["tokens"] = observed ? Json(tokens) : Json();
      criteria["baseline_tokens"] = observed ? Json(base_tokens) : Json();
      criteria["maximum_token_ratio"] = ratio;
      reject(!observed || base_tokens <= 0 || tokens > base_tokens * ratio,
             !observed ? "token_metric_unobserved" : "max_token_ratio",
             observed && base_tokens > 0 && ratio > 0 ? tokens / (base_tokens * ratio) - 1 : 1,
             true);
    }
    if (!resources_report_only() && gate.has("max_llm_call_ratio") && paired) {
      bool observed = true;
      for (const auto &item : base.fields())
        observed &= item.second.has("llm_calls") &&
                    item.second.num("llm_call_observations", 0) == item.second.num("repetitions", 0) &&
                    candidate[item.first].has("llm_calls") &&
                    candidate[item.first].num("llm_call_observations", 0) ==
                        candidate[item.first].num("repetitions", 0);
      const auto calls = observed ? mean(candidate, "llm_calls") : -1;
      const auto base_calls = observed ? mean(base, "llm_calls") : -1;
      const auto ratio = number(gate["max_llm_call_ratio"], 1);
      criteria["llm_calls"] = observed ? Json(calls) : Json();
      criteria["baseline_llm_calls"] = observed ? Json(base_calls) : Json();
      criteria["maximum_llm_call_ratio"] = ratio;
      reject(!observed || base_calls < 0 || calls > base_calls * ratio,
             !observed ? "llm_call_metric_unobserved" : "max_llm_call_ratio",
             observed && base_calls > 0 && ratio > 0 ? calls / (base_calls * ratio) - 1 : 1,
             true);
    }
    if (gate.has("min_invalid_loop_reduction") && paired) {
      bool observed = true;
      std::string method;
      for (const auto &item : base.fields()) {
        const auto &value = candidate[item.first];
        observed &= item.second.has("invalid_loops") && value.has("invalid_loops") &&
                    !item.second["invalid_loops"].null() && !value["invalid_loops"].null() &&
                    item.second.str("loop_method") == value.str("loop_method");
        if (method.empty())
          method = item.second.str("loop_method");
        observed &= item.second.str("loop_method") == method;
      }
      const auto loops = observed ? mean(candidate, "invalid_loops") : -1;
      const auto base_loops = observed ? mean(base, "invalid_loops") : -1;
      const auto reduction = number(gate["min_invalid_loop_reduction"], 0);
      const auto limit = base_loops * (1 - reduction);
      criteria["invalid_loops"] = observed ? Json(loops) : Json();
      criteria["baseline_invalid_loops"] = observed ? Json(base_loops) : Json();
      criteria["minimum_loop_reduction"] = reduction;
      reject(!observed, "loop_metric_unobserved", 1, false);
      reject(observed && reduction > 0 && base_loops <= 0, "loop_baseline_has_no_observed_waste", 1,
             false);
      reject(observed && base_loops > 0 && loops > limit + 1e-12, "loop_target_not_met",
             (loops - limit) / std::max(1.0, base_loops), false);
    }
    std::size_t supported = 0;
    if (paired)
      for (const auto &item : base.fields()) {
        const auto &value = candidate[item.first];
        bool stable = number(value["quality"]) + 1e-12 >= number(item.second["quality"]);
        if (!resources_report_only() && gate.has("max_latency_ratio"))
          stable &=
              number(value["duration_ms"]) <=
              number(item.second["duration_ms"]) * number(gate["max_latency_ratio"], 1) + 1e-12;
        if (!resources_report_only() && gate.has("max_token_ratio"))
          stable &= value.has("tokens") && item.second.has("tokens") &&
                    number(value["tokens"]) <=
                        number(item.second["tokens"]) * number(gate["max_token_ratio"], 1) + 1e-12;
        if (!resources_report_only() && gate.has("max_llm_call_ratio"))
          stable &= value.has("llm_calls") && item.second.has("llm_calls") &&
                    number(value["llm_calls"]) <=
                        number(item.second["llm_calls"]) *
                            number(gate["max_llm_call_ratio"], 1) + 1e-12;
        if (gate.has("min_invalid_loop_reduction"))
          stable &= value.has("invalid_loops") && item.second.has("invalid_loops") &&
                    number(value["invalid_loops"]) <= number(item.second["invalid_loops"]) + 1e-12;
        supported += stable;
      }
    const auto robustness = paired ? double(supported) / base.fields().size() : 0;
    criteria["paired_nonregression"] = robustness;
    if (resources_report_only())
      criteria["resources_report_only"] = true;
    const bool feasible = paired && hard.items().empty();
    const bool passed = feasible && reasons.items().empty();
    return Json{{"passed", passed},         {"feasible", feasible}, {"score", 1.0 / (1.0 + gap)},
                {"robustness", robustness}, {"distance", gap},      {"reasons", reasons},
                {"hard_reasons", hard},     {"criteria", criteria}};
  }

  // 给节点补充验证成绩和来源，不改写训练成绩或混入留出反馈。
  Json annotate_validation(Json &values) const {
    const auto status = optimization_status(values);
    for (auto &item : values.fields()) {
      item.second["goal_score"] = status["score"];
      item.second["feasible_score"] = status.flag("feasible", false) ? 1 : 0;
      item.second["robustness_score"] = status["robustness"];
    }
    return status;
  }

  // 按指定指标方向判断是否改善，保持缺失指标与真实零值的区别。
  bool improves(const Json &child, const Json &parent) const {
    // 发布门禁比较原始循环均值，而非有界效率；更多正确答案不能掩盖配置循环目标的回退。
    if (!gepa::loops_no_worse(child, parent))
      return false;
    const auto difference = mean(child, "quality") - mean(parent, "quality");
    return difference > 1e-12 ||
           (difference >= -1e-12 &&
            mean(child, "fitness") >
                mean(parent, "fitness") + number(config["min_improvement"], 0.000001));
  }

  void annotate_exploration_eligibility(std::size_t index) {
    const auto decision = gepa::exploration_eligibility(tree["nodes"], index, config);
    auto &node = tree["nodes"].items().at(index);
    node["exploration_eligible"] = decision.flag("eligible", false);
    node["exploration_reason"] = decision.str("reason");
    if (decision.has("quality"))
      node["exploration_evidence"] = decision;
    else
      node.fields().erase("exploration_evidence");
  }

  bool refresh_exploration_eligibility() {
    const auto allowed = number(config["exploration_max_quality_drop"], 0.15);
    if (tree.num("exploration_eligibility_revision", 0) >= 2 &&
        std::abs(number(tree["exploration_max_quality_drop"], -1) - allowed) <= 1e-12)
      return false;
    for (std::size_t index = 0; index < tree["nodes"].items().size(); ++index)
      annotate_exploration_eligibility(index);
    tree["exploration_eligibility_revision"] = 2;
    tree["exploration_max_quality_drop"] = allowed;
    return true;
  }

  // 计算当前探索前沿；发布前沿和 winner 仍只包含 accepted 节点。
  std::vector<std::size_t> frontier() const {
    return gepa::pool(gepa::exploration_frontier_map(tree["nodes"], val, config));
  }

  void persist_frontiers() {
    tree["frontier_mapping"] = gepa::exploration_frontier_map(tree["nodes"], val, config);
    tree["frontier"] = Json::Array{};
    for (const auto id : gepa::pool(tree["frontier_mapping"]))
      tree["frontier"].push(id);
    tree["publication_frontier_mapping"] = gepa::frontier_map(tree["nodes"], val, config);
    tree["publication_frontier"] = Json::Array{};
    for (const auto id : gepa::pool(tree["publication_frontier_mapping"]))
      tree["publication_frontier"].push(id);
  }

  // 选择本轮训练子集，控制评测开销；留出集不参与候选改写。
  std::vector<std::string> minibatch(std::size_t round) const {
    const auto count = std::min(config.num("minibatch_size", 4), train.size());
    if (!count)
      return {};
    const bool epochs = config.str("minibatch_strategy", "epoch_shuffled") == "epoch_shuffled";
    auto shuffled = train;
    std::mt19937 random(static_cast<unsigned>(config.num("seed", 42)));
    std::shuffle(shuffled.begin(), shuffled.end(), random);
    std::vector<std::string> selected;
    if (epochs) {
      // 将 epoch 尾部与下一个 epoch 连续连接。跨边界重复的任务仍消费游标，
      // 但不重复放入同一批次；从此前批次重放游标，避免下一批又消费补齐项。
      std::size_t epoch = 0, cursor = 0;
      for (std::size_t batch = 0; batch <= round; ++batch) {
        selected.clear();
        std::set<std::string> present;
        while (selected.size() < count) {
          if (cursor == shuffled.size()) {
            ++epoch;
            cursor = 0;
            shuffled = train;
            random.seed(static_cast<unsigned>(config.num("seed", 42) + epoch));
            std::shuffle(shuffled.begin(), shuffled.end(), random);
          }
          const auto &task = shuffled[cursor++];
          if (present.insert(task).second)
            selected.push_back(task);
        }
      }
    } else
      for (std::size_t i = 0; i < count; ++i)
        selected.push_back(shuffled[(round * count + i) % shuffled.size()]);
    std::sort(selected.begin(), selected.end());
    return selected;
  }

  // 枚举本次允许变化的策略组件；候选不能越过这个集合改写宿主或其他策略部分。
  Json components(const Json &candidate) const {
    Json result = Json::Object{};
    // 候选停用经验时仍保留种子策略已授权的搜索轴，才能把“停用”表示为明确变异。
    const auto authorized = source_experiences(seed);
    for (const auto &item : authorized.items())
      result["experience:" + item.str("memory_id")] = nullptr;
    const auto experiences = source_experiences(candidate);
    for (const auto &item : experiences.items())
      result["experience:" + item.str("memory_id")] = experience_component(item);
    const auto policy = proto<v1::RuntimePolicy>(candidate["policy"]);
    if (controls.flag("enabled", false) && policy.options().requested_level() >= v1::L2) {
      // 通用 schema 规则使用当前请求的工具契约，不依赖预配置的框架或工具白名单。
      result["action_rules"] = Json::Array{};
      for (const auto &rule : policy.options().action_rules())
        if (rule.has_source_experience()) {
          Json arguments = Json::Array{};
          for (const auto &key : rule.required_arguments())
            arguments.push(key);
          Json item{{"memory_id", rule.source_experience().memory_id()},
                    {"operation", rule.operation()},
                    {"required_arguments", arguments},
                    {"max_failed_attempts", std::size_t(rule.max_failed_attempts())},
                    {"reason", rule.reason()},
                    {"match_mode", rule.match_mode()}};
          if (rule.failure_key_arguments_size()) {
            item["failure_key_arguments"] = Json::Array{};
            for (const auto &key : rule.failure_key_arguments())
              item["failure_key_arguments"].push(key);
          }
          if (rule.terminal_error_codes_size()) {
            item["terminal_error_codes"] = Json::Array{};
            item["error_scope_arguments"] = Json::Array{};
            for (const auto &code : rule.terminal_error_codes())
              item["terminal_error_codes"].push(code);
            for (const auto &key : rule.error_scope_arguments())
              item["error_scope_arguments"].push(key);
          }
          if (rule.max_attempts()) {
            item["max_attempts"] = std::size_t(rule.max_attempts());
            item["attempt_key_arguments"] = Json::Array{};
            for (const auto &key : rule.attempt_key_arguments())
              item["attempt_key_arguments"].push(key);
          }
          result["action_rules"].push(std::move(item));
        }
      if (std::find(policy.options().host_capabilities().begin(),
                    policy.options().host_capabilities().end(),
                    "schedule.dispatch") != policy.options().host_capabilities().end()) {
        result["schedule_rules"] = Json::Array{};
        for (const auto &rule : policy.options().schedule_rules())
          if (rule.has_source_experience()) {
            Json task_terms = Json::Array{}, exclude_terms = Json::Array{};
            for (const auto &term : rule.task_terms())
              task_terms.push(term);
            for (const auto &term : rule.exclude_terms())
              exclude_terms.push(term);
            result["schedule_rules"].push(Json{{"memory_id", rule.source_experience().memory_id()},
                                               {"before_operation", rule.before_operation()},
                                               {"after_operation", rule.after_operation()},
                                               {"require_success", rule.require_success()},
                                               {"reason", rule.reason()},
                                               {"task_terms", task_terms},
                                               {"exclude_terms", exclude_terms},
                                               {"before_argument", rule.before_argument()},
                                               {"after_argument", rule.after_argument()}});
          }
      }
      if (policy.options().requested_level() >= v1::L3 &&
          without_generated_controls(policy.options()).loop_rules_size()) {
        result["loop_rules"] = Json::Array{};
        for (const auto &rule : policy.options().loop_rules())
          if (rule.has_source_experience()) {
            result["loop_rules"].push(Json{{"memory_id", rule.source_experience().memory_id()},
                                           {"actor_id", rule.actor_id()},
                                           {"max_iterations", std::size_t(rule.max_iterations())},
                                           {"max_no_progress", std::size_t(rule.max_no_progress())},
                                           {"max_replans", std::size_t(rule.max_replans())},
                                           {"replan_on_failure", rule.replan_on_failure()},
                                           {"reason", rule.reason()},
                                           {"match_mode", rule.match_mode()}});
            auto &value = result["loop_rules"].items().back();
            if (rule.after_action_rejection())
              value["after_action_rejection"] = true;
            if (rule.require_finalization())
              value["require_finalization"] = true;
            if (!rule.error_codes().empty()) {
              value["error_codes"] = Json::Array{};
              for (const auto &code : rule.error_codes())
                value["error_codes"].push(code);
            }
          }
      }
    }
    return result;
  }

  // 在父策略上组合已选择的组件，未选择部分保持父版本内容，便于归因。
  Json compose(const Json &parent, const Json &values) const {
    const auto original = components(parent);
    require(values.object() && original.fields().size() == values.fields().size(),
            "gepa_components_changed");
    for (const auto &item : original.fields())
      require(values.has(item.first), "gepa_components_changed");
    validate_local_data(values);
    auto result = parent;
    const auto policy = proto<v1::RuntimePolicy>(parent["policy"]);
    auto options = policy.options();
    options.clear_experience_overrides();
    options.clear_approved_experiences();
    options.clear_disabled_injections();
    const auto experiences = source_experiences(seed);
    for (const auto &item : experiences.items()) {
      const auto id = item.str("memory_id");
      const auto &value = values["experience:" + id];
      const auto content = experience_content(value);
      require(value.null() ||
                  (!content.empty() && content.size() <= config.num("max_experience_bytes", 2048)),
              "gepa_experience_limit");
      for (const auto &entry : policy.snapshot().experiences())
        if (entry.memory_id() == id) {
          auto *approved = options.add_approved_experiences();
          approved->set_memory_id(id);
          approved->set_content_revision(entry.content_revision());
          if (value.null()) {
            *options.add_disabled_injections() = *approved;
            continue;
          }
          const auto pattern = value.str("trigger_pattern");
          if (entry.content() != content || !pattern.empty() ||
              value["observed_operations"].array()) {
            auto *rewrite = options.add_experience_overrides();
            rewrite->set_content(content);
            *rewrite->mutable_source_experience() = *approved;
            if (pattern != entry.trigger_pattern())
              rewrite->set_trigger_pattern(pattern);
            if (value["observed_operations"].array())
              for (const auto &operation : value["observed_operations"].items())
                rewrite->add_observed_operations(operation.text());
          }
        }
    }
    // 私有/公共重复经验是一个可搜索组件；消融时保留被遮蔽公共经验的批准状态，
    // 并同时停用两侧注入，避免只消融半个语义组件。
    const auto seed_policy = proto<v1::RuntimePolicy>(seed["policy"]);
    // 控制证据不可变，不是可注入的经验组件；在变异与多父合并中都保留其种子批准。
    for (const auto &source : seed_policy.snapshot().experiences()) {
      if (source.control_target_type().empty())
        continue;
      for (const auto &approved : seed_policy.options().approved_experiences())
        if (approved.memory_id() == source.memory_id() &&
            approved.content_revision() == source.content_revision())
          *options.add_approved_experiences() = approved;
    }
    for (const auto &alias : canonical_experience_aliases(seed_policy)) {
      const auto key = "experience:" + alias.second;
      if (!values.has(key))
        continue;
      for (const auto &approved : seed_policy.options().approved_experiences())
        if (approved.memory_id() == alias.first.first &&
            approved.content_revision() == alias.first.second) {
          *options.add_approved_experiences() = approved;
          if (values[key].null())
            *options.add_disabled_injections() = approved;
        }
    }
    std::sort(options.mutable_experience_overrides()->begin(),
              options.mutable_experience_overrides()->end(), [](const auto &a, const auto &b) {
                return a.source_experience().memory_id() < b.source_experience().memory_id();
              });
    if (values.has("action_rules") || values.has("loop_rules") || values.has("schedule_rules")) {
      Json draft{
          {"action_rules",
           values.has("action_rules") ? values["action_rules"] : Json(Json::Array{})},
          {"loop_rules", values.has("loop_rules") ? values["loop_rules"] : Json(Json::Array{})},
          {"schedule_rules",
           values.has("schedule_rules") ? values["schedule_rules"] : Json(Json::Array{})}};
      options = apply_control_draft(policy.snapshot(), without_generated_controls(options),
                                    controls, draft);
    }
    result["policy"] = json(compile_runtime_policy(policy.snapshot(), options));
    require(published_policy_compatible(proto<v1::RuntimePolicy>(result["policy"]),
                                        proto<v1::RuntimePolicy>(seed["policy"]), controls.dump()),
            "gepa_candidate_not_authorized");
    agent_version_id(result.dump());
    return result;
  }

  // 只有明确顺序或真实资源生产/消费才探索新增先后关系。conditional 描述结果条件，
  // 例如“没找到合适结果才执行 B”，不能降格为“A 执行成功后执行 B”。
  Json schedule_training_contract(const std::string &task_id,
                                  const std::string &request = {}) const {
    for (const auto &task : dataset.tasks())
      if (task.task_id() == task_id && task.split() == "train") {
        const auto attributes = json(task.attributes());
        const auto dependency =
            attributes["dependency"].string() ? attributes.str("dependency") : std::string{};
        if (dependency == "parallel" || dependency == "independent" || dependency == "none")
          return Json{{"order_editable", false}, {"ordering_required", false},
                      {"require_success", false}};
        auto text = request.empty() ? task.task_input() : request;
        const auto expectations = turn_expectations(task);
        bool matched_turn = false, conditional = false, any_conditional = false;
        if (expectations.array())
          for (std::size_t i = 0; i < expectations.items().size(); ++i) {
            const bool condition = expectations.items()[i].flag("conditional", false);
            any_conditional |= condition;
            if (i > static_cast<std::size_t>(task.followup_inputs_size()))
              continue;
            const auto &turn_input = i == 0 ? task.task_input() : task.followup_inputs(i - 1);
            if (turn_input == text) {
              matched_turn = true;
              conditional |= condition;
            }
          }
        // 无法定位当前轮时不借用其他轮的成功条件；任务和其他组件照常评测。
        if (!matched_turn)
          conditional = any_conditional;
        std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) {
          return static_cast<char>(c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c);
        });
        auto has = [&](const char *phrase) { return text.find(phrase) != std::string::npos; };
        if (conditional || dependency == "conditional" || has("如果") || has("否则") ||
            has("若没有") || has("若未") || has(" if ") || has(" unless ") || has("otherwise"))
          return Json{{"order_editable", false}, {"ordering_required", false},
                      {"result_conditional", true}};
        if (has("必须并行") || has("要求并行") || has("in parallel"))
          return Json{{"order_editable", false}};
        const bool explicit_dependency = !dependency.empty() && dependency != "independent" &&
                                         dependency != "none";
        const bool success = (explicit_dependency && attributes.flag("stop_on_failure", false)) ||
                             has("成功后") || has("on success") || has("after successful");
        const bool ordering = explicit_dependency || success || (has("先") && has("再")) ||
            has("完成后") || has("弄好后") || has("弄好了") || has("之后再") ||
            has(" before ") || has(" after ") || has(" then ");
        // 未要求先后时是否可编辑仍由绑定生成器检查真实输出→输入依赖。
        return Json{{"order_editable", true}, {"ordering_required", ordering},
                    {"require_success", success},
                    {"priority", ordering ? (explicit_dependency ? (success ? 3 : 2) : 1) : 0}};
      }
    return Json();
  }

  // 每个真实批次按其当前请求绑定契约，多轮任务不会共用另一轮的 conditional 标记。
  void bind_schedule_contracts(Json &row, const std::string &task_id) const {
    auto &runs = row["trial"]["evidence"]["runs"];
    if (!runs.array())
      return;
    for (auto &run : runs.items())
      if (run["schedule_plans"].array())
        for (auto &plan : run["schedule_plans"].items()) {
          auto contract = schedule_training_contract(task_id, plan.str("task_text"));
          if (contract.object())
            contract.fields().erase("priority");
          plan["schedule_contract"] = std::move(contract);
        }
  }

  // 采样仅提高有明确顺序的任务优先级，不删除任务或更改它们的业务验收。
  int schedule_training_priority(const std::string &task_id) const {
    for (const auto &task : dataset.tasks())
      if (task.task_id() == task_id && task.split() == "train") {
        auto priority = schedule_training_contract(task_id, task.task_input()).num("priority", 0);
        for (const auto &turn : task.followup_inputs())
          priority = std::max(priority, schedule_training_contract(task_id, turn).num("priority", 0));
        return static_cast<int>(priority);
      }
    return 0;
  }

  // 反思只读取本轮训练反馈和允许修改的组件；不把验证或留出答案带入提示词。
  Json packet(const Json &parent, const Report &feedback, const Json &selected,
              bool trigger_only = false, const Json *known_scores = nullptr,
              const Json *pool_source_context = nullptr) const {
    // 授权范围仍由冻结种子约束，但反思正文/条件必须来自当前父版本，不能读回旧值。
    const auto available = components(parent), sources = source_experiences(parent);
    Json selected_values = Json::Object{};
    bool control_selected = false;
    for (const auto &item : selected.items()) {
      require(available.has(item.text()), "gepa_component_not_authorized");
      selected_values[item.text()] = available[item.text()];
      control_selected |= item.text() == "action_rules" || item.text() == "loop_rules" ||
                          item.text() == "schedule_rules";
    }
    const bool experience_selected = std::any_of(
        selected.items().begin(), selected.items().end(),
        [](const auto &item) { return item.text().rfind("experience:", 0) == 0; });
    const bool experience_only = experience_selected && !control_selected;
    Json contract = Json::Object{};
    for (const auto *key :
         {"min_pass_rate", "max_pass_rate_drop", "min_pass_rate_gain", "min_quality",
          "min_quality_gain", "max_quality_drop", "min_invalid_loop_reduction",
          "max_latency_ratio", "max_token_ratio", "max_llm_call_ratio"})
      if (gate.has(key) && (!resources_report_only() ||
                          (std::string(key) != "max_latency_ratio" &&
                           std::string(key) != "max_token_ratio" &&
                           std::string(key) != "max_llm_call_ratio")))
        contract[key] = gate[key];
    if (resources_report_only())
      contract["resources_report_only"] = true;
    Json result{
        {"parent_version", agent_version_id(parent.dump())},
        {"experiences", Json::Array{}},
        {"components", selected_values},
        {"selected_components", selected},
        {"feedback", Json::Array{}},
        {"acceptance", config.str("acceptance", "strict_improvement")},
        {"objective", effective_objective()},
        {"optimization_contract", contract},
        {"limits", Json{{"max_experience_bytes", config.num("max_experience_bytes", 2048)}}}};
    if (trigger_only)
      result["trigger_only_exploration"] = true;
    const auto policy = proto<v1::RuntimePolicy>(parent["policy"]);
    for (const auto &source : sources.items()) {
      const auto key = "experience:" + source.str("memory_id");
      if (!control_selected && !selected_values.has(key))
        continue;
      auto experience = source;
      // 控制规则生成需要源经验语义，因为经验组件在该路径被有意省略；经验改写的
      // 当前文本已包含在 components 中，不能重复发送。
      if (!control_selected || selected_values.has(key))
        experience.fields().erase("content");
      result["experiences"].push(experience);
    }
    for (auto &experience : result["experiences"].items()) {
      for (const auto &source : policy.snapshot().experiences())
        if (source.memory_id() == experience.str("memory_id") &&
            selected_values.has("experience:" + source.memory_id()) &&
            experience_content(selected_values["experience:" + source.memory_id()]) !=
                source.content())
          experience["original_content"] = source.content();
    }
    if (controls.flag("enabled", false) && control_selected) {
      result["controls"] = control_generation_input(
          policy.snapshot(), without_generated_controls(policy.options()), controls);
      result["controls"].fields().erase("memories"); // 来源已经在上文提供，不重复发送整份策略快照。
      // 向反思模型提供本次父组件真实可用的增量动作。editable_bindings 是可新增的
      // 候选，不是父规则；父数组为空时只能 add，不能凭候选绑定虚构 set/remove。
      // 该字段只约束候选生成，不改变运行时能力、评分或发布门槛。
      auto &operations = result["controls"]["allowed_update_operations"];
      operations = Json::Object{};
      for (const auto &item : selected.items()) {
        const auto component = item.text();
        if ((component != "action_rules" && component != "loop_rules" &&
             component != "schedule_rules") ||
            !selected_values[component].array())
          continue;
        operations[component] = Json::Array{"add"};
        if (!selected_values[component].items().empty()) {
          operations[component].push("set");
          operations[component].push("remove");
        }
      }
    }
    restrict_reflection_components(result, selected);
    // 只有训练内容能进入反思；验证/留出分数只用于选择候选，其任务、评分标准和轨迹
    // 永远不会进入该请求包，防止验证集泄漏。
    std::vector<const dataset::v1::EvaluationTrial *> trials;
    for (const auto &trial : feedback.trials())
      trials.push_back(&trial);
    const auto feedback_scores = known_scores && feedback.complete() ? *known_scores : scores(feedback);
    result["training_batch_summary"] = Json::Array{};
    std::set<std::string> summary_tasks;
    for (const auto *trial : trials) {
      const auto task = std::find_if(dataset.tasks().begin(), dataset.tasks().end(),
                                    [&](const auto &value) {
                                      return value.task_id() == trial->task_id();
                                    });
      require(task != dataset.tasks().end() && task->split() == "train",
              "gepa_reflection_split_violation");
      if (!summary_tasks.insert(trial->task_id()).second)
        continue;
      Json summary{{"task_id", trial->task_id()}, {"verdict", trial->verdict()},
                   {"execution_status", trial->execution_status()}};
      if (!trial->error_code().empty())
        summary["error_code"] = trial->error_code();
      const auto facts = reflection_scored_facts(feedback_scores[trial->task_id()]);
      for (const auto &field : facts.fields())
        summary[field.first] = field.second;
      result["training_batch_summary"].push(std::move(summary));
    }
    std::map<std::string, bool> relevant;
    std::map<std::string, Json> applications;
    std::set<std::string> associated_sources;
    for (const auto *component : {"action_rules", "loop_rules", "schedule_rules"})
      collect_source_memory_ids(available[component], associated_sources);
    if (pool_source_context && pool_source_context->array())
      for (const auto &source : pool_source_context->items())
        if (source.has("content") || source.has("original_content"))
          associated_sources.insert(source.str("memory_id"));
    bool has_applied_experience = false;
    std::map<const dataset::v1::EvaluationTrial *, Json> reviewed_views;
    for (const auto *trial : trials) {
      auto view = reviewed_feedback_trial(*trial);
      if (selected_values.has("loop_rules"))
        merge_loop_requirements(result["controls"]["loop_requirements"],
                                view["evidence"]["loop_requirements"]);
      if (selected_values.has("action_rules"))
        merge_action_requirements(result["controls"]["action_requirements"],
                                  view["evidence"]["action_requirements"]);
      reviewed_views[trial] = std::move(view);
    }
    for (const auto *trial : trials) {
      const auto &view = reviewed_views.at(trial);
      Json row{{"trial", view}};
      if (selected_values.has("schedule_rules"))
        bind_schedule_contracts(row, trial->task_id());
      Json single{{"selected_components", selected}, {"feedback", Json::Array{row}}};
      if (result.has("controls"))
        single["controls"] = static_cast<const Json &>(result)["controls"];
      // L1 的应用回执与 L2/L3 的可执行条件不同；无关失败不能挤掉用过本经验的样本。
      Report observed;
      *observed.add_trials() = *trial;
      const auto used = applied_experiences(observed);
      for (const auto &key : used)
        if (key.rfind("experience:", 0) == 0)
          associated_sources.insert(key.substr(11));
      relevant[trial->trial_id()] =
          std::any_of(selected.items().begin(), selected.items().end(), [&](const auto &component) {
            return used.count(component.text()) || control_evidence(single, component.text());
          });
      if (experience_selected) {
        applications[trial->trial_id()] = selected_experience_applications(*trial, selected, seed);
        const bool applied = applications.at(trial->trial_id()).num("count", 0) != 0;
        relevant[trial->trial_id()] = relevant[trial->trial_id()] || applied;
        has_applied_experience |= applied;
      }
    }
    if (experience_selected) {
      // 用本批真实应用数预留元数据字节；最终只统计实际装入反思包的题。
      std::size_t application_upper = 0;
      std::set<std::string> applied_tasks;
      for (const auto *trial : trials) {
        const auto count = applications.at(trial->trial_id()).num("count", 0);
        application_upper += count;
        if (count)
          applied_tasks.insert(trial->task_id());
      }
      result["experience_application_count"] = application_upper;
      result["experience_application_task_count"] = applied_tasks.size();
      result["experience_evidence_sufficient"] = false;
    }
    // Keep every source's identity/scope/trigger catalogue. Under metadata pressure,
    // retain full bodies for applied L1 sources, existing controls and executable
    // bindings; unrelated L1 bodies must not crowd out the first real trajectory.
    if (control_selected && reflection_input_bytes(result) >
                                config.num("max_input_bytes", 65536) / 2) {
      Json prepared{{"components", selected_values}, {"controls", result["controls"]},
                    {"experiences", result["experiences"]}, {"feedback", Json::Array{}}};
      for (const auto *trial : trials)
        prepared["feedback"].push(Json{{"task_id", trial->task_id()},
                                      {"trial", reviewed_views.at(trial)}});
      const auto bindings = observed_operation_bindings(
          prepared, std::numeric_limits<std::size_t>::max());
      collect_source_memory_ids(bindings["controls"]["editable_bindings"], associated_sources);
      result = compact_unassociated_source_bodies(std::move(result), associated_sources);
    }
    // 顺序词和数据集提示只参与排序，不作为样本准入条件；缺少这些字样不等于没有
    // 编排机会，也不能让一个误命中的独立任务排除整批其他真实边界证据。
    std::stable_sort(trials.begin(), trials.end(), [&](const auto *a, const auto *b) {
      if (relevant[a->trial_id()] != relevant[b->trial_id()])
        return relevant[a->trial_id()];
      if (selected.items().size() == 1 && selected.items().front().text() == "schedule_rules" &&
          schedule_training_priority(a->task_id()) != schedule_training_priority(b->task_id()))
        return schedule_training_priority(a->task_id()) > schedule_training_priority(b->task_id());
      if ((a->verdict() == "failed") != (b->verdict() == "failed"))
        return a->verdict() == "failed";
      // 将受限反思上下文优先分配给观测最弱的任务，包括总体成功执行中的浪费循环。
      return number(feedback_scores[a->task_id()]["fitness"]) <
             number(feedback_scores[b->task_id()]["fitness"]);
    });
    // L1 不能只看失败就停用整条经验；在原有预算内，把同经验的成功应用放在第二位。
    // 多选组件时按实际回执中的同一 memory_id 配对，不能借另一条经验的成功充数。
    const dataset::v1::EvaluationTrial *paired_success = nullptr;
    if (experience_selected && config.num("max_feedback_trials", 4) >= 2 && trials.size() > 1 &&
        trials.front()->verdict() == "failed" &&
        applications.at(trials.front()->trial_id()).num("count", 0)) {
      const auto &failed_points = applications.at(trials.front()->trial_id())["points"];
      const auto success = std::find_if(trials.begin() + 1, trials.end(), [&](const auto *trial) {
        if (trial->verdict() != "passed" || trial->execution_status() != "completed" ||
            !relevant[trial->trial_id()])
          return false;
        for (const auto &point : applications.at(trial->trial_id())["points"].items())
          for (const auto &failed : failed_points.items())
            if (!point.str("memory_id").empty() &&
                point.str("memory_id") == failed.str("memory_id"))
              return true;
        return false;
      });
      if (success != trials.end()) {
        paired_success = *success;
        std::rotate(trials.begin() + 1, success, success + 1);
      }
    }
    if (control_selected && trials.size() > 1) {
      // 同类失败的大量重复不能占满窗口：先覆盖不同执行对象/结果，再补同类题。
      std::set<std::string> signatures;
      std::vector<const dataset::v1::EvaluationTrial *> diverse, repeated;
      for (const auto *trial : trials) {
        std::set<std::string> operations;
        const auto &runs = reviewed_views.at(trial)["evidence"]["runs"];
        if (runs.array())
          for (const auto &run : runs.items())
            if (run["actions"].array())
              for (const auto &action : run["actions"].items())
                if (action["action_control"].object() || action["loop_control"].object())
                  operations.insert(action.str("actor_id") + ":" + action.str("operation"));
        std::string signature = trial->verdict() + ":" + trial->error_code();
        for (const auto &operation : operations)
          signature += "\n" + operation;
        (signatures.insert(signature).second ? diverse : repeated).push_back(trial);
      }
      diverse.insert(diverse.end(), repeated.begin(), repeated.end());
      trials = std::move(diverse);
      const auto success = std::find_if(trials.begin() + 1, trials.end(), [&](const auto *trial) {
        return trial == paired_success ||
               (trial->verdict() == "passed" && trial->execution_status() == "completed" &&
                relevant[trial->trial_id()]);
      });
      if (success != trials.end()) {
        paired_success = *success;
        std::rotate(trials.begin() + 1, success, success + 1);
      }
    }
    // 先为匹配的历史反证保留少量空间，再装填当前训练证据。否则长轨迹填满预算后，
    // 刚被否决的改动会被完全遗忘，下一轮仍可能重复提出同一候选。
    // 历史最多占总预算的八分之一，且基础字段与历史合计不超过一半；不增加模型输入上限。
    std::map<std::string, Json> rejections;
    const auto current = gepa_reflection_rejections(parent, tree["memory_feedback"],
                                                   controls, evaluation_context);
    for (const auto *history : {&reflection_history, &current})
      if ((*history)["records"].array())
        for (const auto &record : (*history)["records"].items())
          rejections[record.str("id")] = record;
    std::set<std::string> selected_names;
    for (const auto &item : selected.items()) selected_names.insert(item.text());
    const auto input_limit = config.num("max_input_bytes", 65536);
    const auto history_limit = std::min<std::size_t>(8192, input_limit / 8);
    std::size_t history_bytes = 0;
    for (const auto &entry : rejections) {
      const auto &record = entry.second;
      bool before_matches = true, after_matches = true, authorized = true;
      Json background = Json::Object{};
      for (const auto &component : record["before"].fields()) {
        authorized &= selected_names.count(component.first) != 0;
        before_matches &= available.has(component.first) &&
                          available[component.first].dump() == component.second.dump();
        after_matches &= available.has(component.first) &&
                         available[component.first].dump() == record["after"][component.first].dump();
      }
      for (const auto &component : available.fields())
        if (!record["before"].has(component.first) && !component.second.null() &&
            !(component.second.array() && component.second.items().empty()))
          background[component.first] = hash(component.second.dump());
      if (!authorized || (!before_matches && !after_matches) ||
          hash(background.dump()) != record.str("background_fingerprint")) continue;
      // 背景指纹仅供本地匹配，不让模型读无意义的哈希；当前组件已在上文，无需重传。
      Json hint{{"record_id", entry.first}, {"status", record["status"]},
                {"attribution", record["attribution"]}, {"attempted_change", record["after"]},
                {"positive_tasks", record["positive_tasks"]},
                {"negative_tasks", record["negative_tasks"]}};
      const auto bytes = hint.dump().size();
      if (history_bytes + bytes > history_limit) continue;
      auto extended = result;
      if (!extended["prior_search_rejections"].array())
        extended["prior_search_rejections"] = Json::Array{};
      if (extended["prior_search_rejections"].items().size() >= 4) break;
      extended["prior_search_rejections"].push(hint);
      if (extended.dump().size() > input_limit / 2) continue;
      history_bytes += bytes;
      result = std::move(extended);
    }
    bool paired_success_retained = false;
    std::set<std::string> retained_tasks;
    for (const auto *trial : trials) {
      if (result["feedback"].items().size() >= config.num("max_feedback_trials", 4))
        break;
      if (retained_tasks.count(trial->task_id()))
        continue;
      const dataset::v1::EvaluationTask *task = nullptr;
      for (const auto &value : dataset.tasks())
        if (value.task_id() == trial->task_id())
          task = &value;
      require(task && task->split() == "train", "gepa_reflection_split_violation");
      Json turns = Json::Array{}, constraints = Json::Array{};
      for (const auto &turn : task->followup_inputs())
        turns.push(clip(turn, 4096));
      for (const auto &constraint : task->constraints())
        constraints.push(clip(constraint, 4096));
      // 迭代上限由运行时策略授权，而不是由 control_learning 配置单独授权。
      auto feedback_controls = controls;
      feedback_controls["aggressive_controls"] = policy.options().aggressive_controls();
      auto trial_view = control_selected
                            ? compact_control_reflection_trial(
                                  control_feedback_trial(*trial, selected, feedback_controls), selected)
                            : feedback_trial(*trial, controls);
      bind_reflection_scored_facts(
          trial_view, reflection_scored_facts(feedback_scores[trial->task_id()]));
      // 原视图保留 control_feedback_trial 的全部优先级动作；只对入模 packet
      // 生成预算内的详细视图。真实剩余空间决定最终纳入的题数，不预设能装满 8 题。
      const Json complete_control_view = control_selected ? trial_view : Json::Object{};
      std::size_t detail_width = 8;
      if (control_selected)
        trial_view = compact_control_reflection_detail(complete_control_view, selected,
                                                       detail_width);
      const bool counterexample =
          experience_only && has_applied_experience && !relevant[trial->trial_id()];
      // 未使用获选经验的失败只作对照；全未命中或停用待恢复时仍保留原执行反馈。
      if (counterexample)
        trial_view = Json{{"verdict", trial_view["verdict"]},
                          {"execution_status", trial_view["execution_status"]},
                          {"metrics", trial_view["metrics"]},
                          {"checks", compact_excerpt(trial_view["checks"])},
                          {"evidence_truncated", true}};
      Json row{{"task_id", task->task_id()},
               {"task_input", clip(task->task_input(), 4096)},
               {"followup_inputs", turns},
               {"constraints", constraints},
               {"expected_behavior", clip(task->expected_behavior(), 4096)},
               {"trial", trial_view}};
      if (selected_values.has("schedule_rules"))
        bind_schedule_contracts(row, task->task_id());
      if (!turn_expectations(*task).null()) {
        row.fields().erase("followup_inputs");
        row["turn_context"] = feedback_turn_context(*task, json(trial->evidence()));
      }
      if (counterexample) {
        row = compact_excerpt(row);
        row["evidence_role"] = "unapplied_counterexample";
      }
      if (experience_selected)
        row["selected_experience_applications"] = applications.at(trial->trial_id());
      if (experience_selected && control_selected && has_applied_experience &&
          !applications.at(trial->trial_id()).num("count", 0))
        row["evidence_role"] = "unapplied_counterexample";
      if (control_selected) {
        const auto remaining_rows = std::max<std::size_t>(
            1, config.num("max_feedback_trials", 4) - result["feedback"].items().size());
        const auto used = reflection_input_bytes(result);
        const auto remaining = used < input_limit ? input_limit - used : 0;
        const auto preferred_row_bytes =
            std::max<std::size_t>(4096, remaining / std::min<std::size_t>(8, remaining_rows));
        for (const auto width : {6u, 4u, 2u}) {
          auto packet = result;
          packet["feedback"].push(row);
          if (reflection_input_bytes(packet) - used <= preferred_row_bytes)
            break;
          detail_width = width;
          row["trial"] = compact_control_reflection_detail(complete_control_view, selected,
                                                           detail_width);
          if (selected_values.has("schedule_rules"))
            bind_schedule_contracts(row, task->task_id());
        }
      }
      const auto original_row = row;
      for (unsigned attempt = 0; attempt < 2; ++attempt) {
        if (attempt != 0)
          row = original_row;
        auto trial_packet = result;
        trial_packet["feedback"].push(row);
        if (reflection_input_bytes(trial_packet) > input_limit) {
          if (control_selected) {
            row["task_input"] = clip(task->task_input(), 1024);
            row.fields().erase("followup_inputs");
            row["turn_context"] = feedback_turn_context(*task, json(trial->evidence()), 4, 512);
            row["constraints"] = Json::Array{};
            for (const auto &constraint : task->constraints()) {
              if (row["constraints"].items().size() == 8)
                break;
              row["constraints"].push(clip(constraint, 1024));
            }
            row["expected_behavior"] = clip(task->expected_behavior(), 2048);
            trial_packet = result;
            trial_packet["feedback"].push(row);
            for (const auto width : {6u, 4u, 2u}) {
              if (reflection_input_bytes(trial_packet) <= input_limit || width >= detail_width)
                continue;
              detail_width = width;
              row["trial"] = compact_control_reflection_detail(complete_control_view, selected,
                                                               detail_width);
              if (selected_values.has("schedule_rules"))
                bind_schedule_contracts(row, task->task_id());
              trial_packet = result;
              trial_packet["feedback"].push(row);
            }
          } else if (!counterexample) {
            row["trial"] = Json::Object{};
            auto overhead = result;
            overhead["feedback"].push(row);
            const auto limit = config.num("max_input_bytes", 65536), used = overhead.dump().size();
            row["trial"] = compact_experience_feedback_trial(
                *trial, trial_view, selected, used < limit ? limit - used + 2 : 0, seed);
          }
          trial_packet = result;
          trial_packet["feedback"].push(row);
        }
        if (reflection_input_bytes(trial_packet) <= input_limit) {
          result = std::move(trial_packet);
          paired_success_retained |= trial == paired_success;
          retained_tasks.insert(trial->task_id());
          break;
        }
        // 首条证据仍放不下时退还历史空间，避免新增提示让原本可评测的任务报预算错误。
        // 已有当前证据时不回退历史，也不为后续大样本挤掉已选中的证据。
        if (attempt == 0 && result["feedback"].items().empty() &&
            result.has("prior_search_rejections"))
          result.fields().erase("prior_search_rejections");
        else
          break;
      }
    }
    if (paired_success && !paired_success_retained) {
      // 字节实在不足时仍遵守原上限；截断标记自身也不能挤出已有证据或引发预算错误。
      auto truncated = result;
      truncated["feedback_truncated"] = true;
      if (reflection_input_bytes(truncated) <= input_limit)
        result = std::move(truncated);
    }
    if (experience_selected) {
      std::size_t applied_points = 0;
      std::set<std::string> tasks;
      std::map<std::string, std::set<std::string>> memory_tasks;
      for (const auto &row : result["feedback"].items()) {
        const auto &application = row["selected_experience_applications"];
        applied_points += application.num("count", 0);
        if (application.num("count", 0))
          tasks.insert(row.str("task_id"));
        for (const auto &point : application["points"].items())
          if (!point.str("memory_id").empty())
            memory_tasks["experience:" + point.str("memory_id")].insert(row.str("task_id"));
      }
      bool sufficient = true;
      for (const auto &component : selected.items())
        if (component.text().rfind("experience:", 0) == 0)
          sufficient &= memory_tasks[component.text()].size() >= 2;
      result["experience_application_count"] = applied_points;
      result["experience_application_task_count"] = tasks.size();
      result["experience_evidence_sufficient"] = sufficient;
    }
    require(!result["feedback"].items().empty() && reflection_input_bytes(result) <= input_limit,
            "gepa_feedback_budget_too_small");
    validate_local_data(result);
    return result;
  }

  // 汇总可用于生成控制规则的执行事实与失败上下文。
  bool control_evidence(const Json &input, const std::string &component) const {
    // 次数预算只有在全部必要工作下界合并后仍形成 typed binding 才算可执行机会。
    // 其他原语（缺参、终态失败、no-progress 等）仍由各自结构化证据独立开放。
    const auto prepared = observed_operation_bindings(
        input, std::numeric_limits<std::size_t>::max());
    bool action_budget = false, loop_budget = false, schedule_binding = false;
    if (prepared["controls"]["editable_bindings"].array())
      for (const auto &binding : prepared["controls"]["editable_bindings"].items()) {
        action_budget |= binding.str("component") == "action_rules" &&
                         binding.str("primitive_id") == "operation_budget";
        loop_budget |= binding.str("component") == "loop_rules" &&
                       binding.str("primitive_id") == "bounded_iteration_cap";
        schedule_binding |= binding.str("component") == "schedule_rules" &&
                            binding.str("primitive_id") == "order_operations";
      }
    // 空父规则不能因独立操作可排列就占用反思轮次；已有规则若在真实计划
    // 中应用，仍可反思移除错误的旧边。
    if (component == "schedule_rules") {
      if (schedule_binding)
        return true;
      if (input["components"]["schedule_rules"].array() &&
          !input["components"]["schedule_rules"].items().empty())
        for (const auto &feedback : input["feedback"].items()) {
          const auto &runs = feedback["trial"]["evidence"]["runs"];
          if (!runs.array())
            continue;
          for (const auto &run : runs.items()) {
            if (!run["schedule_plans"].array())
              continue;
            for (const auto &plan : run["schedule_plans"].items())
              if (plan["effective_plan"]["rule_ids"].array() &&
                  !plan["effective_plan"]["rule_ids"].items().empty())
                return true;
          }
        }
      return false;
    }
    for (const auto &feedback : input["feedback"].items()) {
      // 受预算限制的行只保留分数和输出，不保留可执行证据，不能据此生成控制规则。
      const auto &runs = feedback["trial"]["evidence"]["runs"];
      if (!runs.array())
        continue;
      for (const auto &run : runs.items()) {
        if (!run["actions"].array())
          continue;
        for (const auto &action : run["actions"].items()) {
          if (component == "action_rules" && action_control_evidence(action)) {
            const auto &control = action["action_control"];
            const bool terminal_opportunity =
                action.has("terminal_error_code_candidate") &&
                action.flag("executed", false) &&
                !control.str("terminal_retry_query_event_id").empty();
            const bool budget_only =
                control.num("prior_operation_rounds", 0) > 0 &&
                action.flag("guard_only", false) && control.num("prior_failures", 0) < 2 &&
                !terminal_opportunity &&
                (!control["failure_key_candidates"].array() ||
                 std::none_of(control["failure_key_candidates"].items().begin(),
                              control["failure_key_candidates"].items().end(),
                              [](const auto &target) {
                                return target.flag("cross_execution", false);
                              })) &&
                (!control["missing_required_arguments"].array() ||
                 control["missing_required_arguments"].items().empty());
            if (!budget_only || action_budget)
              return true;
          }
          if (component == "loop_rules" && action.str("kind") == "loop.decision" &&
              action["loop_control"].object() &&
              !action["loop_control"].flag("pending_actions", false) &&
              (action["loop_control"].str("proposed_decision", "continue") == "continue" ||
               action["loop_control"].str("proposed_decision") == "replan")) {
            const auto reviewed_cap = std::max(
                {controls.num("min_iterations", 2),
                 action["loop_control"].num("reviewed_waste_after", 0),
                 required_iteration_floor(input, action["loop_control"], action.str("actor_id"))});
            if (loop_budget && iteration_cap_scope_available(input["controls"], action) &&
                action["loop_control"].has("reviewed_waste_after") &&
                action["loop_control"].num("iteration", 0) > reviewed_cap)
              return true;
            if (!action["loop_control"]["observed_no_progress"].null() &&
                action["loop_control"].num("observed_no_progress", 0) >=
                    controls.num("min_no_progress", 1))
              return true;
            if (action["loop_control"].num("repeated_failure_steps", 0)) {
              const Json *base = nullptr;
              const auto &rules = input["controls"]["base_loop_rules"];
              if (rules.array())
                for (const auto &rule : rules.items())
                  if (!rule.has("source_experience") &&
                      rule.str("target_scope") == action["loop_control"].str("target_scope") &&
                      (rule.str("actor_id").empty() ||
                       rule.str("actor_id") == action.str("actor_id")) &&
                      (!base || !rule.str("actor_id").empty()))
                    base = &rule;
              if (base && base->str("no_progress_decision") == "replan" &&
                  base->num("max_replans", 0))
                return true;
            }
          }
        }
      }
    }
    return false;
  }

  // 把观测到的控制机会路由到相应组件，不从任务名称硬编码业务动作。
  bool route_control_opportunity(const Json &parent, const Report &feedback, const Json &available,
                                 std::size_t slot, const std::string &current, Json &selected,
                                 Json &input) const {
    const char *candidates[] = {"schedule_rules", "action_rules", "loop_rules"};
    std::size_t best_count = SIZE_MAX;
    Json best = Json::Array{}, best_input;
    for (std::size_t offset = 0; offset < 3; ++offset) {
      const auto candidate = std::string(candidates[(slot + offset) % 3]);
      if (candidate == current || !available.has(candidate))
        continue;
      const Json option = Json::Array{candidate};
      Json probe;
      try {
        probe = packet(parent, feedback, option);
      } catch (const Error &error) {
        if (std::string(error.what()) == "gepa_feedback_budget_too_small")
          continue;
        throw;
      }
      if (!control_evidence(probe, candidate))
        continue;
      const auto count = tree["component_selections"].num(candidate, 0);
      if (count >= best_count)
        continue;
      best_count = count;
      best = option;
      best_input = std::move(probe);
    }
    if (best.items().empty())
      return false;
    selected = std::move(best);
    input = std::move(best_input);
    return true;
  }

  // 整理失败诊断中的可修复部分，保留证据而不直接降低验收标准。
  const Json *repair_feedback_row(const Json &input, const Json &selected) const {
    const bool actions =
        std::any_of(selected.items().begin(), selected.items().end(),
                    [](const auto &item) { return item.text() == "action_rules"; });
    // 单条修复证据优先保留已放行的重复派发；模型计划和已拒绝动作没有新增拦截机会。
    if (actions)
      for (const auto &row : input["feedback"].items()) {
        const auto &runs = row["trial"]["evidence"]["runs"];
        if (!runs.array())
          continue;
        for (const auto &run : runs.items()) {
          if (!run["actions"].array())
            continue;
          for (const auto &action : run["actions"].items())
            if (action.str("semantic") != "model" && action.flag("guard_only", false) &&
                !action.flag("executed", true) && action_control_evidence(action) &&
                action["action_control"].num("prior_operation_rounds", 0) > 0)
              return &row;
        }
      }
    const Json *evidence = nullptr;
    for (const auto &row : input["feedback"].items()) {
      if (!evidence)
        evidence = &row;
      Json single{{"selected_components", selected}, {"feedback", Json::Array{row}}};
      // 单条修复证据仍需当前组件、经验作用域和宿主能力，才能重建同一 typed binding。
      for (const auto *field : {"components", "experiences", "controls"})
        if (input.has(field))
          single[field] = static_cast<const Json &>(input)[field];
      if (std::any_of(selected.items().begin(), selected.items().end(), [&](const auto &component) {
            return control_evidence(single, component.text());
          }))
        return &row;
    }
    return evidence;
  }

  // 只认实际应用回执中的经验版本；被检索到或写在候选里不等于被使用。
  std::set<std::string> applied_experiences(const Report &report) const {
    const auto policy = proto<v1::RuntimePolicy>(seed["policy"]);
    const auto canonical = canonical_experience_aliases(policy);
    std::set<std::string> result;
    for (const auto &trial : report.trials()) {
      if (trial.variant_id() != "candidate")
        continue;
      const auto evidence = json(trial.evidence());
      const auto traces =
          evidence["traces"].array() ? evidence["traces"]
          : evidence["trace"].object() ? Json(Json::Array{evidence["trace"]})
                                       : Json(Json::Array{});
      for (const auto &trace : traces.items()) {
        if (!trace["events"].array())
          continue;
        for (const auto &event : trace["events"].items()) {
          if (event.str("kind") != "advice.applied" ||
              event["application"].str("status") != "applied")
            continue;
          if (event["attributes"]["receipt"].str("boundary") != "llm")
            continue;
          const auto &experiences = event["attributes"]["receipt"]["experiences"];
          // 无经验的模型请求也可能确认应用；Protobuf 会省略空列表，不能据此中断反思。
          if (experiences.null())
            continue;
          for (const auto &source : experiences.items()) {
            auto id = source.str("memory_id");
            const auto alias = canonical.find({id, source.str("content_revision")});
            if (alias != canonical.end())
              id = alias->second;
            if (!id.empty())
              result.insert("experience:" + id);
          }
        }
      }
    }
    return result;
  }

  // 移除选定经验做消融比较，区分经验贡献和模型输出波动。
  Json experience_ablation(const Json &parent, const Json &selected, const Report &report) const {
    if (selected.items().size() != 1)
      return Json();
    const auto key = selected.items().front().text();
    if (key.rfind("experience:", 0) != 0 || components(parent)[key].null())
      return Json();
    for (const auto &trial : report.trials()) {
      if (trial.variant_id() != "candidate" || trial.execution_status() != "completed")
        continue;
      const auto evidence = json(trial.evidence());
      const bool waste = evidence["loop_review"].str("status") == "complete" &&
                         evidence["loop_review"].num("count", 0) > 0;
      if (trial.verdict() != "failed" && !(trial.verdict() == "passed" && waste))
        continue;
      Report one;
      *one.add_trials() = trial;
      if (!applied_experiences(one).count(key))
        continue;
      // 共现仅用于安排消融实验，不把任务失败或浪费直接归因于该经验。
      return Json{{"reason", "Compare disabling an injected experience on an unsuccessful or "
                             "wasteful training trial; evaluation determines its effect."},
                  {"experience", Json{{"memory_id", key.substr(11)}, {"content", nullptr}}}};
    }
    return Json();
  }

  // 检查同一经验消融是否已经执行，避免浪费搜索预算重复测量。
  bool experience_ablation_seen(const std::string &parent_version, const Json &selected,
                                std::size_t before) const {
    const auto &nodes = tree["nodes"].items();
    for (std::size_t previous = 0; previous < std::min(before, nodes.size()); ++previous) {
      const auto &prior = nodes[previous];
      if (prior.str("generation_method") != "experience_ablation")
        continue;
      const auto parent_id = prior.num("parent", SIZE_MAX);
      if (parent_id < nodes.size() && nodes[parent_id].str("version") == parent_version &&
          prior["components"].dump() == selected.dump())
        return true;
    }
    return false;
  }

  // 构建经过约束检查的控制试探，确保候选动作有可执行的宿主入口。
  Json reviewed_control_probe(const Json &parent, const Json &candidate, const Json &input,
                              const Report &report, std::size_t index) const {
    const auto behavior = [&](const Json &artifact) {
      auto values = components(artifact);
      for (const auto *category : {"action_rules", "loop_rules", "schedule_rules"})
        if (values[category].array())
          for (auto &rule : values[category].items())
            rule.fields().erase("reason");
      return values.dump();
    };
    if (behavior(parent) != behavior(candidate))
      return Json();
    const auto prepared = observed_operation_bindings(input, config.num("max_input_bytes", 65536));
    const auto &bindings = prepared["controls"]["editable_bindings"];
    if (!bindings.array())
      return Json();
    for (const auto &binding : bindings.items()) {
      const bool action = binding.str("component") == "action_rules" &&
                          binding.str("primitive_id") == "operation_budget" &&
                          binding.flag("reviewed_redundant_suffix", false);
      const bool loop = binding.str("component") == "loop_rules" &&
                        binding.str("primitive_id") == "bounded_iteration_cap" &&
                        binding.has("reviewed_waste_after");
      if (!action && !loop)
        continue;
      const auto amount = action ? binding.num("prior_operation_rounds", 0)
                                 : control_binding_budget(prepared, binding).num("min", 0);
      if ((action && (!amount || amount > 16)) ||
          (loop && amount >= binding.num("observed_iteration", 0)))
        continue;
      Json signature{{"parent_version", agent_version_id(parent.dump())},
                     {"component", binding["component"]},
                     {"scope", binding["target_scope"]},
                     {"operation", binding.str("operation")},
                     {"actor_id", binding.str("actor_id")},
                     {"budget", amount},
                     {"require_finalization", binding.flag("require_finalization", false)}};
      const auto key = "reviewed-control:" + hash(signature.dump());
      bool seen = false;
      if (tree["nodes"].array())
        for (std::size_t previous = 0; previous < std::min(index, tree["nodes"].items().size());
             ++previous)
          seen |= tree["nodes"].items()[previous]["control_probe"].str("key") == key;
      if (seen)
        continue;
      auto source = control_probe_source(parent, prepared, binding, report);
      if (source.null())
        continue;
      const std::string reason =
          "训练评审提出的受限控制假设，经验仅关联已有批准来源；效果仍由真实对照评测确认。";
      Json update{{"binding_id", binding["binding_id"]},
                  {"memory_id", source["memory_id"]},
                  {"reason", reason}};
      update[action ? "max_attempts" : "max_iterations"] = amount;
      try {
        const auto draft = resolve_control_binding_updates(
            Json{{"reason", reason}, {"control_binding_updates", Json::Array{update}}}, prepared);
        return Json{{"draft", draft},
                    {"detail", Json{{"key", key},
                                    {"component", binding["component"]},
                                    {"evidence_ref", binding["evidence_ref"]},
                                    {"source", source},
                                    {"status", "pending"},
                                    {"hypothesis_origin", "training_review"}}}};
      } catch (const Error &error) {
        if (std::string(error.what()) != "control_binding_invalid")
          throw;
      }
    }
    return Json();
  }

  // 从运行证据判断经验是否真正注入，不只统计检索返回数量。
  Json observed_experience(const Json &available, const std::set<std::string> &observed) const {
    std::string best;
    std::size_t uses = SIZE_MAX;
    for (const auto &key : observed)
      if (available.has(key)) {
        const auto count = tree["component_selections"].num(key, 0);
        if (count < uses || (count == uses && key < best)) {
          best = key;
          uses = count;
        }
      }
    return best.empty() ? Json(Json::Array{}) : Json(Json::Array{best});
  }

  // Guard 与真正派发有独立 step；只关联同一 trace 中宿主给出的确切派发身份。
  std::set<std::string> executed_delegated_guard_steps(const Json &events) const {
    struct Receipt { const Json *start; const Json *end; std::size_t begin, finish; };
    std::map<std::string, std::pair<const Json *, std::size_t>> starts;
    std::set<std::string> duplicate_ids, result;
    std::vector<Receipt> guards, dispatched;
    std::size_t index = 0;
    for (const auto &event : events.items()) {
      const auto current = index++;
      const auto id = event.str("action_id");
      if (id.empty()) continue;
      if (event.str("kind") == "action.start") {
        if (!starts.emplace(id, std::make_pair(&event, current)).second) duplicate_ids.insert(id);
        continue;
      }
      const auto found = starts.find(id);
      if (event.str("kind") != "action.end" || found == starts.end() || duplicate_ids.count(id)) continue;
      const auto &start = *found->second.first;
      if (start.str("step_id").empty() || start.str("execution_id").empty() ||
          start.str("actor_id").empty() || start.str("operation").empty() ||
          start.str("step_id") != event.str("step_id") ||
          start.str("execution_id") != event.str("execution_id") ||
          start.str("actor_id") != event.str("actor_id")) continue;
      const auto &a = event["attributes"];
      const Receipt receipt{&start, &event, found->second.second, current};
      if (!a.flag("executed", false) && event.str("status") == "cancelled" &&
          start["attributes"].str("decision") == "allow" &&
          !a["extensions"]["point_reference"].str("key").empty() &&
          start["input"].dump() == event["input"].dump()) guards.push_back(receipt);
      if (a.str("type") == "schedule.action" && a.flag("executed", false) &&
          (event.str("status") == "completed" || event.str("status") == "failed") &&
          start["attributes"].str("type") == "schedule.action" && start["attributes"].flag("executed", false) &&
          start.str("operation") == event.str("operation") && !a.str("logical_action_id").empty() &&
          a.str("logical_action_id") == start["attributes"].str("logical_action_id") &&
          !a.str("target_scope").empty() && a.str("target_scope") == start["attributes"].str("target_scope"))
        dispatched.push_back(receipt);
    }
    for (const auto &guard : guards) {
      const auto ref = (*guard.end)["attributes"]["extensions"]["point_reference"].str("key");
      std::size_t matches = 0;
      for (const auto &action : dispatched) {
        const auto &start = *action.start;
        const auto step = start.str("step_id");
        if (action.begin <= guard.finish || step.rfind("step-", 0) != 0 ||
            step.substr(5) + ":guard:" + start["attributes"].str("logical_action_id") != ref ||
            start.str("execution_id") != guard.start->str("execution_id") ||
            start.str("actor_id") != guard.start->str("actor_id") ||
            start.str("operation") != guard.start->str("operation") ||
            start["input"].dump() != (*guard.start)["input"].dump()) continue;
        ++matches;
      }
      if (matches == 1) result.insert(guard.start->str("step_id"));
    }
    return result;
  }

  // 从真实回执识别本次实际生效的改动，不以配置中存在规则代替应用证据。
  Json applied_changes(const Json &parent, const Report &report, Json *support = nullptr) const {
    Json changed = Json::Object{};
    auto effect = [](const Json &decision, const std::string &boundary) {
      if (boundary == "llm") {
        auto context = decision.str("context");
        const auto prefix =
            "\n<trajectory-experience policy=\"" + decision.str("policy_id") + "\">\n";
        // 版本编号用于追溯，不是经验内容；仅归一化宿主生成的外层标记。
        if (!decision.str("policy_id").empty() && context.rfind(prefix, 0) == 0)
          context.replace(0, prefix.size(), "\n<trajectory-experience>\n");
        return Json(context);
      }
      if (boundary == "loop")
        return Json(decision["progress"].str("decision"));
      Json result = Json::Object{};
      if (boundary == "actions" && decision["actions"].array())
        for (const auto &action : decision["actions"].items())
          result[action.str("action_id")] = action["decision"];
      if (boundary == "schedule" && decision["schedule"]["actions"].array())
        for (const auto &action : decision["schedule"]["actions"].items()) {
          result[action.str("action_id")] = Json::Object{};
          if (action["after"].array())
            for (const auto &edge : action["after"].items())
              result[action.str("action_id")][edge.str("action_id")] =
                  edge.flag("require_success", false);
        }
      return result;
    };
    for (const auto &trial : report.trials()) {
      if (trial.variant_id() != "candidate")
        continue;
      const auto evidence = json(trial.evidence());
      const auto traces =
          evidence["traces"].array() ? evidence["traces"] : Json(Json::Array{evidence["trace"]});
      for (const auto &trace : traces.items()) {
        if (!trace["events"].array())
          continue;
        const auto bound = bind_runtime_policy(proto<v1::RuntimePolicy>(parent["policy"]),
                                               proto<v1::Identity>(trace["identity"]));
        const PolicyRuntime runtime(bound);
        std::map<std::string, std::string> observed;
        std::set<std::string> executed, completed_schedules, committed;
        for (const auto &event : trace["events"].items()) {
          if (event.str("kind") == "advice.applied")
            observed[event["application"].str("query_id")] = event["application"].str("status");
          if (event.str("kind") == "action.end" && event["attributes"].flag("executed", false))
            executed.insert(event.str("step_id"));
          if (event.str("kind") == "step.end" && event.str("status") == "completed")
            completed_schedules.insert(event.str("step_id"));
          if (event.str("kind") == "loop.decision")
            committed.insert(event.str("step_id"));
        }
        const auto delegated_guards = executed_delegated_guard_steps(trace["events"]);
        const auto replay_options = json(bound.options());
        Json ledger = Json::Object{}, attempts = Json::Object{};
        std::map<std::string, Json> schedule_inputs;
        for (const auto &event : trace["events"].items()) {
          if (event.str("kind") == "step.start" && event["input"]["actions"].array())
            schedule_inputs[event.str("step_id")] = event["input"];
          snapshot_scheduled_target(attempts, ledger, replay_options, event);
          if (event.str("kind") == "action.end")
            settle_target_event(ledger, static_cast<const Json &>(attempts)[event.str("action_id")],
                                replay_options, event);
          if (event["attributes"].str("type") != "policy.query")
            continue;
          const auto &actual = event["attributes"]["decision"];
          auto q = proto<v1::RuntimeQuery>(actual["query"]);
          const auto boundary = q.boundary();
          if (schedule_inputs.count(event.str("step_id")))
            project_schedule_evidence(q, schedule_inputs.at(event.str("step_id")));
          if (boundary != "llm" && boundary != "actions" && boundary != "loop" &&
              boundary != "schedule")
            continue;
          if (q.actor_id().empty())
            q.set_actor_id(event.str("actor_id"));
          if (q.previous().iteration())
            q.mutable_previous()->set_policy_id(bound.policy_id());
          for (auto &action : *q.mutable_actions())
            if (boundary == "actions") {
              const auto snapshot = target_failure_snapshot(
                  replay_options, q.advice_query().target_scope(), action.operation(),
                  json(action.arguments()), ledger, q.execution_id());
              attempts[action.action_id()] = Json{{"entries", snapshot},
                                                  {"scope", q.advice_query().target_scope()},
                                                  {"operation", action.operation()},
                                                  {"execution_id", q.execution_id()}};
              action.clear_target_failures();
              for (const auto &entry : snapshot.fields())
                (*action.mutable_target_failures())[entry.first] = entry.second.num("count", 0);
            }
          if (effect(actual, boundary).dump() == effect(json(runtime.decide(q)), boundary).dump())
            continue;
          const auto status = observed[q.advice_query().query_id()];
          // 移除注入或约束时出现 no-op 回执是预期行为；只有真实边界完成才计入，
          // 失败回执绝不能算作已应用。
          bool applied = status == "applied";
          if (actual.str("status") == "no_change" && status == "rejected") {
            if (boundary == "llm" || boundary == "actions")
              applied = executed.count(event.str("step_id")) ||
                        (boundary == "actions" && delegated_guards.count(event.str("step_id")));
            if (boundary == "loop")
              applied = committed.count(event.str("step_id"));
            if (boundary == "schedule")
              applied = completed_schedules.count(event.str("step_id"));
          }
          if (applied) {
            changed[trial.task_id()][boundary] = changed[trial.task_id()].num(boundary, 0) + 1;
            if (support) {
              if (!q.actor_id().empty())
                (*support)["actors"][q.actor_id()] = true;
              if (!(*support)["events"].array())
                (*support)["events"] = Json::Array{};
              if ((*support)["events"].items().size() < 8)
                (*support)["events"].push(Json{{"task_id", trial.task_id()},
                    {"event_id", event["event_id"]}, {"actor_id", q.actor_id()},
                    {"boundary", boundary}});
            }
          }
        }
      }
    }
    return changed;
  }

  // 用最新执行轨迹刷新策略改动的应用证据，防止复用父节点的命中统计。
  void refresh_change_evidence() {
    if (tree.num("change_evidence_revision", 0) >= 2 || !tree["active"].null() ||
        !tree["batch"].null())
      return;
    // 仅重算派生归因，原始试次与评分不变；批次边界更新，保留已排定的父子关系。
    auto updated = tree;
    // 旧反馈没有记录组件级因果边界，无法安全区分继承层的随机变化。
    // 升级检查点时宁可重新积累训练反馈，也不能把历史假阳性写回唯一记忆库。
    updated["memory_feedback"] = Json();
    for (std::size_t index = 1; index < updated["nodes"].items().size(); ++index) {
      auto &node = updated["nodes"].items()[index];
      if (node.str("version").empty() || !node["screening"].object())
        continue;
      auto cached_report = [&](const std::string &split, const Json &tasks) {
        for (const auto &item : static_cast<const Json &>(tree)["evaluations"].fields()) {
          const auto &cached = item.second;
          if (cached.str("version") != node.str("version") || cached.str("split") != split ||
              cached["task_ids"].dump() != tasks.dump() || cached.str("report_hash").empty())
            continue;
          const auto bytes =
              read(root / "evaluations" / item.first /
                       ("attempt-" + std::to_string(cached.num("attempt", 0))) / "report.json",
                   kMaxEvaluationReportBytes);
          require(hash(bytes) == cached.str("report_hash"), "gepa_report_changed");
          return proto<Report>(Json::parse(bytes));
        }
        throw Error("gepa_attribution_report_unavailable");
      };
      const auto parents = gepa::parents(node);
      if (node["screening"]["applied_changes"].object()) {
        const auto report = cached_report(node["screening"].str("split", "train"), node["tasks"]);
        node["screening"]["applied_changes"] = applied_changes(artifact(parents.front()), report);
        node["screening"]["causal_changes"] =
            component_applied_changes(node["components"], node["screening"]["applied_changes"]);
      }
      if (node["screening"]["parent_applied_changes"].array()) {
        const auto report = cached_report("val", node["tasks"]);
        node["screening"]["parent_applied_changes"] = Json::Array{};
        for (const auto parent : parents)
          node["screening"]["parent_applied_changes"].push(
              applied_changes(artifact(parent), report));
      }
      const auto &screening_changes = node["screening"]["causal_changes"].object()
                                          ? node["screening"]["causal_changes"]
                                          : node["screening"]["applied_changes"];
      bool changed = screening_changes.object() && !screening_changes.fields().empty();
      if (node["validation_applied_changes"].object()) {
        Json tasks = Json::Array{};
        for (const auto &id : val)
          tasks.push(id);
        const auto report = cached_report("val", tasks);
        for (const auto parent : parents) {
          auto changes = component_applied_changes(
              node["components"], applied_changes(artifact(parent), report));
          changed |= !changes.fields().empty();
          node["validation_applied_changes"][std::to_string(parent)] = std::move(changes);
        }
      }
      if ((node.str("status") == "accepted" || node.str("status") == "screened") &&
          !changed) {
        node["attribution_previous_status"] = node.str("status");
        node["status"] = "unobserved_changes";
        node["reason"] = "selected_component_not_applied";
      }
    }
    // 已进入留出集的旧检查点不能沿用失去因果证据的赢家。
    if (updated.flag("holdout_started", false))
      require(updated["nodes"].items().at(updated.num("winner", 0)).str("status") == "accepted",
              "gepa_attribution_winner_invalid");
    tree = std::move(updated);
    tree["change_evidence_revision"] = 2;
    for (std::size_t index = 0; index < tree["nodes"].items().size(); ++index)
      annotate_exploration_eligibility(index);
    persist_frontiers();
    save();
  }

  // 只比较具备对应观测的同一组题目，避免样本缺失造成虚假提升。
  std::pair<Json, Json> observed_score_subset(const Json &candidate, const Json &parent,
                                              const Json &changes) const {
    Json selected = Json::Object{}, previous = Json::Object{};
    if (changes.object())
      for (const auto &task : changes.fields()) {
        bool applied = false;
        if (task.second.object())
          for (const auto &boundary : task.second.fields())
            applied |= number(boundary.second, 0) > 0;
        if (applied && candidate.has(task.first) && parent.has(task.first)) {
          selected[task.first] = candidate[task.first];
          previous[task.first] = parent[task.first];
        }
      }
    return {std::move(selected), std::move(previous)};
  }

  // 计算当前被改动部分在匹配观测上的局部收益，不能代替全局验证。
  bool observed_local_gain(const Json &candidate, const Json &parent, const Json &changes) const {
    const auto [selected, previous] = observed_score_subset(candidate, parent, changes);
    return gepa::local_gain(selected, previous, number(config["min_improvement"], 0.000001));
  }

  // 从训练执行比较中计算改动收益，留出集不参与该反馈。
  bool observed_training_gain(const Json &candidate, const Json &parent,
                              const Json &changes) const {
    const auto [selected, previous] = observed_score_subset(candidate, parent, changes);
    const auto acceptance = config.str("acceptance", "strict_improvement");
    if (acceptance == "pareto")
      // Pareto 只要求当前改动在同题观测上有真实局部收益；全局质量、loop、
      // token 和时延的安全边界由 screening_quality_safe 统一约束，不能再用
      // “所有指标都不得下降”的门槛把可用的单指标改进全部挡掉。
      return gepa::local_gain(selected, previous, number(config["min_improvement"], 0.000001));
    return acceptance == "improvement_or_equal" ? gepa::no_worse(selected, previous)
                                                : improves(selected, previous);
  }

  // 当本次规则确已执行、但受影响题数很少时，允许批次级显著收益进入独立 Val
  // 求证；它只是待归因信号，不能作为接受节点或选择父节点的证据。
  bool screening_batch_gain(const Json &candidate, const Json &parent) const {
    if (!candidate.object() || !parent.object() || candidate.fields().empty() ||
        candidate.fields().size() != parent.fields().size())
      return false;
    for (const auto &task : parent.fields())
      if (!candidate.has(task.first))
        return false;
    auto complete_metric = [&](const char *field) {
      for (const auto &task : parent.fields())
        if (!task.second.has(field) || task.second[field].null() ||
            !candidate[task.first].has(field) || candidate[task.first][field].null())
          return false;
      return true;
    };
    if (mean(candidate, "quality") >= mean(parent, "quality") + 0.01 - 1e-12)
      return true;
    const auto strict = mean(candidate, "strict_pass_rate");
    const auto base_strict = mean(parent, "strict_pass_rate");
    if (complete_metric("strict_pass_rate") &&
        strict >= base_strict + 1.0 / candidate.fields().size() - 1e-12)
      return true;
    for (const auto *field : {"invalid_loops", "tokens", "duration_ms"}) {
      if (resources_report_only() && std::string(field) != "invalid_loops")
        continue;
      if (!complete_metric(field))
        continue;
      const auto value = mean(candidate, field), baseline = mean(parent, field);
      if (value > -1e8 && baseline > 0 && value <= baseline * 0.90 + 1e-12)
        return true;
    }
    return false;
  }

  bool screening_gain_for_validation(const Json &candidate, const Json &parent,
                                     const Json &changes) const {
    return observed_training_gain(candidate, parent, changes) ||
           (config.str("acceptance", "strict_improvement") == "pareto" &&
            screening_batch_gain(candidate, parent));
  }

  // 未观察到策略差分只有在整批父子原始轨迹均完整时才是可用证据。
  // 每个边界查询须有同 policy/query 身份的已应用或明确拒绝回执；缺失不推断为未应用。
  bool complete_policy_receipts(const Report &report, const std::string &version) const {
    if (!report.complete() || !scored(report) || report.trials().empty())
      return false;
    for (const auto &trial : report.trials()) {
      if (trial.variant_id() != "candidate" || !trial.error_code().empty())
        return false;
      const auto evidence = json(trial.evidence());
      if (evidence.str("loaded_version") != version)
        return false;
      const auto traces = evidence["traces"].array()
                              ? evidence["traces"] : Json(Json::Array{evidence["trace"]});
      if (traces.items().empty())
        return false;
      for (const auto &trace : traces.items()) {
        if (!trace["attributes"].flag("capture_complete", false) ||
            trace["identity"].str("run_id").empty() || !trace["events"].array())
          return false;
        bool started = false, ended = false;
        std::map<std::string, Json> receipts;
        for (const auto &event : trace["events"].items()) {
          started |= event.str("kind") == "run.start";
          ended |= event.str("kind") == "run.end";
          if (event.str("kind") != "advice.applied")
            continue;
          const auto &application = event["application"];
          const auto id = application.str("query_id");
          if (id.empty() || application.str("policy_id").empty() ||
              (application.str("status") != "applied" &&
               application.str("status") != "rejected") ||
              !receipts.emplace(id, application).second)
            return false;
        }
        std::set<std::string> queries;
        for (const auto &event : trace["events"].items()) {
          if (event["attributes"].str("type") != "policy.query")
            continue;
          const auto &decision = event["attributes"]["decision"];
          const auto &query = decision["query"];
          const auto id = query["advice_query"].str("query_id");
          const auto boundary = query.str("boundary");
          if (!query.object() || id.empty() || decision.str("policy_id").empty() ||
              (boundary != "llm" && boundary != "actions" && boundary != "loop" &&
               boundary != "schedule") || !queries.insert(id).second ||
              !receipts.count(id) ||
              receipts.at(id).str("policy_id") != decision.str("policy_id"))
            return false;
        }
        if (!started || !ended || queries.empty() || queries.size() != receipts.size())
          return false;
      }
    }
    return true;
  }

  bool task_change_applied(const Json &changes, const std::string &task) const {
    if (!changes[task].object())
      return false;
    for (const auto &boundary : changes[task].fields())
      if (number(boundary.second, 0) > 0)
        return true;
    return false;
  }

  // Pool 只决定是否值得独立验证，不使用最终发布门禁的零质量回退容差。
  // 少量逐题二值波动可由 Val 复核，但大范围退化或新增关键目标失败仍须挡住。
  bool screening_quality_safe(const Json &candidate, const Json &parent,
                              const Json &causal_changes = Json(),
                              bool causal_coverage_complete = false) const {
    if (!candidate.object() || !parent.object() || candidate.fields().empty() ||
        parent.fields().empty() || candidate.fields().size() != parent.fields().size())
      return false;
    const auto allowed = std::clamp(number(config["screening_max_quality_drop"], 0.03), 0.0, 0.15);
    const auto quality = mean(candidate, "quality"), parent_quality = mean(parent, "quality");
    if (quality + allowed < parent_quality - 1e-12)
      return false;
    for (const auto &task : parent.fields()) {
      if (!candidate.has(task.first) || !candidate[task.first].has("quality") ||
          !task.second.has("quality") || candidate[task.first]["quality"].null() ||
          task.second["quality"].null())
        return false;
      if (number(candidate[task.first]["critical_failures"]) >
              number(task.second["critical_failures"]) + 1e-12 &&
          (!causal_coverage_complete || !causal_changes.object() ||
           causal_changes.fields().empty() || task_change_applied(causal_changes, task.first)))
        return false;
    }
    // Pareto 不能用少数质量提升掩盖资源灾难；允许核心指标在 10% 内自然波动，
    // 但不允许明显的 loop/成本灾难；最终 Val/holdout 门禁仍使用严格 1.0 比例。
    // 这样专长节点仍可探索，但 Node22
    // 这类 loop/成本整体回退不会继续占据搜索预算。
    const auto tolerance = 1.10;
    for (const auto *field : {"invalid_loops", "duration_ms", "tokens"}) {
      if (resources_report_only() && std::string(field) != "invalid_loops")
        continue;
      bool observed = true, partly_observed = false;
      for (const auto &task : parent.fields()) {
        const bool before = task.second.has(field) && !task.second[field].null();
        const bool after = candidate[task.first].has(field) &&
                           !candidate[task.first][field].null();
        observed &= before && after;
        partly_observed |= before != after;
      }
      if (partly_observed)
        return false;
      if (!observed)
        continue;
      const auto a = mean(candidate, field), b = mean(parent, field);
      if (a > -1e8 && b > -1e8 && a > b * tolerance + 1e-9)
        return false;
    }
    std::size_t affected = 0, regressed = 0;
    if (causal_changes.object())
      for (const auto &task : causal_changes.fields()) {
        bool applied = false;
        if (task.second.object())
          for (const auto &boundary : task.second.fields())
            applied |= number(boundary.second, 0) > 0;
        if (!applied)
          continue;
        if (!candidate.has(task.first) || !parent.has(task.first))
          return false;
        ++affected;
        regressed += number(candidate[task.first]["quality"]) + 1e-12 <
                     number(parent[task.first]["quality"]);
      }
    if (affected >= 6 && regressed * 3 >= affected)
      return false;
    return true;
  }

  // 使用同一套逻辑归档正常筛选和断点恢复结果，避免两条路径对同一份证据给出不同状态。
  Json classify_training_screening(Json &node, const Json &parent_artifact, const Json &child,
                                   const Json &parent_scores, const Report &screening,
                                   const Report *parent_report = nullptr) {
    const auto values = scores(screening);
    node["screening"] =
        Json{{"parent", parent_scores}, {"candidate", values}, {"split", "train"}};
    node["screening"]["control_coverage"] = control_coverage(child, screening);
    node["screening"]["applied_changes"] = applied_changes(parent_artifact, screening);
    node["screening"]["causal_changes"] =
        component_applied_changes(node["components"], node["screening"]["applied_changes"]);
    const bool causal_complete = parent_report &&
        scores(*parent_report).dump() == parent_scores.dump() &&
        complete_policy_receipts(*parent_report, agent_version_id(parent_artifact.dump())) &&
        complete_policy_receipts(screening, agent_version_id(child.dump()));
    node["screening"]["causal_coverage_complete"] = causal_complete;
    Json critical_risk = Json::Array{};
    for (const auto &task : parent_scores.fields())
      if (values.has(task.first) && number(values[task.first]["critical_failures"]) >
              number(task.second["critical_failures"]) + 1e-12)
        critical_risk.push(Json{{"task_id", task.first},
                               {"before", task.second["critical_failures"]},
                               {"after", values[task.first]["critical_failures"]},
                               {"change_observed", task_change_applied(
                                   node["screening"]["causal_changes"], task.first)},
                               {"coverage_complete", causal_complete}});
    if (!critical_risk.items().empty())
      node["screening"]["critical_risk"] = std::move(critical_risk);
    std::size_t affected = 0, regressed = 0;
    for (const auto &task : node["screening"]["causal_changes"].fields()) {
      bool applied = false;
      for (const auto &boundary : task.second.fields())
        applied |= number(boundary.second, 0) > 0;
      if (!applied || !values.has(task.first) || !parent_scores.has(task.first))
        continue;
      ++affected;
      regressed += number(values[task.first]["quality"]) + 1e-12 <
                   number(parent_scores[task.first]["quality"]);
    }
    node["screening"]["quality_tradeoff"] =
        Json{{"mean_delta", mean(values, "quality") - mean(parent_scores, "quality")},
             {"affected_tasks", affected}, {"regressed_affected_tasks", regressed},
             {"limited_attribution_sample", affected < 6 && regressed > 0}};
    const auto inherited_controls = control_coverage(parent_artifact, Report{});
    node["screening"]["new_control_observation"] =
        new_control_observation(node["screening"]["control_coverage"], inherited_controls);
    const bool controls_observed =
        node["screening"]["new_control_observation"].flag("observed", false);
    node["screening"]["controls_observed"] = controls_observed;
    const auto acceptance = config.str("acceptance", "strict_improvement");
    const bool quality_safe =
        screening_quality_safe(values, parent_scores, node["screening"]["causal_changes"],
                               causal_complete);
    const bool observed_gain =
        observed_training_gain(values, parent_scores, node["screening"]["causal_changes"]);
    const bool batch_gain = screening_batch_gain(values, parent_scores);
    node["screening"]["observed_local_gain"] = observed_gain;
    node["screening"]["batch_gain"] = batch_gain;
    node["screening"]["attribution_pending"] = !observed_gain && batch_gain;
    const bool pass = quality_safe &&
                      (acceptance == "pareto"
                           ? observed_gain || batch_gain
                           : acceptance == "improvement_or_equal"
                               ? gepa::no_worse(values, parent_scores)
                               : improves(values, parent_scores));
    const bool changed = controls_observed &&
                         !node["screening"]["causal_changes"].fields().empty();
    node.fields().erase("reason");
    node["status"] = values.fields().empty() ? "inconclusive"
                     : !changed              ? "unobserved_changes"
                     : pass && screening_gain_for_validation(
                                   values, parent_scores, node["screening"]["causal_changes"])
                                             ? "screened"
                                             : "rejected_minibatch";
    if (node.str("status") == "screened")
      node["screening"]["selection_basis"] =
          observed_gain ? "observed_local_gain" : "batch_gain_attribution_pending";
    if (!changed && !values.fields().empty())
      node["reason"] = !controls_observed ? "new_control_not_applied"
                                            : "selected_component_not_applied";
    else if (changed && !values.fields().empty() && !quality_safe)
      node["reason"] = "pool_quality_regression";
    else if (changed && !values.fields().empty() && !observed_gain)
      node["reason"] = node.str("status") == "screened"
                           ? "batch_gain_attribution_pending"
                           : "no_observed_training_gain";
    return values;
  }

  // 中断前已经完成的报告可能因单题遥测缺失被旧逻辑写成 inconclusive。
  // 恢复时仍走 evaluate 的逐题缓存：完整题直接复用，只补跑缺失题，再按正常路径分类。
  void recover_inconclusive_screenings() {
    if (tree.flag("holdout_started", false))
      return;
    bool recovered = false;
    for (std::size_t index = 1; index < tree["nodes"].items().size(); ++index) {
      auto &node = tree["nodes"].items()[index];
      if (node.str("kind") != "reflection" || node.str("status") != "inconclusive" ||
          node.str("version").empty() || !node["screening"].object() ||
          node.flag("inconclusive_recovery_attempted", false))
        continue;
      node["inconclusive_recovery_attempted"] = true;
      save();
      const auto parent_index = node.num("parent", 0);
      const auto parent_artifact = artifact(parent_index), child = artifact(index);
      std::vector<std::string> tasks;
      for (const auto &id : node["tasks"].items())
        tasks.push_back(id.text());
      const auto parent_report = evaluate(parent_artifact, tasks, "train");
      const auto parent_scores = scores(parent_report);
      if (parent_scores.fields().empty())
        continue;
      const auto screening = evaluate(child, tasks, "train");
      const auto values =
          classify_training_screening(node, parent_artifact, child, parent_scores, screening,
                                      &parent_report);
      node["screening_recovered"] = true;
      const auto source = proto<v1::RuntimePolicy>(parent_artifact["policy"]).snapshot();
      const auto feedback = make_gepa_memory_feedback(
          components(parent_artifact), components(child), source, parent_report, screening,
          parent_scores, values,
          node["screening"].flag("controls_observed", false)
              ? node["screening"]["causal_changes"]
              : Json(Json::Object{}),
          dataset.dataset_version(), evaluation_context, node.str("status") == "screened");
      tree["memory_feedback"] = merge_gepa_memory_feedback(
          tree["memory_feedback"], feedback, source, gate.num("memory_min_pairs", 2));
      save();
      if (node.str("status") == "screened")
        validate(index);
      recovered = true;
    }
    if (recovered) {
      for (std::size_t index = 0; index < tree["nodes"].items().size(); ++index)
        annotate_exploration_eligibility(index);
      persist_frontiers();
      save();
    }
  }

  // 筛选规则迁移只读取当前原生身份的完整缓存，不调用 evaluate 重跑健康 Agent。
  bool cached_training_report(const Json &artifact_value, const Json &tasks, Report &out) const {
    const auto version = agent_version_id(artifact_value.dump());
    if (!tasks.array() || tasks.items().empty())
      return false;
    Json settings = evaluation;
    settings["splits"] = Json::Array{"train"};
    settings["task_ids"] = tasks;
    auto variant = evaluation["variants"].items().front();
    variant["id"] = "candidate";
    variant["artifact"] = artifact_value;
    variant["version"] = version;
    settings["variants"] = Json::Array{variant};
    const auto expected_judge = task_evaluation_judge_fingerprint(evaluation_path, evaluation.dump());
    std::map<std::string, std::string> task_versions;
    for (const auto &task : dataset.tasks())
      task_versions[task.task_id()] = task.task_version();
    for (const auto &item : static_cast<const Json &>(tree)["evaluations"].fields()) {
      const auto &entry = item.second;
      if (entry.str("version") != version || entry.str("split") != "train" ||
          entry.str("status") != "complete" || entry["task_ids"].dump() != tasks.dump() ||
          entry.str("evaluation_context") != evaluation_context ||
          entry.str("candidate_evaluation_context") != candidate_evaluation_context ||
          entry.str("report_hash").empty() || !entry.num("attempt", 0))
        continue;
      try {
        const auto relative = fs::path("evaluations") / item.first /
            ("attempt-" + std::to_string(entry.num("attempt", 0))) / "report.json";
        if (entry.str("report") != relative.string() || fs::path(item.first).filename() != item.first)
          continue;
        const auto path = root / relative;
        if (fs::weakly_canonical(path).lexically_relative(fs::weakly_canonical(root)).string()
                .find("..") == 0)
          continue;
        const auto bytes = read(path, kMaxEvaluationReportBytes);
        const auto report_json = Json::parse(bytes);
        const auto report = proto<Report>(report_json);
        // 当前原生 entry 已绑定完整合并报告；沿用 evaluate 的缓存身份，不新增来源门槛。
        if (hash(bytes) != entry.str("report_hash") ||
            report_json["configuration"].str("judge_fingerprint") != expected_judge ||
            !shared_report_complete(report, settings, true) ||
            !complete_policy_receipts(report, version))
          continue;
        bool valid_tasks = true;
        for (const auto &trial : report.trials())
          valid_tasks &= task_versions.count(trial.task_id()) &&
                         task_versions.at(trial.task_id()) == trial.task_version();
        if (!valid_tasks)
          continue;
        out = report;
        return true;
      } catch (const std::exception &) {
        continue;
      }
    }
    return false;
  }

  // 保留旧决定，正常重算派生状态；原始报告、版本、评分和已消耗预算不变。
  bool reclassify_cached_training(std::size_t index) {
    if (final_holdout_replay || tree.flag("holdout_started", false) || !index ||
        index >= tree["nodes"].items().size())
      return false;
    auto &node = tree["nodes"].items()[index];
    const auto &existing = static_cast<const Json &>(node);
    if (node.str("kind") != "reflection" || node.str("status") != "rejected_minibatch" ||
        node.str("reason") != "pool_quality_regression" || node.str("version").empty() ||
        node.num("screening_decision_revision", 0) >= 2 ||
        (existing["validation"].object() && !existing["validation"].fields().empty()) ||
        existing["validation_probe"].object() || !existing["screening"].object() ||
        node["screening"].str("split", "train") != "train" || node.num("parent", SIZE_MAX) >= index)
      return false;
    for (const auto &item : static_cast<const Json &>(tree)["evaluations"].fields())
      if (item.second.str("version") == node.str("version") &&
          item.second.str("split") != "train")
        return false;
    const auto parent_artifact = artifact(node.num("parent", 0)), child = artifact(index);
    Report before, after;
    if (!cached_training_report(parent_artifact, node["tasks"], before) ||
        !cached_training_report(child, node["tasks"], after) ||
        scores(before).dump() != node["screening"]["parent"].dump() ||
        scores(after).dump() != node["screening"]["candidate"].dump())
      return false;
    const auto old_status = node.str("status"), old_reason = node.str("reason");
    classify_training_screening(node, parent_artifact, child, scores(before), after, &before);
    node["screening_decision_history"].push(Json{{"revision", 1},
                                               {"status", old_status}, {"reason", old_reason}});
    node["screening_decision_revision"] = 2;
    return true;
  }

  void recover_rejected_screenings() {
    if (final_holdout_replay || tree.flag("holdout_started", false))
      return;
    bool recovered = false;
    for (std::size_t index = 1; index < tree["nodes"].items().size(); ++index) {
      if (!reclassify_cached_training(index))
        continue;
      recovered = true;
      save();
      if (tree["nodes"].items()[index].str("status") == "screened") {
        check();
        validate(index);
      }
    }
    if (recovered) {
      for (std::size_t index = 0; index < tree["nodes"].items().size(); ++index)
        annotate_exploration_eligibility(index);
      persist_frontiers();
      save();
    }
  }

  // 记录真实出现的控制机会，帮助后续候选覆盖未被利用的能力。
  void remember_opportunities(const Json &parent, const Report &report) {
    // A new parent report starts a proposal-local cache; controls are fixed in Search.
    reviewed_feedback_cache.clear();
    const auto policy = proto<v1::RuntimePolicy>(parent["policy"]);
    if (!controls.flag("enabled", false) || policy.options().requested_level() < v1::L2)
      return;
    auto packet_controls = control_generation_input(
        policy.snapshot(), without_generated_controls(policy.options()), controls);
    packet_controls.fields().erase("memories");
    for (const auto &trial : report.trials()) {
      if (trial.variant_id() != "candidate" ||
          std::find(train.begin(), train.end(), trial.task_id()) == train.end())
        continue;
      auto view = reviewed_feedback_trial(trial);
      Json row{{"trial", view}};
      bind_schedule_contracts(row, trial.task_id());
      Json input{{"controls", packet_controls}, {"feedback", Json::Array{row}}};
      for (const auto *component : {"schedule_rules", "action_rules", "loop_rules"}) {
        input["selected_components"] = Json::Array{component};
        if (control_evidence(input, component)) {
          auto &opportunity = tree["training_opportunities"][component][trial.task_id()];
          const auto previous = number(opportunity, opportunity.null() ? 0 : 1);
          opportunity = std::max(previous, trial.verdict() == "failed" ? 2.0 : 1.0);
        }
      }
    }
  }

  // 为指定组件构造训练批次，保证相关能力有实际触发机会。
  std::vector<std::string> component_batch(const Json &selected, std::size_t sequence) const {
    auto result = minibatch(sequence);
    if (selected.items().size() != 1)
      return result;
    const auto &available = tree["training_opportunities"][selected.items().front().text()];
    std::vector<std::string> candidates;
    if (available.object())
      for (const auto &item : available.fields())
        if (std::find(train.begin(), train.end(), item.first) != train.end())
          candidates.push_back(item.first);
    if (selected.items().front().text() == "schedule_rules")
      for (const auto &id : train)
        if (schedule_training_priority(id) &&
            std::find(candidates.begin(), candidates.end(), id) == candidates.end())
          candidates.push_back(id);
    if (candidates.empty())
      return result;
    std::stable_sort(candidates.begin(), candidates.end(), [&](const auto &a, const auto &b) {
      if (selected.items().front().text() == "schedule_rules" &&
          schedule_training_priority(a) != schedule_training_priority(b))
        return schedule_training_priority(a) > schedule_training_priority(b);
      return number(available[a], available[a].null() ? 0 : 1) >
             number(available[b], available[b].null() ? 0 : 1);
    });
    if (selected.items().front().text() == "schedule_rules" &&
        schedule_training_priority(candidates.front()) >= 2)
      candidates.erase(
          std::remove_if(candidates.begin(), candidates.end(),
                         [&](const auto &id) { return schedule_training_priority(id) == 0; }),
          candidates.end());
    // 在保持 epoch 覆盖的同时，为当前选中的能力预留一个有对应证据的样本。
    const auto &task = candidates[sequence % candidates.size()];
    if (std::find(result.begin(), result.end(), task) == result.end())
      result.front() = task;
    std::sort(result.begin(), result.end());
    return result;
  }

  // 用保留的执行事实预检控制规则；预检仅排除明显无效项，不替代真实 Agent 评测。
  Json control_preflight(const Json &parent, const Json &candidate, const Report &report,
                         const Json &binding_provenance = Json()) const {
    Json result{{"rules", Json::Object{}}, {"missing", Json::Array{}},
                {"planning_hints", Json::Object{}}, {"execution_changes", Json::Object{}}};
    std::set<std::string> existing, retained;
    std::map<std::string, const v1::LoopRule *> changed_caps;
    std::set<std::string> changed_action_budgets;
    std::set<std::string> changed_schedule_rules;
    const auto old = proto<v1::RuntimePolicy>(parent["policy"]),
               policy = proto<v1::RuntimePolicy>(candidate["policy"]);
    std::map<std::string, Json> repeat_provenance;
    for (const auto &rule : old.options().action_rules())
      existing.insert(rule.rule_id());
    for (const auto &rule : old.options().loop_rules())
      existing.insert(rule.rule_id());
    for (const auto &rule : old.options().schedule_rules())
      existing.insert(rule.rule_id());
    for (const auto &rule : policy.options().action_rules())
      retained.insert(rule.rule_id());
    for (const auto &rule : policy.options().loop_rules())
      retained.insert(rule.rule_id());
    for (const auto &rule : policy.options().schedule_rules())
      retained.insert(rule.rule_id());
    Json removed = Json::Array{};
    for (const auto &rule : old.options().action_rules())
      if (rule.has_source_experience() && !retained.count(rule.rule_id()))
        removed.push(rule.rule_id());
    for (const auto &rule : old.options().loop_rules())
      if (rule.has_source_experience() && !retained.count(rule.rule_id()))
        removed.push(rule.rule_id());
    for (const auto &rule : old.options().schedule_rules())
      if (rule.has_source_experience() && !retained.count(rule.rule_id()))
        removed.push(rule.rule_id());
    if (!removed.items().empty() &&
        std::includes(existing.begin(), existing.end(), retained.begin(), retained.end()) &&
        source_experiences(parent).dump() == source_experiences(candidate).dump()) {
      // 纯删除也需训练差分：在父轨迹上回放候选，复用真实边界回执和分支账本。
      // 混合经验改写不走此捷径；没有新增规则不等于已证明移除约束有效。
      const auto changes = applied_changes(candidate, report);
      result["removed_rule_ids"] = removed;
      result["removal_changes"] = changes;
      result["pure_removal_no_effect"] = changes.fields().empty();
    }
    for (const auto &rule : policy.options().action_rules())
      if (rule.has_source_experience() && !existing.count(rule.rule_id())) {
        result["rules"][rule.rule_id()] = 0;
        if (rule.max_attempts())
          changed_action_budgets.insert(rule.rule_id());
        const bool repeat_failure = rule.max_failed_attempts() >= 2 && !rule.max_attempts() &&
                                    rule.failure_key_arguments().empty() &&
                                    rule.terminal_error_codes().empty() &&
                                    !rule.operation().empty() && rule.operation() != "*";
        if (repeat_failure && binding_provenance.array()) {
          std::size_t matches = 0;
          Json cited;
          for (const auto &item : binding_provenance.items())
            if (item.str("component") == "action_rules" &&
                item.str("primitive_id") == "repeat_failed_operation" &&
                item.str("memory_id") == rule.source_experience().memory_id() &&
                item.str("target_scope") == rule.target_scope() &&
                item.str("operation") == rule.operation()) {
              ++matches;
              cited = item;
            }
          const bool valid =
              matches == 1 && !cited.str("task_id").empty() &&
              !cited.str("evidence_ref").empty() && cited["arguments"].object() &&
              !cited["arguments"].str("action").empty() &&
              cited.flag("observed_failure", false) && cited.num("prior_failures", 0) >= 2;
          if (valid) {
            repeat_provenance[rule.rule_id()] = cited;
            result["provenance"][rule.rule_id()] = cited;
          } else {
            if (!result["invalid_provenance"].array())
              result["invalid_provenance"] = Json::Array{};
            result["invalid_provenance"].push(rule.rule_id());
          }
        }
      }
    for (const auto &rule : policy.options().loop_rules())
      if (rule.has_source_experience() && !existing.count(rule.rule_id())) {
        result["rules"][rule.rule_id()] = 0;
        if (rule.max_iterations())
          changed_caps[rule.rule_id()] = &rule;
        const v1::LoopRule *previous = nullptr;
        for (const auto &before : old.options().loop_rules())
          if (before.target_scope() == rule.target_scope() &&
              (before.actor_id().empty() || before.actor_id() == rule.actor_id()) &&
              (!previous || (previous->actor_id().empty() && !before.actor_id().empty()) ||
               (previous->actor_id() == before.actor_id() && before.has_source_experience())))
            previous = &before;
        // reason 是解释，不是执行条件。新削减恢复预算必须指向结构化事实，
        // 既有未修改规则仍可加载，不借本次变更迁移或重写旧策略。
        if (previous && rule.max_replans() < previous->max_replans() &&
            !rule.after_action_rejection() && rule.error_codes().empty()) {
          if (!result.has("unconditioned_recovery"))
            result["unconditioned_recovery"] = Json::Array{};
          result["unconditioned_recovery"].push(rule.rule_id());
        }
      }
    for (const auto &rule : policy.options().schedule_rules())
      if (rule.has_source_experience() && !existing.count(rule.rule_id())) {
        result["rules"][rule.rule_id()] = 0;
        changed_schedule_rules.insert(rule.rule_id());
      }
    if (!result["rules"].fields().empty())
      for (const auto &trial : report.trials()) {
        if (trial.variant_id() != "candidate")
          continue;
        const auto evidence = json(trial.evidence());
        auto reviewed = feedback_trial(trial, controls);
        bind_iteration_assessments(reviewed, evidence);
        const auto traces =
            evidence["traces"].array() ? evidence["traces"] : Json(Json::Array{evidence["trace"]});
        for (const auto &trace : traces.items()) {
          if (!trace["events"].array())
            continue;
          const auto bound = bind_runtime_policy(policy, proto<v1::Identity>(trace["identity"]));
          const PolicyRuntime runtime(bound);
          const auto replay_options = json(bound.options());
          Json ledger = Json::Object{}, attempts = Json::Object{};
          std::map<std::string, Json> schedule_inputs;
          for (const auto &event : trace["events"].items()) {
            if (event.str("kind") == "step.start" && event["input"]["actions"].array())
              schedule_inputs[event.str("step_id")] = event["input"];
            snapshot_scheduled_target(attempts, ledger, replay_options, event);
            if (event.str("kind") == "action.end")
              settle_target_event(ledger,
                                  static_cast<const Json &>(attempts)[event.str("action_id")],
                                  replay_options, event);
            if (event["attributes"].str("type") != "policy.query")
              continue;
            const auto &before = event["attributes"]["decision"];
            auto query = proto<v1::RuntimeQuery>(before["query"]);
            if (schedule_inputs.count(event.str("step_id")))
              project_schedule_evidence(query, schedule_inputs.at(event.str("step_id")));
            if (query.actor_id().empty())
              query.set_actor_id(event.str("actor_id"));
            if (query.boundary() != "llm" && query.boundary() != "loop" &&
                query.boundary() != "actions" && query.boundary() != "schedule")
              continue;
            if (query.previous().iteration())
              query.mutable_previous()->set_policy_id(bound.policy_id());
            for (auto &action : *query.mutable_actions())
              if (query.boundary() == "actions") {
                const auto snapshot = target_failure_snapshot(
                    replay_options, query.advice_query().target_scope(), action.operation(),
                    json(action.arguments()), ledger, query.execution_id());
                attempts[action.action_id()] = Json{{"entries", snapshot},
                                                    {"scope", query.advice_query().target_scope()},
                                                    {"operation", action.operation()},
                                                    {"execution_id", query.execution_id()}};
                action.clear_target_failures();
                for (const auto &entry : snapshot.fields())
                  (*action.mutable_target_failures())[entry.first] = entry.second.num("count", 0);
              }
            const auto after = runtime.decide(query);
            if (query.boundary() == "llm" && query.observed_operations().empty() &&
                after.context() != before.str("context")) {
              auto lower = [](std::string text) {
                std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) {
                  return static_cast<char>(c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c);
                });
                return text;
              };
              const auto task = lower(query.advice_query().task());
              for (const auto &rule : bound.options().schedule_rules()) {
                if (!result["rules"].has(rule.rule_id()) || rule.task_terms().empty() ||
                    rule.target_scope() != query.advice_query().target_scope())
                  continue;
                bool matched = true;
                for (const auto &term : rule.task_terms())
                  matched &= task.find(lower(term)) != std::string::npos;
                for (const auto &term : rule.exclude_terms())
                  matched &= task.find(lower(term)) == std::string::npos;
                if (matched && after.context().find(rule.before_operation()) != std::string::npos &&
                    after.context().find(rule.after_operation()) != std::string::npos) {
                  result["rules"][rule.rule_id()] = result["rules"].num(rule.rule_id(), 0) + 1;
                  result["planning_hints"][rule.rule_id()] =
                      result["planning_hints"].num(rule.rule_id(), 0) + 1;
                }
              }
            }
            if (query.boundary() == "schedule" && before["schedule"]["actions"].array()) {
              auto edges = [](const v1::SchedulePlan &plan) {
                Json result = Json::Object{};
                for (const auto &action : plan.actions()) {
                  result[action.action_id()] = Json::Object{};
                  for (const auto &dependency : action.after())
                    result[action.action_id()][dependency.action_id()] =
                        dependency.require_success();
                }
                return result.dump();
              };
              // 预检复用生成时的同批绑定：只有能表达当前轮契约的真实顺序变化才算机会。
              // 结果条件不能靠新增成功依赖冒充，即使运行时能机械地加出这条边也不接受。
              const auto query_json = json(query);
              const Json plan{{"target_scope", query.advice_query().target_scope()},
                              {"task_text", query.advice_query().task()},
                              {"actions", query_json["actions"]},
                              {"schedule_contract", schedule_training_contract(
                                  trial.task_id(), query.advice_query().task())}};
              const auto bindings = observed_operation_bindings(
                  Json{{"controls", Json::Object{}},
                       {"selected_components", Json::Array{"schedule_rules"}},
                       {"feedback", Json::Array{Json{{"trial", Json{{"evidence", Json{{"runs",
                           Json::Array{Json{{"schedule_plans", Json::Array{plan}}}}}}}}}}}}},
                  config.num("max_input_bytes", 65536))["controls"]["editable_bindings"];
              if (edges(after.schedule()) != edges(proto<v1::SchedulePlan>(before["schedule"])))
                for (const auto &id : after.schedule().rule_ids())
                  if (result["rules"].has(id)) {
                    bool supported = false;
                    // 无可表达的绑定时正常拒绝候选；缺失数组不是评测基础设施异常。
                    for (const auto &rule : bound.options().schedule_rules())
                      if (rule.rule_id() == id && bindings.array())
                        for (const auto &binding : bindings.items())
                          supported |= binding.str("target_scope") == rule.target_scope() &&
                              binding.str("before_operation") == rule.before_operation() &&
                              binding.str("after_operation") == rule.after_operation() &&
                              binding.flag("require_success", false) == rule.require_success();
                    if (supported)
                      result["rules"][id] = result["rules"].num(id, 0) + 1;
                    else {
                      if (!result.has("unsupported_schedule_contract"))
                        result["unsupported_schedule_contract"] = Json::Array{};
                      result["unsupported_schedule_contract"].push(id);
                    }
                  }
            }
            for (const auto &action : after.actions())
              if (action.decision() == "reject" && result["rules"].has(action.rule_id())) {
                bool allowed = true;
                const Json *matched_action = nullptr;
                for (const auto &previous : before["actions"].items())
                  if (previous.str("action_id") == action.action_id())
                    allowed = previous.str("decision") == "allow";
                for (const auto &previous : before["query"]["actions"].items())
                  if (previous.str("action_id") == action.action_id())
                    matched_action = &previous;
                // repeat_failed_operation 的训练预检只能由生成时引用的同一道题、
                // 同一 operation 和 arguments.action 证明。其他任务上的同 rule_id
                // 命中仍会被真实评测记录，但不能冒充本次候选的生成依据。
                const auto cited = repeat_provenance.find(action.rule_id());
                if (cited != repeat_provenance.end()) {
                  allowed &= trial.task_id() == cited->second.str("task_id") && matched_action &&
                             matched_action->str("operation") ==
                                 cited->second.str("operation") &&
                             (*matched_action)["arguments"].str("action") ==
                                 cited->second["arguments"].str("action") &&
                             matched_action->num("prior_failures", 0) >= 2;
                } else if (binding_provenance.array()) {
                  // 显式 provenance 模式下，新重复失败规则没有合法引用就保持 missing。
                  for (const auto &rule : policy.options().action_rules())
                    if (rule.rule_id() == action.rule_id() && rule.max_failed_attempts() >= 2 &&
                        !rule.max_attempts() && rule.failure_key_arguments().empty() &&
                        rule.terminal_error_codes().empty())
                      allowed = false;
                }
                if (allowed) {
                  result["rules"][action.rule_id()] = result["rules"].num(action.rule_id(), 0) + 1;
                  // 差分命中只证明会拦截，不证明拦截正确；完整训练评审中的必要动作是反例。
                  if (changed_action_budgets.count(action.rule_id()))
                    for (const auto &run : reviewed["evidence"]["runs"].items()) {
                      if (run.str("run_id") != trace["identity"].str("run_id"))
                        continue;
                      for (const auto &reviewed_action : run["actions"].items()) {
                        if (reviewed_action.str("kind") != "action.end" ||
                            !reviewed_action.flag("reviewed_required_action", false) ||
                            reviewed_action["action_control"].str("query_event_id") !=
                                event.str("event_id"))
                          continue;
                        const auto source = std::find_if(
                            trace["events"].items().begin(), trace["events"].items().end(),
                            [&](const auto &value) {
                              return value.str("event_id") == reviewed_action.str("event_id") &&
                                     value.str("kind") == "action.end" &&
                                     value.str("action_id") == action.action_id();
                            });
                        if (source == trace["events"].items().end())
                          continue;
                        if (!result.has("known_keep_truncation"))
                          result["known_keep_truncation"] = Json::Array{};
                        result["known_keep_truncation"].push(
                            Json{{"rule_id", action.rule_id()},
                                 {"task_id", trial.task_id()},
                                 {"event_id", event["event_id"]},
                                 {"action_id", action.action_id()},
                                 {"execution_id", event["execution_id"]}});
                      }
                    }
                }
              }
            const auto &progress = after.progress();
            if (query.boundary() == "loop" && progress.decision() == "stop" &&
                before["progress"].str("decision") != "stop" &&
                changed_caps.count(progress.rule_id()) &&
                query.iteration() > changed_caps.at(progress.rule_id())->max_iterations() &&
                reviewed["evidence"]["runs"].array()) {
              const bool finish = changed_caps.at(progress.rule_id())->require_finalization() &&
                                  query.can_finalize();
              bool cuts_work = false;
              for (const auto &run : reviewed["evidence"]["runs"].items()) {
                if (run.str("run_id") != trace["identity"].str("run_id"))
                  continue;
                for (const auto &action : run["actions"].items())
                  if ((action.str("kind") == "loop.decision" ||
                       action.str("kind") == "action.end") &&
                      action.str("execution_id") == event.str("execution_id") &&
                      action.str("loop_id") == event.str("loop_id") &&
                      proto<v1::TraceEvent>(Json{{"iteration", action["iteration"]}}).iteration() >=
                          query.iteration() &&
                      action["iteration_assessment"].str("decision") == "keep" &&
                      !(finish && action.flag("reviewed_final_answer", false)))
                    cuts_work = true;
              }
              if (cuts_work) {
                if (!result.has("known_keep_truncation"))
                  result["known_keep_truncation"] = Json::Array{};
                result["known_keep_truncation"].push(Json{{"rule_id", progress.rule_id()},
                                                          {"task_id", trial.task_id()},
                                                          {"event_id", event["event_id"]},
                                                          {"execution_id", event["execution_id"]}});
              }
            }
            if (query.boundary() == "loop" &&
                before["progress"].str("decision") != progress.decision() &&
                progress.decision() != "continue" && result["rules"].has(progress.rule_id()))
              result["rules"][progress.rule_id()] = result["rules"].num(progress.rule_id(), 0) + 1;
          }
        }
      }
    // 提示可以为候选争取真实试验机会，但不等于依赖图、动作或循环已经改变。
    // L2 编排必须能在父轨迹中的真实 schedule 边界改变依赖图；只改变首轮提示
    // 不能进入真实评测，更不能被后续模型输出的随机差异证明有效。
    for (const auto &rule : result["rules"].fields()) {
      result["execution_changes"][rule.first] =
          rule.second.integer() - result["planning_hints"].num(rule.first, 0);
      if (!rule.second.integer() ||
          (changed_schedule_rules.count(rule.first) &&
           !result["execution_changes"].num(rule.first, 0)))
        result["missing"].push(rule.first);
    }
    result["passed"] =
        result["missing"].items().empty() && !result.flag("pure_removal_no_effect", false) &&
        !result.has("unconditioned_recovery") && !result.has("known_keep_truncation") &&
        !result.has("unsupported_schedule_contract");
    return result;
  }

  // Soft train-only evidence for reducer ranking, never an acceptance threshold.
  Json reflection_candidate_support(const Json &candidate, const Report &report) const {
    Json support{{"basis", "parent_training_trajectory_replay"},
                 {"task_ids", Json::Array{}}, {"actors", Json::Object{}},
                 {"boundary_changes", 0}, {"events", Json::Array{}}};
    const auto changes = applied_changes(candidate, report, &support);
    for (const auto &task : changes.fields()) {
      support["task_ids"].push(task.first);
      for (const auto &boundary : task.second.fields())
        support["boundary_changes"] = support.num("boundary_changes", 0) + boundary.second.integer();
    }
    support["different_tasks"] = support["task_ids"].items().size();
    return support;
  }

  // 同一提案的独立 actor 可以有不同证据；无差分的新规则不应连带淘汰有效分支。
  // 只撤回本次新增的 actor 规则，不删除或恢复父策略已有规则；危险条件不走此修复。
  // 子集重新编译、授权并完整预检，实际收益仍由随后真实配对评测决定。
  Json triggerable_loop_subset(const Json &parent, const Json &candidate,
                               const Report &report, const Json &preflight,
                               const Json &binding_provenance = Json()) const {
    if (preflight.flag("passed", false) || !preflight["missing"].array() ||
        preflight["missing"].items().empty() || preflight.has("unconditioned_recovery") ||
        preflight.has("known_keep_truncation"))
      return Json();
    auto values = components(candidate);
    if (!values["loop_rules"].array())
      return Json();
    std::set<std::string> missing;
    for (const auto &id : preflight["missing"].items())
      missing.insert(id.text());
    std::set<std::pair<std::string, std::string>> inherited;
    const auto old = proto<v1::RuntimePolicy>(parent["policy"]),
               proposed = proto<v1::RuntimePolicy>(candidate["policy"]);
    for (const auto &rule : old.options().loop_rules())
      if (rule.has_source_experience())
        inherited.emplace(rule.target_scope(), rule.actor_id());
    Json kept = Json::Array{}, removed = Json::Array{};
    std::size_t index = 0;
    for (const auto &rule : proposed.options().loop_rules()) {
      if (!rule.has_source_experience())
        continue;
      const auto &value = values["loop_rules"].items().at(index++);
      if (missing.count(rule.rule_id()) && !rule.actor_id().empty() &&
          !inherited.count({rule.target_scope(), rule.actor_id()}))
        removed.push(rule.rule_id());
      else
        kept.push(value);
    }
    if (removed.items().empty())
      return Json();
    values["loop_rules"] = std::move(kept);
    // 不能把退回原父策略或只改说明文字，当作筛出了可评测的新控制。
    if (values.dump() == components(parent).dump())
      return Json();
    const auto reduced = compose(parent, values);
    auto checked = control_preflight(parent, reduced, report, binding_provenance);
    if (!checked.flag("passed", false) || checked["rules"].fields().empty())
      return Json();
    return Json{{"candidate", reduced}, {"preflight", checked},
                {"removed_rule_ids", removed}};
  }

  // 修正规则与宿主声明的编排契约不一致之处，不新增宿主未提供的能力。
  Json repair_schedule_contracts(const Json &draft, const Report &report, Json &repairs) const {
    repairs = Json::Array{};
    if (!draft["controls"]["schedule_rules"].array())
      return draft;
    auto result = draft;
    auto lower = [](std::string value) {
      std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c);
      });
      return value;
    };
    for (auto &rule : result["controls"]["schedule_rules"].items()) {
      if (rule.flag("require_success", false) || !rule["task_terms"].array() ||
          rule["task_terms"].items().empty())
        continue;
      for (const auto &trial : report.trials()) {
        if (trial.variant_id() != "candidate" || trial.split() != "train")
          continue;
        const auto evidence = json(trial.evidence());
        const auto traces =
            evidence["traces"].array() ? evidence["traces"] : Json(Json::Array{evidence["trace"]});
        bool witnessed = false;
        for (const auto &trace : traces.items())
          for (const auto &event : trace["events"].items()) {
            if (event["attributes"].str("type") != "policy.query")
              continue;
            const auto &q = event["attributes"]["decision"]["query"];
            if (q.str("boundary") != "schedule")
              continue;
            const auto contract =
                schedule_training_contract(trial.task_id(), q["advice_query"].str("task"));
            if (!contract.flag("ordering_required", false) ||
                !contract.flag("require_success", false))
              continue;
            bool before = false, after = false, matches = true;
            for (const auto &action : q["actions"].items()) {
              before |= action.str("operation") == rule.str("before_operation");
              after |= action.str("operation") == rule.str("after_operation");
            }
            const auto text = lower(q["advice_query"].str("task"));
            for (const auto &term : rule["task_terms"].items())
              matches &= text.find(lower(term.text())) != std::string::npos;
            if (rule["exclude_terms"].array())
              for (const auto &term : rule["exclude_terms"].items())
                matches &= text.find(lower(term.text())) == std::string::npos;
            witnessed |= before && after && matches;
          }
        if (!witnessed)
          continue;
        // 已声明的训练约束具有权威性；不能从失败的供应商响应推断，也不能改写独立分支。
        rule["require_success"] = true;
        rule["reason"] = "前置操作成功后才启动依赖操作；前置失败时仅跳过依赖分支。";
        repairs.push(Json{{"task_id", trial.task_id()},
                          {"before_operation", rule["before_operation"]},
                          {"after_operation", rule["after_operation"]},
                          {"constraint", "stop_on_failure"}});
        break;
      }
    }
    return result;
  }

  // 根据实际执行上下文修正规则触发条件，避免规则永远无法命中。
  Json repair_schedule_guards(const Json &draft, const Report &report, Json &repairs) const {
    // 只根据带标签的训练批次校准；验证和留出仅作为选择证据，绝不能改写候选的任务守卫。
    auto result = draft;
    repairs = Json::Array{};
    if (!result["controls"]["schedule_rules"].array())
      return result;
    for (auto &rule : result["controls"]["schedule_rules"].items()) {
      if (!rule["task_terms"].array() || rule["task_terms"].items().size() < 3)
        continue;
      const auto before_operation = rule.str("before_operation"),
                 after_operation = rule.str("after_operation");
      std::vector<std::string> positive, negative;
      for (const auto &trial : report.trials()) {
        if (trial.variant_id() != "candidate")
          continue;
        const dataset::v1::EvaluationTask *task = nullptr;
        for (const auto &item : dataset.tasks())
          if (item.task_id() == trial.task_id()) {
            task = &item;
            break;
          }
        if (!task)
          continue;
        const auto attributes = json(task->attributes());
        const auto dependency =
            attributes["dependency"].string() ? attributes.str("dependency") : std::string{};
        if (dependency.empty())
          continue;
        const bool independent =
            dependency == "independent" || dependency == "parallel" || dependency == "none";
        const auto evidence = json(trial.evidence());
        const auto traces =
            evidence["traces"].array() ? evidence["traces"] : Json(Json::Array{evidence["trace"]});
        for (const auto &trace : traces.items()) {
          if (!trace["events"].array())
            continue;
          for (const auto &event : trace["events"].items()) {
            if (event["attributes"].str("type") != "policy.query")
              continue;
            const auto &query = event["attributes"]["decision"]["query"];
            if (query.str("boundary") != "schedule" || !query["actions"].array())
              continue;
            int before = 0, after = 0;
            for (const auto &action : query["actions"].items()) {
              before += action.str("operation") == before_operation;
              after += action.str("operation") == after_operation;
            }
            if (before && after && (before_operation != after_operation || before >= 2))
              (independent ? negative : positive).push_back(query["advice_query"].str("task"));
          }
        }
      }
      if (positive.empty())
        continue;
      auto lower = [](std::string text) {
        std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) {
          return static_cast<char>(c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c);
        });
        return text;
      };
      std::vector<std::string> retained;
      for (const auto &task : positive) {
        std::vector<std::string> matched;
        const auto text = lower(task);
        for (const auto &term : rule["task_terms"].items())
          if (text.find(lower(term.text())) != std::string::npos)
            matched.push_back(term.text());
        if (matched.size() > retained.size())
          retained = std::move(matched);
      }
      if (retained.size() < 2 || retained.size() == rule["task_terms"].items().size())
        continue;
      bool unsafe = false;
      for (const auto &task : negative) {
        const auto text = lower(task);
        bool matches = true;
        for (const auto &term : retained)
          matches &= text.find(lower(term)) != std::string::npos;
        if (rule["exclude_terms"].array())
          for (const auto &term : rule["exclude_terms"].items())
            if (text.find(lower(term.text())) != std::string::npos)
              matches = false;
        unsafe |= matches;
      }
      if (unsafe)
        continue;
      const auto removed = rule["task_terms"].items().size() - retained.size();
      rule["task_terms"] = Json::Array{};
      for (const auto &term : retained)
        rule["task_terms"].push(term);
      repairs.push(Json{{"before_operation", before_operation},
                        {"after_operation", after_operation},
                        {"removed_terms", removed},
                        {"matched_terms", retained.size()}});
    }
    return result;
  }

  // 将结构化提案应用到指定父节点；校验失败要保留原因，不能顺手扩大修改范围。
  Json mutate(const Json &parent, const Json &proposal, const Json &selected,
              bool trigger_only = false) const {
    const auto before = components(parent);
    const auto draft = expand_control_updates(proposal, before, selected);
    keys(draft, {"reason", "experience", "experiences", "controls"});
    validate_local_data(draft);
    require(!draft.str("reason").empty() && draft.str("reason").size() <= 2048 &&
                !(draft.has("experience") && draft.has("experiences")) &&
                (draft.has("experience") || draft.has("experiences") || draft.has("controls")),
            "gepa_draft_invalid");
    auto values = before;
    Json changes = draft.has("experiences") ? draft["experiences"] : Json(Json::Array{});
    if (draft.has("experience"))
      changes.push(draft["experience"]);
    require(changes.array() && changes.items().size() <= before.fields().size(),
            "gepa_draft_invalid");
    if (trigger_only) {
      require(selected.items().size() == 1 &&
                  selected.items().front().text().rfind("experience:", 0) == 0 &&
                  !draft.has("controls"), "gepa_trigger_only_update_invalid");
      for (const auto &change : changes.items()) {
        const auto key = "experience:" + change.str("memory_id");
        const auto trigger = change.str("trigger_pattern");
        require(key == selected.items().front().text() &&
                    change["content"].string() &&
                    change.str("content") == experience_content(before[key]) &&
                    (!change.has("observed_operations") ||
                     change["observed_operations"].dump() == before[key]["observed_operations"].dump()) &&
                    (!change.has("trigger_pattern") ||
                     (change["trigger_pattern"].string() && !change.str("trigger_pattern").empty() &&
                      std::all_of(trigger.begin(), trigger.end(),
                                  [](unsigned char c) { return c < 128; }))),
                "gepa_trigger_only_update_invalid");
      }
    }
    std::set<std::string> unique;
    for (const auto &change : changes.items()) {
      keys(change, {"memory_id", "content", "trigger_pattern", "observed_operations"});
      require(change.object() && change.has("memory_id") && change.has("content") &&
                  !change.str("memory_id").empty() &&
                  (change["content"].null() ||
                   (change["content"].string() && !change["content"].text().empty() &&
                    change["content"].text().size() <= config.num("max_experience_bytes", 2048))),
              "gepa_draft_invalid");
      const auto key = "experience:" + change.str("memory_id");
      require(values.has(key), "gepa_experience_not_authorized");
      require(unique.insert(key).second, "gepa_draft_invalid");
      if (change["content"].null()) {
        require(!change.has("trigger_pattern") && !change.has("observed_operations"),
                "gepa_draft_invalid");
        values[key] = nullptr;
        continue;
      }
      auto value = values[key];
      if (value.null()) {
        const auto sources = source_experiences(seed);
        for (const auto &source : sources.items())
          if (source.str("memory_id") == change.str("memory_id"))
            value = experience_component(source);
      }
      if (value.object())
        value["content"] = change["content"];
      else
        value = change["content"];
      for (const auto *field : {"trigger_pattern", "observed_operations"})
        if (change.has(field)) {
          if (!value.object())
            value = Json{{"content", value}};
          value[field] = change[field];
        }
      if (value.has("trigger_pattern"))
        require(value["trigger_pattern"].string() && value.str("trigger_pattern").size() <= 2048,
                "gepa_draft_invalid");
      if (value.has("observed_operations")) {
        require(value["observed_operations"].array() &&
                    value["observed_operations"].items().size() <= 16,
                "gepa_draft_invalid");
        std::set<std::string> operations;
        for (const auto &operation : value["observed_operations"].items())
          require(operation.string() && identifier(operation.text()) &&
                      operations.insert(operation.text()).second,
                  "gepa_draft_invalid");
        value["observed_operations"] = Json::Array{};
        for (const auto &operation : operations)
          value["observed_operations"].push(operation);
      }
      values[key] = value;
    }
    if (draft.has("controls")) {
      keys(draft["controls"], {"action_rules", "loop_rules", "schedule_rules"});
      require(draft["controls"].object() && !draft["controls"].fields().empty(),
              "gepa_draft_invalid");
      for (const auto &item : draft["controls"].fields()) {
        require(item.second.array() && (values.has(item.first) || item.second.items().empty()),
                "gepa_component_not_authorized");
        if (item.first == "action_rules")
          for (const auto &rule : item.second.items()) {
            const auto empty = [](const Json &value) {
              return value.null() || (value.array() && value.items().empty());
            };
            const bool repeat = rule.str("primitive_id") == "repeat_failed_operation" ||
                                (rule.num("max_failed_attempts", 0) &&
                                 empty(rule["required_arguments"]) &&
                                 empty(rule["failure_key_arguments"]) &&
                                 empty(rule["terminal_error_codes"]) &&
                                 !rule.num("max_attempts", 0));
            if (!repeat || (!rule.str("operation").empty() && rule.str("operation") != "*"))
              continue;
            // 旧版通配规则可以原样继承或移除；新规则不能把单操作证据泛化到全工具。
            const bool inherited =
                std::any_of(before["action_rules"].items().begin(),
                            before["action_rules"].items().end(),
                            [&](const auto &old) { return old.dump() == rule.dump(); });
            require(inherited, "control_repeat_operation_required");
          }
        if (values.has(item.first))
          values[item.first] = item.second;
      }
    }
    std::set<std::string> allowed;
    for (const auto &item : selected.items())
      allowed.insert(item.text());
    for (const auto &item : values.fields())
      if (item.second.dump() != before[item.first].dump())
        require(allowed.count(item.first), "gepa_component_not_authorized");
    // 空草稿是合法弃权，不是一个新 Agent 版本；必须保留原始封装及其公开版本标识。
    if (values.dump() == before.dump())
      return parent;
    const auto candidate = compose(parent, values);
    return components(candidate).dump() == before.dump() ? parent : candidate;
  }

  // 请求模型生成有界提案；模型返回仍需协议和作用域校验，不能直接当成可执行策略。
  Json reflect(const Json &input, std::size_t node, const std::string &suffix = {},
               bool aggregate = false) {
    // 发送端不再以丢弃绑定来压进预算；大小不符时在调用模型前明确失败。
    const auto prepared =
        observed_operation_bindings(input, std::numeric_limits<std::size_t>::max());
    require(prepared.dump().size() <= config.num("max_input_bytes", 65536),
            "gepa_feedback_budget_too_small");
    auto &reflection_node = tree["nodes"].items()[node];
    search_check();
    require(tree.num("model_calls", 0) < config.num("max_model_calls", 8),
            "gepa_model_budget_exhausted");
    tree["model_calls"] = tree.num("model_calls", 0) + 1;
    tree["nodes"].items()[node]["status"] = "reflecting";
    save();
    auto adapter = model;
    if (!adapter) {
      auto runtime = legacy::load_runtime_configuration(config.str("runtime_config"));
      auto http = runtime.model;
      if (http.endpoint.empty())
        if (const auto *base = std::getenv("OPENAI_BASE_URL")) {
          http.endpoint = base;
          while (!http.endpoint.empty() && http.endpoint.back() == '/')
            http.endpoint.pop_back();
          http.endpoint += "/chat/completions";
        }
      if (http.model_id.empty())
        if (const auto *name = std::getenv("OPENAI_MODEL"))
          http.model_id = name;
      require(!http.endpoint.empty() && !http.model_id.empty(), "gepa_model_not_configured");
      if (!runtime.no_api_key) {
        const auto *key = std::getenv(
            (runtime.api_key_env.empty() ? "OPENAI_API_KEY" : runtime.api_key_env).c_str());
        require(key && *key, "gepa_model_key_missing");
        http.api_key = key;
      }
      http.max_retries = 0; // 预留一次反思调用就只允许一次传输尝试。
      search_check();
      http.timeout_ms = std::max<std::size_t>(
          1, std::min<std::size_t>(http.timeout_ms, deadline() - holdout_time() - now()));
      const auto cutoff = deadline() - holdout_time();
      http.cancelled = [&, cutoff] { return (cancelled && cancelled()) || now() >= cutoff; };
      http.max_request_bytes =
          std::max<std::size_t>(http.max_request_bytes, config.num("max_input_bytes", 65536) * 2);
      adapter = legacy::make_openai_compatible_model_adapter(http);
    }
    legacy::ModelRequest request;
    const auto name = std::to_string(node) + suffix;
    request.packet_id = "gepa-" + context.substr(0, 16) + "-" + name;
    request.system_prompt = reflection_prompt_for_components(
        read(config.str(aggregate ? "aggregation_prompt_file" : "prompt_file"), 65536),
        prepared["selected_components"]);
    request.system_prompt +=
        "\nChanging a retrieval trigger does not change the host's policy activation boundary. "
        "Claim prevention before an action only when feedback contains an actual pre-action "
        "application; otherwise optimize recovery in the observed eligible failure window.\n";
    if (prepared.flag("trigger_only_exploration", false))
      request.system_prompt +=
          "This approved experience was not sufficiently applied. Explore only its English "
          "trigger_pattern using the complete TRAIN failure facts. Copy its current content "
          "exactly; keep source, scope, observed_operations, controls and budgets unchanged. "
          "Do not claim an application that was not observed. Abstention is valid.\n";
    request.user_prompt = prepared.dump();
    validate_local_data(Json(request.system_prompt));
    write(root / "reflections" / (name + ".request.json"),
          Json{{"system", request.system_prompt}, {"input", prepared}}.dump());
    record_reflection_coverage(reflection_node, suffix, prepared);
    save();
    const auto call_started = std::chrono::steady_clock::now();
    const auto response = adapter->evaluate(request);
    const auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::steady_clock::now() - call_started)
                                 .count();
    tree["reflection_calls"][std::to_string(tree.num("model_calls", 0))] =
        reflection_call_diagnostic(response, static_cast<std::size_t>(duration_ms));
    save();
    search_check();
    if (response.error_code == "http_429") {
      // 供应商拒绝本次调用时，保留已预留预算和完成的执行检查点，重试同一代生成。
      tree["nodes"].items()[node]["status"] = "pending";
      save();
      throw Error("gepa_model_rate_limited");
    }
    require(response.transport_ok && response.error_code.empty() && response.body.size() <= 262144,
            "gepa_reflection_transport_failed");
    bool response_recovered = false, content_recovered = false;
    auto body = parse_model_object(response.body, &response_recovered);
    if (body.has("choices")) {
      require(body["choices"].array() && body["choices"].items().size() == 1,
              "gepa_reflection_choice_invalid");
      const auto &choice = body["choices"].items().front();
      require(choice.str("finish_reason") == "stop", "gepa_reflection_incomplete");
      body = parse_model_object(choice["message"].str("content"), &content_recovered);
    }
    if (response_recovered || content_recovered) {
      tree["nodes"].items()[node]["response_recovery"] =
          Json{{"method", "single_json_object"},
               {"provider_envelope", response_recovered},
               {"message_content", content_recovered}};
      save();
    }
    validate_local_data(body);
    // 自由说明只用于审计，不参与策略语义。供应商偶尔会略微超过协议上限；
    // 在本地按 UTF-8 边界裁剪，不能让一个合法的空更新因此变成失败节点。
    if (body["reason"].string() && body.str("reason").size() > 2048)
      body["reason"] = clip(body.str("reason"), 2000);
    Json binding_error, binding_provenance;
    try {
      require_typed_budget_updates(body, prepared, &binding_error);
      auto resolved =
          resolve_control_binding_updates(body, prepared, &binding_error, &binding_provenance);
      if (binding_provenance.array() && !binding_provenance.items().empty())
        tree["nodes"].items()[node]["control_binding_provenance"] = binding_provenance;
      else
        tree["nodes"].items()[node].fields().erase("control_binding_provenance");
      return resolved;
    } catch (const Error &) {
      // 固定码与可信范围足以纠正绑定；不落盘模型原文、来源 ID 或自由文本。
      if (binding_error.object()) {
        tree["reflection_calls"][std::to_string(tree.num("model_calls", 0))]
            ["control_binding_error"] = binding_error;
        save();
      }
      throw;
    }
  }

  // 基于父节点、训练反馈和允许修改的组件生成候选。
  // 生成结果还需通过结构与动作能力校验，不因为模型提出了规则就假定宿主能执行。
  Json propose(const Json &parent, const Json &input, std::size_t node, Json &pending) {
    const auto n = input["feedback"].items().size(),
               k = static_cast<std::size_t>(std::sqrt(double(n)));
    if (config.str("reflection_strategy", "single") != "combee" || k < 2)
      return reflect(input, node);
    const auto &selected = input["selected_components"];
    const auto needed = selected.items().size() * (k + 1);
    require(tree.num("model_calls", 0) + needed <= config.num("max_model_calls", 8),
            "gepa_model_budget_exhausted");
    auto values = components(parent);
    Json drafts{{"reason", "分组反思并汇总训练证据，保留各组件的受限改进。"},
                {"experiences", Json::Array{}}};
    Json stages = Json::Array{};
    std::mt19937 random(static_cast<unsigned>(config.num("seed", 42) + node));
    std::size_t component_index = 0;
    for (const auto &component : selected.items()) {
      const auto key = component.text(), prefix = ".combee-" + std::to_string(component_index++);
      std::vector<std::size_t> shuffled;
      for (std::size_t repeat = 0; repeat < config.num("duplication_factor", 2); ++repeat)
        for (std::size_t i = 0; i < n; ++i)
          shuffled.push_back(i);
      std::shuffle(shuffled.begin(), shuffled.end(), random);
      Json intermediates = Json::Array{};
      const Json one = Json::Array{key};
      for (std::size_t group = 0; group < k; ++group) {
        auto packet = input;
        restrict_reflection_components(packet, one);
        packet["feedback"] = Json::Array{};
        packet["phase"] = "combee_map";
        for (std::size_t i = group; i < shuffled.size(); i += k)
          packet["feedback"].push(input["feedback"].items()[shuffled[i]]);
        // 分组重复记录可能超限。控制反思先去重复行，再减少整行样本，不能先把所有
        // actions/loop evidence 换成分数摘要，否则虽有“反馈”却没有任何可执行修改依据。
        if (key == "action_rules" || key == "loop_rules" || key == "schedule_rules") {
          while (packet["feedback"].items().size() > 1 &&
                 reflection_input_bytes(packet) > config.num("max_input_bytes", 65536)) {
            auto &rows = packet["feedback"].items();
            std::size_t drop = rows.size() - 1;
            bool duplicate = false;
            for (std::size_t i = 1; i < rows.size() && !duplicate; ++i)
              for (std::size_t j = 0; j < i; ++j)
                if (rows[i].dump() == rows[j].dump()) {
                  drop = i;
                  duplicate = true;
                  break;
                }
            rows.erase(rows.begin() + drop);
            packet["feedback_truncated"] = true;
          }
          require(reflection_input_bytes(packet) <= config.num("max_input_bytes", 65536),
                  "gepa_feedback_budget_too_small");
        }
        // L1 沿用原有有界摘要；控制反馈在上面按整行保留执行结构和绑定。
        if (reflection_input_bytes(packet) > config.num("max_input_bytes", 65536)) {
          for (auto &row : packet["feedback"].items()) {
            const auto trial = row["trial"];
            row["trial"] = Json{
                {"verdict", trial["verdict"]},
                {"checks", trial["checks"]},
                {"output", trial.has("output") ? trial["output"] : trial["evidence"]["output"]},
                {"metrics", trial["metrics"]},
                {"evidence_truncated", true}};
          }
          while (packet["feedback"].items().size() > 1 &&
                 reflection_input_bytes(packet) > config.num("max_input_bytes", 65536)) {
            packet["feedback"].items().pop_back();
            packet["feedback_truncated"] = true;
          }
        }
        const auto suffix = prefix + "-map-" + std::to_string(group);
        const auto draft = reflect(packet, node, suffix);
        // 分组校验失败时保留真实草稿供一次受限修复，不重跑已完成的分组。
        pending = draft;
        write(root / "reflections" / (std::to_string(node) + suffix + ".draft.json"), draft.dump());
        const auto candidate = mutate(parent, draft, one);
        intermediates.push(Json{{"reason", draft["reason"]},
                                {"component", key},
                                {"value", components(candidate)[key]}});
        stages.push(Json{{"component", key},
                         {"phase", "map"},
                         {"file", std::to_string(node) + suffix},
                         {"feedback_records", packet["feedback"].items().size()}});
        tree["nodes"].items()[node]["reflection_stages"] = stages;
        save();
      }
      auto packet = input;
      packet.fields().erase("feedback");
      packet["phase"] = "combee_reduce";
      restrict_reflection_components(packet, one);
      packet["intermediate_proposals"] = intermediates;
      const auto draft = reflect(packet, node, prefix + "-reduce", true);
      pending = draft;
      write(root / "reflections" / (std::to_string(node) + prefix + "-reduce.draft.json"),
            draft.dump());
      const auto candidate = mutate(parent, draft, one);
      values[key] = components(candidate)[key];
      stages.push(Json{{"component", key},
                       {"phase", "reduce"},
                       {"file", std::to_string(node) + prefix + "-reduce"},
                       {"groups", k}});
      tree["nodes"].items()[node]["reflection_stages"] = stages;
      save();
      if (key.compare(0, 11, "experience:") == 0) {
        auto change = values[key].object() ? values[key] : Json{{"content", values[key]}};
        change["memory_id"] = key.substr(11);
        drafts["experiences"].push(change);
      } else
        drafts["controls"][key] = values[key];
    }
    return drafts;
  }

  // Map covers every distinct training task in the parent pool. The regular packet builder
  // enforces the same byte and trial limits on each group; unlike the old COMBEE shuffling,
  // no row is silently dropped after a model call has been assigned to it.
  Json propose_full_pool(const Json &parent, const Json &input, const Report &parent_report,
                         std::size_t node, Json &pending) {
    if (config.str("reflection_strategy", "single") != "combee")
      return reflect(input, node);
    try {
    const auto &selected = input["selected_components"];
    std::map<std::string, dataset::v1::EvaluationTrial> remaining;
    bool unique_candidate_tasks = true;
    for (const auto &trial : parent_report.trials())
      if (trial.variant_id() == "candidate" && !trial.task_id().empty())
        unique_candidate_tasks &= remaining.emplace(trial.task_id(), trial).second;
    require(!remaining.empty(), "gepa_parent_evaluation_inconclusive");
    // Each immutable pool task has the same formal score in every rest/smaller packet.
    // Repeated trials are reduced to their first row below, so retain their original
    // per-fragment scoring instead of substituting the full report's repetition mean.
    const auto pool_scores = scores(parent_report);
    const Json *shared_scores = parent_report.complete() && unique_candidate_tasks &&
                                       pool_scores.fields().size() == remaining.size()
                                   ? &pool_scores
                                   : nullptr;
    const auto all_trials = remaining;
    std::map<std::string, std::set<std::string>> applied_memory_tasks;
    for (const auto &entry : all_trials) {
      const auto applications = selected_experience_applications(entry.second, selected, seed);
      for (const auto &point : applications["points"].items())
        if (!point.str("memory_id").empty())
          applied_memory_tasks["experience:" + point.str("memory_id")].insert(entry.first);
    }
    const auto pool_size = remaining.size();
    const auto input_limit = config.num("max_input_bytes", 65536);
    std::vector<Json> groups;
    std::vector<Json> continuation_rows;
    std::set<std::string> covered;
    const bool control_selected = std::any_of(
        selected.items().begin(), selected.items().end(), [](const auto &item) {
          return item.text() == "action_rules" || item.text() == "loop_rules" ||
                 item.text() == "schedule_rules";
        });
    while (!remaining.empty()) {
      Report rest;
      rest.set_complete(parent_report.complete());
      for (const auto &entry : remaining)
        *rest.add_trials() = entry.second;
      auto group = packet(parent, rest, selected, input.flag("trigger_only_exploration", false),
                          shared_scores, &input["experiences"]);
      // A later group cannot forget necessary work observed in an earlier successful
      // parent run. Bindings still come only from this group's real trajectories.
      const auto merge_global_requirements = [&](Json &target) {
        for (const auto *field : {"action_requirements", "loop_requirements"})
          if (input["controls"][field].array())
            target["controls"][field] = input["controls"][field];
      };
      merge_global_requirements(group);
      while (reflection_input_bytes(group) > input_limit &&
             group["feedback"].items().size() > 1) {
        Report smaller;
        smaller.set_complete(parent_report.complete());
        const auto &rows = group["feedback"].items();
        for (std::size_t i = 0; i + 1 < rows.size(); ++i)
          *smaller.add_trials() = remaining.at(rows[i].str("task_id"));
        group = packet(parent, smaller, selected, input.flag("trigger_only_exploration", false),
                       shared_scores, &input["experiences"]);
        merge_global_requirements(group);
      }
      if (reflection_input_bytes(group) > input_limit)
        group.fields().erase("prior_search_rejections");
      Json ids = Json::Array{};
      for (const auto &row : group["feedback"].items()) {
        const auto id = row.str("task_id");
        if (remaining.count(id))
          ids.push(id);
      }
      require(!ids.items().empty(), "gepa_reflection_pool_coverage_stalled");
      group["phase"] = "pool_diagnostic_map";
      group["pool_task_count"] = pool_size;
      group["group_task_ids"] = ids;
      // The packet builder may use its last byte for real task evidence. Keep its
      // original rows instead of rejecting a valid group for diagnostic metadata.
      if (reflection_input_bytes(group) > input_limit)
        group.fields().erase("group_task_ids");
      if (reflection_input_bytes(group) > input_limit)
        group.fields().erase("pool_task_count");
      if (reflection_input_bytes(group) > input_limit)
        group.fields().erase("phase");
      require(reflection_input_bytes(group) <= input_limit,
              "gepa_feedback_budget_too_small");
      for (const auto &id : ids.items()) {
        covered.insert(id.text());
        remaining.erase(id.text());
      }
      if (control_selected)
        for (const auto &row : group["feedback"].items()) {
          const auto found = all_trials.find(row.str("task_id"));
          if (found == all_trials.end())
            continue;
          auto feedback_controls = controls;
          const auto policy = proto<v1::RuntimePolicy>(parent["policy"]);
          feedback_controls["aggressive_controls"] = policy.options().aggressive_controls();
          const auto complete = compact_control_reflection_trial(
              control_feedback_trial(found->second, selected, feedback_controls), selected);
          std::map<std::string, std::size_t> displayed;
          for (const auto &run : row["trial"]["evidence"]["runs"].items())
            for (const auto &action : run["actions"].items())
              ++displayed[run.str("run_id") + "\n" + action.dump()];
          Json supplemental = row;
          supplemental["trial"]["evidence"]["runs"] = Json::Array{};
          Json kind_counts = Json::Object{};
          std::size_t unshown = 0, highlighted = 0;
          for (const auto &run : complete["evidence"]["runs"].items()) {
            Json partial{{"run_id", run["run_id"]}, {"actions", Json::Array{}}};
            for (const auto &action : run["actions"].items()) {
              auto &seen = displayed[run.str("run_id") + "\n" + action.dump()];
              if (seen) {
                --seen;
                continue;
              }
              ++unshown;
              const auto kind = action.str("kind") + ":" + action.str("actor_id") + ":" +
                                action.str("operation") + ":" + action.str("status");
              kind_counts[kind] = kind_counts.num(kind, 0) + 1;
              const bool relevant = action_control_evidence(action) ||
                  (action["loop_control"].object() &&
                   (action["loop_control"].num("observed_no_progress", 0) ||
                    action["loop_control"].num("repeated_failure_steps", 0) ||
                    action["loop_control"].has("reviewed_waste_after")));
              if (relevant && highlighted < 8) {
                partial["actions"].push(action);
                ++highlighted;
              }
            }
            if (!partial["actions"].items().empty())
              supplemental["trial"]["evidence"]["runs"].push(std::move(partial));
          }
          if (!unshown)
            continue;
          std::size_t other_kinds = 0, other_actions = 0;
          if (kind_counts.fields().size() > 16) {
            Json retained = Json::Object{};
            std::vector<std::pair<std::string, std::size_t>> ranked;
            for (const auto &entry : kind_counts.fields())
              ranked.emplace_back(entry.first, kind_counts.num(entry.first, 0));
            std::stable_sort(ranked.begin(), ranked.end(), [](const auto &a, const auto &b) {
              return a.second > b.second;
            });
            for (std::size_t i = 0; i < ranked.size(); ++i)
              if (i < 16)
                retained[ranked[i].first] = ranked[i].second;
              else {
                ++other_kinds;
                other_actions += ranked[i].second;
              }
            kind_counts = std::move(retained);
          }
          std::size_t raw_events = 0;
          const auto raw = json(found->second.evidence());
          const auto traces = raw["traces"].array() ? raw["traces"]
                            : raw["trace"].object() ? Json(Json::Array{raw["trace"]})
                                                     : Json(Json::Array{});
          for (const auto &trace : traces.items())
            for (const auto &event : trace["events"].items())
              raw_events += event.str("kind") == "action.end" ||
                            event.str("kind") == "loop.decision";
          // The remaining selected action types are counted; up to eight additional
          // real typed examples give a second view without replaying redundant raw events.
          supplemental["trial"]["reflection_segment"] =
              Json{{"index", 2}, {"total", 2},
                   {"unshown_selected_actions", unshown},
                   {"typed_examples", highlighted},
                   {"remaining_without_detail", unshown - highlighted},
                   {"raw_action_events", raw_events},
                   {"other_action_kinds", other_kinds},
                   {"other_actions", other_actions},
                   {"action_counts", kind_counts}};
          supplemental["trial"]["reflection_actions_truncated"] =
              unshown > highlighted;
          continuation_rows.push_back(std::move(supplemental));
      }
      // Store the authoritative group ID list outside the request-size envelope.
      group["pool_group_task_ids_audit"] = ids;
      groups.push_back(std::move(group));
    }
    require(covered.size() == pool_size, "gepa_reflection_pool_coverage_incomplete");
    std::size_t continuation_index = 0;
    while (continuation_index < continuation_rows.size()) {
      auto segment = input;
      segment["phase"] = "pool_trajectory_segment";
      segment["feedback"] = Json::Array{};
      Json ids = Json::Array{};
      while (continuation_index < continuation_rows.size() &&
             segment["feedback"].items().size() < config.num("max_feedback_trials", 8)) {
        auto attempt = segment;
        attempt["feedback"].push(continuation_rows[continuation_index]);
        if (reflection_input_bytes(attempt) > input_limit)
          break;
        segment = std::move(attempt);
        ids.push(continuation_rows[continuation_index].str("task_id"));
        ++continuation_index;
      }
      require(!ids.items().empty(), "gepa_feedback_budget_too_small");
      segment["pool_group_task_ids_audit"] = ids;
      groups.push_back(std::move(segment));
    }

    auto &reflection_node = tree["nodes"].items()[node];
    Json stages = Json::Array{}, intermediates = Json::Array{};
    const auto before = components(parent);
    std::map<std::string, Json> support_cache;
    const auto support_for = [&](const Json &candidate) {
      const auto key = candidate["policy"].str("policy_id");
      const auto cached = support_cache.find(key);
      if (cached != support_cache.end())
        return cached->second;
      auto support = reflection_candidate_support(candidate, parent_report);
      support_cache.emplace(key, support);
      return support;
    };
    auto proposal_record = [&](const Json &draft, const Json &ids,
                               const std::string &id, const Json &task_facts) {
      Json candidate;
      try {
        candidate = mutate(parent, draft, selected,
                           input.flag("trigger_only_exploration", false));
      } catch (const legacy::EvaluationError &error) {
        if (std::string(error.what()) == "runtime_action_rule_invalid")
          throw Error("runtime_action_rule_invalid");
        throw;
      }
      const auto after = components(candidate);
      Json changes = Json::Object{};
      for (const auto &component : selected.items()) {
        const auto key = component.text();
        if (after[key].dump() != before[key].dump())
          changes[key] = after[key];
      }
      Json record{{"id", id}, {"reason", diagnostic_excerpt(draft.str("reason"), 768)},
                  {"task_ids", ids}, {"task_facts", task_facts},
                  {"candidate_values", changes}};
      if (changes.has("action_rules") || changes.has("loop_rules") || changes.has("schedule_rules"))
        record["actual_support"] = support_for(candidate);
      return record;
    };
    // Reduce and review select component values that already passed a real map trial's
    // typed-binding and mutate checks. This avoids inventing a new unbound budget in a
    // summary packet which has no original action evidence.
    auto selected_draft = [&](const Json &selection, const Json &records) {
      require(selection["selected_proposals"].object() &&
                  !selection.str("reason").empty(),
              "gepa_reflection_selection_invalid");
      Json draft{{"reason", diagnostic_excerpt(selection.str("reason"), 2048)},
                 {"experiences", Json::Array{}}};
      for (const auto &component : selected.items()) {
        const auto key = component.text();
        const auto id = selection["selected_proposals"].str(key);
        if (id.empty())
          continue;
        const Json *value = nullptr;
        for (const auto &record : records.items())
          if (record.str("id") == id && record.flag("valid", true) &&
              record["candidate_values"].has(key)) {
            value = &record["candidate_values"][key];
            break;
          }
        require(value != nullptr, "gepa_reflection_selection_invalid");
        if (key.rfind("experience:", 0) == 0) {
          Json change{{"memory_id", key.substr(11)},
                      {"content", value->object() ? (*value)["content"] : *value}};
          if (value->object())
            for (const auto *field : {"trigger_pattern", "observed_operations"})
              if (value->has(field))
                change[field] = (*value)[field];
          draft["experiences"].push(std::move(change));
        } else {
          require(value->array(), "gepa_reflection_selection_invalid");
          draft["controls"][key] = *value;
        }
      }
      // A cross-pool synthesis may rewrite an actually used L1 memory after seeing all
      // task facts. This does not grant new control bindings or unselected memory IDs.
      if (selection.has("experience_revisions")) {
        require(selection["experience_revisions"].array(),
                "gepa_reflection_selection_invalid");
        for (const auto &revision : selection["experience_revisions"].items()) {
          const auto key = "experience:" + revision.str("memory_id");
          const bool authorized = std::any_of(
              selected.items().begin(), selected.items().end(),
              [&](const auto &item) { return item.text() == key; });
          require(authorized && applied_memory_tasks[key].size() >= 2,
                  "gepa_reflection_selection_invalid");
          auto &changes = draft["experiences"].items();
          changes.erase(std::remove_if(changes.begin(), changes.end(),
                                       [&](const auto &item) {
                                         return item.str("memory_id") == revision.str("memory_id");
                                       }),
                        changes.end());
          changes.push_back(revision);
        }
      }
      (void)mutate(parent, draft, selected);
      return draft;
    };
    // A completed stage is reusable after interruption in this same job. Match its
    // selected components and actual task IDs; reduce/review also match the supplied
    // intermediate records. No external version gate blocks recovery.
    auto call_stage = [&](const Json &request, const std::string &suffix, bool aggregate,
                          bool &reused) {
      const auto stem = std::to_string(node) + suffix;
      const auto request_path = root / "reflections" / (stem + ".request.json");
      const auto draft_path = root / "reflections" / (stem + ".draft.json");
      reused = false;
      if (fs::exists(request_path) && fs::exists(draft_path))
        try {
          const auto earlier = Json::parse(read(request_path, 262144))["input"];
          // Check the same final input shape that reflect() persisted. Comparing
          // bound controls with the pre-binding request needlessly repeats stages.
          const auto prepared = observed_operation_bindings(
              request, std::numeric_limits<std::size_t>::max());
          const auto same = [&](const char *field) {
            return earlier[field].dump() == prepared[field].dump();
          };
          Json earlier_ids = Json::Array{}, current_ids = Json::Array{};
          if (earlier["feedback"].array())
            for (const auto &row : earlier["feedback"].items())
              earlier_ids.push(row["task_id"]);
          if (request["feedback"].array())
            for (const auto &row : request["feedback"].items())
              current_ids.push(row["task_id"]);
          if (same("selected_components") && same("phase") &&
              earlier_ids.dump() == current_ids.dump() &&
              same("feedback") && same("case_outcomes") && same("controls") &&
              same("intermediate_proposals") && same("proposed_strategy")) {
            auto draft = Json::parse(read(draft_path, 262144));
            reused = true;
            return draft;
          }
        } catch (const std::exception &) {
          // A partial or corrupt checkpoint is safe to recompute from the cached report.
        }
      auto draft = reflect(request, node, suffix, aggregate);
      write(draft_path, draft.dump());
      return draft;
    };
    auto choose_stage = [&](const Json &request, const std::string &suffix,
                            const Json &records, Json &selection, bool &reused) {
      selection = call_stage(request, suffix, true, reused);
      try {
        return selected_draft(selection, records);
      } catch (const Error &error) {
        const auto code = std::string(error.what());
        if (code != "gepa_reflection_selection_invalid" && code != "gepa_draft_invalid")
          throw;
        auto repair = request;
        repair["phase"] = "pool_selection_repair";
        repair["selection_error"] = code;
        repair["rejected_selection"] = selection;
        if (reflection_input_bytes(repair) > input_limit)
          repair.fields().erase("rejected_selection");
        if (reflection_input_bytes(repair) > input_limit)
          repair.fields().erase("prior_search_rejections");
        if (reflection_input_bytes(repair) > input_limit)
          repair.fields().erase("training_batch_summary");
        require(reflection_input_bytes(repair) <= input_limit,
                "gepa_feedback_budget_too_small");
        bool repair_reused = false;
        selection = call_stage(repair, suffix + "-selection-repair", true, repair_reused);
        auto corrected = selected_draft(selection, records);
        stages.push(Json{{"phase", "pool_selection_repair"},
                         {"file", std::to_string(node) + suffix + "-selection-repair"},
                         {"error", code}, {"reused", repair_reused}});
        reflection_node["reflection_stages"] = stages;
        save();
        return corrected;
      }
    };
    const auto repairable_map_error = [](const std::string &code) {
      return code == "control_binding_invalid" || code == "control_argument_not_authorized" ||
             code == "gepa_draft_invalid" ||
             code == "unknown_field" || code == "gepa_component_not_authorized" ||
             code == "gepa_candidate_not_authorized" || code == "control_duplicate_loop_scope" ||
             code == "gepa_trigger_only_update_invalid" ||
             code == "runtime_action_rule_invalid";
    };
    for (std::size_t i = 0; i < groups.size(); ++i) {
      auto &group = groups[i];
      const auto suffix = ".pool-map-" + std::to_string(i);
      const auto ids = group["pool_group_task_ids_audit"];
      group.fields().erase("pool_group_task_ids_audit");
      bool reused = false;
      bool repaired = false;
      Json draft, record;
      try {
        draft = call_stage(group, suffix, false, reused);
        record = proposal_record(draft, ids, "map-" + std::to_string(i),
                                 reflection_task_facts(group["feedback"], selected));
      } catch (const Error &error) {
        const auto code = std::string(error.what());
        if (!repairable_map_error(code))
          throw;
        auto repair = group;
        repair["phase"] = "pool_map_repair";
        repair["error"] = code;
        if (code == "control_duplicate_loop_scope")
          repair["loop_scope_conflict"] =
              loop_scope_repair_context(before, parent["policy"]["snapshot"]);
        if (code == "runtime_action_rule_invalid")
          repair["action_rule_conflict"] =
              action_rule_repair_context(before, parent["policy"]["snapshot"]);
        if (draft.object())
          repair["failed_draft"] = draft;
        if (code == "control_binding_invalid") {
          const auto &diagnostic =
              tree["reflection_calls"][std::to_string(tree.num("model_calls", 0))]
                  ["control_binding_error"];
          if (diagnostic.object())
            repair["binding_error"] = diagnostic;
        }
        if (reflection_input_bytes(repair) > input_limit)
          repair.fields().erase("failed_draft");
        if (reflection_input_bytes(repair) > input_limit)
          repair.fields().erase("prior_search_rejections");
        if (reflection_input_bytes(repair) > input_limit)
          repair.fields().erase("training_batch_summary");
        require(reflection_input_bytes(repair) <= input_limit,
                "gepa_feedback_budget_too_small");
        bool repair_reused = false;
        draft = call_stage(repair, suffix + "-repair", false, repair_reused);
        std::string rejected_error;
        try {
          record = proposal_record(draft, ids, "map-" + std::to_string(i),
                                   reflection_task_facts(group["feedback"], selected));
        } catch (const Error &rejected) {
          rejected_error = rejected.what();
          if (!repairable_map_error(rejected_error))
            throw;
          // A rejected proposal is still a real diagnosis of these task facts.
          // Keep its provenance, but never offer unauthorized component values
          // to reduce. Continue the pool after this one bounded repair.
          record = Json{{"id", "map-" + std::to_string(i)}, {"valid", false},
                        {"error", rejected_error}, {"initial_error", code},
                        {"reason", diagnostic_excerpt(draft.str("reason"), 768)},
                        {"task_ids", ids},
                        {"task_facts", reflection_task_facts(group["feedback"], selected)},
                        {"candidate_values", Json::Object{}}};
        }
        repaired = true;
        stages.push(Json{{"phase", "pool_map_repair"},
                         {"file", std::to_string(node) + suffix + "-repair"},
                         {"error", code}, {"task_ids", ids},
                         {"reused", repair_reused},
                         {"valid", rejected_error.empty()},
                         {"rejected_error", rejected_error}});
      }
      pending = draft;
      intermediates.push(std::move(record));
      stages.push(Json{{"phase", group.str("phase", "pool_diagnostic_map")},
                       {"file", std::to_string(node) + suffix},
                       {"task_ids", ids},
                       {"feedback_records", group["feedback"].items().size()},
                       {"reused", reused},
                       {"repaired", repaired},
                       {"input_bytes", observed_operation_bindings(
                                           group, std::numeric_limits<std::size_t>::max())
                                           .dump().size()}});
      reflection_node["reflection_stages"] = stages;
      reflection_node["reflection_pool_task_count"] = pool_size;
      std::set<std::string> completed;
      for (const auto &stage : stages.items())
        if (stage.str("phase") == "pool_diagnostic_map")
          for (const auto &id : stage["task_ids"].items())
            completed.insert(id.text());
      reflection_node["reflection_pool_covered_task_count"] = completed.size();
      save();
    }

    // Hierarchical reduce keeps every map conclusion if 32 long cases produce too many
    // intermediate proposals for a single request. Each internal record retains its task IDs.
    std::size_t level = 0;
    Json reduced, final_choices;
    while (true) {
      if (full_pool_records_abstain(intermediates)) {
        // A completed pool whose valid maps/reductions all chose no change is a
        // legitimate abstention, not a failed hierarchy or a fresh Agent trial.
        reduced = Json{{"reason", "Complete training-pool proposals chose no executable change."},
                       {"experiences", Json::Array{}}};
        pending = reduced;
        stages.push(Json{{"phase", "pool_diagnostic_abstention"},
                         {"task_count", pool_size}, {"model_called", false}});
        reflection_node["reflection_stages"] = stages;
        save();
        return reduced;
      }
      auto reduction = input;
      reduction.fields().erase("feedback");
      reduction["phase"] = "pool_diagnostic_reduce";
      reduction["pool_task_count"] = pool_size;
      reduction["intermediate_proposals"] = intermediates;
      reduction = compact_full_pool_reduction(std::move(reduction));
      reduction["support_interpretation"] =
          "actual_support counts changed boundaries on the same parent training trajectories, "
          "not child efficacy. Prefer broader real support when necessary work is preserved; "
          "task_ids alone describe map coverage, not trigger coverage. This is soft evidence, "
          "not a minimum-task gate; sparse candidates remain eligible.";
      std::stable_sort(reduction["intermediate_proposals"].items().begin(),
                       reduction["intermediate_proposals"].items().end(),
                       [](const auto &a, const auto &b) {
        const auto ac = a["actual_support"].num("different_tasks", 0);
        const auto bc = b["actual_support"].num("different_tasks", 0);
        return ac != bc ? ac > bc : a["actual_support"].num("boundary_changes", 0) >
                                   b["actual_support"].num("boundary_changes", 0);
      });
      if (reflection_input_bytes(reduction) <= input_limit) {
        const auto suffix = ".pool-reduce-" + std::to_string(level);
        bool reused = false;
        reduced = choose_stage(reduction, suffix, intermediates, final_choices, reused);
        pending = reduced;
        stages.push(Json{{"phase", "pool_diagnostic_reduce"},
                         {"file", std::to_string(node) + suffix},
                         {"task_count", pool_size},
                         {"reused", reused},
                         {"input_bytes", observed_operation_bindings(
                                             reduction, std::numeric_limits<std::size_t>::max())
                                             .dump().size()}});
        reflection_node["reflection_stages"] = stages;
        save();
        break;
      }
      Json next = Json::Array{};
      std::size_t cursor = 0, part = 0;
      while (cursor < intermediates.items().size()) {
        auto chunk = reduction;
        chunk["intermediate_proposals"] = Json::Array{};
        while (cursor < intermediates.items().size()) {
          auto attempt = chunk;
          attempt["intermediate_proposals"].push(intermediates.items()[cursor]);
          if (reflection_input_bytes(attempt) > input_limit)
            break;
          chunk = std::move(attempt);
          ++cursor;
        }
        require(!chunk["intermediate_proposals"].items().empty(),
                "gepa_reflection_reduce_budget_too_small");
        if (chunk["intermediate_proposals"].items().size() == 1) {
          next.push(chunk["intermediate_proposals"].items().front());
          continue;
        }
        chunk["phase"] = "pool_diagnostic_reduce_part";
        const auto suffix = ".pool-reduce-" + std::to_string(level) + "-" +
                            std::to_string(part++);
        bool reused = false;
        Json selection;
        const auto draft = choose_stage(chunk, suffix, chunk["intermediate_proposals"],
                                        selection, reused);
        pending = draft;
        Json ids = Json::Array{};
        Json facts = Json::Array{};
        std::set<std::string> unique;
        for (const auto &item : chunk["intermediate_proposals"].items())
          for (const auto &id : item["task_ids"].items())
            if (unique.insert(id.text()).second)
              ids.push(id);
        for (const auto &item : chunk["intermediate_proposals"].items())
          for (const auto &fact : item["task_facts"].items())
            facts.push(fact);
        next.push(proposal_record(draft, ids, "reduce-" + std::to_string(level) + "-" +
                                              std::to_string(part - 1), facts));
        stages.push(Json{{"phase", "pool_diagnostic_reduce_part"},
                         {"file", std::to_string(node) + suffix},
                         {"task_ids", ids},
                         {"reused", reused},
                         {"input_bytes", observed_operation_bindings(
                                             chunk, std::numeric_limits<std::size_t>::max())
                                             .dump().size()}});
        reflection_node["reflection_stages"] = stages;
        save();
      }
      require(next.items().size() < intermediates.items().size(),
              "gepa_reflection_reduce_no_progress");
      intermediates = std::move(next);
      ++level;
    }

    // The final review sees all task outcomes plus a compact real success/failure pair.
    // A score table alone is not sufficient to recognize a missing requested goal.
    auto review = input;
    review["phase"] = "pool_counterexample_review";
    review["pool_task_count"] = pool_size;
    review["proposed_strategy"] = reduced;
    review["proposed_selection"] = final_choices["selected_proposals"];
    review["intermediate_proposals"] = intermediates;
    review["case_outcomes"] = Json::Array{};
    review["feedback"] = Json::Array{};
    Json all_feedback = Json::Array{};
    const auto reviewed_candidate = mutate(parent, reduced, selected);
    Json candidate_support = Json::Object{};
    const auto reviewed_components = components(reviewed_candidate);
    if (reviewed_components["action_rules"].dump() != before["action_rules"].dump() ||
        reviewed_components["loop_rules"].dump() != before["loop_rules"].dump() ||
        reviewed_components["schedule_rules"].dump() != before["schedule_rules"].dump())
      candidate_support = support_for(reviewed_candidate);
    review["actual_support"] = candidate_support;
    std::set<std::string> outcome_tasks;
    for (const auto &group : groups)
      for (const auto &row : group["feedback"].items()) {
        const auto &trial = row["trial"];
        Json outcome{{"task_id", row["task_id"]},
                     {"verdict", trial["verdict"]},
                     {"execution_status", trial["execution_status"]},
                     {"metrics", trial["metrics"]}};
        for (const auto *field : {"quality", "strict_pass_rate", "strict_passed", "invalid_loops",
                                 "invalid_loop_count", "metric_coverage"})
          if (trial["metrics"].has(field))
            outcome[field] = trial["metrics"][field];
        if (trial["observed_tool_failures"].object())
          outcome["observed_tool_failures"] = trial["observed_tool_failures"];
        if (trial["goal_completion"].object()) {
          outcome["goal_completion"] = trial["goal_completion"];
          if (outcome["goal_completion"]["issues"].array()) {
            auto &issues = outcome["goal_completion"]["issues"].items();
            if (issues.size() > 2)
              issues.resize(2);
            for (auto &issue : issues)
              issue["reason"] = diagnostic_excerpt(issue.str("reason"), 256);
          }
        }
        if (row["selected_experience_applications"].object())
          outcome["selected_experience_applications"] =
              row["selected_experience_applications"];
        if (outcome_tasks.insert(row.str("task_id")).second)
          review["case_outcomes"].push(std::move(outcome));
        all_feedback.push(row);
      }
    const auto pair = select_full_pool_review_cases(all_feedback, candidate_support);
    const auto failed_case = pair["failure"], successful_case = pair["success"];
    for (auto example : {failed_case, successful_case})
      if (example.object()) {
        if (example["trial"]["evidence"]["runs"].array())
          example = compact_full_pool_review_case(std::move(example), selected, candidate_support);
        review["feedback"].push(std::move(example));
      }
    review["counterexample_pair"] = Json{{"failure_present", failed_case.object()},
                                          {"success_present", successful_case.object()},
                                          {"selection_basis", "actual_support_and_same_actor_keep_work"}};
    review = compact_full_pool_review(std::move(review), final_choices, input_limit);
    const auto suffix = ".pool-counterexample-review";
    bool reused = false;
    Json reviewed_choices;
    const auto final_draft = choose_stage(review, suffix, review["intermediate_proposals"],
                                          reviewed_choices, reused);
    pending = final_draft;
    (void)mutate(parent, final_draft, selected);
    Json review_ids = Json::Array{};
    for (const auto &row : review["feedback"].items())
      review_ids.push(row["task_id"]);
    stages.push(Json{{"phase", "pool_counterexample_review"},
                     {"file", std::to_string(node) + suffix},
                     {"task_count", pool_size},
                     {"case_outcomes", review["case_outcomes"].items().size()},
                     {"selected_proposals_visible",
                      review["intermediate_proposals"].items().size()},
                     {"feedback_task_ids", review_ids},
                     {"failure_example", failed_case.object()},
                     {"success_example", successful_case.object()},
                     {"reused", reused},
                     {"input_bytes", observed_operation_bindings(
                                         review, std::numeric_limits<std::size_t>::max())
                                         .dump().size()}});
    reflection_node["reflection_stages"] = stages;
    save();
    return final_draft;
    } catch (const std::exception &error) {
      throw FullPoolProposalIncomplete(error.what());
    }
  }

  // 计算能够关联到真实策略应用的验证收益，避免把随机差异全部归功于候选。
  bool attributed_validation_gain(std::size_t index) const {
    if (index == 0)
      return true;
    const auto &node = tree["nodes"].items().at(index);
    // 训练应用证据允许保留分支；选优时必须在实际改变行为的验证样本上有收益。
    // 未触发样本仍计入完整验证分数，但不能替新增规则证明效果。
    for (const auto parent : gepa::parents(node)) {
      if (parent >= tree["nodes"].items().size())
        continue;
      Json candidate = Json::Object{}, previous = Json::Object{};
      const auto &prior = tree["nodes"].items()[parent]["validation"];
      const auto &changes = node["validation_applied_changes"][std::to_string(parent)];
      if (!changes.object())
        continue;
      for (const auto &task : changes.fields()) {
        bool applied = false;
        for (const auto &boundary : task.second.fields())
          applied |= number(boundary.second, 0) > 0;
        if (applied && prior.has(task.first) && node["validation"].has(task.first)) {
          candidate[task.first] = node["validation"][task.first];
          previous[task.first] = prior[task.first];
        }
      }
      if (candidate.fields().empty() || !gepa::no_worse(candidate, previous))
        continue;
      // 此处只证明策略改动与局部收益存在因果联系。时延由完整验证和最终留出
      // 门禁检查，不能用少量命中题上的时延波动提前阻断独立复核。
      if (gepa::local_gain(candidate, previous, number(config["min_improvement"], 0.000001)))
        return true;
    }
    return false;
  }

  // 从满足条件的已评测节点中选择当前最优者；不把未完成或无效节点参与比较。
  std::size_t winner(std::size_t limit = SIZE_MAX) const {
    std::size_t best = 0;
    const auto &nodes = tree["nodes"].items();
    limit = std::min(limit, nodes.size());
    auto preferable = [&](const Json &candidate, const Json &current) {
      const auto &a = candidate["optimization"];
      const auto &b = current["optimization"];
      if (a.flag("feasible", false) != b.flag("feasible", false))
        return a.flag("feasible", false);
      if (a.flag("passed", false) != b.flag("passed", false))
        return a.flag("passed", false);
      const auto goal = number(a["score"]) - number(b["score"]);
      if (std::abs(goal) > 1e-12)
        return goal > 0;
      const auto robustness = number(a["robustness"]) - number(b["robustness"]);
      if (std::abs(robustness) > 1e-12)
        return robustness > 0;
      return improves(candidate["validation"], current["validation"]);
    };
    // 默认按验证效果选优；覆盖记录用于诊断，只有显式严格验收才要求全规则、全能力命中。
    const bool strict_coverage = gate.flag("require_level_coverage", false);
    for (std::size_t i = 1; i < limit; ++i)
      if (nodes[i].str("status") == "accepted" && attributed_validation_gain(i) &&
          (!strict_coverage || (nodes[i]["control_coverage"].flag("complete", false) &&
                                nodes[i]["control_coverage"].flag("level_complete", false))) &&
          preferable(nodes[i], nodes[best]))
        best = i;
    return best;
  }

  // 检查候选是否具备进入冻结留出比较的条件。留出用于复核已经观察到的收益，
  // 发布门槛只在留出完成后决定是否发布，不能在这里重复执行。
  bool final_holdout_eligible(std::size_t index) const {
    const auto &nodes = tree["nodes"].items();
    if (index >= nodes.size())
      return false;
    const auto &node = nodes[index];
    const bool attributed = node.str("status") == "accepted" && attributed_validation_gain(index);
    // exploration_eligible 已要求训练因果收益、完整验证和质量灾难保护；允许它进入
    // 留出正是为了区分真实泛化收益与 validation 波动，不能直接视为可发布。
    const bool explorable = node.flag("exploration_eligible", false);
    if (!attributed && !explorable)
      return false;
    return !gate.flag("require_level_coverage", false) ||
           (node["control_coverage"].flag("complete", false) &&
            node["control_coverage"].flag("level_complete", false));
  }

  // 留出候选只按冻结时的验证排序确定；后续留出得分只负责门禁，绝不能重新排序。
  // 从已验证节点中选最终候选，留出成绩不反流到生成或日常搜索选择。
  std::vector<std::size_t> final_holdout_candidates() const {
    const auto &nodes = tree["nodes"].items();
    auto preferable = [&](const Json &candidate, const Json &current) {
      const auto &a = candidate["optimization"];
      const auto &b = current["optimization"];
      if (a.flag("feasible", false) != b.flag("feasible", false))
        return a.flag("feasible", false);
      if (a.flag("passed", false) != b.flag("passed", false))
        return a.flag("passed", false);
      const auto goal = number(a["score"]) - number(b["score"]);
      if (std::abs(goal) > 1e-12)
        return goal > 0;
      const auto robustness = number(a["robustness"]) - number(b["robustness"]);
      if (std::abs(robustness) > 1e-12)
        return robustness > 0;
      return improves(candidate["validation"], current["validation"]);
    };
    std::vector<std::size_t> candidates;
    std::set<std::string> versions;
    for (std::size_t index = 0; index < nodes.size(); ++index) {
      if (!final_holdout_eligible(index) || nodes[index].str("version").empty())
        continue;
      if (versions.insert(nodes[index].str("version")).second)
        candidates.push_back(index);
    }
    std::sort(candidates.begin(), candidates.end(), [&](const auto left, const auto right) {
      if (preferable(nodes[left], nodes[right]))
        return true;
      if (preferable(nodes[right], nodes[left]))
        return false;
      return left < right;
    });
    const auto requested = final_holdout_top_k_override ? final_holdout_top_k_override
                                                        : config.num("holdout_top_k", 1);
    if (candidates.size() > requested)
      candidates.resize(requested);
    return candidates;
  }

  // 创建并登记候选提案的身份、父节点和待测状态。
  void proposal(std::size_t index, const Json &candidate) {
    auto &node = tree["nodes"].items()[index];
    const auto version = agent_version_id(candidate.dump());
    node["version"] = version;
    node["status"] = "proposed";
    auto executable = [&](const Json &artifact) {
      auto values = components(artifact);
      for (const auto *category : {"action_rules", "loop_rules", "schedule_rules"})
        if (values[category].array())
          for (auto &rule : values[category].items())
            rule.fields().erase("reason");
      return values.dump();
    };
    const auto behavior = executable(candidate);
    for (std::size_t j = 0; j < index; ++j) {
      const auto &previous = tree["nodes"].items()[j];
      if (previous.str("version").empty())
        continue;
      // 组件经 Protobuf 读取时，省略字段与显式默认值等价，不能使既有导出包失效。
      if (previous.str("version") == version || executable(artifact(j)) == behavior) {
        node["duplicate_of"] = j;
        node["status"] = "duplicate";
        node["skip_reason"] = components(artifact(j)).dump() == components(candidate).dump()
                                  ? "no_behavior_change"
                                  : "no_control_predicate_change";
        break;
      }
    }
    write(root / "versions" / (std::to_string(index) + ".json"), candidate.dump());
    save();
  }

  // 将当前搜索失败记为受限诊断，保留节点历史但不把错误成绩纳入选优。
  void error(std::size_t index, const std::exception &exception) {
    const std::string code = exception.what();
    if (code == "gepa_cancelled" || code == "gepa_time_budget_exhausted" ||
        code == "gepa_model_rate_limited") {
      save();
      throw Error(code.c_str());
    }
    auto &node = tree["nodes"].items()[index];
    node["error_stage"] = node["status"];
    // 父批次未决属于观测未完成；控制绑定无法依据 typed evidence 修复时也只是
    // 当前组件不可触发，不能把一次模型格式/绑定问题升级成整轮搜索 error。
    node["status"] = code == "gepa_parent_evaluation_inconclusive"
                         ? "inconclusive"
                         : code == "control_binding_invalid" ? "untriggerable_controls" : "error";
    if (code == "control_binding_invalid")
      node["reason"] = "control_binding_unrepairable";
    // 只持久化固定错误码；模型/传输诊断可能包含密钥等敏感信息。
    const std::set<std::string> codes{"gepa_trial_budget_exhausted",
                                      "gepa_search_time_reserved",
                                      "gepa_parent_evaluation_inconclusive",
                                      "gepa_feedback_budget_too_small",
                                      "gepa_reflection_transport_failed",
                                      "gepa_reflection_incomplete",
                                      "gepa_reflection_reduce_no_progress",
                                      "gepa_reflection_reduce_budget_too_small",
                                      "gepa_reflection_component_binding_changed",
                                      "gepa_experience_not_authorized",
                                      "gepa_experience_limit",
                                      "gepa_draft_invalid",
                                      "gepa_candidate_not_authorized",
                                      "gepa_model_not_configured",
                                      "gepa_model_key_missing",
                                      "gepa_final_holdout_legacy_replay_required",
                                      "gepa_final_holdout_plan_changed",
                                      "gepa_holdout_baseline_changed",
                                      "credential_value_rejected",
                                      "sensitive_field_rejected",
                                      "invalid_json",
                                      "unknown_field",
                                      "control_operation_not_authorized",
                                      "control_argument_not_authorized",
                                      "control_loop_relaxes_limit",
                                      "control_source_not_approved",
                                      "control_base_loop_required",
                                      "control_loop_semantic_scope_required",
                                      "control_failure_replan_not_authorized",
                                      "control_draft_limits_invalid",
                                      "control_duplicate_loop_scope",
                                      "gepa_trigger_only_update_invalid",
                                      "runtime_action_rule_invalid",
                                      "control_binding_invalid",
                                      "control_repeat_operation_required",
                                      "control_loop_relaxes_condition",
                                      "control_loop_condition_invalid",
                                      "schedule_rule_cycle",
                                      "gepa_component_not_authorized",
                                      "gepa_components_changed",
                                      "gepa_model_budget_exhausted"};
    node["error"] = codes.count(code) ? code : "gepa_proposal_or_evaluation_failed";
    if (code == "gepa_trial_budget_exhausted" || code == "gepa_search_time_reserved")
      tree["stop_reason"] = code;
    if (code == "gepa_model_budget_exhausted")
      tree["reflection_exhausted"] = true;
    if (code == "gepa_model_not_configured" || code == "gepa_model_key_missing") {
      tree["status"] = "error";
      tree["active"] = nullptr;
      save();
      throw Error(code.c_str());
    }
    save();
  }

  // 候选先完成固定验证，再更新节点状态和比较指标；不要把训练改善直接当验证收益。
  void validate(std::size_t index) {
    try {
      auto &node = tree["nodes"].items()[index];
      const auto &screened = static_cast<const Json &>(node)["screening"];
      // 续跑旧 screened 节点也先核对真实差分子集，不用无改动题的随机收益消耗验证预算。
      // merge 使用多父验证筛选；缺少旧字段时不把未记录的证据推断成零。
      const auto screening_causal_changes = screened["causal_changes"].object()
                                          ? screened["causal_changes"]
                                          : component_applied_changes(node["components"],
                                                                      screened["applied_changes"]);
      if (node.str("kind") != "merge" && screened.object() &&
          screened.str("split", "train") == "train" && screened["parent"].object() &&
          screened["candidate"].object() &&
          !screening_quality_safe(screened["candidate"], screened["parent"],
                                  screening_causal_changes,
                                  screened.flag("causal_coverage_complete", false))) {
        node["status"] = "rejected_minibatch";
        node["reason"] = "pool_quality_regression";
        save();
        return;
      }
      if (node.str("kind") != "merge" && screened.object() &&
          screened.str("split", "train") == "train" && screened["parent"].object() &&
          screened["candidate"].object() && screened["applied_changes"].object() &&
          (screening_causal_changes.fields().empty() ||
           !screening_gain_for_validation(screened["candidate"], screened["parent"],
                                          screening_causal_changes))) {
        node["status"] = "rejected_minibatch";
        node["reason"] = screening_causal_changes.fields().empty()
                             ? "selected_component_not_applied"
                             : "no_observed_training_gain";
        save();
        return;
      }
      const auto candidate = artifact(index);
      // 未改变的规则继承任一父节点已建立的证据；改变后的规则拥有新的 rule_id，
      // 因而必须在当前节点重新观测。
      Json inherited{{"rules", Json::Object{}}};
      for (const auto parent : gepa::parents(node)) {
        const auto &parent_node = static_cast<const Json &>(tree)["nodes"].items()[parent];
        if (!parent_node["control_coverage"]["rules"].object())
          continue;
        for (const auto &rule : parent_node["control_coverage"]["rules"].fields()) {
          auto &counts = inherited["rules"][rule.first];
          for (const auto *field : {"queries", "triggered", "applied"})
            counts[field] = std::max(counts.num(field, 0), rule.second.num(field, 0));
        }
      }
      const auto &screening = static_cast<const Json &>(node)["screening"]["control_coverage"];
      const auto training = merge_control_coverage(
          screening.object() ? screening : Json{{"rules", Json::Object{}}}, inherited);
      const auto probe_size = std::min(val.size(), config.num("minibatch_size", 4));
      if (!control_changes_observed(training, inherited) && probe_size < val.size() &&
          config.flag("cache_evaluations", true)) {
        // 为训练阶段未命中的控制规则提供一次验证机会；若此处命中，完整验证可复用
        // 已缓存 trial，避免额外执行。
        const std::vector<std::string> probe(val.begin(), val.begin() + probe_size);
        const auto report = evaluate(candidate, probe, "val");
        const auto coverage = control_coverage(candidate, report);
        const auto values = scores(report);
        node["validation_probe"] = Json{{"scores", values}, {"control_coverage", coverage}};
        const auto combined = merge_control_coverage(coverage, training);
        const bool any_new_control_applied = control_changes_observed(combined, inherited);
        for (const auto &rule : coverage["rules"].fields()) {
          const auto &previous = training["rules"][rule.first];
          if (!any_new_control_applied && !inherited["rules"].has(rule.first) &&
              !values.fields().empty() &&
              previous.num("queries", 0) && !previous.num("applied", 0) &&
              rule.second.num("queries", 0) && !rule.second.num("applied", 0)) {
            // 即使另一条规则被淘汰，已执行的训练证据也不能丢失。
            node["control_coverage"] = combined;
            node["status"] = "unobserved_controls";
            node["reason"] = "control_unobserved_in_train_and_validation";
            save();
            return;
          }
        }
        save();
      }
      const auto report = evaluate(candidate, val, "val");
      auto values = scores(report);
      node["validation"] = values;
      node["validation_applied_changes"] = Json::Object{};
      for (const auto parent : gepa::parents(node)) {
        const auto changes = applied_changes(artifact(parent), report);
        node["validation_applied_changes"][std::to_string(parent)] =
            component_applied_changes(node["components"], changes);
      }
      if (!values.fields().empty())
        node["optimization"] = annotate_validation(node["validation"]);
      node["validation_control_coverage"] = control_coverage(candidate, report);
      node["control_coverage"] =
          merge_control_coverage(node["validation_control_coverage"], training);
      node["validation_control_observation"] =
          new_control_observation(node["control_coverage"], inherited);
      const auto &screening_changes = node["screening"]["causal_changes"].object()
                                          ? node["screening"]["causal_changes"]
                                          : node["screening"]["applied_changes"];
      bool changed = screening_changes.object() && !screening_changes.fields().empty();
      for (const auto &parent : node["validation_applied_changes"].fields())
        changed |= !parent.second.fields().empty();
      const bool controls_observed =
          node["validation_control_observation"].flag("observed", false);
      const bool attributed = !values.fields().empty() && controls_observed && changed &&
                              attributed_validation_gain(index);
      node["validation_gain_attributed"] = attributed;
      node.fields().erase("reason");
      node["status"] = values.fields().empty() ? "inconclusive"
                       : !controls_observed    ? "unobserved_controls"
                       : !changed              ? "unobserved_changes"
                       : attributed            ? "accepted"
                                               : "unobserved_changes";
      if (node.str("status") == "unobserved_changes")
        node["reason"] = changed ? "no_attributed_validation_gain"
                                   : "no_observed_decision_change";
      annotate_exploration_eligibility(index);
      save();
    } catch (const std::exception &exception) {
      error(index, exception);
    }
  }
  // 升级旧检查点的验证归因状态。旧版先按“行为发生变化”接纳节点，随后虽然
  // 计算了 validation_gain_attributed，却没有用它否决接纳，导致无收益节点
  // 继续进入前沿并成为父节点。这里只重算已有派生字段，不执行 Agent、不调用
  // 模型，也不改写原始报告；历史子节点通过 attribution_previous_status 保持可回放。
  bool refresh_validation_attribution() {
    if (tree.num("validation_attribution_revision", 0) >= 1)
      return false;
    for (std::size_t index = 1; index < tree["nodes"].items().size(); ++index) {
      auto &node = tree["nodes"].items()[index];
      if (node.str("status") != "accepted" || !node["validation"].object())
        continue;
      const bool attributed = attributed_validation_gain(index);
      node["validation_gain_attributed"] = attributed;
      if (!attributed) {
        node["attribution_previous_status"] = "accepted";
        node["status"] = "unobserved_changes";
        node["reason"] = "no_attributed_validation_gain";
      }
    }
    tree["validation_attribution_revision"] = 1;
    for (std::size_t index = 0; index < tree["nodes"].items().size(); ++index)
      annotate_exploration_eligibility(index);
    persist_frontiers();
    return true;
  }

  // patience 只统计已经形成候选且得到真实训练筛选或验证成绩的尝试。
  // 预检拒绝、重复提案和其他未执行候选不能被解释为“评测后无改进”。
  bool assessed_for_patience(const Json &node) const {
    const auto status = node.str("status");
    const bool assessed_terminal =
        status == "accepted" || status == "rejected_minibatch" ||
        status == "rejected_merge" || status == "unobserved_changes" ||
        status == "unobserved_controls" || status == "not_selected";
    if (!assessed_terminal)
      return false;
    const auto &screening = node["screening"]["candidate"];
    const auto &validation = node["validation"];
    return (screening.object() && !screening.fields().empty()) ||
           (validation.object() && !validation.fields().empty());
  }

  // 旧检查点曾把 duplicate/untriggerable 等未执行候选计入 stagnation。
  // 按节点创建顺序重放 frontier/winner 更新；同一 reflection batch 最多计一次，merge
  // 则按单节点计。当前尚未 finish_batch 的批次不参与，以保持中断恢复语义。
  bool refresh_patience_accounting() {
    if (tree.num("patience_accounting_revision", 0) >= 2)
      return false;
    const auto &nodes = static_cast<const Json &>(tree)["nodes"].items();
    Json prefix = Json::Array{};
    prefix.push(nodes.front());
    const auto frontier_ids = [&](const Json &values) {
      return gepa::pool(gepa::exploration_frontier_map(values, val, config));
    };
    auto previous_best = winner(1);
    auto previous_frontier = frontier_ids(prefix);
    std::size_t stagnation = 0;
    const bool pending_batch = tree["batch"].object() && tree["batch"].has("id");
    const auto pending_batch_id = pending_batch ? tree["batch"]["id"].dump() : std::string();
    for (std::size_t begin = 1; begin < nodes.size();) {
      const auto &first = nodes[begin];
      const bool grouped = first.str("kind") == "reflection" && first.has("batch");
      const auto batch_id = grouped ? first["batch"].dump() : std::string();
      if (grouped && pending_batch && batch_id == pending_batch_id)
        break;
      auto end = begin + 1;
      if (grouped)
        while (end < nodes.size() && nodes[end].str("kind") == "reflection" &&
               nodes[end].has("batch") && nodes[end]["batch"].dump() == batch_id)
          ++end;
      bool assessed = false;
      for (auto index = begin; index < end; ++index) {
        prefix.push(nodes[index]);
        assessed |= assessed_for_patience(nodes[index]);
      }
      const auto current_best = winner(end);
      const auto current_frontier = frontier_ids(prefix);
      if (current_best != previous_best || current_frontier != previous_frontier)
        stagnation = 0;
      else if (assessed)
        ++stagnation;
      previous_best = current_best;
      previous_frontier = current_frontier;
      begin = end;
    }
    tree["stagnation"] = stagnation;
    tree["patience_accounting_revision"] = 2;
    if (tree.str("stop_reason") == "patience" &&
        (!config.num("patience", 0) || stagnation < config.num("patience", 0)))
      tree.fields().erase("stop_reason");
    return true;
  }


  // 根据新节点的完整成绩更新前沿，保留可互补而非只看总分第一的父节点。
  void update_frontier(std::size_t previous_best, const std::string &previous_frontier,
                       bool assessed = true) {
    Json ids = Json::Array{};
    for (auto i : frontier())
      ids.push(i);
    // 评测未知或执行异常不是“策略没有收益”；不增加、也不清零已有停滞计数。
    // 总轮数、调用数、题数和时间预算仍约束这些尝试，不能因此无限重试。
    if (winner() != previous_best || ids.dump() != previous_frontier)
      tree["stagnation"] = 0;
    else if (assessed)
      tree["stagnation"] = tree.num("stagnation", 0) + 1;
    tree["frontier"] = ids;
    tree["frontier_mapping"] = gepa::exploration_frontier_map(tree["nodes"], val, config);
    tree["publication_frontier_mapping"] = gepa::frontier_map(tree["nodes"], val, config);
    tree["publication_frontier"] = Json::Array{};
    for (const auto id : gepa::pool(tree["publication_frontier_mapping"]))
      tree["publication_frontier"].push(id);
    save();
  }

  // 提交当前训练批的节点结果和预算消耗，失败节点也保留明确状态。
  void finish_batch() {
    auto &batch = tree["batch"];
    if (!batch.has("selected")) {
      batch["selected"] = Json::Array{};
      const auto selected = gepa::select_proposals(tree["nodes"], batch["nodes"], config);
      std::set<std::size_t> kept(selected.begin(), selected.end());
      for (auto index : selected)
        batch["selected"].push(index);
      for (const auto &id : batch["nodes"].items()) {
        auto &node = tree["nodes"].items()[id.integer()];
        if (node.str("status") == "screened" && !kept.count(id.integer()))
          node["status"] = "not_selected";
      }
      save();
    }
    for (const auto &id : batch["selected"].items()) {
      auto &node = tree["nodes"].items()[id.integer()];
      if (node.str("status") != "screened")
        continue;
      if (tree.str("stop_reason") == "gepa_trial_budget_exhausted" ||
          tree.str("stop_reason") == "gepa_search_time_reserved") {
        node["status"] = "skipped_budget";
        continue;
      }
      validate(id.integer());
      if (gepa::exploration_parent(node))
        tree["merge_due"] = true;
    }
    const auto previous = batch.num("previous_best", 0);
    const auto old = batch.str("previous_frontier");
    const auto assessed = std::any_of(batch["nodes"].items().begin(),
                                      batch["nodes"].items().end(), [&](const auto &id) {
      return assessed_for_patience(tree["nodes"].items()[id.integer()]);
    });
    tree["batch"] = nullptr;
    update_frontier(previous, old, assessed);
  }

  // 将多个来源的组件融合为新节点，记录所有父节点并重新评测融合结果。
  std::vector<std::string> merge_batch(std::size_t first, std::size_t second) const {
    std::vector<std::string> buckets[3], result;
    const auto &a = tree["nodes"].items()[first]["validation"];
    const auto &b = tree["nodes"].items()[second]["validation"];
    for (const auto &task : val) {
      require(a.has(task) && b.has(task), "gepa_merge_validation_missing");
      const auto difference = number(a[task]["fitness"]) - number(b[task]["fitness"]);
      buckets[difference > 1e-12 ? 0 : difference < -1e-12 ? 1 : 2].push_back(task);
    }
    std::mt19937 random(static_cast<unsigned>(config.num("seed", 42) + tree.num("rounds", 0)));
    for (auto &bucket : buckets)
      std::shuffle(bucket.begin(), bucket.end(), random);
    const auto count = std::min(val.size(), config.num("merge_minibatch_size", 4));
    while (result.size() < count)
      for (auto &bucket : buckets)
        if (!bucket.empty() && result.size() < count) {
          result.push_back(bucket.back());
          bucket.pop_back();
        }
    std::sort(result.begin(), result.end());
    return result;
  }

  // 从多个已有分支构造下一代；保留所有父节点来源，融合后仍需重新验证。
  void merge(std::size_t index) {
    auto &node = tree["nodes"].items()[index];
    const auto ids = gepa::parents(node);
    if (node.str("status") == "pending") {
      const auto a = artifact(ids[0]), b = artifact(ids[1]),
                 base = artifact(node.num("ancestor", 0));
      const auto combined =
          gepa::recombine(components(base), components(a), components(b), ids[0], ids[1],
                          !improves(tree["nodes"].items()[ids[1]]["validation"],
                                    tree["nodes"].items()[ids[0]]["validation"]));
      node["component_sources"] = combined["sources"];
      node["conflicts"] = combined["conflicts"];
      if (!combined.flag("useful", false)) {
        node["status"] = "skipped_merge";
        node["reason"] = "no_complementary_components";
        save();
        return;
      }
      proposal(index, compose(a, combined["components"]));
    }
    if (node.str("status") == "proposed") {
      std::vector<std::string> tasks;
      for (const auto &id : node["tasks"].items())
        tasks.push_back(id.text());
      const auto report = evaluate(artifact(index), tasks, "val");
      const auto values = scores(report);
      Json first = Json::Object{}, second = Json::Object{};
      for (const auto &id : tasks) {
        first[id] = tree["nodes"].items()[ids[0]]["validation"][id];
        second[id] = tree["nodes"].items()[ids[1]]["validation"][id];
      }
      node["screening"] =
          Json{{"parents", Json::Array{first, second}}, {"candidate", values}, {"split", "val"}};
      const auto changes_a = applied_changes(artifact(ids[0]), report),
                 changes_b = applied_changes(artifact(ids[1]), report);
      node["screening"]["parent_applied_changes"] = Json::Array{changes_a, changes_b};
      const auto retained = [&](const Json &parent, const Json &changes) {
        return gepa::no_worse(values, parent) || (config.str("acceptance") == "pareto" &&
                                                  observed_local_gain(values, parent, changes));
      };
      node["status"] = values.fields().empty() ? "inconclusive"
                       : retained(first, changes_a) && retained(second, changes_b)
                           ? "screened"
                           : "rejected_merge";
      save();
    }
    if (node.str("status") == "screened")
      validate(index);
  }

  // 旧执行只在策略、数据、裁判和选择题集合完全匹配时复用。
  // 种子和 L0 共用相同入口；换裁判须先重评原始轨迹，不复用旧分数。
  Report import_validation_report(const std::string &kind, const Json &candidate) const {
    const char *mismatch = kind == "seed" ? "gepa_seed_judge_mismatch"
                                           : "gepa_baseline_judge_mismatch";
    const char *incompatible = kind == "seed" ? "gepa_seed_report_incompatible"
                                               : "gepa_baseline_report_incompatible";
    const auto bytes = read(config.str(kind + "_report_file"), kMaxEvaluationReportBytes);
    const auto report = proto<Report>(Json::parse(bytes));
    const auto source = json(report.configuration());
    // 模型同名不代表评分口径相同。旧报告须先用正式重评/同配置恢复入口
    // 获得有效裁判指纹，不能把旧提示词下的分数直接与新候选比较。
    require(source.str("judge_fingerprint") ==
                task_evaluation_judge_fingerprint(evaluation_path, evaluation.dump()),
            mismatch);
    const auto &judge_settings = static_cast<const Json &>(evaluation)["judge"];
    auto judge_path = fs::path(judge_settings.str("runtime_config"));
    if (judge_path.is_relative())
      judge_path = evaluation_path.parent_path() / judge_path;
    const auto judge = legacy::load_runtime_configuration(judge_path);
    require(report.complete() && report.dataset_version() == dataset.dataset_version() &&
                report.evaluator_version() ==
                    (judge_settings.str("assessment_mode", "legacy") == "objectives"
                         ? "task-evaluator-v2-goals"
                         : "task-evaluator-v1") &&
                source["variants"].array() && source["variants"].items().size() == 1 &&
                source["variants"].items().front().str("version") ==
                    agent_version_id(candidate.dump()) &&
                source["splits"].array() && source["splits"].items().size() == 1 &&
                source["splits"].items().front().text() == "val" &&
                source.str("judge_model") == judge.model.model_id &&
                source.flag("model_enabled", false) ==
                    judge_settings.flag("enabled", false),
            incompatible);
    std::set<std::string> observed;
    for (const auto &trial : report.trials())
      require(trial.variant_id() == "candidate" && trial.split() == "val" &&
                  trial.repetition() == 1 && observed.insert(trial.task_id()).second,
              incompatible);
    const auto values = scores(report);
    require(observed.size() == val.size() && values.fields().size() == val.size(),
            incompatible);
    for (const auto &id : val)
      require(observed.count(id) && values.has(id), incompatible);
    return report;
  }

  // 执行当前对象的主工作流程，按阶段检查预算、取消和结果状态。
  GepaResult run() {
    dataset = read_evaluation_dataset(dataset_path);
    for (const auto &issue : dataset.issues())
      require(issue.severity() != "error", "gepa_dataset_invalid");
    for (const auto &task : dataset.tasks()) {
      require(task.review_status() == "reviewed", "gepa_dataset_review_required");
      if (task.split() == "train")
        train.push_back(task.task_id());
      else if (task.split() == "val")
        val.push_back(task.task_id());
      else if (task.split() == "holdout")
        holdout.push_back(task.task_id());
    }
    require(!train.empty() && !val.empty() && !holdout.empty(), "gepa_three_splits_required");
    require(!source_experiences(seed).items().empty(), "gepa_approved_experiences_required");
    const auto finalist_budget_count = final_holdout_top_k_override
                                           ? final_holdout_top_k_override
                                           : config.num("holdout_top_k", 1);
    const auto holdout_trials = holdout.size() * evaluation.num("repetitions", 1);
    // 先为所有最终候选及可能需要的 L0 对照预留 trial，搜索阶段不能透支这部分预算。
    reserve = holdout_trials * (config.flag("cache_evaluations", true) ? finalist_budget_count + 1
                                                                       : finalist_budget_count * 2);
    require(config.num("max_trials", 400) >=
                reserve + val.size() * evaluation.num("repetitions", 1) * 2,
            "gepa_holdout_budget_required");
    if (fs::exists(root / "tree.json")) {
      tree = Json::parse(read(root / "tree.json", 16777216));
      // Recovery is checkpoint based.  Runtime/config/version fingerprints are
      // diagnostic metadata only; an interrupted job must continue after a
      // harmless binary, prompt, or host revision.  The tree shape, node
      // identity and trial identities below remain the safety boundary.
      require(tree.num("schema_version", 0) == 1 || tree.num("schema_version", 0) == 2,
              "gepa_tree_invalid");
      require(tree["nodes"].array() && !tree["nodes"].items().empty() &&
                  tree["nodes"].items().size() <= config.num("max_rounds", 8) + 1,
              "gepa_tree_invalid");
      gepa::validate_nodes(tree["nodes"]);
      require(tree.num("rounds", SIZE_MAX) == tree["nodes"].items().size() - 1 &&
                  tree.num("model_calls", SIZE_MAX) <= config.num("max_model_calls", 8) &&
                  tree.num("trials_used", SIZE_MAX) <= config.num("max_trials", 400),
              "gepa_tree_invalid");
      if (!tree["active"].null())
        require(tree["active"].integer() > 0 &&
                    tree["active"].integer() < tree["nodes"].items().size(),
                "gepa_tree_invalid");
      if (tree.has("batch") && !tree["batch"].null()) {
        const auto &batch = tree["batch"];
        std::set<std::size_t> ids;
        require(batch.object() && batch["nodes"].array() && batch.num("limit", 0) > 0 &&
                    batch.num("limit", 0) <= 8 &&
                    batch["nodes"].items().size() <= batch.num("limit", 0),
                "gepa_tree_invalid");
        for (const auto &id : batch["nodes"].items())
          require(id.integer() > 0 && id.integer() < tree["nodes"].items().size() &&
                      ids.insert(id.integer()).second &&
                      tree["nodes"].items()[id.integer()].str("kind") == "reflection",
                  "gepa_tree_invalid");
        if (batch.has("plans")) {
          require(batch["plans"].array() && batch["plans"].items().size() == batch.num("limit", 0),
                  "gepa_tree_invalid");
          for (const auto &plan : batch["plans"].items()) {
            require(plan.num("parent", SIZE_MAX) < tree["nodes"].items().size() &&
                        gepa::exploration_parent(
                            tree["nodes"].items()[plan.num("parent", 0)]) &&
                        plan["tasks"].array() && !plan["tasks"].items().empty() &&
                        plan["components"].array(),
                    "gepa_tree_invalid");
            for (const auto &task : plan["tasks"].items())
              require(std::find(train.begin(), train.end(), task.text()) != train.end(),
                      "gepa_tree_invalid");
          }
        }
        if (batch.has("selected"))
          for (const auto &id : batch["selected"].items())
            require(ids.count(id.integer()), "gepa_tree_invalid");
      }
      for (auto &node : tree["nodes"].items())
        if (node.str("status") == "reflecting") {
          node["status"] = "interrupted";
          node["reason"] = "reflection_outcome_unknown";
          tree["active"] = nullptr;
        }
      resume_clock(now());
      save();
    } else {
      tree = Json{{"schema_version", 2},
                  {"change_evidence_revision", 2},
                  {"algorithm", "gepa-reflective-dag-v4-constrained-portfolio"},
                  {"context", context},
                  {"evaluation_context", evaluation_context},
                  {"candidate_evaluation_context", candidate_evaluation_context},
                  {"seed_version", agent_version_id(seed.dump())},
                  {"baseline_version", agent_version_id(baseline.dump())},
                  {"dataset_version", dataset.dataset_version()},
                  {"started_ms", now()},
                  {"rounds", 0},
                  {"model_calls", 0},
                  {"trials_used", 0},
                  {"component_cursor", 0},
                  {"active", nullptr},
                  {"evaluations", Json::Object{}},
                  {"nodes", Json::Array{Json{{"id", 0},
                                             {"parent", nullptr},
                                             {"parents", Json::Array{}},
                                             {"kind", "seed"},
                                             {"depth", 0},
                                             {"version", agent_version_id(seed.dump())},
                                             {"status", "seed"},
                                             {"selections", 0}}}}};
      write(root / "versions/0.json", seed.dump());
      save();
    }
    if (!tree.has("merge_attempts"))
      tree["merge_attempts"] = Json::Array{};
    if (!tree.has("batch"))
      tree["batch"] = nullptr;
    if (!tree.has("component_cursor"))
      tree["component_cursor"] = 0;
    if (!tree["component_selections"].object())
      tree["component_selections"] = Json::Object{};
    if (!tree["training_opportunities"].object())
      tree["training_opportunities"] = Json::Object{};
    tree["schema_version"] = 2;
    tree["algorithm"] = "gepa-reflective-dag-v4-constrained-portfolio";
    tree["optimization_contract"] = gate;
    tree["available_components"] = Json::Array{};
    const auto available = components(seed);
    for (const auto &item : available.fields())
      tree["available_components"].push(item.first);
    tree["search_warnings"] = Json::Array{};
    tree["fixed_controls"] = Json::Array{};
    if (proto<v1::RuntimePolicy>(seed["policy"]).options().requested_level() >= v1::L2)
      tree["fixed_controls"].push("observed_tool_required_arguments");
    // L0 baseline 必须先于 seed 的 validation 诊断生成；它是所有后续
    // pool/validation/holdout 比较的固定参考，不能由 seed 评测顺序隐式替代。
    if (!tree.has("baseline_validation")) {
      Json task_ids = Json::Array{};
      for (const auto &id : val)
        task_ids.push(id);
      const Json identity{{"schema_version", 1},
                          {"evaluation_context", evaluation_context},
                          {"candidate_evaluation_context", candidate_evaluation_context},
                          {"deterministic_contracts", "schedule-success-v1"},
                          {"baseline_version", agent_version_id(baseline.dump())},
                          {"dataset_version", dataset.dataset_version()},
                          {"task_ids", task_ids},
                          {"objective", config["objective"]},
                          {"unknown_token_policy", config.str("unknown_token_policy", "reject")},
                          {"gate", gate}};
      const auto key = hash(identity.dump());
      const auto cache_path = root.parent_path().parent_path() / "baseline-cache" / (key + ".json");
      Json values;
      bool hit = false;
      bool imported = false;
      if (config.has("baseline_report_file")) {
        values = scores(import_validation_report("baseline", baseline));
        hit = imported = true;
      }
      if (!imported && fs::exists(cache_path))
        try {
          const auto cached = Json::parse(read(cache_path, 1048576));
          const auto values_bytes = cached.str("values_json");
          values = Json::parse(values_bytes);
          hit = cached["identity"].dump() == identity.dump() && values.object() &&
                values.fields().size() == val.size() &&
                cached.str("values_hash") == hash(values_bytes);
          for (const auto &id : val)
            hit &= values.has(id) && values[id].object() && values[id].has("fitness") &&
                   values[id].has("quality") && values[id].has("duration_ms");
        } catch (const std::exception &) {
          hit = false;
        }
      tree["baseline_cache"] = Json{{"key", key},
                                    {"status", imported ? "imported"
                                               : hit    ? "hit"
                                                        : "miss"}};
      if (!hit) {
        const auto report = evaluate(baseline, val, "val");
        values = scores(report);
        require(values.fields().size() == val.size(), "gepa_baseline_evaluation_inconclusive");
      }
      tree["baseline_validation"] = values;
      save();
      if (!hit)
        try {
          const auto values_bytes = values.dump();
          write(cache_path, Json{{"identity", identity},
                                 {"values_json", values_bytes},
                                 {"values_hash", hash(values_bytes)},
                                 {"created_ms", now()}}
                                .dump());
        } catch (const std::exception &) {
          tree["baseline_cache"]["status"] = "write_failed";
          save();
        }
    }
    if (config.flag("cache_evaluations", true)) {
      const auto pinned = tree["holdout_baseline_cache"];
      const bool locked = pinned.has("source");
      auto cached = holdout_baselines().lookup(pinned);
      if (locked && cached.str("status") != "hit")
        cached["source"] = pinned["source"];
      tree["holdout_baseline_cache"] = cached;
      if (cached.str("status") == "hit")
        reserve = holdout_trials * finalist_budget_count;
      save();
      require(!locked || cached.str("status") == "hit", "gepa_holdout_baseline_changed");
    }
    // L0 已落盘后先评测 seed；节点优化标注必须读取真实的同题验证成绩。
    // 恢复时只补缺失的 seed validation，不能重跑已完成的 L0。
    {
      auto &root_node = tree["nodes"].items()[0];
      if (!root_node["validation"].object() || root_node["validation"].fields().empty()) {
        const bool imported = config.has("seed_report_file");
        const auto report = imported ? import_validation_report("seed", seed)
                                     : evaluate(seed, val, "val");
        if (imported)
          root_node["validation_source"] = Json{{"status", "imported"},
              {"report_sha256", hash(read(config.str("seed_report_file"), kMaxEvaluationReportBytes))}};
        const auto values = scores(report);
        require(!values.fields().empty(), "gepa_seed_evaluation_inconclusive");
        root_node["validation"] = values;
        root_node["control_coverage"] = control_coverage(seed, report);
        save();
      }
    }
    if (tree["nodes"].items()[0].str("status") == "seed") {
      auto &root_node = tree["nodes"].items()[0];
      root_node["optimization"] = annotate_validation(root_node["validation"]);
      root_node["status"] = "accepted";
      save();
    } else if (!tree["nodes"].items()[0]["optimization"].object()) {
      tree["nodes"].items()[0]["optimization"] =
          annotate_validation(tree["nodes"].items()[0]["validation"]);
      save();
    }
    // 必须在任何父节点选择之前迁移；迁移只更新派生状态，因此即使存在已排定的
    // active/batch 也不会改变其父子关系，后续新节点则只从修正后的前沿产生。
    if (refresh_validation_attribution())
      save();
    if (refresh_exploration_eligibility()) {
      persist_frontiers();
      save();
    }
    if (refresh_patience_accounting())
      save();
    if (!tree.has("frontier") || tree["frontier"].null()) {
      persist_frontiers();
    }
    if (config.flag("use_merge", true) &&
        tree["merge_attempts"].items().size() < config.num("max_merge_invocations", 5) &&
        !gepa::select_merge(tree["nodes"], tree["frontier_mapping"], tree["merge_attempts"],
                            config, tree.num("rounds", 0))
             .null())
      tree["merge_due"] = true;
    recover_rejected_screenings();
    recover_inconclusive_screenings();
    tree["status"] = "searching";
    tree.fields().erase("error");
    tree.fields().erase("finished_ms");
    save();
    // batch 将父节点、组件和训练任务一起持久化；中断恢复时必须执行原计划，
    // 不能因前面并行候选的结果改变后续候选的父子关系。
    while (!final_holdout_replay && !tree.flag("holdout_started", false) &&
           (!tree["active"].null() || !tree["batch"].null() ||
            tree.num("rounds", 0) < config.num("max_rounds", 8))) {
      refresh_change_evidence();
      check();
      if (now() >= deadline() - holdout_time() ||
          tree.str("stop_reason") == "gepa_search_time_reserved") {
        tree["stop_reason"] = "gepa_search_time_reserved";
        if (!tree["active"].null())
          tree["nodes"].items()[tree["active"].integer()]["status"] = "skipped_budget";
        tree["active"] = nullptr;
        if (!tree["batch"].null())
          finish_batch();
        save();
        break;
      }
      if (tree["active"].null() && !tree["batch"].null() &&
          (tree["batch"].has("selected") ||
           tree["batch"]["nodes"].items().size() >= tree["batch"].num("limit", 1) ||
           tree.num("model_calls", 0) >= config.num("max_model_calls", 8) ||
           tree.flag("reflection_exhausted", false) ||
           tree.str("stop_reason") == "gepa_trial_budget_exhausted")) {
        finish_batch();
        if (tree.str("stop_reason") == "gepa_trial_budget_exhausted")
          break;
        continue;
      }
      if (tree["active"].null() && tree["batch"].null()) {
        if (config.has("target_fitness") &&
            mean(tree["nodes"].items()[winner()]["validation"], "quality") >= 1 &&
            mean(tree["nodes"].items()[winner()]["validation"], "fitness") >=
                number(config["target_fitness"], 1)) {
          tree["stop_reason"] = "target_fitness";
          break;
        }
        if (config.num("patience", 0) && tree.num("stagnation", 0) >= config.num("patience", 0)) {
          tree["stop_reason"] = "patience";
          break;
        }
        if (config.flag("use_merge", true) && tree.flag("merge_due", false) &&
            tree["merge_attempts"].items().size() < config.num("max_merge_invocations", 5)) {
          const auto plan =
              gepa::select_merge(tree["nodes"],
                                 gepa::exploration_frontier_map(tree["nodes"], val, config),
                                 tree["merge_attempts"], config, tree.num("rounds", 0));
          tree["merge_due"] = false;
          if (!plan.null()) {
            const auto a = plan.num("first", 0), b = plan.num("second", 0),
                       index = tree["nodes"].items().size();
            Json node{
                {"id", index},
                {"parent", a},
                {"parents", Json::Array{a, b}},
                {"ancestor", plan["ancestor"]},
                {"kind", "merge"},
                {"status", "pending"},
                {"selections", 0},
                {"previous_best", winner()},
                {"component_cursor", std::max(tree["nodes"].items()[a].num("component_cursor", 0),
                                              tree["nodes"].items()[b].num("component_cursor", 0))},
                {"previous_frontier", tree["frontier"].dump()},
                {"depth", std::max(tree["nodes"].items()[a].num("depth", 0),
                                   tree["nodes"].items()[b].num("depth", 0)) +
                              1},
                {"tasks", Json::Array{}}};
            for (const auto &id : merge_batch(a, b))
              node["tasks"].push(id);
            for (auto p : {a, b})
              tree["nodes"].items()[p]["selections"] =
                  tree["nodes"].items()[p].num("selections", 0) + 1;
            auto attempt = plan;
            attempt["node"] = index;
            tree["merge_attempts"].push(attempt);
            tree["nodes"].push(node);
            tree["active"] = index;
            tree["rounds"] = tree.num("rounds", 0) + 1;
            save();
          }
        }
        if (tree["active"].null()) {
          if (tree.flag("reflection_exhausted", false) ||
              tree.num("model_calls", 0) >= config.num("max_model_calls", 8)) {
            tree["stop_reason"] = "model_budget";
            break;
          }
          const auto sequence = tree.num("training_batches", tree.num("reflection_batches", 0));
          const auto sampling = config.str("sampling_strategy", "same_parent");
          const auto n = sampling == "single" ? 1 : config.num("proposals_per_round", 1);
          const auto total = n * (sampling == "pxn" ? config.num("parents_per_round", 2) : 1);
          const auto count =
              std::min({total, config.num("max_rounds", 8) - tree.num("rounds", 0),
                        config.num("max_model_calls", 8) - tree.num("model_calls", 0)});
          tree["batch"] = Json{{"id", tree.num("reflection_batches", 0)},
                               {"limit", count},
                               {"nodes", Json::Array{}},
                               {"plans", Json::Array{}},
                               {"previous_best", winner()},
                               {"previous_frontier", tree["frontier"].dump()}};
          std::size_t groups = 0;
          while (tree["batch"]["plans"].items().size() < count) {
            const auto p =
                gepa::select_parent(
                    tree["nodes"], gepa::exploration_frontier_map(tree["nodes"], val, config),
                    config, tree.num("rounds", 0) + groups);
            const auto cursor = tree.num("component_cursor", 0);
            Json plan{{"parent", p},
                      {"tasks", Json::Array{}},
                      {"components", gepa::select_components(
                                         components(artifact(p)),
                                         config.str("component_selector", "round_robin"), cursor)},
                      {"component_slot", cursor},
                      {"component_cursor", cursor + 1}};
            for (const auto &id : component_batch(plan["components"], sequence + groups))
              plan["tasks"].push(id);
            tree["component_cursor"] = cursor + 1;
            ++groups;
            const auto repeats = sampling == "independent" ? 1 : n;
            for (std::size_t i = 0; i < repeats && tree["batch"]["plans"].items().size() < count;
                 ++i) {
              auto proposal = plan;
              proposal["proposal_ordinal"] = i;
              tree["batch"]["plans"].push(std::move(proposal));
            }
          }
          tree["training_batches"] = sequence + groups;
          tree["reflection_batches"] = tree.num("reflection_batches", 0) + 1;
          save();
        }
      }
      if (tree["active"].null()) {
        const auto &plan =
            tree["batch"].has("plans")
                ? tree["batch"]["plans"].items().at(tree["batch"]["nodes"].items().size())
                : tree["batch"];
        const auto p = plan.num("parent", 0), index = tree["nodes"].items().size();
        tree["nodes"].items()[p]["selections"] = tree["nodes"].items()[p].num("selections", 0) + 1;
        tree["nodes"].push(Json{
            {"id", index},
            {"parent", p},
            {"parents", Json::Array{p}},
            {"kind", "reflection"},
            {"depth", tree["nodes"].items()[p].num("depth", 0) + 1},
            {"status", "pending"},
            {"selections", 0},
            {"component_slot", plan.num("component_slot", 0)},
            {"proposal_ordinal", plan.num("proposal_ordinal", 0)},
            {"component_cursor",
             plan.num("component_cursor", tree["nodes"].items()[p].num("component_cursor", 0))},
            {"batch", tree["batch"]["id"]},
            {"components", plan["components"]},
            {"tasks", plan["tasks"]}});
        tree["batch"]["nodes"].push(index);
        tree["active"] = index;
        tree["rounds"] = tree.num("rounds", 0) + 1;
        save();
      }
      const auto index = tree["active"].integer();
      auto &node = tree["nodes"].items()[index];
      try {
        if (node.str("kind") == "merge") {
          merge(index);
        } else {
          const auto p = node.num("parent", 0);
          const auto parent_artifact = artifact(p);
          std::vector<std::string> batch;
          if (node.has("tasks"))
            for (const auto &id : node["tasks"].items())
              batch.push_back(id.text());
          else
            batch = minibatch(index - 1);
          const auto parent_report = evaluate_parent(node, parent_artifact, batch);
          const auto parent_scores = scores(parent_report);
          require(!parent_scores.fields().empty(), "gepa_parent_evaluation_inconclusive");
          remember_opportunities(parent_artifact, parent_report);
          if (node.str("status") == "pending") {
            bool perfect = config.flag("skip_perfect_score", false);
            for (const auto &item : parent_scores.fields())
              perfect &= number(item.second["quality"]) >= 1 &&
                         number(item.second["fitness"]) >= number(config["perfect_score"], 1);
            if (perfect) {
              node["status"] = "skipped_perfect";
              save();
            } else {
              auto selected = node.has("components")
                                  ? node["components"]
                                  : gepa::select_components(components(parent_artifact), "all", 0);
              const auto available = components(parent_artifact);
              const auto observed = applied_experiences(parent_report);
              const auto initially_related = selected.items().size() == 1 &&
                                                     !observed.count(selected.items().front().text())
                                                 ? trigger_only_failure_evidence(parent_artifact,
                                                                                 parent_report, selected)
                                                 : Json(Json::Array{});
              const bool retain_trigger_exploration = !initially_related.items().empty();
              // 保留 L1 自己的搜索机会：当前经验没被用到时，先换成同批确实应用过的
              // 经验。只在没有这类证据时才让位给 L2/L3；停用经验的恢复探索不受影响。
              if (config.str("component_selector", "round_robin") == "round_robin" &&
                  selected.items().size() == 1) {
                const auto current = selected.items().front().text();
                if (current.rfind("experience:", 0) == 0 && !available[current].null() &&
                    !observed.count(current) && !retain_trigger_exploration) {
                  const auto fallback = observed_experience(available, observed);
                  if (!fallback.items().empty()) {
                    selected = fallback;
                    node["components"] = selected;
                    node["component_fallback"] =
                        Json{{"from", current}, {"to", selected.items().front()},
                             {"reason", "experience_not_applied_in_training"}};
                  }
                }
              }
              auto input = packet(parent_artifact, parent_report, selected,
                                  retain_trigger_exploration);
              if (retain_trigger_exploration) {
                node["trigger_only_exploration"] = true;
                node["trigger_only_evidence"] = initially_related;
              }
              if (config.str("component_selector", "round_robin") == "round_robin" &&
                  selected.items().size() == 1) {
                // 已应用经验与停用经验的恢复探索保留轮次；其他未应用经验再让位。
                const auto current = selected.items().front().text();
                const bool current_control = current == "schedule_rules" ||
                                             current == "action_rules" || current == "loop_rules";
                if (current_control ? !control_evidence(input, current)
                                    : !observed.count(current) && !available[current].null() &&
                                          !input.flag("trigger_only_exploration", false)) {
                  for (const auto *component : {"schedule_rules", "action_rules", "loop_rules"}) {
                    if (!available.has(component) || tree["component_selections"].num(component, 0))
                      continue;
                    const Json option = Json::Array{component};
                    Json probe;
                    try {
                      probe = packet(parent_artifact, parent_report, option);
                    } catch (const Error &error) {
                      if (std::string(error.what()) == "gepa_feedback_budget_too_small")
                        continue;
                      throw;
                    }
                    if (!control_evidence(probe, component))
                      continue;
                    if (current != component)
                      node["component_fallback"] =
                          Json{{"from", current},
                               {"to", component},
                               {"reason", "unexplored_control_opportunity"}};
                    selected = option;
                    node["components"] = selected;
                    input = std::move(probe);
                    break;
                  }
                }
              }
              if (config.str("component_selector", "round_robin") == "round_robin" &&
                  selected.items().size() == 1 &&
                  (selected.items().front().text() == "action_rules" ||
                   selected.items().front().text() == "loop_rules" ||
                   selected.items().front().text() == "schedule_rules") &&
                  !control_evidence(input, selected.items().front().text())) {
                const auto skipped = selected.items().front().text();
                tree["deferred_components"][skipped] =
                    tree["deferred_components"].num(skipped, 0) + 1;
                const auto slot = node.num("component_slot", 0);
                if (route_control_opportunity(parent_artifact, parent_report, available, slot,
                                              skipped, selected, input)) {
                  node["components"] = selected;
                  node["component_fallback"] = Json{{"from", skipped},
                                                    {"to", selected.items().front()},
                                                    {"reason", "executable_control_opportunity"}};
                  save();
                } else {
                  auto fallback = observed_experience(available, observed);
                  if (fallback.items().empty())
                    for (std::size_t offset = 1; offset <= available.fields().size() * 3;
                         ++offset) {
                      const auto candidate =
                          gepa::select_components(available, "round_robin", slot + offset);
                      if (candidate.items().front().text() == "action_rules" ||
                          candidate.items().front().text() == "loop_rules" ||
                          candidate.items().front().text() == "schedule_rules")
                        continue;
                      fallback = candidate;
                      break;
                    }
                  if (!fallback.items().empty()) {
                    selected = fallback;
                    node["components"] = selected;
                    node["component_fallback"] =
                        Json{{"from", skipped},
                             {"to", selected.items().front()},
                             {"reason", "no_executable_training_evidence"}};
                    input = packet(parent_artifact, parent_report, selected);
                    save();
                  }
                }
              }
              if (config.str("component_selector", "round_robin") == "round_robin" &&
                  selected.items().size() == 1 &&
                  selected.items().front().text().rfind("experience:", 0) == 0 &&
                  !available[selected.items().front().text()].null() &&
                  !observed.count(selected.items().front().text())) {
                const auto skipped = selected.items().front().text();
                const auto slot = node.num("component_slot", 0);
                if (route_control_opportunity(parent_artifact, parent_report, available, slot,
                                              skipped, selected, input)) {
                  node["components"] = selected;
                  node["component_fallback"] = Json{{"from", skipped},
                                                    {"to", selected.items().front()},
                                                    {"reason", "executable_control_opportunity"}};
                  save();
                } else {
                  const auto fallback = observed_experience(available, observed);
                  if (!fallback.items().empty()) {
                    selected = fallback;
                    node["components"] = selected;
                    node["component_fallback"] =
                        Json{{"from", skipped},
                             {"to", selected.items().front()},
                             {"reason", "experience_not_applied_in_training"}};
                    input = packet(parent_artifact, parent_report, selected);
                    save();
                  }
                }
              }
              // 单次经验命中不足以支持可靠反思；优先把同一批次的控制证据交给
              // 反思模型，若没有可执行控制机会则记录 untriggerable，不让一次偶然
              // 应用生成新的全局经验改写。
              bool skip_proposal = false;
              Json uncoupled_selection, uncoupled_input;
              if (selected.items().size() == 1 &&
                  selected.items().front().text().rfind("experience:", 0) == 0 &&
                  !input.flag("experience_evidence_sufficient", false)) {
                const auto related = trigger_only_failure_evidence(parent_artifact, parent_report, selected);
                if (!related.items().empty()) {
                  node["trigger_only_exploration"] = true;
                  node["trigger_only_evidence"] = related;
                  input = packet(parent_artifact, parent_report, selected, true);
                }
              }
              if (selected.items().size() == 1 &&
                  selected.items().front().text().rfind("experience:", 0) == 0 &&
                  !input.flag("experience_evidence_sufficient", false) &&
                  !input.flag("trigger_only_exploration", false)) {
                Json rerouted, rerouted_input;
                if (route_control_opportunity(parent_artifact, parent_report, available,
                                              node.num("component_slot", 0),
                                              selected.items().front().text(), rerouted,
                                              rerouted_input)) {
                  node["component_fallback"] =
                      Json{{"from", selected.items().front()},
                           {"to", rerouted.items().front()},
                           {"reason", "experience_evidence_insufficient"}};
                  selected = std::move(rerouted);
                  node["components"] = selected;
                  input = std::move(rerouted_input);
                } else {
                  skip_proposal = true;
                  node["status"] = "untriggerable_controls";
                  node["reason"] = "experience_evidence_insufficient";
                  save();
                }
              }
              if (!skip_proposal) {
                // 使用同一训练批次已有的证据，不额外调用模型、不扩展可执行权限。
                if (selected.items().size() == 1 && available.has("action_rules") &&
                    available.has("loop_rules") &&
                    (selected.items().front().text() == "action_rules" ||
                     selected.items().front().text() == "loop_rules")) {
                  const auto coupled = Json(Json::Array{"action_rules", "loop_rules"});
                  const auto joint_input = coupling_packet_or_single(input, [&] {
                    return packet(parent_artifact, parent_report, coupled);
                  });
                  const auto next = joint_input["selected_components"].dump() == coupled.dump()
                                        ? recovery_components(selected, joint_input, available)
                                        : selected;
                  if (next.items().size() > selected.items().size()) {
                    uncoupled_selection = selected;
                    uncoupled_input = input;
                    selected = next;
                    node["components"] = selected;
                    input = joint_input;
                    node["component_coupling"] = "same_branch_failure_recovery";
                  }
                }
                // COMBEE 可以同时改一条确实注入的经验和同一 run 的动作/循环控制。
                // 先以原始回执定位同一因果链，再用最终反思包验证 typed 控制证据；
                // 每次最多一条 L1、两种控制，绝不因经验在库中存在就批量授权。
                if (!input.flag("trigger_only_exploration", false) &&
                    config.str("reflection_strategy", "single") == "combee" &&
                    selected.items().size() <= 2 &&
                    std::none_of(selected.items().begin(), selected.items().end(),
                                 [](const Json &item) {
                                   return item.text() == "schedule_rules";
                                 })) {
                  std::map<std::string, std::set<std::string>> experience_runs,
                      experience_tasks;
                  const auto aliases = canonical_experience_aliases(
                      proto<v1::RuntimePolicy>(seed["policy"]));
                  for (const auto &trial : parent_report.trials()) {
                    if (trial.variant_id() != "candidate")
                      continue;
                    const auto evidence = json(trial.evidence());
                    const auto traces = evidence["traces"].array()
                                            ? evidence["traces"]
                                            : Json(Json::Array{evidence["trace"]});
                    for (const auto &trace : traces.items()) {
                      const auto run_id = trace["identity"].str("run_id");
                      if (run_id.empty() || !trace["events"].array())
                        continue;
                      for (const auto &event : trace["events"].items()) {
                        if (event.str("kind") != "advice.applied" ||
                            event["application"].str("status") != "applied" ||
                            event["attributes"]["receipt"].str("boundary") != "llm")
                          continue;
                        const auto &sources = event["attributes"]["receipt"]["experiences"];
                        if (!sources.array())
                          continue;
                        for (const auto &source : sources.items()) {
                          auto id = source.str("memory_id");
                          const auto alias = aliases.find({id, source.str("content_revision")});
                          if (alias != aliases.end())
                            id = alias->second;
                          const auto key = "experience:" + id;
                          if (id.empty() || !available.has(key) || !observed.count(key))
                            continue;
                          experience_runs[key].insert(trial.task_id() + "\n" + run_id);
                          experience_tasks[key].insert(trial.task_id());
                        }
                      }
                    }
                  }
                  std::vector<std::string> candidates;
                  for (const auto &component : selected.items())
                    if (component.text().rfind("experience:", 0) == 0)
                      candidates.push_back(component.text());
                  if (candidates.empty())
                    for (const auto &entry : experience_tasks)
                      if (entry.second.size() >= 2)
                        candidates.push_back(entry.first);
                  std::stable_sort(candidates.begin(), candidates.end(), [&](const auto &a,
                                                                               const auto &b) {
                    return tree["component_selections"].num(a, 0) <
                           tree["component_selections"].num(b, 0);
                  });
                  for (const auto &key : candidates) {
                    if (experience_tasks[key].size() < 2)
                      continue;
                    Json expanded = Json::Array{key};
                    for (const auto *control : {"action_rules", "loop_rules"})
                      if (available.has(control))
                        expanded.push(control);
                    if (expanded.items().size() == 1)
                      continue;
                    Json probe;
                    try {
                      probe = packet(parent_artifact, parent_report, expanded);
                    } catch (const Error &error) {
                      if (std::string(error.what()) == "gepa_feedback_budget_too_small")
                        continue;
                      throw;
                    }
                    Json authorized = Json::Array{};
                    std::string evidence_task, evidence_run;
                    for (const auto &row : probe["feedback"].items()) {
                      const auto &runs = row["trial"]["evidence"]["runs"];
                      if (!runs.array())
                        continue;
                      for (const auto &run : runs.items()) {
                        const auto run_id = run.str("run_id");
                        if (!experience_runs[key].count(row.str("task_id") + "\n" + run_id))
                          continue;
                        auto isolated = row;
                        isolated["trial"]["evidence"]["runs"] = Json::Array{run};
                        auto single = probe;
                        single["feedback"] = Json::Array{isolated};
                        Json controls_here = Json::Array{};
                        for (const auto *control : {"action_rules", "loop_rules"})
                          if (available.has(control) && control_evidence(single, control))
                            controls_here.push(control);
                        bool covers_selected = true;
                        for (const auto &component : selected.items())
                          if (component.text().rfind("experience:", 0) != 0 &&
                              std::none_of(controls_here.items().begin(),
                                           controls_here.items().end(), [&](const Json &item) {
                                             return item.text() == component.text();
                                           }))
                            covers_selected = false;
                        if (!covers_selected || controls_here.items().size() <=
                                                    authorized.items().size())
                          continue;
                        authorized = controls_here;
                        evidence_task = row.str("task_id");
                        evidence_run = run_id;
                      }
                    }
                    if (authorized.items().empty())
                      continue;
                    Json coupled = Json::Array{key};
                    for (const auto &control : authorized.items())
                      coupled.push(control);
                    try {
                      auto joint = packet(parent_artifact, parent_report, coupled);
                      if (!joint.flag("experience_evidence_sufficient", false))
                        continue;
                      selected = std::move(coupled);
                      input = std::move(joint);
                    } catch (const Error &error) {
                      if (std::string(error.what()) == "gepa_feedback_budget_too_small")
                        continue;
                      throw;
                    }
                    node["components"] = selected;
                    node["component_coupling"] = "same_run_applied_experience_control";
                    node["component_coupling_evidence"] =
                        Json{{"task_id", evidence_task}, {"run_id", evidence_run},
                             {"experience", key}, {"control_count", authorized.items().size()}};
                    break;
                  }
                }
                if (!node.flag("component_reserved", false)) {
                  for (const auto &component : selected.items())
                    tree["component_selections"][component.text()] =
                        tree["component_selections"].num(component.text(), 0) + 1;
                  node["component_reserved"] = true;
                  save();
                }
                // Rerouting can finish before its first response is saved. Persist
                // the actual authorization/request binding before any paid call.
                require(input["selected_components"].dump() == selected.dump() &&
                            node["components"].dump() == selected.dump(),
                        "gepa_reflection_component_binding_changed");
                save();
                Json draft, candidate, accepted;
                Json contract_repairs;
                node.fields().erase("control_binding_provenance");
                try {
                  bool paired = false;
                  if (node.num("proposal_ordinal", 0) == 1 && tree["batch"].object())
                    for (const auto &sibling_id : tree["batch"]["nodes"].items()) {
                      if (sibling_id.integer() >= index)
                        continue;
                      const auto &sibling = tree["nodes"].items()[sibling_id.integer()];
                      paired |= sibling.num("parent", SIZE_MAX) == node.num("parent", SIZE_MAX) &&
                                sibling["components"].dump() == selected.dump() &&
                                sibling["tasks"].dump() == node["tasks"].dump() &&
                                sibling.flag("component_reserved", false) &&
                                sibling.str("generation_method") != "experience_ablation";
                    }
                  bool independent_ablation =
                      config.str("sampling_strategy", "same_parent") == "independent";
                  if (independent_ablation)
                    independent_ablation = !experience_ablation_seen(
                        tree["nodes"].items()[node.num("parent", 0)].str("version"), selected, index);
                  // 独立采样没有同轮兄弟：以实测父版本作对照，每版本/经验只消融一次。
                  if ((paired || independent_ablation) &&
                      !input.flag("trigger_only_exploration", false))
                    draft = experience_ablation(parent_artifact, selected, parent_report);
                  if (!draft.null())
                    node["generation_method"] = "experience_ablation";
                  else {
                    const auto calls_before = tree.num("model_calls", 0);
                    try {
                      draft = propose_full_pool(parent_artifact, input, parent_report, index, draft);
                    } catch (const FullPoolProposalIncomplete &error) {
                      if (uncoupled_input.null() ||
                          !coupling_budget_before_maps(error.what(), calls_before,
                                                      tree.num("model_calls", 0),
                                                      node["reflection_stages"]))
                        throw;
                      // A coupled preview can fit while a later full-pool group cannot.
                      // No map/model has started: keep the already validated single scope.
                      selected = uncoupled_selection;
                      input = uncoupled_input;
                      node["components"] = selected;
                      node["component_coupling"] = "single_retained_for_feedback_budget";
                      require(input["selected_components"].dump() == selected.dump(),
                              "gepa_reflection_component_binding_changed");
                      save();
                      draft = propose_full_pool(parent_artifact, input, parent_report, index, draft);
                    }
                  }
                  write(root / "reflections" / (std::to_string(index) + ".draft.json"), draft.dump());
                  // 普通草稿和 COMBEE 均先校验再进入同一修复入口，权限与预算不变。
                  accepted = expand_control_updates(draft, components(parent_artifact), selected);
                  accepted = repair_schedule_contracts(accepted, parent_report, contract_repairs);
                  if (!contract_repairs.items().empty())
                    node["schedule_contract_repair"] = contract_repairs;
                  candidate = mutate(parent_artifact, accepted, selected,
                                     input.flag("trigger_only_exploration", false));
                } catch (const FullPoolProposalIncomplete &) {
                  // Never replace unfinished maps/reduce/review with a single feedback row.
                  // The saved full-pool stages remain available for explicit recovery.
                  throw;
                } catch (const std::exception &exception) {
                  const std::string code = exception.what();
                  if ((code != "control_source_not_approved" &&
                       code != "gepa_component_not_authorized" &&
                       code != "gepa_experience_not_authorized" &&
                       code != "control_failure_replan_not_authorized" && code != "unknown_field" &&
                       code != "gepa_draft_invalid" && code != "control_duplicate_loop_scope" &&
                       code != "control_binding_invalid" &&
                       code != "control_repeat_operation_required" &&
                       code != "control_loop_semantic_scope_required" &&
                       code != "control_loop_relaxes_condition" &&
                       code != "control_loop_condition_invalid" && code != "schedule_rule_cycle") ||
                      controls.num("max_attempts", 2) < 2 ||
                      tree.num("model_calls", 0) >= config.num("max_model_calls", 8))
                    throw;
                  // 在相同模型与输入预算内最多修复一次。来源和格式修正无需轨迹；
                  // 组件选择修正必须附带证据。
                  Json repair{{"phase", code == "control_source_not_approved"
                                            ? "repair_control_source"
                                        : (code == "unknown_field" || code == "gepa_draft_invalid" ||
                                           code == "control_duplicate_loop_scope")
                                            ? "repair_draft_format"
                                            : "repair_component_selection"},
                              {"error", code},
                              {"draft", draft},
                              {"experiences", input["experiences"]},
                              {"selected_components", selected},
                              {"components", input["components"]}};
                  if (input.has("controls"))
                    repair["controls"] = input["controls"];
                  if (code == "schedule_rule_cycle") {
                    // 仍只修复一次；保留真实父规则和草稿，由模型根据证据选边，程序不删边或放宽无环校验。
                    repair["phase"] = "repair_schedule_cycle";
                    repair["instruction"] =
                        "editable_bindings只是候选，不是已有规则；真实父规则以components和base_schedule_rules为准。"
                        "add添加有向依赖，remove只删除真实父规则；反向add不表示并行。请根据证据修复环。";
                    repair["feedback"] = Json::Array{};
                    if (const auto *evidence = repair_feedback_row(input, selected))
                      repair["feedback"].push(*evidence);
                  } else if (code == "control_binding_invalid") {
                    repair = input;
                    repair.fields().erase("parent_version");
                    repair["phase"] = "repair_control_binding";
                    repair["error"] = code;
                    const Json &saved = tree;
                    const auto &diagnostic =
                        saved["reflection_calls"][std::to_string(tree.num("model_calls", 0))]
                             ["control_binding_error"];
                    if (diagnostic.object()) {
                      repair["binding_error"] = diagnostic;
                      if (repair.dump().size() > config.num("max_input_bytes", 65536))
                        repair["binding_error"].fields().erase("budget");
                    }
                  } else if (code == "gepa_component_not_authorized" ||
                             code == "gepa_experience_not_authorized" ||
                             code == "control_failure_replan_not_authorized" ||
                             code == "control_repeat_operation_required" ||
                             code == "control_loop_semantic_scope_required" ||
                             code == "control_loop_relaxes_condition" ||
                             code == "control_loop_condition_invalid") {
                    repair = input;
                    repair.fields().erase("parent_version");
                    repair["phase"] = (code == "gepa_component_not_authorized" ||
                                       code == "gepa_experience_not_authorized")
                                          ? "repair_component_selection"
                                          : "repair_control_capability";
                    repair["error"] = code;
                    repair["feedback"] = Json::Array{};
                    const auto *evidence = repair_feedback_row(input, selected);
                    // 只复用一条相关训练行，不复用未授权草稿或整个小批；保留其任务契约和轨迹。
                    // 轮次上限漏写收尾条件也走这一次修复；不能由程序代填宿主能力或放宽校验。
                    if (evidence)
                      repair["feedback"].push(*evidence);
                  }
                  node["repair_error"] = code;
                  save();
                  accepted = reflect(repair, index, "-repair");
                  write(root / "reflections" / (std::to_string(index) + "-repair.draft.json"),
                        accepted.dump());
                  accepted = expand_control_updates(accepted, components(parent_artifact), selected);
                  accepted = repair_schedule_contracts(accepted, parent_report, contract_repairs);
                  if (!contract_repairs.items().empty())
                    node["schedule_contract_repair"] = contract_repairs;
                  candidate = mutate(parent_artifact, accepted, selected,
                                     input.flag("trigger_only_exploration", false));
                }
                if (!node.has("repair_error") && controls.num("max_attempts", 2) >= 2 &&
                    tree.num("model_calls", 0) < config.num("max_model_calls", 8) &&
                    components(candidate).dump() == components(parent_artifact).dump()) {
                  const auto repair = noop_experience_repair_packet(
                      input, draft, config.num("max_input_bytes", 65536));
                  if (!repair.null()) {
                    node["repair_error"] = "experience_update_matches_parent";
                    save();
                    const auto repaired = reflect(repair, index, "-noop-repair");
                    write(root / "reflections" / (std::to_string(index) + "-noop-repair.draft.json"),
                          repaired.dump());
                    accepted = expand_control_updates(repaired, components(parent_artifact), selected);
                    candidate = mutate(parent_artifact, accepted, selected,
                                       input.flag("trigger_only_exploration", false));
                  }
                }
                // 合法弃权才补一个有完整评审依据的候选，不覆盖有效提案或绕开原有门禁。
                const auto probe =
                    reviewed_control_probe(parent_artifact, candidate, input, parent_report, index);
                if (!probe.null()) {
                  node["control_probe"] = probe["detail"];
                  try {
                    const auto probe_draft =
                        expand_control_updates(probe["draft"], components(parent_artifact), selected);
                    const auto probe_candidate = mutate(parent_artifact, probe_draft, selected,
                                                        input.flag("trigger_only_exploration", false));
                    const auto check =
                        control_preflight(parent_artifact, probe_candidate, parent_report);
                    if (check.flag("passed", false) &&
                        components(probe_candidate).dump() != components(parent_artifact).dump()) {
                      accepted = probe_draft;
                      candidate = probe_candidate;
                      node["generation_method"] = "reviewed_control_probe";
                      node["control_probe"]["status"] = "proposed";
                    } else
                      node["control_probe"]["status"] = "untriggerable";
                  } catch (const Error &error) {
                    const std::string code = error.what();
                    if (code.rfind("control_", 0) != 0 && code != "gepa_candidate_not_authorized")
                      throw;
                    node["control_probe"]["status"] = "invalid";
                  }
                }
                node["reason"] = accepted.str("reason");
                node["control_preflight"] = control_preflight(
                    parent_artifact, candidate, parent_report,
                    node.has("control_binding_provenance")
                        ? node["control_binding_provenance"]
                        : Json(Json::Array{}));
                if (node["control_preflight"].has("unconditioned_recovery") &&
                    !node.has("repair_error") && controls.num("max_attempts", 2) >= 2 &&
                    tree.num("model_calls", 0) < config.num("max_model_calls", 8)) {
                  // 预检缺少恢复条件也复用同一次修复预算；条件由模型根据训练事实补充，不能代填。
                  auto repair = input;
                  repair.fields().erase("parent_version");
                  repair["phase"] = "repair_control_capability";
                  repair["error"] = "recovery_condition_required";
                  repair["draft"] = draft;
                  repair["preflight"] = node["control_preflight"];
                  repair["feedback"] = Json::Array{};
                  const auto proposed_policy = proto<v1::RuntimePolicy>(candidate["policy"]);
                  const Json *evidence = nullptr;
                  for (const auto &row : input["feedback"].items()) {
                    const auto &runs = row["trial"]["evidence"]["runs"];
                    if (!runs.array())
                      continue;
                    for (const auto &run : runs.items()) {
                      if (!run["actions"].array())
                        continue;
                      for (const auto &action : run["actions"].items()) {
                        const auto &control = action["loop_control"];
                        if (action.str("kind") != "loop.decision" || !control.object() ||
                            (!control.num("rejected_actions", 0) &&
                             (!control["error_codes"].array() ||
                              control["error_codes"].items().empty())))
                          continue;
                        for (const auto &rule : proposed_policy.options().loop_rules())
                          if (rule.target_scope() == control.str("target_scope") &&
                              (rule.actor_id().empty() ||
                               rule.actor_id() == action.str("actor_id")) &&
                              std::any_of(
                                  node["control_preflight"]["unconditioned_recovery"].items().begin(),
                                  node["control_preflight"]["unconditioned_recovery"].items().end(),
                                  [&](const auto &id) { return id.text() == rule.rule_id(); }))
                            evidence = &row;
                      }
                    }
                    if (evidence)
                      break;
                  }
                  // 没有该规则作用域/actor 的真实条件，或原草稿与证据放不下时，保留原预检拒绝。
                  if (evidence)
                    repair["feedback"].push(*evidence);
                  if (evidence && repair.dump().size() <= config.num("max_input_bytes", 65536)) {
                    node["preflight_before_repair"] = node["control_preflight"];
                    node["repair_error"] = "recovery_condition_required";
                    save();
                    accepted = reflect(repair, index, "-repair");
                    write(root / "reflections" / (std::to_string(index) + "-repair.draft.json"),
                          accepted.dump());
                    accepted =
                        expand_control_updates(accepted, components(parent_artifact), selected);
                    accepted = repair_schedule_contracts(accepted, parent_report, contract_repairs);
                    if (!contract_repairs.items().empty())
                      node["schedule_contract_repair"] = contract_repairs;
                    candidate = mutate(parent_artifact, accepted, selected,
                                       input.flag("trigger_only_exploration", false));
                    node["reason"] = accepted.str("reason");
                    node["control_preflight"] = control_preflight(
                        parent_artifact, candidate, parent_report,
                        node.has("control_binding_provenance")
                            ? node["control_binding_provenance"]
                            : Json(Json::Array{}));
                  }
                }
                if (!node["control_preflight"].flag("passed", false) &&
                    std::any_of(selected.items().begin(), selected.items().end(),
                                [](const auto &item) { return item.text() == "schedule_rules"; })) {
                  Json repairs;
                  auto repaired_draft = repair_schedule_guards(accepted, parent_report, repairs);
                  if (!repairs.items().empty()) {
                    node["schedule_guard_repair"] = repairs;
                    auto repaired = mutate(parent_artifact, repaired_draft, selected);
                    auto check = control_preflight(
                        parent_artifact, repaired, parent_report,
                        node.has("control_binding_provenance")
                            ? node["control_binding_provenance"]
                            : Json(Json::Array{}));
                    if (check.flag("passed", false)) {
                      candidate = std::move(repaired);
                      accepted = std::move(repaired_draft);
                      node["control_preflight"] = std::move(check);
                    }
                  }
                }
                const auto subset = triggerable_loop_subset(
                    parent_artifact, candidate, parent_report, node["control_preflight"],
                    node.has("control_binding_provenance")
                        ? node["control_binding_provenance"]
                        : Json(Json::Array{}));
                if (!subset.null()) {
                  node["preflight_before_subset"] = node["control_preflight"];
                  node["control_subset"] = Json{{"removed_rule_ids", subset["removed_rule_ids"]}};
                  candidate = subset["candidate"];
                  node["control_preflight"] = subset["preflight"];
                  node["generation_method"] = "triggerable_loop_subset";
                  node["reason"] = "retained_independent_loop_changes_with_training_evidence";
                }
                if (!node["control_preflight"].flag("passed", false) &&
                    (config.str("component_selector", "round_robin") == "round_robin" ||
                     node["control_preflight"].flag("pure_removal_no_effect", false) ||
                     node["control_preflight"].has("unconditioned_recovery") ||
                     node["control_preflight"].has("known_keep_truncation"))) {
                  node["status"] = "untriggerable_controls";
                  node["reason"] = node["control_preflight"].has("unconditioned_recovery")
                                       ? "recovery_condition_required"
                                   : node["control_preflight"].has("known_keep_truncation")
                                       ? "known_required_iterations_would_be_truncated"
                                       : "no_differential_training_trigger";
                  save();
                } else
                  proposal(index, candidate);
              }
            }
          }
          if (node.str("status") == "proposed") {
            const auto child = artifact(index);
            const auto screening = evaluate(child, batch, "train");
            const auto values = classify_training_screening(
                node, parent_artifact, child, parent_scores, screening, &parent_report);
            // 仅训练配对进入记忆反馈；验证和留出不会经过这个入口。
            // 失败提案也保存反证，但不会把其损失归给未修改的来源经验。
            const auto source = proto<v1::RuntimePolicy>(parent_artifact["policy"]).snapshot();
            const auto feedback = make_gepa_memory_feedback(
                components(parent_artifact), components(child), source, parent_report, screening,
                parent_scores, values,
                node["screening"].flag("controls_observed", false)
                    ? node["screening"]["causal_changes"] : Json(Json::Object{}),
                dataset.dataset_version(), evaluation_context,
                node.str("status") == "screened");
            tree["memory_feedback"] = merge_gepa_memory_feedback(
                tree["memory_feedback"], feedback, source, gate.num("memory_min_pairs", 2));
            save();
            // 无行为差分不靠验证随机收益续命；真实消融由父策略回放识别。
          }
          if (node.str("status") == "screened" && tree["batch"].null())
            validate(index);
        }
      } catch (const std::exception &exception) {
        error(index, exception);
      }
      tree["active"] = nullptr;
      if (node.str("kind") == "merge" || tree["batch"].null()) {
        update_frontier(node.num("previous_best", 0), node.str("previous_frontier"),
                        assessed_for_patience(node));
        if (node.str("kind") == "merge")
          tree["merge_due"] =
              config.flag("use_merge", true) &&
              tree["merge_attempts"].items().size() < config.num("max_merge_invocations", 5) &&
              !gepa::select_merge(tree["nodes"], tree["frontier_mapping"],
                                  tree["merge_attempts"], config, tree.num("rounds", 0))
                   .null();
        else
          tree["merge_due"] = gepa::exploration_parent(node);
      }
      save();
      if ((tree.str("stop_reason") == "gepa_trial_budget_exhausted" ||
           tree.str("stop_reason") == "gepa_search_time_reserved") &&
          tree["batch"].null())
        break;
    }
    if (!final_holdout_replay)
      refresh_change_evidence();
    if (final_holdout_replay && tree["derived_selection_protocol"].object()) {
      for (auto &node : tree["nodes"].items())
        if (node["validation"].object() && !node["validation"].fields().empty())
          node["optimization"] = annotate_validation(node["validation"]);
      tree["derived_selection_protocol"]["optimization_recomputed"] = true;
      tree["derived_selection_protocol"]["gate"] = gate;
      save();
    }
    if (!tree.flag("holdout_started", false)) {
      tree["winner"] = winner();
      if (!tree.has("stop_reason"))
        tree["stop_reason"] = tree.flag("reflection_exhausted", false) ||
                                      tree.num("model_calls", 0) >= config.num("max_model_calls", 8)
                                  ? "model_budget"
                                  : "max_rounds";
      save();
    }
    // 先把验证排名、版本和覆盖记录写入持久化计划，再依次执行留出。
    // 这样恢复会沿用同一批 Top-K；留出分数不会反馈到候选排序，避免数据泄漏。
    const auto validation_winner = tree.num("winner", 0);
    const auto selected = artifact(validation_winner);
    Report report;
    std::vector<GepaFinalist> finalists;
    auto &final_holdout = tree["final_holdout"];
    const bool prepare_finalists =
        !final_holdout.object() && (!tree.flag("holdout_started", false) || final_holdout_replay);
    if (!final_holdout.object() && tree.flag("holdout_started", false) && !final_holdout_replay)
      throw Error("gepa_final_holdout_legacy_replay_required");
    if (prepare_finalists) {
      if (final_holdout.object())
        tree["previous_final_holdout"] = final_holdout;
      const auto candidates = final_holdout_candidates();
      if (candidates.empty()) {
        tree["status"] = "validation_rejected";
        fs::remove(root / "selected-version.json");
      } else {
        const auto epoch = tree.num("final_holdout_epoch", 0) + 1;
        Json plan{{"protocol", tree["derived_selection_protocol"].object()
                                   ? tree["derived_selection_protocol"].str("protocol")
                                   : "top-k-v1"},
                  {"selection", "frozen_validation_rank"},
                  {"requested_top_k", finalist_budget_count},
                  {"epoch", epoch},
                  {"status", "prepared"},
                  {"candidates", Json::Array{}}};
        std::string campaign =
            "gepa-final-holdout-v1\n" + tree.str("context") + "\n" + std::to_string(epoch);
        for (std::size_t rank = 0; rank < candidates.size(); ++rank) {
          const auto index = candidates[rank];
          const auto candidate = artifact(index);
          plan["candidates"].push(
              Json{{"rank", rank + 1},
                   {"node", index},
                   {"version", agent_version_id(candidate.dump())},
                   {"optimization_scope", "validation"},
                   {"validation_optimization", tree["nodes"].items()[index]["optimization"]},
                   {"optimization", tree["nodes"].items()[index]["optimization"]},
                   {"control_coverage_scope", "validation"},
                   {"validation_control_coverage", tree["nodes"].items()[index]["control_coverage"]},
                   {"control_coverage", tree["nodes"].items()[index]["control_coverage"]},
                   {"status", "pending"}});
          campaign += "\n" + plan["candidates"].items().back().str("version");
        }
        plan["campaign_id"] = hash(campaign);
        if (final_holdout_replay) {
          plan["retrospective"] = true;
          plan["replay_deadline_ms"] = now() + config.num("timeout_ms", 1800000);
        }
        tree["final_holdout_epoch"] = epoch;
        final_holdout = std::move(plan);
        tree["holdout_started"] = true;
        fs::remove(root / "selected-version.json");
        save();
      }
    }
    if (final_holdout.object() && final_holdout["candidates"].array() &&
        !final_holdout["candidates"].items().empty()) {
      final_holdout["status"] = "running";
      save();
      for (std::size_t rank = 0; rank < final_holdout["candidates"].items().size(); ++rank) {
        auto &entry = final_holdout["candidates"].items()[rank];
        const auto index = entry.num("node", SIZE_MAX);
        require(index < tree["nodes"].items().size() && entry.num("rank", 0) == rank + 1,
                "gepa_final_holdout_plan_changed");
        const auto candidate = artifact(index);
        require(entry.str("version") == agent_version_id(candidate.dump()),
                "gepa_final_holdout_plan_changed");
        const auto remaining = final_holdout["candidates"].items().size() - rank - 1;
        // 缓存开启时，首个候选若尚无 L0 缓存会执行一次“基线+候选”，随后固定并
        // 复用该基线；后续候选只需各自一次 trial。这里不能把首个候选开始前的
        // cache miss 外推到所有剩余候选，否则 Top-K 会被按 K 份基线过度预留。
        const bool baseline_reusable = tree["holdout_baseline_cache"].str("status") == "hit" ||
                                       config.flag("cache_evaluations", true);
        const auto per_remaining_candidate = holdout_trials * (baseline_reusable ? 1 : 2);
        final_holdout_reserve = remaining * per_remaining_candidate;
        entry["status"] = "running";
        save();
        // 每个最终候选使用同一冻结 campaign，且不复用通用候选缓存，确保本次留出
        // 真正执行候选；L0 仅可复用已锁定且经校验的基线来源。
        const auto final_report = evaluate(candidate, holdout, "holdout", true, false, false,
                                           final_holdout.str("campaign_id"));
        entry["status"] = scored(final_report) ? "complete" : "inconclusive";
        entry["report_kind"] = final_report.report_kind();
        // 缓存迁移命中已有完整报告时，计划中已经保存过指纹；不要为了
        // 更新状态再次把数百 MB 的 protobuf 转成 JSON。新报告仍计算指纹，
        // 保持首次落盘和历史恢复的可追溯性。
        if (entry.str("report_fingerprint").empty())
          entry["report_fingerprint"] = hash(json(final_report).dump());
        entry["trials"] = std::size_t(final_report.trials_size());
        entry["baseline_source"] = tree["holdout_baseline_cache"]["source"];
        // validation 排名字段保留作选优审计；独立保存真实 Holdout 摘要，避免
        // retrospective/frozen 计划看起来像一次新的留出观测。
        entry["holdout_summary"] = json(final_report.summary());
        entry["holdout_control_coverage"] = control_coverage(candidate, final_report);
        save();
        finalists.push_back(
            GepaFinalist{index, candidate, final_report, entry["control_coverage"]});
      }
      final_holdout_reserve = 0;
      bool complete = true;
      for (const auto &entry : final_holdout["candidates"].items())
        complete &= entry.str("status") == "complete";
      final_holdout["status"] = complete ? "completed" : "inconclusive";
      tree["status"] = complete ? "completed" : "inconclusive";
      report = finalists.front().holdout;
    } else if (tree.str("status") != "validation_rejected") {
      tree["status"] = "validation_rejected";
    }
    tree["finished_ms"] = now();
    tree["selected_version"] = agent_version_id(selected.dump());
    save();
    Json summary{
        {"algorithm", tree["algorithm"]},     {"status", tree["status"]},
        {"rounds", tree["rounds"]},           {"nodes", tree["nodes"].items().size()},
        {"model_calls", tree["model_calls"]}, {"trials_used", tree["trials_used"]},
        {"winner", tree["winner"]},           {"selected_version", tree["selected_version"]},
        {"stop_reason", tree["stop_reason"]}};
    summary["merge_attempts"] = tree["merge_attempts"].items().size();
    summary["reflection_batches"] = tree.num("reflection_batches", 0);
    summary["available_components"] = tree["available_components"];
    summary["search_warnings"] = tree["search_warnings"];
    summary["fixed_controls"] = tree["fixed_controls"];
    summary["control_coverage"] = tree["nodes"].items()[tree.num("winner", 0)]["control_coverage"];
    summary["optimization"] = tree["nodes"].items()[tree.num("winner", 0)]["optimization"];
    summary["component_selections"] = tree["component_selections"];
    summary["training_opportunities"] = tree["training_opportunities"];
    summary["deferred_components"] = tree["deferred_components"];
    summary["final_holdout"] = final_holdout;
    GepaResult result;
    result.artifact = selected;
    result.holdout = report;
    result.summary = std::move(summary);
    result.memory_feedback = tree["memory_feedback"];
    result.finalists = std::move(finalists);
    return result;
  }
};
} // 结束当前匿名命名空间。

// 读取外部搜索参数并检查范围与能力依赖，配置变化不需要改写搜索代码。
Json load_gepa_config(const fs::path &path) {
  auto config = Json::parse(read(path));
  keys(config, {"schema_version",
                "runtime_config",
                "prompt_file",
                "max_rounds",
                "max_model_calls",
                "max_trials",
                "timeout_ms",
                "holdout_reserve_ms",
                "holdout_max_report_bytes",
                "holdout_top_k",
                "minibatch_size",
                "max_feedback_trials",
                "max_input_bytes",
                "max_experience_bytes",
                "patience",
                "seed",
                "min_improvement",
                "screening_max_quality_drop",
                "exploration_max_quality_drop",
                "objective",
                "use_merge",
                "max_merge_invocations",
                "merge_minibatch_size",
                "candidate_selection_strategy",
                "frontier_type",
                "prune_dominated",
                "epsilon",
                "top_k",
                "component_selector",
                "minibatch_strategy",
                "skip_perfect_score",
                "perfect_score",
                "acceptance",
                "proposals_per_round",
                "proposal_selection",
                "proposal_top_k",
                "cache_evaluations",
                "sampling_strategy",
                "parents_per_round",
                "target_fitness",
                "reflection_strategy",
                "duplication_factor",
                "aggregation_prompt_file",
                "unknown_token_policy",
                "baseline_report_file",
                "holdout_baseline_result_file", "holdout_baseline_dataset", "holdout_baseline_evaluation_file",
                "seed_report_file"});
  require(config.num("schema_version", 0) == 1, "gepa_config_invalid");
  bound(config, "max_rounds", 8, 1, 256);
  bound(config, "max_model_calls", 8, 1, 1024);
  bound(config, "max_trials", 400, 4, 20000);
  // 离线大规模搜索可跨多个小时运行；单题执行超时仍由 evaluation 配置独立限制。
  // 这里仅放宽整棵搜索树的总墙钟预算，上限固定为七天，避免无界作业。
  bound(config, "timeout_ms", 1800000, 1000, 604800000);
  bound(config, "holdout_reserve_ms", 0, 0, config.num("timeout_ms", 1800000) - 1);
  bound(config, "holdout_max_report_bytes", 0, 0, kMaxEvaluationReportBytes);
  require(!config.num("holdout_max_report_bytes", 0) ||
              config.num("holdout_max_report_bytes", 0) >= 1024,
          "gepa_config_invalid");
  bound(config, "holdout_top_k", 1, 1, 3);
  bound(config, "minibatch_size", 4, 1, 128);
  bound(config, "max_feedback_trials", 4, 1, 32);
  bound(config, "max_input_bytes", 65536, 8192, 1048576);
  bound(config, "max_experience_bytes", 2048, 64, 2048);
  bound(config, "patience", 0, 0, 256);
  bound(config, "seed", 42, 0, 2147483000);
  bound(config, "max_merge_invocations", 5, 0, 256);
  bound(config, "merge_minibatch_size", 4, 1, 128);
  bound(config, "top_k", 5, 1, 256);
  bound(config, "proposals_per_round", 1, 1, 8);
  bound(config, "proposal_top_k", 2, 1, 8);
  bound(config, "parents_per_round", 2, 1, 8);
  bound(config, "duplication_factor", 2, 1, 4);
  require(config.str("sampling_strategy", "same_parent") != "pxn" ||
              config.num("proposals_per_round", 1) * config.num("parents_per_round", 2) <= 8,
          "gepa_batch_limit");
  for (const auto *key :
       {"use_merge", "prune_dominated", "skip_perfect_score", "cache_evaluations"})
    if (config.has(key))
      config[key].boolean();
  auto choice = [&](const char *key, const char *fallback,
                    std::initializer_list<const char *> allowed) {
    const auto value = config.str(key, fallback);
    require(std::any_of(allowed.begin(), allowed.end(), [&](auto item) { return value == item; }),
            "gepa_strategy_invalid");
  };
  choice("candidate_selection_strategy", "pareto",
         {"pareto", "current_best", "epsilon_greedy", "top_k_pareto", "fair"});
  choice("frontier_type", "instance", {"instance", "objective", "hybrid", "cartesian"});
  choice("component_selector", "round_robin", {"round_robin", "all", "model_choice"});
  choice("minibatch_strategy", "epoch_shuffled", {"epoch_shuffled", "fixed"});
  choice("acceptance", "strict_improvement",
         {"strict_improvement", "improvement_or_equal", "pareto"});
  choice("proposal_selection", "all_improvements", {"all_improvements", "best", "top_k"});
  choice("sampling_strategy", "same_parent", {"single", "same_parent", "independent", "pxn"});
  choice("reflection_strategy", "single", {"single", "combee"});
  choice("unknown_token_policy", "reject", {"reject", "worst_case"});
  for (const auto *key : {"epsilon", "perfect_score", "target_fitness"})
    if (config.has(key))
      require(number(config[key], -1) >= 0 && number(config[key], -1) <= 1,
              "gepa_strategy_invalid");
  require(number(config["min_improvement"], 0.000001) > 0 &&
              number(config["min_improvement"], 0.000001) <= 1,
          "gepa_improvement_invalid");
  require(number(config["screening_max_quality_drop"], 0.03) >= 0 &&
              number(config["screening_max_quality_drop"], 0.03) <= 0.15,
          "gepa_strategy_invalid");
  require(number(config["exploration_max_quality_drop"], 0.15) >= 0 &&
              number(config["exploration_max_quality_drop"], 0.15) <= 1,
          "gepa_strategy_invalid");
  if (!config.has("objective"))
    config["objective"] = Json::Object{};
  keys(config["objective"], {"invalid_loop_weight", "token_weight", "latency_weight",
                             "invalid_loop_scale", "token_scale", "latency_scale_ms"});
  double sum = 0;
  for (const auto &item : config["objective"].fields()) {
    const auto value = number(item.second, -1);
    require(value >= 0, "gepa_objective_invalid");
    if (item.first.find("weight") != std::string::npos)
      require(value <= 0.49, "gepa_objective_invalid");
    else
      require(value > 0, "gepa_objective_invalid");
  }
  sum = number(config["objective"]["invalid_loop_weight"], 0.2) +
        number(config["objective"]["token_weight"], 0.05) +
        number(config["objective"]["latency_weight"], 0);
  require(sum < 0.5, "gepa_quality_weight_required");
  for (const auto *key : {"runtime_config", "prompt_file"}) {
    fs::path value(config.str(key));
    require(!value.empty(), "gepa_configuration_path_required");
    if (value.is_relative())
      value = fs::absolute(path).parent_path() / value;
    config[key] = value.lexically_normal().string();
  }
  if (config.has("aggregation_prompt_file") ||
      config.str("reflection_strategy", "single") == "combee") {
    fs::path value = config.has("aggregation_prompt_file")
                         ? fs::path(config.str("aggregation_prompt_file"))
                         : fs::path(config.str("prompt_file")).parent_path() / "gepa-aggregate.txt";
    require(!value.empty(), "gepa_configuration_path_required");
    if (value.is_relative())
      value = fs::absolute(path).parent_path() / value;
    config["aggregation_prompt_file"] = value.lexically_normal().string();
  }
  require(config.has("holdout_baseline_result_file") == config.has("holdout_baseline_dataset") &&
              config.has("holdout_baseline_result_file") == config.has("holdout_baseline_evaluation_file"),
          "gepa_external_baseline_paths_required");
  for (const auto *key : {"baseline_report_file", "seed_report_file", "holdout_baseline_result_file",
                          "holdout_baseline_dataset", "holdout_baseline_evaluation_file"})
    if (config.has(key)) {
      fs::path value(config.str(key));
      require(!value.empty(), "gepa_configuration_path_required");
      if (value.is_relative())
        value = fs::absolute(path).parent_path() / value;
      config[key] = value.lexically_normal().string();
    }
  config["config_file"] = fs::absolute(path).lexically_normal().string();
  validate_local_data(config);
  return config;
}

// 为影响搜索行为的配置生成稳定身份，供断点续跑校验。
std::string gepa_config_fingerprint(const Json &config) {
  if (config.null())
    return {};
  auto runtime = legacy::load_runtime_configuration(config.str("runtime_config"));
  std::string bytes = "gepa-reflective-dag-v27-grounded-recovery-options:" + config.dump() +
                      read(config.str("config_file")) + read(config.str("prompt_file"));
  if (config.has("aggregation_prompt_file"))
    bytes += read(config.str("aggregation_prompt_file"));
  // 报告正文而非仅路径绑定本轮身份，防止续跑时把替换后的评分混入旧搜索。
  for (const auto *key : {"baseline_report_file", "seed_report_file"})
    if (config.has(key))
      bytes += hash(read(config.str(key), kMaxEvaluationReportBytes));
  for (const auto &path : runtime.source_files)
    bytes += read(path);
  if (runtime.model.endpoint.empty())
    if (const auto *base = std::getenv("OPENAI_BASE_URL"))
      bytes += base;
  if (runtime.model.model_id.empty())
    if (const auto *model = std::getenv("OPENAI_MODEL"))
      bytes += model;
  return hash(bytes);
}

Json gepa_reflection_rejections(const Json &artifact, const Json &feedback,
                                const Json &control_learning,
                                const std::string &evaluation_context) {
  if (artifact.str("level") == "L0" || !feedback["records"].object()) return Json();
  const auto snapshot = proto<v1::RuntimePolicy>(artifact["policy"]).snapshot();
  require(feedback.str("user_id") == snapshot.identity().user_id(),
          "gepa_memory_feedback_identity_invalid");
  Search compiler;
  compiler.seed = artifact;
  compiler.controls = control_learning;
  const auto values = compiler.components(artifact);
  std::vector<Json> eligible;
  for (const auto &entry : feedback["records"].fields()) {
    const auto &record = entry.second;
    if ((record.str("status") != "regressed" && record.str("status") != "conflicting") ||
        record.str("split") != "train" ||
        record.str("comparison_context") != evaluation_context ||
        !record.num("negative_task_count", 0) || !record["before"].object() ||
        record["before"].fields().empty() ||
        !gepa_feedback_sources_match(record, snapshot) ||
        !gepa_feedback_context_matches(record, values)) continue;
    bool before_matches = true, after_matches = true;
    for (const auto &component : record["before"].fields()) {
      before_matches &= values.has(component.first) &&
                        values[component.first].dump() == component.second.dump();
      after_matches &= values.has(component.first) &&
                       values[component.first].dump() == record["after"][component.first].dump();
    }
    if (!before_matches && !after_matches) continue;
    eligible.push_back(Json{{"id", entry.first}, {"status", record["status"]},
        {"attribution", record["attribution"]}, {"before", record["before"]},
        {"after", record["after"]},
        {"background_fingerprint", hash(record["background"].dump())},
        {"positive_tasks", record.num("positive_task_count", 0)},
        {"negative_tasks", record.num("negative_task_count", 0)}});
  }
  std::sort(eligible.begin(), eligible.end(), [](const auto &a, const auto &b) {
    if (a.num("negative_tasks", 0) != b.num("negative_tasks", 0))
      return a.num("negative_tasks", 0) > b.num("negative_tasks", 0);
    return a.str("id") < b.str("id");
  });
  Json result{{"schema_version", 1}, {"user_id", snapshot.identity().user_id()},
              {"evaluation_context", evaluation_context}, {"records", Json::Array{}}};
  for (const auto &record : eligible) {
    auto bounded = result;
    bounded["records"].push(record);
    if (bounded.dump().size() > 16384) continue;
    result = std::move(bounded);
    if (result["records"].items().size() == 4) break;
  }
  validate_local_data(result);
  return result["records"].items().empty() ? Json() : result;
}

Json apply_gepa_memory_feedback(const Json &artifact, const Json &feedback,
                                const Json &control_learning) {
  if (artifact.str("level") == "L0" || !feedback["records"].object()) return artifact;
  const auto snapshot = proto<v1::RuntimePolicy>(artifact["policy"]).snapshot();
  require(feedback.str("user_id") == snapshot.identity().user_id(),
          "gepa_memory_feedback_identity_invalid");
  Search compiler;
  compiler.seed = artifact;
  compiler.controls = control_learning;
  compiler.config = Json{{"max_experience_bytes", 8192}};
  auto result = artifact;
  std::set<std::string> visited{compiler.components(result).dump()};
  std::vector<std::pair<std::string, Json>> eligible;
  for (const auto &record : feedback["records"].fields())
    if (record.second.str("status") == "supported" &&
        gepa_feedback_sources_match(record.second, snapshot)) eligible.push_back(record);
  std::sort(eligible.begin(), eligible.end(), [](const auto &a, const auto &b) {
    if (a.second.num("positive_task_count", 0) != b.second.num("positive_task_count", 0))
      return a.second.num("positive_task_count", 0) > b.second.num("positive_task_count", 0);
    return a.first < b.first;
  });
  // 逐个应用可匹配的增量，允许继承一条经过多轮改进的链；循环或旧来源不能复活。
  for (std::size_t round = 0; round < eligible.size(); ++round) {
    bool changed = false;
    for (const auto &item : eligible) {
      auto values = compiler.components(result);
      bool matches = item.second["before"].object() && !item.second["before"].fields().empty() &&
                     gepa_feedback_context_matches(item.second, values);
      for (const auto &component : item.second["before"].fields())
        matches &= values.has(component.first) &&
                   values[component.first].dump() == component.second.dump();
      if (!matches) continue;
      for (const auto &component : item.second["after"].fields())
        values[component.first] = component.second;
      if (visited.count(values.dump())) continue;
      try {
        const auto candidate = compiler.compose(result, values);
        const auto compiled = compiler.components(candidate).dump();
        if (visited.count(compiled)) continue;
        visited.insert(compiled);
        result = candidate;
        changed = true;
        break;
      } catch (const Error &) {
        // 运行能力、授权或基础预算改变后，旧修订不再适用；保留可运行的原种子。
      }
    }
    if (!changed) break;
  }
  return result;
}

// 驱动多轮候选生成、训练反馈、验证选择和冻结留出评测。
// 所有阶段受同一预算和取消信号约束，缓存必须匹配数据、宿主与评分版本。
GepaResult run_gepa_search(const Json &seed, const Json &baseline, const fs::path &dataset,
                           const fs::path &evaluation_path, const Json &evaluation,
                           const Json &config, const Json &gate, const Json &controls,
                           const fs::path &output, const std::string &context,
                           const std::string &evaluation_context,
                           const std::string &candidate_evaluation_context,
                           const std::function<bool()> &cancelled,
                           std::shared_ptr<legacy::ModelAdapter> reflection_model,
                           std::size_t final_holdout_top_k_override, bool final_holdout_replay,
                           const Json &reflection_history,
                           const std::string &final_holdout_execution_context) {
  Search search;
  search.seed = seed;
  search.baseline = baseline;
  search.dataset_path = dataset;
  search.evaluation_path = evaluation_path;
  search.evaluation = evaluation;
  search.config = config;
  search.gate = gate;
  search.controls = controls;
  search.root = output;
  search.context = context;
  search.evaluation_context = evaluation_context;
  search.candidate_evaluation_context = candidate_evaluation_context;
  search.cancelled = cancelled;
  search.model = std::move(reflection_model);
  search.final_holdout_top_k_override = final_holdout_top_k_override;
  search.final_holdout_replay = final_holdout_replay;
  search.final_holdout_execution_context = final_holdout_execution_context;
  search.reflection_history = reflection_history;
  try {
    return search.run();
  } catch (const std::exception &exc) {
    if (search.tree.str("context") == context && search.tree["nodes"].array()) {
      const std::string code = exc.what();
      search.tree["status"] = code == "gepa_cancelled" ? "cancelled" : "error";
      const std::set<std::string> known{"gepa_cancelled",
                                        "gepa_time_budget_exhausted",
                                        "gepa_search_time_reserved",
                                        "gepa_model_rate_limited",
                                        "gepa_baseline_evaluation_inconclusive",
                                        "gepa_seed_evaluation_inconclusive",
                                        "gepa_model_not_configured",
                                        "gepa_model_key_missing"};
      search.tree["error"] = known.count(code) ? code : "gepa_search_failed";
      search.tree["finished_ms"] = now();
      search.save();
    }
    throw;
  }
}
} // 命名空间结束：trajectory::evolution::integration
