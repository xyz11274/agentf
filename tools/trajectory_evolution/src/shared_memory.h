#pragma once

#include <functional>
#include "integration_json.h"
#include "trajectory_evolution/shadow_runtime.h"

namespace trajectory::evolution::integration {

using PrivateMemoryLoader = std::function<agentflow::trajectory::evaluation::EvolutionMemoryStore(const std::string&)>;
using SharedCheckpoint = std::function<void(const Json&)>;
using PublicEvidenceCheck = std::function<Json(const std::string&,
    const agentflow::trajectory::evaluation::EvolutionMemoryEntry&)>;

// These functions run only on the single background worker. Model text is not
// persisted; checkpoints contain reviewed public content and opaque provenance.
Json shared_memory_options(Json options);
Json new_shared_memory(const std::string& application);
void validate_shared_memory(const Json& state, const std::string& application, const Json& options);
void reconcile_shared_memory(Json& state, const Json& options);
void enqueue_shared_memory(Json& state, const std::string& user);
void process_shared_memory(Json& state, const Json& config, const PrivateMemoryLoader& load,
                          const std::shared_ptr<agentflow::trajectory::ModelAdapter>& model,
                          const SharedCheckpoint& checkpoint, const PublicEvidenceCheck& evidence);
v1::SharedExperienceStore shared_memory_view(const Json& state, const std::string& user = {});
std::string shared_content_revision(const v1::PublishedExperience& experience);

}  // namespace trajectory::evolution::integration
