#pragma once

#include "../service/integration_json.h"
#include "agentflow/trajectory/evolution_pipeline.h"

#include <set>

namespace trajectory::evolution::integration {

// These mappings are derived by the local projector after source and old-code
// verification. Consumers keep the original trace and apply them only to a
// temporary model view, without reopening official files or changing identity.
inline const Json *code_projection_event(const Json &evidence, const std::string &run,
                                         const std::string &id) {
  const Json *found = nullptr;
  auto inspect = [&](const Json &trace) {
    if (trace["identity"].str("run_id") != run) return;
    for (const auto &event : trace["events"].items())
      if (event.str("event_id") == id) {
        require(found == nullptr, "code_projection_event_ambiguous");
        found = &event;
      }
  };
  if (evidence.has("traces"))
    for (const auto &trace : evidence["traces"].items()) inspect(trace);
  else if (evidence.has("trace")) inspect(evidence["trace"]);
  return found;
}

inline Json validated_code_projection(const Json &evidence, const Json &projection) {
  require(projection.str("status") != "error", "code_projection_unavailable");
  // Old reports contain no mappings and retain their original behavior.
  if (!projection.has("code_patches")) return nullptr;
  require(projection.str("status") == "complete" && projection["code_patches"].array() &&
              projection["sources"].array() &&
              projection["recovered_events"].integer() == projection["code_patches"].items().size() &&
              projection["sources"].items().size() == projection["code_patches"].items().size(),
          "code_projection_metadata_invalid");
  std::set<std::pair<std::string, std::string>> identities, sources;
  for (const auto &source : projection["sources"].items()) {
    keys(source, {"run_id", "event_id", "source"});
    require(source["source"].object(), "code_projection_source_invalid");
    keys(source["source"], {"path", "input_index"});
    require(!source["source"].str("path").empty() &&
                sources.emplace(source.str("run_id"), source.str("event_id")).second,
            "code_projection_source_invalid");
    (void)source["source"]["input_index"].integer();
  }
  for (const auto &patch : projection["code_patches"].items()) {
    keys(patch, {"run_id", "event_id", "code"});
    const auto run = patch.str("run_id"), id = patch.str("event_id");
    const auto *event = code_projection_event(evidence, run, id);
    require(!run.empty() && !id.empty() && identities.emplace(run, id).second &&
                sources.count({run, id}) && event &&
                (event->str("kind") == "action.start" || event->str("kind") == "action.end") &&
                (*event)["input"]["code"].string() && patch["code"].string(),
            "code_projection_binding_invalid");
    try {
      agentflow::trajectory::ensure_safe_evolution_data(proto<google::protobuf::Value>(patch["code"]));
    } catch (const agentflow::trajectory::EvaluationError &) {
      throw Error("code_projection_credential_rejected");
    }
  }
  return projection;
}

inline Json validated_code_projection(const Json &evidence) {
  return validated_code_projection(evidence, evidence["evidence_projection"]);
}

inline void apply_code_projection(Json &evidence, const Json &projection) {
  if (projection.null()) return;
  // Check all bindings before mutating the temporary view.
  (void)validated_code_projection(evidence, projection);
  auto apply = [&](Json &trace) {
    const auto run = trace["identity"].str("run_id");
    for (auto &event : trace["events"].items())
      for (const auto &patch : projection["code_patches"].items())
        if (patch.str("run_id") == run && patch.str("event_id") == event.str("event_id"))
          event["input"]["code"] = patch["code"];
  };
  if (evidence.has("traces"))
    for (auto &trace : evidence["traces"].items()) apply(trace);
  else if (evidence.has("trace")) apply(evidence["trace"]);
}

inline Json code_projection_view(Json evidence) {
  const auto projection = validated_code_projection(evidence);
  apply_code_projection(evidence, projection);
  evidence.fields().erase("evidence_projection");
  return evidence;
}

inline Json code_projection_for_run(const Json &projection, const std::string &run) {
  if (projection.null()) return nullptr;
  auto selected = projection;
  selected["code_patches"] = Json::Array{};
  selected["sources"] = Json::Array{};
  for (const auto &patch : projection["code_patches"].items())
    if (patch.str("run_id") == run) selected["code_patches"].push(patch);
  if (selected["code_patches"].items().empty()) return nullptr;
  for (const auto &source : projection["sources"].items())
    if (source.str("run_id") == run) selected["sources"].push(source);
  selected["recovered_events"] = selected["code_patches"].items().size();
  return selected;
}

} // namespace trajectory::evolution::integration
