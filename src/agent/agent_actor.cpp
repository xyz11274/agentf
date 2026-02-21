// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License

#include "agent/agent_actor.h"
#include "utils/logger.h"

namespace agentflow {
namespace agent {

AgentActor::AgentActor(std::shared_ptr<framework::Agent> agent) : agent_(agent) {
}

void AgentActor::on_start() {
  if(agent_) {
    Logger::instance().info_f("AgentActor started for agent: %s", agent_->get_id().c_str());
  }
}

void AgentActor::on_stop() {
  if(agent_) {
    Logger::instance().info_f("AgentActor stopped for agent: %s", agent_->get_id().c_str());
  }
}

void AgentActor::on_message(std::shared_ptr<actor::adapter::IContext> ctx) {
  if(!agent_) {
    return;
  }

  auto msg = ctx->current_message();

  Logger::instance().debug_f("AgentActor %s received message: %s", agent_->get_id().c_str(),
                             msg.message_type.c_str());

  if(msg.message_type == "execute_workflow") {
    // Payload should be a struct containing workflow_id and inputs
    // For now, assume it's just workflow_id string
    auto workflow_id = msg.get_payload<std::string>();
    if(workflow_id) {
      // Execute workflow (async)
      // Note: agent->execute_workflow is not available directly on Agent, 
      // it is on AgentRuntime. Agent manages resources.
      // But maybe Agent has some methods to trigger execution or we should use Runtime.
      // For now, just log.
      Logger::instance().info_f("AgentActor received execute_workflow: %s", workflow_id->c_str());
    }
  } else if(msg.message_type == "publish_data") {
    // Payload: pair<string, DataVariant>
    using DataPair = std::pair<std::string, DataVariant>;
    auto data = msg.get_payload<DataPair>();
    if(data) {
      agent_->publish_data(data->first, data->second);
    }
  } else if(msg.message_type == "get_data") {
    auto key = msg.get_payload<std::string>();
    if(key) {
      DataVariant value;
      if(agent_->get_data(*key, value)) {
        // Reply with value
        if(ctx->sender()) {
          // Send back the value
          // We need a way to send DataVariant back.
          // For now, just log.
        }
      }
    }
  } else {
    Logger::instance().warn_f("Unknown message type: %s", msg.message_type.c_str());
  }
}

}  // namespace agent
}  // namespace agentflow
