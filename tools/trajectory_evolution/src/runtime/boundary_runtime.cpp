// 边界运行时：维护 run/step/action 的生命周期与策略应用证据。
// 打点既采集事实又可能返回策略决策，必须区分建议输入、实际输入和实际执行结果。
// 异步完成由显式终点确认，不能用派发回执关闭整个 run。

#include "boundary_runtime.h"
#include "model_contract.h"
#include "retrieval_text.h"
#include "failure_ledger.h"
#include "schedule_evidence.h"

#include "trajectory_evolution/evolution.h"
#include "trajectory_evolution/request_binding.h"
#include "trajectory_evolution/orchestration.h"

#include <chrono>
#include <set>
#include <openssl/sha.h>
#include <rapidjson/pointer.h>

namespace trajectory::evolution::integration {
namespace {
// 计算稳定内容摘要，供请求标识、缓存或版本比较使用。
std::string hash(const std::string &value) {
  unsigned char bytes[SHA256_DIGEST_LENGTH];
  require(SHA256(reinterpret_cast<const unsigned char *>(value.data()), value.size(), bytes),
          "hash_failed", 500);
  std::string result;
  constexpr char hex[] = "0123456789abcdef";
  for (auto b : bytes) {
    result += hex[b >> 4];
    result += hex[b & 15];
  }
  return result;
}

// 校验或编码单段身份字段，供当前模块构造隔离路径或稳定标识。
std::string component(const std::string &value) {
  std::string result;
  constexpr char hex[] = "0123456789ABCDEF";
  for (unsigned char c : value) {
    if ((std::isalnum(c) && c < 128) || c == '-' || c == '_' || c == '.')
      result += c;
    else {
      result += '%';
      result += hex[c >> 4];
      result += hex[c & 15];
    }
  }
  return result;
}

// 把局部作用域绑定到完整应用身份，避免不同运行的短名称冲突。
std::string qualified_scope(const Json &config, const std::string &scope) {
  return "agent:" + component(config.str("agent_id")) + "/actor:" + component(scope);
}

// 检查运行是否进入终态；终态之后的迟到事件不能重新启动同一个执行实例。
bool ended(const Json &value) {
  return value.flag("ended", false);
}

// 提取当前执行进度供控制规则判断，缺失状态保持未知而非猜测已完成。
Json progress_state(Json value) {
  auto normalize_message = [](Json &message) {
    if (message.str("role") != "tool" || !message.has("content"))
      return;
    // 该标识属于传输信封，而非工具业务结果。
    // 绝不能删除载荷内部的业务 ID、时间戳或字段。
    message.fields().erase("tool_call_id");
    if (message["content"].string()) {
      try {
        const auto parsed = Json::parse(message["content"].text());
        if (parsed.object() || parsed.array())
          message["content"] = parsed;
      } catch (...) { /* 纯文本工具结果仍是有效观察。 */
      }
    }
  };
  normalize_message(value);
  if (value.has("last_result"))
    normalize_message(value["last_result"]);
  return value;
}

// 根据真实动作回执识别失败，不使用查询命中或模型描述替代执行结果。
bool failed_action(const Json &request) {
  if (request.str("status") == "failed")
    return true;
  if (!request.flag("executed", false))
    return false;
  auto output = request["output"];
  if (output.string()) {
    if (output.text().size() > 65536)
      return false;
    try {
      output = Json::parse(output.text());
    } catch (...) {
      return false;
    }
  }
  if (!output.object())
    return false;
  const auto &status = output["status"];
  if (status.string() &&
      (status.text() == "failed" || status.text() == "error" || status.text() == "rejected" ||
       status.text() == "tool_error" || status.text() == "security_policy_blocked" ||
       status.text() == "timeout" || status.text() == "blocked" || status.text() == "denied"))
    return true;
  if (std::holds_alternative<bool>(output["ok"].value) && !output["ok"].boolean())
    return true;
  if (std::holds_alternative<bool>(output["success"].value) && !output["success"].boolean())
    return true;
  if (std::holds_alternative<double>(output["exit_code"].value) &&
      std::get<double>(output["exit_code"].value) != 0)
    return true;
  // 已声明成功的结果可以附带历史错误，不能因此恢复已结束的失败链。
  if ((status.string() && (status.text() == "completed" || status.text() == "success" ||
                           status.text() == "succeeded")) ||
      (std::holds_alternative<bool>(output["success"].value) && output["success"].boolean()) ||
      (std::holds_alternative<bool>(output["ok"].value) && output["ok"].boolean()) ||
      (std::holds_alternative<double>(output["exit_code"].value) &&
       std::get<double>(output["exit_code"].value) == 0))
    return false;
  const auto &error = output["error"];
  if ((error.string() && !error.text().empty()) || (error.object() && !error.fields().empty()) ||
      (std::holds_alternative<bool>(error.value) && error.boolean()))
    return true;
  // 不解释自由文本；这种观察不改变工作流状态。
  return false;
}

// 判断状态或事件是否已经终结，供收尾与重复回执处理使用。
std::string terminal(const Json &request) {
  const auto status = request.str("status");
  require(status == "completed" || status == "failed" || status == "cancelled" ||
              status == "unknown",
          "status_invalid");
  return status;
}

// 提取保留的应用扩展字段，组件不解释业务私有结构。
Json extensions(const Json &request) {
  require(!request.has("extensions") || request["extensions"].object(),
          "extensions_object_required");
  return request.has("extensions") ? request["extensions"] : Json(Json::Object{});
}

// 构造当前查询的有界文本，减少无关历史对检索的干扰。
std::string query_excerpt(const Json &value, std::size_t limit) {
  const auto text = value.string() ? value.text() : value.dump();
  if (text.size() <= limit)
    return text;
  const std::string marker = "\n[query excerpt]\n";
  std::size_t head = (limit - marker.size()) / 2,
              tail = text.size() - (limit - marker.size() - head);
  while (head && (static_cast<unsigned char>(text[head]) & 0xc0) == 0x80)
    --head;
  while (tail < text.size() && (static_cast<unsigned char>(text[tail]) & 0xc0) == 0x80)
    ++tail;
  return text.substr(0, head) + marker + text.substr(tail);
}

// 生成当前控制或采集作用域的稳定身份。
std::string scope_id(const Json &request, const Json &config) {
  const auto scope = request.str("scope_id", config["actors"].items().front().text());
  bool found = false;
  for (const auto &item : config["actors"].items())
    found |= item.text() == scope;
  require(found, "policy_scope_not_authorized", 403);
  return scope;
}

// 构造事件所需运行、父子和因果上下文，保证异步回执可关联。
Json event_context(const Json &item) {
  Json result{{"actor_id", item["actor_id"]},
              {"execution_id", item["stream_id"]},
              {"step_id", item["step_id"]}};
  if (item.has("loop_id")) {
    result["loop_id"] = item["loop_id"];
    result["iteration"] = item["iteration"];
  }
  return result;
}

// 读取当前入口的字段绑定选项，不改变冻结策略授权范围。
v1::RuntimeOptions binding_options(const Json &binding) {
  keys(binding, {"pointer", "mode"});
  require(binding.has("pointer") && binding.has("mode"), "model_binding_required");
  const auto pointer = binding.str("pointer"), mode = binding.str("mode");
  require(pointer.empty() || pointer.front() == '/', "binding_pointer_invalid");
  v1::RuntimeOptions options;
  // 私有信封允许任意 JSON 模型参数，包括裸字符串。
  options.set_request_context_pointer("/payload" + pointer);
  options.set_request_context_mode(mode);
  validate_request_binding(options);
  return options;
}

// 组织当前活动的查询状态，用于规则匹配，避免其他并行分支的状态串入。
std::string activity_query_state(const Json &request, const std::string &semantic,
                                 const Json &config, const Json &binding) {
  if (!request["state"].null())
    return bounded_query_text(request["state"], config, false);
  if (semantic != "model" || binding.str("mode") != "system_message")
    return bounded_query_text(request["input"], config, false);
  // 必须遵循配置的绑定路径，不能假定某个供应商特有的 messages 字段。
  const auto path = binding.str("pointer");
  const rapidjson::Pointer pointer(path.c_str(), path.size());
  const Json *value = &request["input"];
  for (std::size_t i = 0; value && i < pointer.GetTokenCount(); ++i) {
    const auto &token = pointer.GetTokens()[i];
    if (value->object()) {
      const auto found = value->fields().find(std::string(token.name, token.length));
      value = found == value->fields().end() ? nullptr : &found->second;
    } else if (value->array() && token.index != rapidjson::kPointerInvalidIndex &&
               token.index < value->items().size()) {
      value = &value->items()[token.index];
    } else
      value = nullptr;
  }
  Json messages = Json::Array{};
  if (value && value->array()) {
    // 检索只面向当前请求及最近的 assistant/tool 片段。
    // 分支已转入另一动作后，已消费的历史结果不能反复触发经验。
    // 模型和轨迹仍保留完整输入，明确的宿主状态始终具有最高优先级。
    // 这使检索窗口收缩不会改写真实的业务上下文。
    std::size_t latest_user = value->items().size(), segment = 0;
    for (std::size_t i = 0; i < value->items().size(); ++i) {
      const auto &role = value->items()[i]["role"];
      if (!role.string())
        continue;
      if (role.text() != "user" && role.text() != "assistant")
        continue;
      // 纯提示消息没有可检索观察，不能截断它之前尚待消费的真实工具结果。
      // 混有实际用户内容时仍建立新边界；过滤规则与后续检索投影保持一致。
      if (retrieval_text(value->items()[i]).find_first_not_of(" \t\r\n") == std::string::npos)
        continue;
      if (role.text() == "user") {
        latest_user = i;
        segment = i;
      } else if (role.text() == "assistant")
        segment = i;
    }
    if (latest_user < segment)
      messages.push(value->items()[latest_user]);
    for (std::size_t i = segment; i < value->items().size(); ++i) {
      const auto &message = value->items()[i];
      const auto &role = message["role"];
      if (role.string() && (role.text() == "system" || role.text() == "developer"))
        continue;
      messages.push(message);
    }
  }
  return bounded_query_text(messages, config, false);
}

// 单个 run 的可恢复边界状态机：跨语言打点最终在此收敛为 events、activities、loops 与 controls。
// 它只记录宿主已经观察到的事实；策略建议必须经 activity/loop 的提交打点才能改变运行状态。
class Runtime {
public:
  Json &run;
  const Json &config;
  const PolicyRuntime *cached_policy;

  // 初始化 Runtime 的状态和资源；输入参数的校验与所有权在构造阶段确定。
  // 对象创建后复用既有配置，实际执行与收尾分别由后续接口负责。
  Runtime(Json &value, const Json &configuration, const PolicyRuntime *policy = nullptr)
      : run(value), config(configuration), cached_policy(policy) {}

  // 将当前事实写为标准事件，保留身份和因果链接，不把建议冒充执行。
  std::string emit(const std::string &kind, Json fields, const std::string &producer,
                   Json causes = Json::Array{}) {
    // event_id 在 run 内单调递增；producer 的 sequence 在各执行流内独立递增，二者共同支持并发回放。
    require(run["events"].items().size() < config.num("max_events", 10000),
            "event_capacity_reached", 507);
    require(causes.items().size() <= 64, "too_many_causes");
    const auto id = "event-" + std::to_string(run["events"].items().size() + 1);
    const auto sequence = run["sequences"].num(producer, 0) + 1;
    run["sequences"][producer] = sequence;
    fields["event_id"] = id;
    fields["kind"] = kind;
    fields["producer_id"] = producer;
    if (!fields.has("actor_id"))
      fields["actor_id"] = "unspecified";
    fields["sequence"] = std::to_string(sequence);
    fields["timestamp_us"] = std::to_string(std::chrono::duration_cast<std::chrono::microseconds>(
                                                std::chrono::system_clock::now().time_since_epoch())
                                                .count());
    if (!causes.items().empty())
      fields["caused_by"] = causes;
    run["events"].push(fields);
    return id;
  }

  // 收集真实前因事件，不能以时间先后代替因果依赖。
  Json causes(const Json &request) const {
    Json result = Json::Array{};
    std::set<std::string> unique;
    if (!request.has("caused_by"))
      return result;
    require(request["caused_by"].items().size() <= 64, "too_many_causes");
    for (const auto &id : request["caused_by"].items()) {
      bool exists = false;
      for (const auto &event : run["events"].items())
        exists |= event.str("event_id") == id.text();
      require(exists, "causal_event_not_found", 409);
      if (unique.insert(id.text()).second)
        result.push(id);
    }
    return result;
  }

  // 汇聚已完成分支的因果依赖，完成顺序不应改变父节点的身份或结束条件。
  Json join(Json links, const std::string &producer) {
    std::set<std::string> unique;
    auto &values = links.items();
    values.erase(std::remove_if(values.begin(), values.end(),
                                [&](const auto &id) { return !unique.insert(id.text()).second; }),
                 values.end());
    while (links.items().size() > 64) {
      Json part = Json::Array{};
      for (unsigned i = 0; i < 64; ++i) {
        part.push(links.items().back());
        links.items().pop_back();
      }
      links.push(
          emit("custom", Json{{"attributes", Json{{"type", "boundary.join"}}}}, producer, part));
    }
    return links;
  }

  // 为策略构造有界、分支隔离的查询快照；该快照不替代宿主保存的完整状态。
  v1::RuntimeQuery query(const Json &item, const std::string &boundary, const std::string &id,
                         const Json &state) const {
    v1::RuntimeQuery q;
    *q.mutable_advice_query()->mutable_identity() = proto<v1::Identity>(run["identity"]);
    q.mutable_advice_query()->set_target_scope(qualified_scope(config, item.str("scope_id")));
    q.mutable_advice_query()->set_query_id("query-" + id);
    q.mutable_advice_query()->set_decision_id("decision-" + id);
    if (run.num("level", 0) > 0) {
      // 只有供检索的视图受长度限制；原始请求和完整轨迹保持不变。
      q.mutable_advice_query()->set_task(bounded_query_text(run["input"], config, true));
      q.mutable_advice_query()->set_state(bounded_query_text(state, config, false));
      if (boundary == "llm" && item.has("observation_scope")) {
        const auto &operations =
            static_cast<const Json &>(run)["observed_operations"][item.str("observation_scope")];
        if (operations.object())
          for (const auto &operation : operations.fields())
            q.add_observed_operations(operation.first);
      }
    }
    q.set_execution_id(item.str("stream_id"));
    q.set_boundary(boundary);
    q.set_actor_id(item.str("actor_id"));
    return q;
  }

  // 基于当前状态和固定策略计算控制决定，结果需要对应的应用回执确认。
  v1::RuntimeDecision decide(const Json &item, v1::RuntimeQuery q) {
    const auto decision = cached_policy
                              ? cached_policy->decide(q)
                              : PolicyRuntime(proto<v1::RuntimePolicy>(run["policy"])).decide(q);
    auto fields = event_context(item);
    fields["attributes"] = Json{{"type", "policy.query"}, {"decision", json(decision)}};
    emit("custom", fields, item.str("step_id"));
    return decision;
  }

  // 构造或解析标准回执，使宿主可以区分建议、应用、降级和业务结果。
  void receipt(const Json &item, const Json &stored, const v1::RuntimeObservation &observed,
               const Json &binding = Json{}) {
    if (stored.null())
      return;
    std::unique_ptr<PolicyRuntime> owned;
    if (!cached_policy)
      owned = std::make_unique<PolicyRuntime>(proto<v1::RuntimePolicy>(run["policy"]));
    const auto &runtime = cached_policy ? *cached_policy : *owned;
    const auto q = proto<v1::RuntimeQuery>(stored);
    const auto result = binding.null() ? runtime.verify(q, observed)
                                       : runtime.verify(q, observed, binding_options(binding));
    auto fields = event_context(item);
    fields["application"] = json(result.application());
    fields["attributes"] = Json{{"receipt", json(result)}};
    if (!binding.null())
      fields["attributes"]["request_binding"] = binding;
    emit("advice.applied", fields, item.str("step_id"));
    run["receipt_counts"][result.application().status()] =
        run["receipt_counts"].num(result.application().status(), 0) + 1;
  }

  // 只在宿主报告真实结束后写入循环账本；派发建议本身不能被当作业务结果。
  void record_action_outcome(const Json &item, const Json &request, bool failed) {
    const auto status = request.str("status");
    if (request.flag("executed", false) && (status == "completed" || status == "failed") &&
        !item.flag("delegates_schedule", false) && item["target_attempts"].object()) {
      auto attempts = item["target_attempts"];
      if (request.has("actual_input")) {
        const auto actual = target_failure_snapshot(run["policy"]["options"],
                                                    qualified_scope(config, item.str("scope_id")),
                                                    item.str("name"), request["actual_input"],
                                                    run["target_failures"], item.str("stream_id"));
        for (auto it = attempts.fields().begin(); it != attempts.fields().end();)
          if (actual[it->first].str("key") != it->second.str("key"))
            it = attempts.fields().erase(it);
          else
            ++it;
      }
      const auto &output = request.has("progress_output") ? request["progress_output"]
                                                          : request["output"];
      // 结果中的结构化错误码优先；结果未携带码时，才采用宿主显式声明的 ErrorInfo。
      // 不从自然语言猜码，也不改变执行实例边界，避免终态失败误伤并发兄弟分支。
      const auto &failure_output = structured_error_code(output).empty() && request.has("error")
                                       ? request["error"] : output;
      settle_target_failure(run["target_failures"], attempts, failed, failure_output);
    }
    if (!item.has("loop_id"))
      return;
    auto &loop = run["loops"][item.str("loop_id")];
    if (failed)
      loop["failed_actions"] = loop.num("failed_actions", 0) + 1;
    // 保留真实的派发错误，但业务进展由其子动作的真实结果衡量。
    if (item.flag("delegates_schedule", false))
      return;
    if (!request.flag("executed", false) &&
        ((item.str("decision") == "reject" && item["query"].str("boundary") == "actions") ||
         status == "failed")) {
      // 策略拦截或宿主明确报告的执行前失败都没有业务进展；后者不伪造策略回执。
      // 不计入实际工具尝试，也不把普通取消、未知状态当作失败。
      // 同轮其他分支有新结果时 outcome_revision 会前进，不会因此全局停止。
      loop["observed_actions"] = loop.num("observed_actions", 0) + 1;
      loop["rejected_actions"] = loop.num("rejected_actions", 0) + 1;
      return;
    }
    if (!request.flag("executed", false) || (status != "completed" && status != "failed"))
      return;
    // 观察真实业务结果，而不是下一条消息里的传输 ID。
    // 保留全部业务字段：参数或结果发生变化都意味着进展。
    const auto outcome =
        progress_state(Json{{"role", "tool"},
                            {"content", request.has("progress_output") ? request["progress_output"]
                                                                       : request["output"]}});
    const auto signature = hash(Json{
        {"operation", item["name"]},
        {"input", request.has("actual_input") ? request["actual_input"] : item["input"]},
        {"status", status},
        {"output", outcome}}.dump());
    const auto operation = item.str("name");
    if (!loop["action_outcomes"].object())
      loop["action_outcomes"] = Json::Object{};
    if (!loop["action_outcomes"].has(signature)) {
      if (loop["action_outcomes"].fields().size() < 256)
        loop["action_outcomes"][signature] = true;
      else
        loop["action_outcomes_overflow"] = true;
      loop["outcome_revision"] = loop.num("outcome_revision", 0) + 1;
    }
    loop["observed_actions"] = loop.num("observed_actions", 0) + 1;
    // 失败进度与参数、传输文本分离。
    // 同一结算批次中的成功动作仍是有效进展。
    if (!loop["operation_failures"].object())
      loop["operation_failures"] = Json::Object{};
    if (!failed) {
      loop["operation_failures"].fields().erase(operation);
      // 成功意味着当前执行有进展；旧故障码不能污染后续无关步骤。
      for (auto &entry : loop["operation_failures"].fields())
        if (entry.second.object())
          entry.second.fields().erase("error_code");
      loop["last_successful_action"] = loop["observed_actions"];
    } else if (loop["operation_failures"].has(operation) ||
               loop["operation_failures"].fields().size() < 64) {
      auto &failure = loop["operation_failures"][operation];
      if (!failure.has("epoch"))
        failure["epoch"] = loop["observed_actions"];
      const auto prior = item.num("prior_failures", failure.num("count", 0));
      const bool same_streak =
          item.num("failure_epoch", failure.num("epoch", 0)) == failure.num("epoch", 0);
      // 并行尝试看见的是同一份先验失败，彼此并不是重试。
      // 其中任一成功也可能已经重置旧的失败链。
      failure["count"] = std::min<std::size_t>(
          10000, std::max(failure.num("count", 0), same_streak ? prior + 1 : std::size_t(1)));
      failure["last_action"] = loop["observed_actions"];
      auto code = structured_error_code(request.has("progress_output") ? request["progress_output"]
                                                                       : request["output"]);
      if (code.empty() && request.has("error"))
        code = structured_error_code(request["error"]);
      failure["error_code"] = code;
    }
  }

  // begin 只准备输入、记录策略决定并发出开始事件；动作是否执行及其结果必须由 end/abort 回写。
  Json begin_activity(const Json &request) {
    keys(request, {"operation", "request_id", "run_id", "kind", "name", "input", "actor_id",
                   "scope_id", "stream_id", "control_id", "state", "binding", "caused_by",
                   "extensions", "apply_policy"});
    const bool apply_policy = request.flag("apply_policy", true);
    const auto kind = request.str("kind"), name = request.str("name"),
               id = "activity-" + hash(request.str("request_id")).substr(0, 28);
    require(identifier(kind) && identifier(name) && request.has("input"),
            "activity_fields_required");
    std::string semantic = kind == "model" || kind == "action" ? kind : "custom";
    if (config["activity_types"].has(kind)) {
      const auto &type = config["activity_types"][kind];
      semantic = type.str("semantic");
      if (type.has("required_input_paths")) {
        rapidjson::Document data;
        const auto raw = request["input"].dump();
        data.Parse(raw.data(), raw.size());
        for (const auto &path : type["required_input_paths"].items())
          require(rapidjson::Pointer(path.text().c_str()).Get(data) != nullptr,
                  "required_input_field_missing");
      }
    }
    Json item{{"activity_id", id},
              {"step_id", "step-" + id},
              {"actor_id", request.str("actor_id", "unspecified")},
              {"scope_id", scope_id(request, config)},
              {"stream_id", request.str("stream_id", id)},
              {"kind", kind},
              {"semantic", semantic},
              {"name", name},
              {"input", request["input"]},
              {"decision", "allow"},
              {"ended", false}};
    require(identifier(item.str("actor_id")) && identifier(item.str("stream_id")),
            "activity_identity_invalid");
    Json links = causes(request);
    if (request.has("control_id")) {
      const auto control = request.str("control_id");
      require(run["controls"].has(control), "control_not_found", 404);
      const auto &c = run["controls"][control];
      const auto &loop = run["loops"][c.str("loop_id")];
      const bool finalizing = loop.flag("finalizing", false);
      require(c.flag("committed", false) && (c.str("actual_decision") != "stop" || finalizing),
              "control_does_not_allow_activity", 409);
      require(!loop.flag("stopped", false) || finalizing, "loop_already_stopped", 409);
      if (finalizing) {
        // 收尾不恢复执行预算：只允许一次模型答复，动作在边界直接拒绝。
        item["finalizing"] = true;
        if (semantic != "model" || loop.flag("final_model_started", false))
          item["decision"] = "reject";
      }
      require(item.str("scope_id") == c.str("scope_id"), "control_scope_mismatch", 409);
      item["control_id"] = control;
      item["loop_id"] = c["loop_id"];
      item["iteration"] = c["iteration"];
      if (!request.has("stream_id"))
        item["stream_id"] = c["stream_id"];
      links.push(c["commit_event"]);
    }
    // 工具契约只在当前任务/分支内生效，其他用户轨迹或并发同级分支均不能为本活动授权。
    // 这样才能避免跨分支错误复用工具权限。
    const auto &ref = request["extensions"]["point_reference"];
    const auto branch = ref.object()                ? ref.str("parent", "root")
                        : request.has("control_id") ? request.str("control_id")
                                                    : request["caused_by"].dump();
    item["contract_scope"] =
        hash(item.str("scope_id") + "\n" + item.str("actor_id") + "\n" + branch);
    // 复用包维护的打点祖先关系或显式执行流。
    // 匿名活动不能继承并发同级分支的证据。
    if (ref.object() || request.has("stream_id") || item.has("loop_id")) {
      const auto observation_branch = ref.object()          ? ref.str("parent", "root")
                                      : item.has("loop_id") ? item.str("loop_id")
                                                            : item.str("stream_id");
      item["observation_scope"] =
          hash(item.str("scope_id") + "\n" + item.str("actor_id") + "\n" + observation_branch);
      item["observation_cursor"] = run["events"].items().size();
    }
    auto fields = event_context(item);
    fields["input"] = request["input"];
    fields["operation"] = name;
    fields["attributes"] =
        Json{{"type", "boundary.activity"},  {"activity_kind", kind},
             {"semantic", semantic},         {"policy_scope", item["scope_id"]},
             {"apply_policy", apply_policy}, {"extensions", extensions(request)}};
    item["start_event"] = emit("step.start", fields, item.str("step_id"), join(links, id));
    Json prepared = request["input"];
    std::string reason;
    if (item.str("decision") == "reject")
      reason = "loop_finalization_only";
    if (semantic == "model") {
      const auto binding =
          request.has("binding") ? request["binding"] : infer_model_binding(prepared);
      require(!apply_policy || run.num("level", 0) == 0 || !binding.null(),
              "model_binding_required");
      if (!binding.null()) {
        binding_options(binding);
        item["binding"] = binding;
      }
    }
    bool contract_failed = false;
    if (semantic == "action") {
      item["target_attempts"] = target_failure_snapshot(
          run["policy"]["options"], qualified_scope(config, item.str("scope_id")), name, prepared,
          run["target_failures"], item.str("stream_id"));
      if (item.has("loop_id")) {
        const auto &failure = static_cast<const Json &>(
            run)["loops"][item.str("loop_id")]["operation_failures"][name];
        item["prior_failures"] = failure.num("count", 0);
        item["failure_epoch"] = failure.num("epoch", 0);
      }
      const auto &contracts =
          static_cast<const Json &>(run)["tool_contracts"][item.str("contract_scope")];
      item["contract_check"] = check_tool_contract(contracts, name, prepared);
      contract_failed = apply_policy && run.num("level", 0) >= 2 &&
                        item["contract_check"].str("status") == "failed";
      item["contract_check"]["enforced"] = contract_failed;
      if (contract_failed) {
        item["decision"] = "reject";
        reason =
            item["contract_check"].str("reason") + ": " + item["contract_check"]["missing"].dump();
      }
    }
    if (apply_policy && !contract_failed && !item.flag("finalizing", false) &&
        semantic != "custom" && run.num("level", 0) > 0) {
      auto q = query(item, semantic == "model" ? "llm" : "actions", id,
                     activity_query_state(request, semantic, config, item["binding"]));
      if (semantic == "action") {
        auto *action = q.add_actions();
        action->set_action_id(id);
        action->set_operation(name);
        if (item.has("loop_id"))
          action->set_prior_failures(item.num("prior_failures", 0));
        *action->mutable_arguments() = proto<google::protobuf::Struct>(
            prepared.object() ? prepared : Json{{"value", prepared}});
        for (const auto &attempt : item["target_attempts"].fields())
          (*action->mutable_target_failures())[attempt.first] = attempt.second.num("count", 0);
        const auto &contracts =
            static_cast<const Json &>(run)["tool_contracts"][item.str("contract_scope")];
        if (contracts.object() && contracts[name].object())
          for (const auto &key : contracts[name]["required"].items())
            action->add_schema_required_arguments(key.text());
      }
      const auto decision = decide(item, q);
      item["query"] = json(q);
      if (semantic == "model") {
        const auto options = binding_options(item["binding"]);
        auto wrapped = proto<google::protobuf::Struct>(Json{{"payload", prepared}});
        wrapped = inject_request_context(wrapped, decision.context(), options);
        if (item.has("control_id")) {
          const auto &control = run["controls"][item.str("control_id")];
          if (control.str("actual_decision") == "replan")
            wrapped = inject_request_context(wrapped, control.str("context"), options);
        }
        prepared = json(wrapped)["payload"];
      } else {
        item["decision"] = decision.actions(0).decision();
        reason = decision.actions(0).reason();
      }
    }
    if (item.flag("finalizing", false) && semantic == "model" && item.str("decision") != "reject") {
      // 使用既有模型请求绑定；不改写业务历史，不再检索或注入新经验。
      const auto options = binding_options(item["binding"]);
      auto wrapped = proto<google::protobuf::Struct>(Json{{"payload", prepared}});
      // 有冻结版本时只读取当前运行的模板，避免并发运行或部署配置改变策略行为。
      wrapped = inject_request_context(
          wrapped, run.has("finalization_context") ? run.str("finalization_context")
                                                   : config.str("finalization_context"),
          options);
      prepared = json(wrapped)["payload"];
      // 通用 messages 协议的工具声明；其他协议仍由动作边界阻止执行。
      if (prepared.object()) {
        prepared.fields().erase("tools");
        prepared.fields().erase("tool_choice");
        prepared.fields().erase("functions");
        prepared.fields().erase("function_call");
        prepared.fields().erase("parallel_tool_calls");
      }
      run["loops"][item.str("loop_id")]["final_model_started"] = true;
    }
    if (semantic != "custom") {
      fields = event_context(item);
      fields["action_id"] = id;
      fields["operation"] = name;
      fields["input"] = prepared;
      fields["attributes"] = Json{{"phase", "prepared"},
                                  {"activity_kind", kind},
                                  {"semantic", semantic},
                                  {"decision", item["decision"]}};
      fields["attributes"]["metric_tool_call"] =
          request["extensions"]["metrics"].flag("tool_call", true);
      if (item.flag("finalizing", false))
        fields["attributes"]["finalizing"] = true;
      if (item.has("binding"))
        fields["attributes"]["request_binding"] = item["binding"];
      if (item.has("contract_check"))
        fields["attributes"]["contract_check"] = item["contract_check"];
      item["call_event"] = emit("action.start", fields, id, Json::Array{item["start_event"]});
    }
    if (kind == "schedule") {
      require(semantic == "custom" && prepared["actions"].array(), "schedule_input_invalid");
      auto q = query(item, "schedule", id, Json{});
      for (const auto &action : prepared["actions"].items()) {
        auto parsed = proto<v1::RuntimeAction>(action);
        parsed.clear_arguments();
        *q.add_actions() = parsed;
      }
      // 在没有资源规则时也保留有限的候选资源，供后续生成与反事实回放使用。
      project_schedule_evidence(q, prepared);
      v1::SchedulePlan plan;
      if (apply_policy && !item.flag("finalizing", false) && run.num("level", 0) >= 2) {
        plan = decide(item, q).schedule();
        item["query"] = json(q);
      } else
        plan = plan_schedule(v1::RuntimeOptions{}, q);
      auto schedule = begin_schedule(plan);
      if (item.flag("finalizing", false))
        for (auto &action : *schedule.mutable_actions())
          action.set_status("skipped");
      item["schedule"] = json(schedule);
      item["schedule_events"] = Json::Array{};
      item["schedule_skipped"] = Json::Array{};
      // 显式因果父节点是派发信封，而非另一已完成业务动作。
      // 不能从操作名推断这一关系。
      for (auto &entry : run["activities"].fields()) {
        auto &parent = entry.second;
        if (ended(parent) || parent.str("semantic") != "action" ||
            parent.str("loop_id") != item.str("loop_id"))
          continue;
        for (const auto &cause : links.items())
          if (cause.text() == parent.str("start_event") || cause.text() == parent.str("call_event"))
            parent["delegates_schedule"] = true;
      }
    }
    run["activities"][id] = item;
    Json result{{"activity_id", id},
                {"event_id", item["start_event"]},
                {"decision", item["decision"]},
                {"reason", reason},
                {"semantic", semantic},
                {"input", prepared},
                {"context", Json{{"run_id", run["identity"]["run_id"]}, {"activity_id", id}}}};
    if (kind == "schedule") {
      const auto view = schedule_view(item);
      for (const auto &value : view.fields())
        result[value.first] = value.second;
    }
    return result;
  }

  // 提取编排状态的只读视图，供规则查询和回执归因使用。
  Json schedule_view(const Json &item) const {
    const auto state = proto<v1::ScheduleState>(item["schedule"]);
    Json ready = Json::Array{}, skipped = Json::Array{};
    for (const auto &id : ready_schedule(state))
      for (const auto &action : item["input"]["actions"].items())
        if (action.str("action_id") == id)
          ready.push(action);
    for (const auto &action : state.actions())
      if (action.status() == "skipped")
        skipped.push(action.action_id());
    return Json{{"schedule", json(state.plan())},
                {"ready", ready},
                {"skipped", skipped},
                {"finished", schedule_finished(state)}};
  }

  // 编排分支逐项结算：并发动作可任意顺序返回，但每次状态迁移都按同一计划校验前置条件。
  Json observe_schedule(const Json &request) {
    keys(request,
         {"operation", "request_id", "run_id", "activity_id", "input", "caused_by", "extensions"});
    const auto id = request.str("activity_id");
    require(run["activities"].has(id), "activity_not_found", 404);
    auto &item = run["activities"][id];
    require(item.has("schedule") && !ended(item), "schedule_not_active", 409);
    const auto &input = request["input"];
    keys(input, {"action_id", "status", "output"});
    if (input.null() || input.fields().empty())
      return schedule_view(item);
    const auto action_id = input.str("action_id"), status = input.str("status");
    auto state = proto<v1::ScheduleState>(item["schedule"]);
    std::string previous;
    for (const auto &action : state.actions())
      if (action.action_id() == action_id)
        previous = action.status();
    require(!previous.empty(), "schedule_action_not_found", 404);
    if (status == "cancelled" && previous == "waiting") {
      require(input.has("output") && input["output"].has("executed") &&
                  !input["output"]["executed"].boolean(),
              "schedule_cancellation_observation_required");
      // 未派发的取消不制造 running/执行事件，也不占业务失败预算。
      state = advance_schedule(std::move(state), action_id, status);
      auto fields = event_context(item);
      fields["output"] = input["output"];
      fields["status"] = status;
      fields["attributes"] = Json{
          {"type", "schedule.cancelled"}, {"logical_action_id", action_id}, {"executed", false}};
      Json links = causes(request);
      links.push(item["start_event"]);
      item["schedule_end_events"][action_id] =
          emit("custom", fields, item.str("step_id"), join(links, id));
      item["schedule_output_hashes"][action_id] = hash(input["output"].dump());
      item["schedule_events"].push(Json{{"action_id", action_id}, {"status", status}});
      item["schedule"] = json(state);
      auto result = schedule_view(item);
      result["changed"] = true;
      result["event_id"] = item["schedule_end_events"][action_id];
      return result;
    }
    if (status == "skipped") {
      require(previous == "skipped", "schedule_skip_not_authorized", 409);
      bool known = false;
      for (const auto &action : item["schedule_skipped"].items())
        known |= action.text() == action_id;
      if (known) {
        auto result = schedule_view(item);
        result["changed"] = false;
        return result;
      }
      item["schedule_skipped"].push(action_id);
      auto fields = event_context(item);
      fields["attributes"] =
          Json{{"type", "schedule.skipped"}, {"logical_action_id", action_id}, {"executed", false}};
      Json links = causes(request);
      links.push(item["start_event"]);
      for (const auto &action : state.plan().actions())
        if (action.action_id() == action_id)
          for (const auto &dependency : action.after()) {
            const auto &end = item["schedule_end_events"][dependency.action_id()];
            if (end.string())
              links.push(end);
          }
      item["schedule_end_events"][action_id] =
          emit("custom", fields, item.str("step_id"), join(links, id));
    } else {
      state = advance_schedule(std::move(state), action_id, status);
      if (previous == status) {
        require(status == "running" ||
                    (input.has("output") &&
                     item["schedule_output_hashes"].str(action_id) == hash(input["output"].dump())),
                "schedule_outcome_changed", 409);
        auto result = schedule_view(item);
        result["changed"] = false;
        return result;
      }
      const auto event_action_id = id + "-" + hash(action_id).substr(0, 16);
      auto fields = event_context(item);
      fields["action_id"] = event_action_id;
      fields["attributes"] = Json{{"type", "schedule.action"},
                                  {"logical_action_id", action_id},
                                  {"metric_tool_call", false},
                                  {"executed", true},
                                  {"target_scope", qualified_scope(config, item.str("scope_id"))}};
      const Json *scheduled = nullptr;
      for (const auto &action : item["input"]["actions"].items())
        if (action.str("action_id") == action_id)
          scheduled = &action;
      require(scheduled, "schedule_action_not_found", 404);
      fields["operation"] = (*scheduled)["operation"];
      Json links = causes(request);
      if (status == "running") {
        item["schedule_attempts"][action_id]["stream_id"] = event_action_id;
        item["schedule_attempts"][action_id]["target_attempts"] = target_failure_snapshot(
            run["policy"]["options"], qualified_scope(config, item.str("scope_id")),
            fields.str("operation"), (*scheduled)["arguments"], run["target_failures"],
            event_action_id);
        if (item.has("loop_id")) {
          const auto &failure = static_cast<const Json &>(
              run)["loops"][item.str("loop_id")]["operation_failures"][fields.str("operation")];
          item["schedule_attempts"][action_id]["prior_failures"] = failure.num("count", 0);
          item["schedule_attempts"][action_id]["failure_epoch"] = failure.num("epoch", 0);
        }
        links.push(item["start_event"]);
        for (const auto &action : state.plan().actions())
          if (action.action_id() == action_id)
            for (const auto &dependency : action.after()) {
              const auto &end = item["schedule_end_events"][dependency.action_id()];
              if (end.string())
                links.push(end);
            }
        fields["input"] = (*scheduled)["arguments"];
        item["schedule_start_events"][action_id] =
            emit("action.start", fields, event_action_id, join(links, event_action_id));
      } else {
        require(input.has("output"), "schedule_output_required");
        fields["output"] = input["output"];
        fields["status"] = status == "succeeded" ? "completed" : status;
        const auto &executed = input["output"]["executed"];
        const bool ran = previous == "running" &&
                         (!std::holds_alternative<bool>(executed.value) || executed.boolean());
        fields["attributes"]["executed"] = ran;
        if (status == "succeeded" || status == "failed") {
          fields["attributes"]["observed_failure"] = status == "failed";
          auto attempt = item["schedule_attempts"][action_id];
          if (item.has("loop_id"))
            attempt["loop_id"] = item["loop_id"];
          attempt["name"] = fields["operation"];
          attempt["scope_id"] = item["scope_id"];
          attempt["input"] = (*scheduled)["arguments"];
          Json observed{
              {"status", fields["status"]}, {"executed", ran}, {"output", input["output"]}};
          if (request["extensions"].has("progress_output")) {
            observed["progress_output"] = request["extensions"]["progress_output"];
            fields["attributes"]["progress_output_bound"] = true;
          }
          if (status == "failed") {
            const auto code = structured_error_code(
                observed.has("progress_output") ? observed["progress_output"] : observed["output"]);
            if (!code.empty())
              fields["attributes"]["observed_error_code"] = code;
          }
          record_action_outcome(attempt, observed, status == "failed");
        }
        links.push(item["schedule_start_events"][action_id]);
        item["schedule_end_events"][action_id] =
            emit("action.end", fields, event_action_id, join(links, event_action_id));
        item["schedule_output_hashes"][action_id] = hash(input["output"].dump());
      }
      item["schedule_events"].push(Json{{"action_id", action_id}, {"status", status}});
      item["schedule"] = json(state);
    }
    auto result = schedule_view(item);
    result["changed"] = true;
    result["event_id"] = status == "running" ? item["schedule_start_events"][action_id]
                                             : item["schedule_end_events"][action_id];
    return result;
  }

  // end/abort 是写入真实结果、工具契约和故障账本的唯一入口；中断只标记捕获不完整。
  Json end_activity(const Json &request, bool interrupted = false) {
    keys(request, {"operation", "request_id", "run_id", "activity_id", "status", "output",
                   "actual_input", "executed", "error", "extensions", "caused_by", "metrics"});
    const auto id = request.str("activity_id"), status = terminal(request);
    require(run["activities"].has(id), "activity_not_found", 404);
    auto &item = run["activities"][id];
    require(!ended(item), "activity_already_ended", 409);
    require(request.has("output"), "activity_output_required");
    if (item.has("schedule")) {
      const auto state = proto<v1::ScheduleState>(item["schedule"]);
      require(interrupted || schedule_finished(state), "schedule_has_pending_actions", 409);
      if (interrupted)
        for (const auto &action : state.actions())
          if (action.status() == "running")
            observe_schedule(Json{{"operation", "schedule.observe"},
                                  {"activity_id", id},
                                  {"input", Json{{"action_id", action.action_id()},
                                                 {"status", "unknown"},
                                                 {"output", nullptr}}}});
      v1::RuntimeObservation observation;
      if (!interrupted) {
        for (const auto &event : item["schedule_events"].items()) {
          *observation.add_schedule_events() = proto<v1::ScheduledActionState>(event);
          if (event.str("status") == "running")
            observation.add_executed_action_ids(event.str("action_id"));
        }
        for (const auto &action : item["schedule_skipped"].items())
          observation.add_skipped_action_ids(action.text());
      }
      receipt(item, item["query"], observation);
      if (!interrupted)
        run["observed_boundaries"]["schedule"] = true;
    }
    if (interrupted || status == "unknown")
      run["incomplete"] = true;
    const auto semantic = item.str("semantic");
    if (request.has("metrics")) {
      keys(request["metrics"], {"input_tokens", "output_tokens", "tokens", "cached_input_tokens"});
      for (const auto &metric : request["metrics"].fields())
        require(metric.second.integer() <= 1000000000, "metric_value_invalid");
      require(semantic == "model", "model_usage_required");
    }
    if (!interrupted && semantic != "custom") {
      require(request.has("executed"), "execution_observation_required");
      require(request["executed"].boolean() || status != "completed",
              "skipped_activity_cannot_succeed");
      if (semantic == "model")
        require(request.has("actual_input"), "actual_input_required");
    }
    auto fields = event_context(item);
    fields["output"] = request["output"];
    fields["status"] = status;
    fields["attributes"] = Json{{"extensions", extensions(request)}};
    if (semantic == "model" && !interrupted && status == "completed" &&
        request.flag("executed", false)) {
      const auto inferred = infer_model_usage(request["output"]);
      fields["attributes"]["metrics"] = inferred;
      // 显式测量或映射的值优先于从线缆格式推断的默认值。
      if (request.has("metrics"))
        for (const auto &metric : request["metrics"].fields())
          fields["attributes"]["metrics"][metric.first] = metric.second;
      const auto contracts = model_tool_contracts(request["actual_input"]);
      fields["attributes"]["tool_contracts"] = contracts;
      const auto scope = item.str("contract_scope");
      if (run["tool_contracts"].has(scope) || run["tool_contracts"].null() ||
          run["tool_contracts"].fields().size() < 128) {
        auto &stored = run["tool_contracts"][scope];
        if (stored.null())
          stored = contracts;
        else if (stored.object()) {
          for (auto &previous : stored.fields())
            if (!contracts.has(previous.first) ||
                previous.second.dump() != contracts[previous.first].dump())
              previous.second = nullptr;
          for (const auto &entry : contracts.fields())
            if (!stored.has(entry.first))
              stored[entry.first] = entry.second;
          if (stored.fields().size() > 64 || stored.dump().size() > 65536)
            stored = "capacity_exceeded";
        }
      }
    }
    if (request.has("error")) {
      const auto error = proto<v1::ErrorInfo>(request["error"]);
      require(status != "completed" && identifier(error.code()) &&
                  (error.retryability() == "unknown" || error.retryability() == "retryable" ||
                   error.retryability() == "non_retryable"),
              "error_info_invalid");
      fields["error"] = request["error"];
    }
    Json links = causes(request);
    if (semantic != "custom") {
      fields["action_id"] = id;
      fields["attributes"]["semantic"] = semantic;
      if (semantic == "action" && !interrupted) {
        const bool failed = failed_action(request);
        fields["attributes"]["observed_failure"] = failed;
        if (failed) {
          auto code = structured_error_code(request["output"]);
          // 将已校验的显式错误同步到轨迹属性，使在线控制和离线回放使用相同的失败码。
          if (code.empty() && request.has("error"))
            code = structured_error_code(request["error"]);
          if (!code.empty())
            fields["attributes"]["observed_error_code"] = code;
        }
        if (item.flag("delegates_schedule", false))
          fields["attributes"]["delegates_schedule"] = true;
        record_action_outcome(item, request, failed);
      }
      if (request.has("metrics") && !interrupted && request.flag("executed", false))
        for (const auto &metric : request["metrics"].fields())
          fields["attributes"]["metrics"][metric.first] = metric.second;
      if (request.has("actual_input"))
        fields["input"] = request["actual_input"];
      if (!interrupted)
        fields["attributes"]["executed"] = request["executed"];
      links.push(item["call_event"]);
      const auto call_end = emit("action.end", fields, id, join(links, id));
      links = Json::Array{call_end};
      if (item.has("observation_scope") && !interrupted && request.flag("executed", false)) {
        const auto scope = item.str("observation_scope");
        auto &by_scope = run["observed_operations"];
        if (by_scope.null())
          by_scope = Json::Object{};
        if (by_scope.has(scope) || by_scope.fields().size() < 128) {
          auto &operations = by_scope[scope];
          if (operations.null())
            operations = Json::Object{};
          if (semantic == "action" && (status == "completed" || status == "failed")) {
            if (operations.has(item.str("name")) || operations.fields().size() < 64)
              operations[item.str("name")] = run["events"].items().size();
          } else if (semantic == "model" && status == "completed") {
            // 只消费当前请求可见的证据。
            // 并发完成的动作仍留给该分支的下一次请求。
            for (auto it = operations.fields().begin(); it != operations.fields().end();) {
              if (it->second.integer() <= item.num("observation_cursor", 0))
                it = operations.fields().erase(it);
              else
                ++it;
            }
          }
          if (operations.fields().empty())
            by_scope.fields().erase(scope);
        }
      }
      v1::RuntimeObservation observation;
      if (!interrupted) {
        observation.set_call_id(id);
        observation.set_transport_allowed(request["executed"].boolean());
        if (semantic == "model")
          *observation.mutable_request() =
              proto<google::protobuf::Struct>(Json{{"payload", request["actual_input"]}});
        else if (request["executed"].boolean())
          observation.add_executed_action_ids(id);
        else
          observation.add_skipped_action_ids(id);
      }
      receipt(item, item["query"], observation, item["binding"]);
      if (semantic == "model" && item.has("control_id")) {
        auto &control = run["controls"][item.str("control_id")];
        if (control.str("decision") == "replan" && control.str("actual_decision") == "replan") {
          // 并发模型调用对同一次 replan 具有彼此独立的观察结果。
          const auto actual =
              control.str("actual_decision") == "replan" ? observation : v1::RuntimeObservation{};
          receipt(control, control["query"], actual, item["binding"]);
          control["replan_observed"] = true;
        }
      }
      if (!interrupted)
        run["observed_boundaries"][semantic == "model" ? "model" : "actions"] = true;
    } else {
      links.push(item["start_event"]);
      if (item.has("schedule") && item["schedule_end_events"].object())
        for (const auto &event : item["schedule_end_events"].fields())
          if (event.second.string())
            links.push(event.second);
    }
    fields = event_context(item);
    fields["output"] = request["output"];
    fields["status"] = status;
    fields["attributes"] = Json{{"extensions", extensions(request)}};
    item["end_event"] = emit("step.end", fields, item.str("step_id"), join(links, id));
    item["ended"] = true;
    item["status"] = status;
    item["output"] = request["output"];
    return Json{{"ended", true}, {"event_id", item["end_event"]}};
  }

  // check 只快照截至当前的状态；真正消费故障和进度要等宿主通过 commit 确认决定后才发生。
  Json check_loop(const Json &request) {
    keys(request, {"operation", "request_id", "run_id", "loop_id", "state", "actor_id", "scope_id",
                   "caused_by", "extensions", "apply_policy"});
    const bool apply_policy = request.flag("apply_policy", true);
    const auto loop_id = request.str("loop_id"), scope = scope_id(request, config);
    require(identifier(loop_id) && request.has("state"), "loop_fields_required");
    auto &loop = run["loops"][loop_id];
    if (loop.null())
      loop = Json{{"scope_id", scope}, {"iteration", 0}, {"stopped", false}};
    require(loop.str("scope_id") == scope &&
                (!loop.flag("stopped", false) || loop.flag("finalizing", false)),
            "loop_stopped_or_scope_changed", 409);
    Json links = causes(request);
    if (loop.has("control_id")) {
      const auto &previous = run["controls"][loop.str("control_id")];
      require(previous.flag("committed", false), "previous_control_unobserved", 409);
      links.push(previous["commit_event"]);
    }
    const auto id = "control-" + hash(request.str("request_id")).substr(0, 28);
    const auto iteration = loop.num("iteration", 0) + 1;
    Json item{{"step_id", "step-" + id},
              {"stream_id", loop_id},
              {"loop_id", loop_id},
              {"iteration", std::to_string(iteration)},
              {"actor_id", request.str("actor_id", "unspecified")},
              {"scope_id", scope},
              {"decision", "continue"},
              {"context", ""},
              {"committed", false}};
    require(identifier(item.str("actor_id")), "activity_identity_invalid");
    item["finalize_on_stop"] = request["extensions"].flag("finalize_on_stop", false) &&
                               !(run.has("finalization_context")
                                     ? run.str("finalization_context")
                                     : config.str("finalization_context")).empty();
    auto fields = event_context(item);
    fields["input"] = request["state"];
    fields["attributes"] = Json{{"type", "boundary.control"},
                                {"policy_scope", scope},
                                {"apply_policy", apply_policy},
                                {"extensions", extensions(request)}};
    item["start_event"] = emit("step.start", fields, item.str("step_id"), join(links, id));
    if (loop.flag("stopped", false)) {
      item["decision"] = "stop";
      item["reason"] = "loop_already_stopped";
    } else if (apply_policy && run.num("level", 0) > 0) {
      auto q = query(item, "loop", id, request["state"]);
      q.set_iteration(iteration);
      q.set_state_fingerprint(hash(progress_state(request["state"]).dump()));
      // 收尾能力由当前打点和已加载模板共同确认，不能从任务文字或其它分支继承。
      q.set_can_finalize(item.flag("finalize_on_stop", false));
      item["observed_actions"] = loop.num("observed_actions", 0);
      item["outcome_revision"] = loop.num("outcome_revision", 0);
      item["observed_rejections"] = loop.num("rejected_actions", 0);
      item["observed_successful_action"] = loop.num("last_successful_action", 0);
      if (!loop.flag("action_outcomes_overflow", false) &&
          loop.num("observed_actions", 0) > loop.num("consumed_actions", 0) &&
          loop.num("outcome_revision", 0) == loop.num("consumed_outcome_revision", 0))
        q.set_repeated_action_steps(loop.num("repeated_action_steps", 0) + 1);
      if (loop.num("last_successful_action", 0) <= loop.num("consumed_actions", 0) &&
          loop["operation_failures"].object())
        for (const auto &entry : loop["operation_failures"].fields())
          if (entry.second.num("last_action", 0) > loop.num("consumed_actions", 0))
            q.set_repeated_failure_steps(std::max<std::size_t>(q.repeated_failure_steps(),
                                                               entry.second.num("count", 1) - 1));
      q.set_failed_actions(loop.num("failed_actions", 0));
      if (loop.num("last_successful_action", 0) <= loop.num("consumed_actions", 0)) {
        const auto rejected = loop.num("rejected_actions", 0),
                   consumed = loop.num("consumed_rejections", 0);
        q.set_rejected_actions(
            std::min<std::size_t>(10000, rejected > consumed ? rejected - consumed : 0));
        std::set<std::string> codes;
        if (loop["operation_failures"].object())
          for (const auto &entry : loop["operation_failures"].fields()) {
            const auto code = entry.second.str("error_code");
            if (identifier(code) && q.error_codes_size() < 64 && codes.insert(code).second)
              q.add_error_codes(code);
          }
      }
      if (loop.has("progress"))
        *q.mutable_previous() = proto<v1::LoopProgress>(loop["progress"]);
      bool pending = false;
      for (const auto &pair : run["activities"].fields())
        pending |= !ended(pair.second) && pair.second.str("loop_id") == loop_id;
      q.set_pending_actions(pending);
      const auto decision = decide(item, q);
      item["query"] = json(q);
      item["decision"] = decision.progress().decision();
      item["reason"] = decision.progress().reason();
      item["context"] = decision.context();
      item["proposed_progress"] = json(decision.progress());
    }
    loop["iteration"] = iteration;
    loop["control_id"] = id;
    run["controls"][id] = item;
    return Json{{"control_id", id},
                {"decision", item["decision"]},
                {"reason", item.str("reason")},
                {"context", item["context"]},
                {"iteration", iteration},
                {"event_id", item["start_event"]}};
  }

  // commit 写入宿主实际采用的决定；若与建议不同，则丢弃未经确认的进度推断并从事实重新计算。
  Json commit_loop(const Json &request, bool interrupted = false) {
    keys(request, {"operation", "request_id", "run_id", "control_id", "decision", "extensions",
                   "caused_by"});
    const auto id = request.str("control_id"), decision = request.str("decision");
    require(run["controls"].has(id), "control_not_found", 404);
    require(decision == "continue" || decision == "replan" || decision == "stop",
            "loop_decision_invalid");
    auto &item = run["controls"][id];
    require(!item.flag("committed", false), "control_already_observed", 409);
    auto fields = event_context(item);
    fields["decision"] = decision;
    fields["reason"] = "host_boundary_observation";
    fields["attributes"] = Json{{"extensions", extensions(request)}, {"interrupted", interrupted}};
    auto links = causes(request);
    links.push(item["start_event"]);
    if (!interrupted)
      item["commit_event"] = emit("loop.decision", fields, item.str("step_id"), join(links, id));
    if (item.str("decision") != "replan" || decision != "replan" || interrupted) {
      v1::RuntimeObservation observation;
      if (!interrupted)
        observation.set_loop_decision(decision);
      receipt(item, item["query"], observation);
      item["replan_observed"] = true;
    }
    fields = event_context(item);
    fields["status"] = interrupted ? "unknown" : "completed";
    fields["output"] = nullptr;
    item["end_event"] = emit("step.end", fields, item.str("step_id"),
                             Json::Array{interrupted ? item["start_event"] : item["commit_event"]});
    if (interrupted)
      item["commit_event"] = item["end_event"];
    item["committed"] = true;
    item["actual_decision"] = decision;
    auto &loop = run["loops"][item.str("loop_id")];
    if (item["query"].object() && !item["query"].flag("pending_actions", false) && !interrupted) {
      const auto observed = item["query"].num("failed_actions", 0),
                 pending = loop.num("failed_actions", 0);
      loop["failed_actions"] = pending > observed ? pending - observed : 0;
      // 整批结算时清理旧错误，避免成功/失败的返回顺序改变下一轮条件。
      // 只消费 check 已观察的范围，check 后新到达的失败仍留给下一轮。
      if (item.num("observed_successful_action", 0) > loop.num("consumed_actions", 0) &&
          loop["operation_failures"].object())
        for (auto &entry : loop["operation_failures"].fields())
          if (entry.second.object() &&
              entry.second.num("last_action", 0) <= item.num("observed_actions", 0))
            entry.second.fields().erase("error_code");
      loop["consumed_actions"] = item.num("observed_actions", 0);
      loop["consumed_outcome_revision"] = item.num("outcome_revision", 0);
      loop["consumed_rejections"] = item.num("observed_rejections", 0);
      loop["repeated_action_steps"] = decision == "replan" && item.str("decision") == "replan"
                                          ? 0
                                          : item["query"].num("repeated_action_steps", 0);
    }
    if (item.has("proposed_progress")) {
      auto progress = proto<v1::LoopProgress>(item["proposed_progress"]);
      if (decision != item.str("decision") || interrupted) {
        const auto query = proto<v1::RuntimeQuery>(item["query"]);
        progress.set_replans(query.previous().replans());
        progress.set_no_progress(
            std::max({query.repeated_action_steps(),
                      run["policy"]["options"].flag("aggressive_controls", false)
                          ? query.repeated_failure_steps()
                          : 0U,
                      !query.state_fingerprint().empty() && query.previous().iteration() > 0 &&
                              query.previous().fingerprint() == query.state_fingerprint()
                          ? query.previous().no_progress() + 1
                          : 0U}));
        progress.clear_rule_id();
        progress.clear_reason();
      }
      progress.set_decision(decision);
      loop["progress"] = json(progress);
    }
    if (decision == "stop") {
      loop["stopped"] = true;
      if (!interrupted && item.flag("finalize_on_stop", false) && !loop.has("finalizing"))
        loop["finalizing"] = true;
    }
    if (!interrupted)
      run["observed_boundaries"]["loop"] = true;
    Json result{{"ended", true}, {"event_id", item["end_event"]}};
    if (loop.flag("finalizing", false))
      result["finalizing"] = decision == "stop" && !loop.flag("final_model_started", false);
    return result;
  }
};
} // 匿名命名空间

// 限制查询文本规模，保留原业务请求不受检索裁剪影响。
std::string bounded_query_text(const Json &value, const Json &config, bool task) {
  const auto available = config["shadow_options"].num("max_query_bytes", 32768);
  const auto budget = available > 2048 ? (available - 2048) / 2 : 128;
  // 先投影再截断，保证嵌套工具 JSON 与指令块仍可识别。
  // 原始输入仍完整保留在轨迹中。
  return query_excerpt(task ? current_task_retrieval_text(value) : retrieval_text(value),
                       std::min(budget, config["query_context"].num(
                                            task ? "max_task_bytes" : "max_state_bytes", 8192)));
}

// 从已记录边界事件统计运行指标，不能把未执行的策略建议计入调用次数。
Json boundary_metrics(const Json &events) {
  Json result{{"steps", 0},
              {"llm_calls", 0},
              {"tool_calls", 0},
              {"loop_count", 0},
              {"token_reported_calls", 0}};
  std::map<std::string, std::string> semantics;
  std::map<std::string, bool> metric_tool_calls;
  std::size_t input_calls = 0, output_calls = 0, total_calls = 0, inputs = 0, outputs = 0,
              totals = 0;
  for (const auto &event : events.items()) {
    const auto kind = event.str("kind");
    if (kind == "step.start")
      result["steps"] = result.num("steps", 0) + 1;
    if (kind == "loop.decision")
      result["loop_count"] = result.num("loop_count", 0) + 1;
    if (kind == "action.start") {
      semantics[event.str("action_id")] = event["attributes"].str("semantic");
      metric_tool_calls[event.str("action_id")] =
          event["attributes"].flag("metric_tool_call", true);
    }
    if (kind != "action.end" || !event["attributes"].flag("executed", false))
      continue;
    const auto semantic = semantics[event.str("action_id")];
    if (semantic == "action" && metric_tool_calls[event.str("action_id")])
      result["tool_calls"] = result.num("tool_calls", 0) + 1;
    if (semantic != "model")
      continue;
    result["llm_calls"] = result.num("llm_calls", 0) + 1;
    const auto &metrics = event["attributes"]["metrics"];
    if (metrics.has("input_tokens")) {
      ++input_calls;
      inputs += metrics.num("input_tokens", 0);
    }
    if (metrics.has("output_tokens")) {
      ++output_calls;
      outputs += metrics.num("output_tokens", 0);
    }
    if (metrics.has("tokens") || (metrics.has("input_tokens") && metrics.has("output_tokens"))) {
      ++total_calls;
      totals += metrics.has("tokens")
                    ? metrics.num("tokens", 0)
                    : metrics.num("input_tokens", 0) + metrics.num("output_tokens", 0);
    }
  }
  // 未知用量必须保持未知，不能让高成本试验被误记为免费。
  const auto calls = result.num("llm_calls", 0);
  if (input_calls == calls)
    result["input_tokens"] = inputs;
  if (output_calls == calls)
    result["output_tokens"] = outputs;
  if (total_calls == calls)
    result["tokens"] = totals;
  result["token_reported_calls"] = total_calls;
  return result;
}

// 把边界打点状态整理为标准轨迹，保留执行、因果与结束信息。
Json boundary_trace(const Json &run) {
  Json attributes{{"capture_complete", !run.flag("incomplete", false)}};
  if (run.has("local_degradation_reason"))
    attributes["degradation_reason"] = run["local_degradation_reason"];
  if (run.flag("local_capture", false))
    attributes["transport"] = "local.native.v1";
  if (run.has("profile"))
    attributes["protocol"] = run["profile"];
  if (run.has("extensions"))
    attributes["extensions"] = run["extensions"];
  if (run.num("metrics_version", 0) == 1)
    attributes["metrics"] = boundary_metrics(run["events"]);
  return Json{{"schema_version", 1},
              {"trace_kind", "trajectory.v1"},
              {"identity", run["identity"]},
              {"events", run["events"]},
              {"attributes", attributes}};
}

// 验证打点入口的绑定与采集规则，配置错误必须在运行前暴露。
void validate_boundary_configuration(const Json &config) {
  if (config.has("query_context")) {
    keys(config["query_context"], {"max_task_bytes", "max_state_bytes"});
    for (const auto *key : {"max_task_bytes", "max_state_bytes"})
      require(config["query_context"].num(key, 8192) >= 128 &&
                  config["query_context"].num(key, 8192) <= 32768,
              "query_context_limits_invalid");
  }
  if (!config.has("activity_types"))
    return;
  require(config["activity_types"].object() && config["activity_types"].fields().size() <= 128,
          "activity_types_invalid");
  for (const auto &pair : config["activity_types"].fields()) {
    require(identifier(pair.first) && pair.first != "model" && pair.first != "action" &&
                pair.first != "custom",
            "activity_type_invalid");
    keys(pair.second, {"semantic", "required_input_paths"});
    const auto semantic = pair.second.str("semantic");
    require(semantic == "model" || semantic == "action" || semantic == "custom",
            "activity_semantic_invalid");
    if (pair.second.has("required_input_paths")) {
      require(pair.second["required_input_paths"].items().size() <= 64,
              "too_many_required_input_fields");
      for (const auto &path : pair.second["required_input_paths"].items())
        require(path.text().size() <= 256 && rapidjson::Pointer(path.text().c_str()).IsValid(),
                "input_pointer_invalid");
    }
  }
}

// 描述框架无关的输入与控制能力，供调用方发现可用插入点。
Json describe_boundary_contract(const Json &config) {
  Json result{
      {"protocol", "evolution.boundary.v1"},
      {"contract_version", 1},
      {"max_level", config["max_level"]},
      {"operations",
       Json::Array{"point", "run.begin", "run.heartbeat", "activity.begin", "activity.end",
                   "activity.abort", "activity.status", "loop.check", "loop.commit", "loop.abort",
                   "schedule.observe", "event.record", "capture.gap", "run.end", "run.abort",
                   "run.status", "evaluation.retry", "memory.status"}},
      {"required_fields", Json{{"run.begin", Json::Array{"input"}},
                               {"activity.begin", Json::Array{"kind", "name", "input"}},
                               {"activity.end", Json::Array{"activity_id", "status", "output"}},
                               {"loop.check", Json::Array{"loop_id", "state"}},
                               {"loop.commit", Json::Array{"control_id", "decision"}},
                               {"event.record", Json::Array{"type", "data"}}}},
      {"optional_fields",
       Json::Array{"actor_id", "scope_id", "stream_id", "control_id", "caused_by", "extensions"}},
      {"extension_field", "extensions"},
      {"policy_scopes", config["actors"]},
      {"activity_types",
       config.has("activity_types") ? config["activity_types"] : Json(Json::Object{})},
      {"limits", Json{{"max_request_bytes", config.num("max_request_bytes", 1048576)},
                      {"max_events", config.num("max_events", 10000)}}}};
  if (config.has("policy_evaluation_file"))
    for (const auto *operation :
         {"agent_evaluation.status", "agent_evaluation.feedback", "agent_evaluation.submit",
          "agent_evaluation.retry", "agent_evaluation.cancel", "agent_evaluation.tree",
          "agent_evaluation.replay_holdout"})
      result["operations"].push(operation);
  return result;
}

// 从运行开始打点构造规范请求，绑定身份和上下文，不接管宿主调度器。
Json boundary_begin_request(const Json &request, const std::string &run_id) {
  keys(request, {"operation", "request_id", "contract_version", "input", "conversation_id", "level",
                 "capabilities", "extensions"});
  require(request.num("contract_version", 1) == 1, "contract_version_not_supported");
  require(request.has("input"), "run_input_required");
  extensions(request);
  return Json{{"operation", "task.begin"},
              {"request_id", request["request_id"]},
              {"input", request["input"]},
              {"conversation_id", request.str("conversation_id", run_id)},
              {"level", request.str("level", "L0")},
              {"capabilities",
               request.has("capabilities") ? request["capabilities"] : Json(Json::Array{})}};
}

// 建立单次运行的状态、策略快照与采集上下文，后续打点复用同一身份。
void initialize_boundary_run(Json &run, const Json &request) {
  run["profile"] = "evolution.boundary.v1";
  run["metrics_version"] = 1;
  run["activities"] = Json::Object{};
  run["loops"] = Json::Object{};
  run["controls"] = Json::Object{};
  run["extensions"] = extensions(request);
  run["events"].items().front()["attributes"] =
      Json{{"protocol", "evolution.boundary.v1"}, {"extensions", run["extensions"]}};
  if (run.has("agent_version_id"))
    run["events"].items().front()["attributes"]["agent_version_id"] = run["agent_version_id"];
  if (run.has("evaluation_trial_id"))
    run["events"].items().front()["attributes"]["evaluation_trial_id"] = run["evaluation_trial_id"];
}

// 按协议操作名分发打点；所有状态迁移经此入口校验，避免不同语言客户端绕过 run 的因果与关闭约束。
Json dispatch_boundary(Json &run, const Json &request, const Json &config,
                       const PolicyRuntime *cached_policy) {
  Runtime runtime(run, config, cached_policy);
  const auto op = request.str("operation");
  if (op == "activity.status") {
    keys(request, {"operation", "request_id", "run_id", "activity_id"});
    require(run["activities"].has(request.str("activity_id")), "activity_not_found", 404);
    const auto &item = run["activities"][request.str("activity_id")];
    return Json{{"activity_id", item["activity_id"]},
                {"ended", ended(item)},
                {"status", item.str("status", "pending")},
                {"output", item["output"]},
                {"event_id", item.str("end_event")}};
  }
  require(!run.flag("closed", false), "task_already_closed", 409);
  if (op == "run.heartbeat") {
    keys(request, {"operation", "request_id", "run_id"});
    return Json{{"alive", true}};
  }
  if (op == "activity.begin")
    return runtime.begin_activity(request);
  if (op == "schedule.observe")
    return runtime.observe_schedule(request);
  if (op == "activity.end" || op == "activity.abort") {
    if (op == "activity.abort")
      require(terminal(request) != "completed", "abort_status_invalid");
    return runtime.end_activity(request, op == "activity.abort");
  }
  if (op == "loop.check")
    return runtime.check_loop(request);
  if (op == "loop.commit" || op == "loop.abort") {
    if (op == "loop.abort")
      run["incomplete"] = true;
    return runtime.commit_loop(request, op == "loop.abort");
  }
  if (op == "event.record") {
    keys(request, {"operation", "request_id", "run_id", "type", "data", "activity_id", "control_id",
                   "caused_by", "extensions"});
    require(identifier(request.str("type")) && request.has("data"), "event_fields_required");
    require(!request.has("activity_id") || !request.has("control_id"), "event_context_exclusive");
    auto fields = Json{{"attributes", Json{{"type", "extension.event"},
                                           {"event_type", request["type"]},
                                           {"data", request["data"]},
                                           {"extensions", extensions(request)}}}};
    if (request.has("activity_id")) {
      require(run["activities"].has(request.str("activity_id")), "activity_not_found", 404);
      const auto context = event_context(run["activities"][request.str("activity_id")]);
      for (const auto &pair : context.fields())
        fields[pair.first] = pair.second;
    } else if (request.has("control_id")) {
      // loop.commit 之后打点仍可上报局部结果，无需新开活动。
      // 保留其显式轮次，不能改挂到最近的同级循环。
      require(run["controls"].has(request.str("control_id")), "control_not_found", 404);
      const auto context = event_context(run["controls"][request.str("control_id")]);
      for (const auto &pair : context.fields())
        fields[pair.first] = pair.second;
    }
    return Json{
        {"event_id", runtime.emit("custom", fields,
                                  "extension-" + hash(request.str("request_id")).substr(0, 24),
                                  runtime.causes(request))}};
  }
  if (op == "run.end" || op == "run.abort") {
    keys(request, {"operation", "request_id", "run_id", "status", "business_status", "output",
                   "capture_complete", "extensions", "defer_if_pending", "caused_by"});
    const auto status = terminal(request), business = request.str("business_status", "unknown");
    require(request.has("output"), "run_output_required");
    require(business == "succeeded" || business == "failed" || business == "partial" ||
                business == "unknown",
            "business_status_invalid");
    const bool interrupted = op == "run.abort";
    if (!interrupted && request.flag("defer_if_pending", false)) {
      bool pending = false;
      for (const auto &pair : run["activities"].fields())
        pending |= !ended(pair.second);
      for (const auto &pair : run["controls"].fields())
        pending |= !pair.second.flag("committed", false);
      if (pending)
        return Json{{"closed", false},
                    {"pending", true},
                    {"capture_complete", !run.flag("incomplete", false)}};
    }
    require(!interrupted || status != "completed", "abort_status_invalid");
    if (interrupted || status == "unknown" || !request.flag("capture_complete", true))
      run["incomplete"] = true;
    for (auto &pair : run["activities"].fields())
      if (!ended(pair.second)) {
        require(interrupted, "run_has_pending_activities", 409);
        runtime.end_activity(
            Json{{"activity_id", pair.first}, {"status", "unknown"}, {"output", nullptr}}, true);
      }
    for (auto &pair : run["controls"].fields()) {
      auto &control = pair.second;
      if (!control.flag("committed", false)) {
        require(interrupted, "run_has_unobserved_controls", 409);
        runtime.commit_loop(Json{{"control_id", pair.first}, {"decision", "stop"}}, true);
      }
      if (control.str("decision") == "replan" && !control.flag("replan_observed", false)) {
        runtime.receipt(control, control["query"], v1::RuntimeObservation{});
        control["replan_observed"] = true;
      }
    }
    runtime.emit(
        "run.end",
        Json{{"output", request["output"]},
             {"outcome", Json{{"execution_status", status}, {"business_status", business}}},
             {"attributes", Json{{"extensions", extensions(request)}}}},
        "task", runtime.causes(request));
    run["closed"] = true;
    run["evaluation_status"] = run.flag("incomplete", false)
                                   ? "quarantined"
                                   : (config.flag("auto_evaluate", true) ? "pending" : "disabled");
    return Json{{"closed", true},
                {"capture_complete", !run.flag("incomplete", false)},
                {"evaluation_status", run["evaluation_status"]},
                {"metrics", boundary_metrics(run["events"])}};
  }
  throw Error("operation_not_found", 404);
}
} // trajectory::evolution::integration 命名空间
