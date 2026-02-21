// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License

#ifndef AGENTFLOW_AGENT_ACTOR_H
#define AGENTFLOW_AGENT_ACTOR_H

#include <memory>
#include <string>

#include "agent/agent.h"
#include "vendor/actor/adapter.h"

namespace agentflow {
namespace agent {

class AgentActor : public actor::adapter::IActor {
public:
  explicit AgentActor(std::shared_ptr<framework::Agent> agent);
  ~AgentActor() override = default;

  void on_message(std::shared_ptr<actor::adapter::IContext> ctx) override;
  void on_start() override;
  void on_stop() override;

  std::string actor_type() const override { return "AgentActor"; }

private:
  std::shared_ptr<framework::Agent> agent_;
};

}  // namespace agent
}  // namespace agentflow

#endif  // AGENTFLOW_AGENT_ACTOR_H
