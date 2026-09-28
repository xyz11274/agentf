// 通用打点解释：将语言无关的请求映射到标准事件与策略边界。
// 所有宿主关联信息通过公开字段传递，不读取具体框架的内部对象。
// 输入验证、身份绑定和起止关系在此收口，调用方不必实现额外观察器。

#include "instrumentation.h"
#include "boundary_runtime.h"
#include "dispatch_runtime.h"

#include <set>
#include <openssl/sha.h>
#include <rapidjson/pointer.h>

namespace trajectory::evolution::integration {

namespace {
// 对跨进程引用计算稳定摘要，避免把长身份或原始内容直接作为键。
std::string reference_hash(const std::string &value) {
  unsigned char bytes[SHA256_DIGEST_LENGTH];
  SHA256(reinterpret_cast<const unsigned char *>(value.data()), value.size(), bytes);
  std::string result;
  const char *hex = "0123456789abcdef";
  for (const auto byte : bytes) {
    result += hex[byte >> 4];
    result += hex[byte & 15];
  }
  return result;
}

// 从跨进程引用中提取稳定键，保证后续回执能关联同一打点。
std::string reference_key(const Json &value) {
  require(value.string() && !value.text().empty() && value.text().size() <= 256 &&
              value.text().find('\0') == std::string::npos,
          "point_reference_invalid");
  return reference_hash(value.text());
}

// 查找最新有效引用，避免使用已经关闭或不属于当前运行的上下文。
Json latest_reference(const Json &binding) {
  require(binding.has("before"), "point_parent_not_found");
  return binding.has("after") ? binding["after"] : binding["before"];
}

// 统计尚未结束的子分支，父节点不能因派发函数返回就提前关闭。
bool pending_children(const Json &run, const std::string &key) {
  const auto &bindings = run["point_bindings"];
  if (bindings[key].has("children"))
    for (const auto &child : bindings[key]["children"].items())
      if (!bindings[child.text()].has("after"))
        return true;
  return false;
}
}

// 提取打点所属运行键，建立采集隔离边界。
std::string point_run_key(const Json &request) {
  keys(request["ref"], {"run", "key", "parent", "after"});
  require(!request.has("context") && !request.has("causes"), "point_reference_context_exclusive");
  return "point-run-" + reference_key(request["ref"]["run"]);
}

// 提取调用级稳定键，区分同一运行内并发操作。
std::string point_call_key(const Json &request) {
  return request.str("kind") == "run" ? "root" : reference_key(request["ref"]["key"]);
}

// 生成传输请求幂等键，重复请求不能创建第二个业务调用。
std::string point_request_key(const Json &request) {
  if (request.str("phase") == "heartbeat") {
    point_run_key(request);
    return request.str("request_id");
  }
  return request.str("kind") == "run" && request.str("phase") == "before"
             ? point_run_key(request)
             : "point-" + reference_hash(point_run_key(request) + ":" + point_call_key(request) +
                                         ":" + request.str("phase"));
}

// 把调用方稳定引用解析为内部执行身份；异步跨进程传递仍使用公开引用字段。
Json resolve_point_reference(const Json &request, Json &run, const std::string &run_id) {
  point_run_key(request);
  const auto key = point_call_key(request), phase = request.str("phase");
  const auto &bindings = run["point_bindings"];
  const auto &entry = bindings[key];
  Json point = request;
  point.fields().erase("ref");
  if (request.flag("capture_gap", false) || phase == "gap")
    run["incomplete"] = true;
  point.fields().erase("capture_gap");
  if (run.flag("incomplete", false) && phase == "before")
    point["apply_policy"] = false;
  point["request_id"] = point_request_key(request);
  point["extensions"]["point_reference"] = request["ref"];
  if (request.str("kind") == "run" && phase == "before")
    return point;
  require(run.object() && bindings.has("root"), "point_run_not_found");
  require(phase != "before" || !entry.has("after"), "point_key_already_completed");
  const auto parent =
      request["ref"].has("parent") ? reference_key(request["ref"]["parent"]) : "root";
  if (phase == "before" || phase == "event") {
    require(!bindings[parent].has("after"), "point_parent_already_completed");
    point["context"] = latest_reference(bindings[parent]);
    if (bindings[parent].has("control"))
      point["context"] = bindings[parent]["control"];
  } else {
    require(entry.has("before"), "point_boundary_not_found");
    point["context"] = latest_reference(entry);
    if (!point.has("kind"))
      point["kind"] = entry["before"]["kind"];
  }
  require(point["context"].str("run_id") == run_id, "point_reference_run_mismatch");
  Json causes = Json::Array{};
  if (request["ref"].has("after")) {
    require(request["ref"]["after"].array() && request["ref"]["after"].items().size() <= 64,
            "point_references_limit");
    for (const auto &value : request["ref"]["after"].items()) {
      const auto &predecessor = bindings[reference_key(value)];
      require(predecessor.has("after"), "point_predecessor_not_completed");
      causes.push(predecessor["after"]);
    }
  }
  // 父节点依赖全部已登记子节点完成，不受完成顺序影响。
  if ((phase == "after" || phase == "abort") && entry.has("children"))
    for (const auto &child : entry["children"].items())
      if (bindings[child.text()].has("after"))
        causes.push(bindings[child.text()]["after"]);
  point["causes"] = causes;
  if (phase == "before" && request.str("kind") == "loop" && !request.has("loop_id"))
    point["loop_id"] = "loop-" + reference_hash(parent).substr(0, 32);
  return point;
}

// 保存起点的关联信息，终点必须能找到对应起点，不能串接同名的其他调用。
void record_point_reference(Json &run, const Json &request, const Json &result) {
  if (request.flag("capture_gap", false))
    run["incomplete"] = true;
  const auto key = point_call_key(request), phase = request.str("phase");
  auto &entries = run["point_bindings"];
  if (phase == "before" || phase == "event") {
    entries[key]["before"] = result["context"];
    if (phase == "event")
      entries[key]["after"] = result["context"];
    if (key != "root") {
      const auto parent =
          request["ref"].has("parent") ? reference_key(request["ref"]["parent"]) : "root";
      entries[key]["parent"] = parent;
      if (!entries[parent].has("children"))
        entries[parent]["children"] = Json::Array{};
      entries[parent]["children"].push(key);
    }
  } else if (phase == "after" || phase == "abort") {
    if (result.flag("closed", false) || result.flag("ended", false))
      entries[key]["after"] = result["context"];
    if (result["context"].str("kind") == "loop" && entries[key].has("parent"))
      entries[entries[key].str("parent")]["control"] = result["context"];
  }
}

// 按稳定引用恢复并推进派发打点，适配异步、跨线程和跨进程返回。
Json dispatch_referenced_point(Json &run, const Json &request, const Json &config,
                               const PolicyRuntime *cached_policy) {
  if (request.str("kind") == "dispatch")
    return dispatch_batch_point(run, request, config, cached_policy);
  if (request.str("phase") == "guard") {
    require(request.str("kind") == "loop" || request.str("kind") == "action", "guard_kind_invalid");
    auto before = request;
    before["phase"] = "before";
    if (request.str("kind") == "action")
      before["extensions"]["guard_only"] = true;
    auto proposal = dispatch_referenced_point(run, before, config, cached_policy);
    Json after{{"operation", "point"},
               {"phase", "after"},
               {"kind", request["kind"]},
               {"ref", request["ref"]}};
    if (request.str("kind") == "loop")
      after["decision"] = proposal["decision"];
    else {
      after["status"] = "cancelled";
      after["executed"] = false;
      after["actual_input"] = proposal["input"];
      after["output"] = Json{{"guard_decision", proposal["decision"]}};
    }
    auto committed = dispatch_referenced_point(run, after, config, cached_policy);
    committed["decision"] = proposal["decision"];
    committed["reason"] = proposal["reason"];
    if (request.str("kind") == "loop" && committed.flag("finalizing", false))
      committed["decision"] = "finalize";
    return committed;
  }
  auto point = resolve_point_reference(request, run, run["identity"].str("run_id"));
  if (request.str("phase") == "before" && request["extensions"].flag("guard_only", false) &&
      point["context"].str("activity_id").size()) {
    const auto &parent = run["activities"][point["context"].str("activity_id")];
    if (parent.has("control_id"))
      point["context"]["control_id"] = parent["control_id"];
  }
  if (request.str("phase") == "gap")
    return Json{{"context", point["context"]}, {"capture_complete", false}, {"degraded", true}};
  const auto key = point_call_key(request);
  if (key != "root" && request.str("phase") == "after" && pending_children(run, key)) {
    run["point_pending_children"][key] = request;
    return point_result(point, point_request(point, config),
                        Json{{"ended", false}, {"pending", true}});
  }
  // 汇聚逻辑属于组件内部，不要求宿主另写回调收集器。
  auto &causes = point["causes"].items();
  while (causes.size() > 63) {
    Json ids = Json::Array{};
    for (unsigned i = 0; i < 32; ++i) {
      for (const auto &id : causes.back()["caused_by"].items())
        ids.push(id);
      causes.pop_back();
    }
    const auto event = dispatch_boundary(
        run,
        Json{{"operation", "event.record"},
             {"type", "point.join"},
             {"request_id", point_request_key(request) + "-join-" + std::to_string(causes.size())},
             {"data", nullptr},
             {"caused_by", ids}},
        config, cached_policy);
    causes.push_back(
        Json{{"run_id", run["identity"]["run_id"]}, {"caused_by", Json::Array{event["event_id"]}}});
  }
  const auto packet = point_request(point, config);
  auto result = point_result(point, packet, dispatch_boundary(run, packet, config, cached_policy));
  record_point_reference(run, request, result);
  // 派发回调可能先于后代任务返回；
  // 只有全部已登记后代真正回报后才结束该边界。
  if (run.has("point_pending_children")) {
    const auto pending = run["point_pending_children"];
    for (const auto &item : pending.fields())
      if (!pending_children(run, item.first) && run["point_pending_children"].has(item.first)) {
        run["point_pending_children"].fields().erase(item.first);
        const auto final = dispatch_referenced_point(run, item.second, config, cached_policy);
        const auto request_key = point_request_key(item.second);
        if (run["requests"].has(request_key))
          run["requests"][request_key]["response"] = final;
        if (final.flag("run_closed", false))
          result["run_closed"] = true;
      }
  }
  if (request.str("kind") == "run" && request.str("phase") == "after" &&
      result.flag("pending", false))
    run["point_pending_end"] = request;
  else if (run.has("point_pending_end") && !run.flag("closed", false)) {
    bool active = false;
    for (const auto &item : run["activities"].fields())
      active |= !item.second.flag("ended", false);
    for (const auto &item : run["controls"].fields())
      active |= !item.second.flag("committed", false);
    if (!active) {
      const auto end = run["point_pending_end"];
      run.fields().erase("point_pending_end");
      auto final = dispatch_referenced_point(run, end, config, cached_policy);
      const auto key = point_request_key(end);
      if (run["requests"].has(key))
        run["requests"][key]["response"] = final;
      result["run_closed"] = final.flag("closed", false);
    }
  }
  return result;
}

// 异常路径释放或标记未闭合引用，避免正常路径之外的 return 留下悬挂状态。
void abort_point_reference(Json &run, const Json &request, const Json &config,
                           const PolicyRuntime *cached_policy) noexcept {
  try {
    run["incomplete"] = true;
    const auto phase = request.str("phase");
    if (phase != "after" && phase != "abort")
      return;
    const auto key = point_call_key(request);
    const auto &binding = run["point_bindings"][key];
    if (!binding.has("before") || binding.has("after") || run.flag("closed", false))
      return;
    Json ref{{"run", request["ref"]["run"]}};
    if (key != "root")
      ref["key"] = request["ref"]["key"];
    if (run.has("point_pending_children"))
      run["point_pending_children"].fields().erase(key);
    dispatch_referenced_point(run,
                              Json{{"operation", "point"},
                                   {"phase", "abort"},
                                   {"kind", binding["before"]["kind"]},
                                   {"ref", ref},
                                   {"status", "unknown"},
                                   {"output", nullptr}},
                              config, cached_policy);
  } catch (...) { /* 即使终态恢复数据放不下，也必须保留采集缺口标记。 */
  }
}

// 验证采集与注入配置，保留框架无关的协议边界。
void validate_instrumentation(const Json &config) {
  if (!config.has("instrumentation"))
    return;
  const auto &options = config["instrumentation"];
  keys(options, {"level", "capabilities", "binding", "scope_id", "output_metrics",
                 "action_outcomes", "model_outcomes"});
  const auto level = options.str("level", "L0");
  require(level == "L0" || level == "L1" || level == "L2" || level == "L3",
          "instrumentation_level_invalid");
  if (options.has("capabilities")) {
    require(options["capabilities"].array(), "capabilities_array_required");
    require(options["capabilities"].items().size() <= 4, "capability_not_supported");
    for (const auto &cap : options["capabilities"].items())
      require(cap.text() == "model" || cap.text() == "actions" || cap.text() == "loop" ||
                  cap.text() == "schedule",
              "capability_not_supported");
  }
  if (options.has("scope_id"))
    require(identifier(options.str("scope_id")), "scope_id_invalid");
  if (options.has("output_metrics")) {
    keys(options["output_metrics"],
         {"input_tokens", "output_tokens", "tokens", "cached_input_tokens"});
    for (const auto &metric : options["output_metrics"].fields())
      require(metric.second.string() && metric.second.text().size() <= 256 &&
                  rapidjson::Pointer(metric.second.text().c_str()).IsValid(),
              "output_metric_pointer_invalid");
  }
  for (const auto *mapping : {"action_outcomes", "model_outcomes"})
    if (options.has(mapping)) {
      const auto &rules = options[mapping];
      require(rules.array() && rules.items().size() <= 32, "action_outcomes_invalid");
      for (const auto &rule : rules.items()) {
        keys(rule, {"pointer", "equals", "nonempty", "status", "error"});
        const auto path = rule.str("pointer");
        require(rule.has("pointer") && rule["pointer"].string() && path.size() <= 256 &&
                    path.find('\0') == std::string::npos &&
                    rapidjson::Pointer(path.c_str()).IsValid(),
                "action_outcome_pointer_invalid");
        require(rule.has("equals") != rule.has("nonempty"), "action_outcome_match_invalid");
        if (rule.has("equals"))
          require(!rule["equals"].object() && !rule["equals"].array() &&
                      rule["equals"].dump().size() <= 256,
                  "action_outcome_match_invalid");
        if (rule.has("nonempty"))
          require(rule["nonempty"].boolean(), "action_outcome_match_invalid");
        const auto status = rule.str("status");
        require(status == "completed" || status == "failed" || status == "cancelled" ||
                    status == "unknown",
                "status_invalid");
        if (rule.has("error")) {
          keys(rule["error"], {"code", "retryability"});
          const auto retry = rule["error"].str("retryability");
          require(status != "completed" && identifier(rule["error"].str("code")) &&
                      (retry == "unknown" || retry == "retryable" || retry == "non_retryable"),
                  "error_info_invalid");
        }
      }
    }
}

// 把宿主打点字段转换为组件请求，先规范化身份与阶段，再进入运行时。
Json point_request(const Json &request, const Json &config) {
  keys(request,
       {"operation",    "request_id",  "phase",   "kind",       "context",         "causes",
        "name",         "input",       "output",  "status",     "business_status", "executed",
        "actual_input", "decision",    "loop_id", "actor_id",   "scope_id",        "state",
        "binding",      "metrics",     "error",   "extensions", "conversation_id", "level",
        "capabilities", "apply_policy"});
  const auto &context = request["context"];
  require(context.null() || context.object(), "point_context_invalid");
  const auto phase = request.str("phase"),
             kind = request.str("kind", context.str("kind", "custom"));
  const auto semantic =
      config["activity_types"].has(kind) ? config["activity_types"][kind].str("semantic") : kind;
  const auto &options = config["instrumentation"];
  Json packet{{"request_id", request["request_id"]}};
  auto copy = [&](const char *key) {
    if (request.has(key))
      packet[key] = request[key];
  };
  copy("extensions");
  if (kind == "run" && phase == "before") {
    packet["operation"] = "run.begin";
    packet["input"] = request["input"];
    require(request.has("input"), "run_input_required");
    packet["level"] = request.str("level", options.str("level", "L0"));
    packet["capabilities"] = request.has("capabilities")   ? request["capabilities"]
                             : options.has("capabilities") ? options["capabilities"]
                                                           : Json(Json::Array{});
    copy("conversation_id");
    return packet;
  }
  require(identifier(context.str("run_id")), "point_context_required");
  packet["run_id"] = context["run_id"];
  if (phase == "heartbeat") {
    packet["operation"] = "run.heartbeat";
    return packet;
  }
  Json causes = Json::Array{};
  std::set<std::string> ids;
  auto inherit = [&](const Json &value) {
    require(value.str("run_id") == context.str("run_id"), "point_cause_run_mismatch");
    if (value.has("caused_by"))
      for (const auto &id : value["caused_by"].items())
        if (ids.insert(id.text()).second)
          causes.push(id);
  };
  inherit(context);
  if (request.has("causes"))
    for (const auto &parent : request["causes"].items())
      inherit(parent);
  require(causes.items().size() <= 64, "too_many_causes");
  packet["caused_by"] = causes;
  if (phase == "after" || phase == "abort") {
    require(phase == "abort" || kind == "loop" || request.has("output"), "point_output_required");
    packet["status"] = request.str(
        "status", phase == "abort"                                            ? "unknown"
                  : request.has("executed") && !request["executed"].boolean() ? "cancelled"
                                                                              : "completed");
    packet["output"] = request["output"];
    if (kind == "run") {
      packet["operation"] = phase == "abort" ? "run.abort" : "run.end";
      copy("business_status");
      if (phase == "after")
        packet["defer_if_pending"] = true;
    } else if (kind == "loop") {
      packet.fields().erase("status");
      packet.fields().erase("output");
      packet["operation"] = phase == "abort" ? "loop.abort" : "loop.commit";
      packet["control_id"] = context["control_id"];
      // 返回建议不是实际观测，实际决定必须由宿主提供。
      require(phase == "abort" || request.has("decision"), "actual_decision_required");
      packet["decision"] = phase == "abort" ? Json("continue") : request["decision"];
    } else {
      packet["operation"] = phase == "abort" ? "activity.abort" : "activity.end";
      packet["activity_id"] = context["activity_id"];
      copy("executed");
      copy("error");
      copy("metrics");
      // 函数正常返回不一定代表业务成功，只应用
      // 显式配置的结果映射，并保留宿主原始状态和输出。
      const auto mapping = semantic == "model" ? "model_outcomes" : "action_outcomes";
      if ((semantic == "action" || semantic == "model") && phase == "after" &&
          request.flag("executed", false) && !request.has("status") && !request.has("error") &&
          options.has(mapping)) {
        const auto bytes =
            request["output"].string() ? request["output"].text() : request["output"].dump();
        rapidjson::Document output;
        output.Parse<rapidjson::kParseValidateEncodingFlag | rapidjson::kParseIterativeFlag>(
            bytes.data(), bytes.size());
        if (!output.HasParseError())
          for (const auto &rule : options[mapping].items()) {
            const auto *value = rapidjson::Pointer(rule.str("pointer").c_str()).Get(output);
            if (!value)
              continue;
            bool matched = false;
            if (rule.has("equals")) {
              const auto expected_bytes = rule["equals"].dump();
              rapidjson::Document expected;
              expected.Parse(expected_bytes.data(), expected_bytes.size());
              matched = *value == expected;
            } else {
              matched = !value->IsNull() && !(value->IsBool() && !value->GetBool()) &&
                        !(value->IsNumber() && value->GetDouble() == 0) &&
                        !(value->IsString() && value->GetStringLength() == 0) &&
                        !(value->IsArray() && value->Empty()) &&
                        !(value->IsObject() && value->ObjectEmpty());
            }
            if (!matched)
              continue;
            packet["status"] = rule["status"];
            if (rule.has("error"))
              packet["error"] = rule["error"];
            break;
          }
      }
      if (semantic == "model" && phase == "after" && options.has("output_metrics")) {
        rapidjson::Document output;
        const auto bytes = request["output"].dump();
        output.Parse(bytes.c_str());
        for (const auto &metric : options["output_metrics"].fields()) {
          const auto *value = rapidjson::Pointer(metric.second.text().c_str()).Get(output);
          if (value && !packet["metrics"].has(metric.first)) {
            require(value->IsNumber(), "metric_value_invalid");
            const auto number = value->GetDouble();
            require(std::isfinite(number) && number >= 0 && number <= 1000000000 &&
                        std::floor(number) == number,
                    "metric_value_invalid");
            packet["metrics"][metric.first] = std::size_t(number);
          }
        }
        if (packet["metrics"].null())
          packet.fields().erase("metrics");
      }
      if (request.has("actual_input"))
        copy("actual_input");
      else if (semantic == "model" && request.has("input"))
        packet["actual_input"] = request["input"];
      if (semantic == "model" || semantic == "action") {
        // 缺失执行证据时隔离该记录，不能从准备好的输入推断已执行。
        if (!packet.has("executed") || (semantic == "model" && !packet.has("actual_input"))) {
          packet["operation"] = "activity.abort";
          packet["status"] = "unknown";
        }
      }
    }
    return packet;
  }
  if (phase == "event") {
    require(request.has("input"), "point_input_required");
    if (kind == "schedule") {
      require(context.has("activity_id"), "schedule_context_required");
      packet["operation"] = "schedule.observe";
      packet["activity_id"] = context["activity_id"];
      packet["input"] = request["input"];
      return packet;
    }
    packet["operation"] = "event.record";
    packet["type"] = request["name"];
    packet["data"] = request["input"];
    if (context.has("activity_id"))
      packet["activity_id"] = context["activity_id"];
    else if (context.has("control_id"))
      packet["control_id"] = context["control_id"];
    return packet;
  }
  require(phase == "before", "point_phase_invalid");
  for (const auto *key : {"actor_id", "scope_id"}) {
    if (request.has(key))
      packet[key] = request[key];
    else if (context.has(key))
      packet[key] = context[key];
    else if (options.has(key))
      packet[key] = options[key];
  }
  copy("apply_policy");
  if (kind == "loop") {
    require(request.has("state") || request.has("input"), "point_state_required");
    packet["operation"] = "loop.check";
    packet["loop_id"] = request.str("loop_id", context.str("loop_id", "main"));
    packet["state"] = request.has("state") ? request["state"] : request["input"];
  } else {
    require(request.has("input"), "point_input_required");
    packet["operation"] = "activity.begin";
    packet["kind"] = kind;
    packet["name"] = request.str("name", kind);
    packet["input"] = request["input"];
    copy("state");
    if (context.has("control_id"))
      packet["control_id"] = context["control_id"];
    if (semantic == "model") {
      if (request.has("binding"))
        copy("binding");
      else if (options.has("binding"))
        packet["binding"] = options["binding"];
    }
  }
  return packet;
}

// 把组件结果转换为宿主可消费的打点回执，保留降级与实际控制状态。
Json point_result(const Json &request, const Json &normalized, Json result) {
  Json context = request["context"].object() ? request["context"] : Json(Json::Object{});
  const auto phase = request.str("phase"),
             kind = request.str("kind", context.str("kind", "custom"));
  context["kind"] = kind;
  if (result.has("run_id"))
    context["run_id"] = result["run_id"];
  for (const auto *key : {"actor_id", "scope_id", "loop_id"})
    if (normalized.has(key))
      context[key] = normalized[key];
  if (result.has("activity_id"))
    context["activity_id"] = result["activity_id"];
  if (result.has("control_id")) {
    context["control_id"] = result["control_id"];
    context.fields().erase("activity_id");
  }
  if (kind == "run" && phase == "before")
    context["caused_by"] = Json::Array{"event-1"};
  else if (result.has("event_id") && !result.str("event_id").empty())
    context["caused_by"] = Json::Array{result["event_id"]};
  if (result["context"].string())
    result["replan_context"] = result["context"];
  if (result.flag("degraded", false))
    context["degraded"] = true;
  result["context"] = context;
  return result;
}

// 在观测组件不可用时生成受限降级结果，保持原业务输入并标记采集缺口。
Json point_fallback(const Json &request, const std::string &reason) {
  Json context = request["context"].object() ? request["context"] : Json(Json::Object{});
  const auto kind = request["kind"].string()   ? request["kind"].text()
                    : context["kind"].string() ? context["kind"].text()
                                               : "custom";
  context["kind"] = kind;
  context["degraded"] = true;
  context["caused_by"] = Json::Array{};
  if (!context.has("run_id"))
    context["run_id"] =
        "unobserved-" + (request["request_id"].string() ? request["request_id"].text() : "invalid");
  return Json{{"degraded", true},
              {"reason", reason},
              {"capture_complete", false},
              {"policy_applied", false},
              {"context", context},
              {"input", request["input"]},
              {"decision", kind == "loop" ? "continue" : "allow"},
              {"ended", false},
              {"closed", false}};
}

} // 命名空间结束：trajectory::evolution::integration
