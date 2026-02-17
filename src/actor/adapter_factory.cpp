// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License
//
// Actor adapter factory - selects implementation at runtime

#include "agentflow/actor/adapter.h"

#ifdef AGENTFLOW_WITH_PROTOACTOR
#include "agentflow/actor/adapter/protoactor_impl.h"
#else
#include "agentflow/actor/adapter/stub_impl.h"
#endif

namespace agentflow {
namespace actor {
namespace adapter {

std::shared_ptr<IActorSystem> ActorSystemFactory::create(const std::string& system_name) {
  return create_with_workers(system_name, 0);
}

std::shared_ptr<IActorSystem> ActorSystemFactory::create_with_workers(
    const std::string& system_name, int num_workers) {
#ifdef AGENTFLOW_WITH_PROTOACTOR
  try {
    // Create Protoactor actor system
    auto protoactor_system = protoactor::ActorSystem::New();
    if(!protoactor_system) {
      return nullptr;
    }
    return std::make_shared<ProtoactorActorSystem>(protoactor_system);
  } catch(const std::exception&) {
    return nullptr;
  }
#else
  // Use stub implementation
  return std::make_shared<StubActorSystem>();
#endif
}

std::string ActorSystemFactory::implementation_name() {
#ifdef AGENTFLOW_WITH_PROTOACTOR
  return "protoactor-cpp";
#else
  return "stub";
#endif
}

std::string ActorSystemFactory::implementation_version() {
#ifdef AGENTFLOW_WITH_PROTOACTOR
  return "1.0.0+";  // Protoactor-cpp version
#else
  return "1.0.0-stub";  // Stub implementation
#endif
}

}  // namespace adapter
}  // namespace actor
}  // namespace agentflow
