/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2020-2025. All rights reserved.
 */
#include "mockcpp/mockcpp.hpp"
#include "gtest/gtest.h"
#include <securec.h>
#include "graphManager/GraphManager.h"
#include <functional>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include "aisf_platform/common_define.h"
#include "graphManager/GraphData.h"
#include "graphManager/GraphUtils.h"
#include "graphManager/base/EndNode.h"
#include "graphManager/base/StartNode.h"
#include "graphManager/cognition/PlanNode.h"
#include "graphManager/cognition/RuleNode.h"
#include "graphManager/cognition/parallel/DecisionNode.h"
#include "graphManager/cognition/parallel/LModelParallelNode.h"
#include "graphManager/cognition/parallel/ParallelStartNode.h"
#include "graphManager/cognition/parallel/RagParallelNode.h"
#include "graphManager/cognition/parallel/SModelParallelNode.h"
#include "graphManager/cognition/parallel/subgraph/AdjudicationNode.h"
#include "graphManager/cognition/parallel/subgraph/ClassifyingNode.h"
#include "graphManager/cognition/parallel/subgraph/ExtractParamNode.h"
#include "graphManager/cognition/parallel/subgraph/SubEndNode.h"
#include "graphManager/cognition/serial/LModelSerialNode.h"
#include "graphManager/cognition/serial/RagSerialNode.h"
#include "graphManager/execute/ExecuteNode.h"
#include "graphManager/perceptions/PerceptionNode.h"
#include "mm_graph_manager.h"
#include "mm_node.h"
#include "mm_inner_agent_graph_config.h"

using namespace mmagentflow;

class TestGraphManagerInit : public ::testing::Test {
protected:
    virtual void SetUp()
    {
    }

    virtual void TearDown()
    {
    }

public:
    static void SetUpTestCase()
    {
        MMAgentflowConfig config {
                .maxSchedOnceTaskNum = 1000,
                .maxSchedTaskNum = 1000,
                .maxReqQueLen = 1000,
                .maxAgentGraphNum = 1000,
                .maxMMNodeNumPerGraph = 100,
                .maxMMGraphNum = 100
        };
        MMInnerAgentGraphConfig::GetInstance().SetMMAgentflowConfig(config);
        GraphManager::GetInstance().Init();
    }

    static void TearDownTestCase()
    {
    }
};

TEST_F(TestGraphManagerInit, ShouldCreateStaticGraphAndSubGraph_WhenInitCalled) {
    GraphManager& instance = GraphManager::GetInstance();
    std::string teId = "123456";
    auto ret = instance.CreateAgentGraph(teId, MMScheduleParam{
            .priority = 1,
    });
    EXPECT_EQ(ret, true);

    auto graph1 = instance.GetAgentGraph(teId, AGENT_GRAPH_INTENT);
    auto graph2 = instance.GetAgentGraph(teId, AGENT_GRAPH_SUB_INTENT);
    EXPECT_NE(graph1, nullptr);
    EXPECT_NE(graph2, nullptr);

    instance.DeleteAgentGraph(teId);
    graph1 = instance.GetAgentGraph(teId, AGENT_GRAPH_INTENT);
    graph2 = instance.GetAgentGraph(teId, AGENT_GRAPH_SUB_INTENT);
    EXPECT_EQ(graph1, nullptr);
    EXPECT_EQ(graph2, nullptr);

}
