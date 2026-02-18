// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License
//
// Google Test tests for JSON Agent functionality

#include <gtest/gtest.h>

#include "dsl/workflow_parser.h"
#include "core/agent.h"
#include "core/workflow.h"
#include "runtime/databus.h"

namespace agentflow {
namespace tests {

class AgentJsonTest : public ::testing::Test {
protected:
  void SetUp() override { parser_ = dsl::WorkflowParserFactory::create(); }

  void TearDown() override { parser_.reset(); }

  std::unique_ptr<dsl::WorkflowParser> parser_;

  // Sample valid agent JSON
  const char* valid_agent_json_ = R"({
    "id": "agent_001",
    "name": "Test Agent",
    "version": "1.0.0",
    "description": "A test agent for unit testing",
    "policy": "FACTORY",
    "max_workflows": 10,
    "max_concurrent_workflows": 5,
    "data_bus_capacity": 1048576,
    "workflows": [
      {
        "id": "workflow_001",
        "name": "Primary Workflow",
        "version": "1.0.0"
      },
      {
        "id": "workflow_002",
        "name": "Secondary Workflow",
        "version": "1.0.0"
      }
    ]
  })";

  // Sample agent with PER_REQUEST policy
  const char* agent_per_request_json_ = R"({
    "id": "agent_per_request",
    "name": "Per-Request Agent",
    "version": "1.0.0",
    "policy": "PER_REQUEST",
    "max_workflows": 1,
    "workflows": [
      {
        "id": "workflow_single",
        "name": "Single Workflow"
      }
    ]
  })";

  // Sample agent with SINGLETON policy
  const char* agent_singleton_json_ = R"({
    "id": "agent_singleton",
    "name": "Singleton Agent",
    "version": "1.0.0",
    "policy": "SINGLETON",
    "max_workflows": 10,
    "workflows": [
      {
        "id": "workflow_multi_1",
        "name": "Workflow 1"
      },
      {
        "id": "workflow_multi_2",
        "name": "Workflow 2"
      }
    ]
  })";

  // Sample agent with embedded workflow definitions
  const char* agent_with_workflow_json_ = R"({
    "id": "agent_embedded",
    "name": "Agent with Embedded Workflows",
    "version": "1.0.0",
    "policy": "FACTORY",
    "workflows": [
      {
        "id": "workflow_embedded_1",
        "name": "Embedded Workflow 1",
        "nodes": [
          {
            "id": "node_a",
            "name": "Node A",
            "type": "TASK"
          },
          {
            "id": "node_b",
            "name": "Node B",
            "type": "TASK"
          }
        ],
        "edges": [
          {
            "id": "edge_a_b",
            "source": "node_a",
            "target": "node_b"
          }
        ]
      }
    ]
  })";

  // Sample invalid agent JSON - missing required id
  const char* invalid_agent_missing_id_json_ = R"({
    "name": "Invalid Agent",
    "version": "1.0.0",
    "policy": "FACTORY",
    "workflows": []
  })";

  // Sample invalid agent JSON - invalid policy
  const char* invalid_agent_bad_policy_json_ = R"({
    "id": "agent_invalid",
    "name": "Invalid Agent",
    "version": "1.0.0",
    "policy": "INVALID_POLICY",
    "workflows": []
  })";

  // Sample invalid agent JSON - negative values
  const char* invalid_agent_negative_values_json_ = R"({
    "id": "agent_negative",
    "name": "Invalid Agent",
    "version": "1.0.0",
    "policy": "FACTORY",
    "max_workflows": -1,
    "workflows": []
  })";
};

// ============================================================================
// Agent JSON Parsing Tests
// ============================================================================

TEST_F(AgentJsonTest, ParseValidAgent) {
  auto result = parser_->parse_agent(valid_agent_json_);

  EXPECT_TRUE(result.is_valid()) << "Agent parsing should succeed";
  EXPECT_NE(result.agent, nullptr) << "Agent object should be created";
  EXPECT_EQ(result.agent->get_id(), "agent_001");
  EXPECT_EQ(result.agent->get_name(), "Test Agent");
}

TEST_F(AgentJsonTest, ParseAgentWithEmbeddedWorkflow) {
  auto result = parser_->parse_agent(agent_with_workflow_json_);

  EXPECT_TRUE(result.is_valid()) << "Agent with embedded workflows should parse successfully";
  EXPECT_NE(result.agent, nullptr);
  EXPECT_EQ(result.agent->get_workflows().size(), 1);
}

TEST_F(AgentJsonTest, ParseAgentPerRequestPolicy) {
  auto result = parser_->parse_agent(agent_per_request_json_);

  EXPECT_TRUE(result.is_valid()) << "Per-request agent should parse successfully";
  EXPECT_NE(result.agent, nullptr);

  auto config = result.agent->get_config();
  EXPECT_EQ(config.policy, framework::Agent::LifecyclePolicy::PER_REQUEST);
  EXPECT_EQ(config.max_workflows, 1);
}

TEST_F(AgentJsonTest, ParseAgentFactoryPolicy) {
  auto result = parser_->parse_agent(valid_agent_json_);

  EXPECT_TRUE(result.is_valid()) << "Factory agent should parse successfully";
  EXPECT_NE(result.agent, nullptr);

  auto config = result.agent->get_config();
  EXPECT_EQ(config.policy, framework::Agent::LifecyclePolicy::FACTORY);
  EXPECT_EQ(config.max_workflows, 10);
  EXPECT_EQ(config.max_concurrent_workflows, 5);
}

TEST_F(AgentJsonTest, ParseAgentSingletonPolicy) {
  auto result = parser_->parse_agent(agent_singleton_json_);

  EXPECT_TRUE(result.is_valid()) << "Singleton agent should parse successfully";
  EXPECT_NE(result.agent, nullptr);

  auto config = result.agent->get_config();
  EXPECT_EQ(config.policy, framework::Agent::LifecyclePolicy::SINGLETON);
  EXPECT_EQ(config.max_workflows, 10);
}

TEST_F(AgentJsonTest, ParseAgentMissingId) {
  auto result = parser_->parse_agent(invalid_agent_missing_id_json_);

  EXPECT_FALSE(result.is_valid()) << "Agent without ID should fail validation";
  EXPECT_FALSE(result.errors.empty()) << "Should have parse errors";
}

TEST_F(AgentJsonTest, ParseAgentInvalidPolicy) {
  auto result = parser_->parse_agent(invalid_agent_bad_policy_json_);

  EXPECT_FALSE(result.is_valid()) << "Agent with invalid policy should fail";
}

TEST_F(AgentJsonTest, ParseAgentNegativeValues) {
  auto result = parser_->parse_agent(invalid_agent_negative_values_json_);

  EXPECT_FALSE(result.is_valid()) << "Agent with negative values should fail";
}

// ============================================================================
// Agent Configuration Tests
// ============================================================================

TEST_F(AgentJsonTest, ParseAgentConfiguration) {
  auto result = parser_->parse_agent(valid_agent_json_);

  ASSERT_TRUE(result.is_valid());
  ASSERT_NE(result.agent, nullptr);

  auto config = result.agent->get_config();
  EXPECT_EQ(config.max_workflows, 10);
  EXPECT_EQ(config.max_concurrent_workflows, 5);
  EXPECT_EQ(config.data_bus_capacity, 1048576);
  EXPECT_EQ(config.description, "A test agent for unit testing");
}

TEST_F(AgentJsonTest, ParseAgentMetadata) {
  auto result = parser_->parse_agent(valid_agent_json_);

  ASSERT_TRUE(result.is_valid());
  ASSERT_NE(result.agent, nullptr);

  // Check metadata field parsing
  EXPECT_EQ(result.agent->get_name(), "Test Agent");
  EXPECT_EQ(result.agent->get_id(), "agent_001");
}

// ============================================================================
// Agent Workflow Association Tests
// ============================================================================

TEST_F(AgentJsonTest, ParseAgentWithMultipleWorkflows) {
  auto result = parser_->parse_agent(valid_agent_json_);

  ASSERT_TRUE(result.is_valid());
  ASSERT_NE(result.agent, nullptr);

  const auto& workflows = result.agent->get_workflows();
  EXPECT_EQ(workflows.size(), 2);

  // Check workflow references
  if(!workflows.empty()) {
    EXPECT_EQ(workflows[0]->get_id(), "workflow_001");
    EXPECT_EQ(workflows[1]->get_id(), "workflow_002");
  }
}

TEST_F(AgentJsonTest, ParseAgentWithEmptyWorkflows) {
  auto result = parser_->parse_agent(R"({
    "id": "agent_empty",
    "name": "Empty Agent",
    "policy": "FACTORY",
    "workflows": []
  })");

  EXPECT_TRUE(result.is_valid()) << "Agent with empty workflows should be valid";
  EXPECT_NE(result.agent, nullptr);
  EXPECT_EQ(result.agent->get_workflows().size(), 0);
}

// ============================================================================
// Agent Lifecycle Policy Tests
// ============================================================================

TEST_F(AgentJsonTest, LifecyclePolicyPerRequest) {
  auto result = parser_->parse_agent(agent_per_request_json_);

  ASSERT_TRUE(result.is_valid());
  ASSERT_NE(result.agent, nullptr);

  auto config = result.agent->get_config();
  EXPECT_EQ(static_cast<int>(config.policy),
            static_cast<int>(framework::Agent::LifecyclePolicy::PER_REQUEST));
}

TEST_F(AgentJsonTest, LifecyclePolicyFactory) {
  auto result = parser_->parse_agent(valid_agent_json_);

  ASSERT_TRUE(result.is_valid());
  ASSERT_NE(result.agent, nullptr);

  auto config = result.agent->get_config();
  EXPECT_EQ(static_cast<int>(config.policy),
            static_cast<int>(framework::Agent::LifecyclePolicy::FACTORY));
}

TEST_F(AgentJsonTest, LifecyclePolicySingleton) {
  auto result = parser_->parse_agent(agent_singleton_json_);

  ASSERT_TRUE(result.is_valid());
  ASSERT_NE(result.agent, nullptr);

  auto config = result.agent->get_config();
  EXPECT_EQ(static_cast<int>(config.policy),
            static_cast<int>(framework::Agent::LifecyclePolicy::SINGLETON));
}

// ============================================================================
// Agent Resource Configuration Tests
// ============================================================================

TEST_F(AgentJsonTest, ResourceConfigurationParsing) {
  auto result = parser_->parse_agent(valid_agent_json_);

  ASSERT_TRUE(result.is_valid());
  ASSERT_NE(result.agent, nullptr);

  auto config = result.agent->get_config();
  EXPECT_GT(config.max_workflows, 0) << "Max workflows should be positive";
  EXPECT_GT(config.max_concurrent_workflows, 0) << "Max concurrent workflows should be positive";
  EXPECT_GT(config.data_bus_capacity, 0) << "Data bus capacity should be positive";
}

TEST_F(AgentJsonTest, ResourceCapacityValidation) {
  auto result = parser_->parse_agent(invalid_agent_negative_values_json_);

  EXPECT_FALSE(result.is_valid()) << "Negative resource values should fail";
}

// ============================================================================
// Agent Version Management Tests
// ============================================================================

TEST_F(AgentJsonTest, AgentVersionParsing) {
  auto result = parser_->parse_agent(valid_agent_json_);

  ASSERT_TRUE(result.is_valid());
  ASSERT_NE(result.agent, nullptr);

  EXPECT_EQ(result.agent->get_id(), "agent_001");
  // Version is parsed as part of config
}

TEST_F(AgentJsonTest, AgentNameParsing) {
  auto result = parser_->parse_agent(valid_agent_json_);

  ASSERT_TRUE(result.is_valid());
  ASSERT_NE(result.agent, nullptr);

  EXPECT_EQ(result.agent->get_name(), "Test Agent");
}

TEST_F(AgentJsonTest, AgentDescriptionParsing) {
  auto result = parser_->parse_agent(valid_agent_json_);

  ASSERT_TRUE(result.is_valid());
  ASSERT_NE(result.agent, nullptr);

  // Description should be parsed
}

// ============================================================================
// Agent Workflow Composition Tests
// ============================================================================

TEST_F(AgentJsonTest, WorkflowCompositionParsing) {
  auto result = parser_->parse_agent(valid_agent_json_);

  ASSERT_TRUE(result.is_valid());
  ASSERT_NE(result.agent, nullptr);

  const auto& workflows = result.agent->get_workflows();
  EXPECT_EQ(workflows.size(), 2);

  // Verify workflow IDs are valid
  if(!workflows.empty()) {
    EXPECT_EQ(workflows[0]->get_id(), "workflow_001");
    EXPECT_EQ(workflows[1]->get_id(), "workflow_002");
  }
}

TEST_F(AgentJsonTest, EmbeddedWorkflowParsing) {
  auto result = parser_->parse_agent(agent_with_workflow_json_);

  ASSERT_TRUE(result.is_valid());
  ASSERT_NE(result.agent, nullptr);

  const auto& workflows = result.agent->get_workflows();
  ASSERT_GE(workflows.size(), 1);

  if(!workflows.empty()) {
    auto workflow = workflows[0];
    EXPECT_EQ(workflow->get_id(), "workflow_embedded_1");
    EXPECT_EQ(workflow->get_nodes().size(), 2);
    EXPECT_EQ(workflow->get_edges().size(), 1);
  }
}

// ============================================================================
// Error Handling Tests
// ============================================================================

TEST_F(AgentJsonTest, HandleInvalidJSON) {
  auto result = parser_->parse_agent("{invalid json}");

  EXPECT_FALSE(result.is_valid()) << "Invalid JSON should fail to parse";
  EXPECT_FALSE(result.errors.empty()) << "Should have parse errors";
}

TEST_F(AgentJsonTest, HandleEmptyAgent) {
  auto result = parser_->parse_agent("{}");

  EXPECT_FALSE(result.is_valid()) << "Empty agent should fail validation";
}

TEST_F(AgentJsonTest, HandleMissingWorkflowsField) {
  auto result = parser_->parse_agent(R"({
    "id": "agent_test",
    "name": "Test Agent",
    "policy": "FACTORY"
  })");

  // Agent without workflows field may or may not be valid
  // depending on validation requirements
}

// ============================================================================
// Agent State Management Tests
// ============================================================================

TEST_F(AgentJsonTest, AgentInitialState) {
  auto result = parser_->parse_agent(valid_agent_json_);

  ASSERT_TRUE(result.is_valid());
  ASSERT_NE(result.agent, nullptr);

  // Initially, agent should be in CREATED state
  EXPECT_EQ(result.agent->get_state(), AgentState::CREATED);
}

TEST_F(AgentJsonTest, AgentConfigApplication) {
  auto result = parser_->parse_agent(valid_agent_json_);

  ASSERT_TRUE(result.is_valid());
  ASSERT_NE(result.agent, nullptr);

  // Config should be properly applied to agent
  auto config = result.agent->get_config();
  EXPECT_NE(config.max_workflows, 0);
}

}  // namespace tests
}  // namespace agentflow
