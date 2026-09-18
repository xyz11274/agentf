#pragma once

#include "local_support.h"
#include <map>
#include <set>

namespace trajectory::evolution::integration {

// Enforce declared dependency outcomes from execution evidence, independently
// of the final answer. Ordering-only edges are not success dependencies unless
// the reviewed task explicitly requires stopping dependent work on failure.
inline Json schedule_contract_check(const Json& attributes, const Json& evidence) {
  const auto dependency = attributes["dependency"].string() ? attributes.str("dependency") : std::string{};
  const bool task_requires_success = std::holds_alternative<bool>(attributes["stop_on_failure"].value)
      && attributes["stop_on_failure"].boolean() && !dependency.empty()
      && dependency != "independent" && dependency != "parallel" && dependency != "none";
  Json check{{"check_id", "contract.schedule_success"}, {"status", "passed"},
      {"reason", "未观察到失败前置之后仍执行其成功依赖分支。"}, {"evidence_paths", Json::Array{}}};
  const auto traces = evidence["traces"].array() ? evidence["traces"] : Json(Json::Array{evidence["trace"]});
  bool applicable = task_requires_success;
  for(std::size_t r = 0; r < traces.items().size(); ++r) {
    const auto& trace = traces.items()[r];
    if(!trace["events"].array()) continue;
    std::map<std::string, Json> plans;
    std::map<std::string, std::set<std::string>> executed, failed;
    for(const auto& event : trace["events"].items()) {
      const auto& attr = event["attributes"];
      const auto step = event.str("step_id");
      if(attr.str("type") == "policy.query" && attr["decision"]["query"].str("boundary") == "schedule")
        plans[step] = attr["decision"]["schedule"];
      if(attr.str("type") != "schedule.action" || !attr.flag("executed", false)) continue;
      if(event.str("kind") == "action.start" || event.str("kind") == "action.end")
        executed[step].insert(attr.str("logical_action_id"));
      if(event.str("kind") == "action.end" && (event.str("status") == "failed" || attr.flag("observed_failure", false)))
        failed[step].insert(attr.str("logical_action_id"));
    }
    for(const auto& plan : plans) {
      if(!plan.second["actions"].array()) continue;
      for(const auto& action : plan.second["actions"].items()) {
        if(!action["after"].array()) continue;
        for(const auto& edge : action["after"].items()) {
          if(!task_requires_success && !edge.flag("require_success", false)) continue;
          applicable = true;
          if(!executed[plan.first].count(action.str("action_id")) || !failed[plan.first].count(edge.str("action_id"))) continue;
          check["status"] = "failed";
          check["reason"] = "前置操作已失败，但实际仍启动了要求其成功的依赖分支；最终未调用下游工具不抵消这次错误派发。";
          const auto path = evidence["traces"].array() ? "/traces/" + std::to_string(r) + "/events" : "/trace/events";
          if(check["evidence_paths"].items().empty() || check["evidence_paths"].items().back().text() != path)
            check["evidence_paths"].push(path);
        }
      }
    }
  }
  return applicable ? check : Json();
}

}  // namespace trajectory::evolution::integration
