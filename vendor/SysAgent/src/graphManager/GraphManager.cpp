// Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.

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
#include "graphManager/cognition/parallel/ExtractToolNode.h"
#include "graphManager/cognition/parallel/DecisionNode.h"
#include "graphManager/cognition/parallel/LModelParallelNode.h"
#include "graphManager/cognition/parallel/ParallelStartNode.h"
#include "graphManager/cognition/parallel/RagParallelNode.h"
#include "graphManager/cognition/parallel/SModelParallelNode.h"
#include "graphManager/cognition/parallel/subgraph/AdjudicationNode.h"
#include "graphManager/cognition/parallel/subgraph/ClassifyingNode.h"
#include "graphManager/cognition/parallel/subgraph/ExtractParamNode.h"
#include "graphManager/cognition/parallel/subgraph/SubEndNode.h"
#include "graphManager/cognition/unify/UnifyFastIntentNode.h"
#include "graphManager/cognition/unify/UnifyIntentDecisionNode.h"
#include "graphManager/cognition/unify/UnifyIntentStartNode.h"
#include "graphManager/cognition/context/ReWriteQueryNode.h"
#include "graphManager/execute/ExecuteNode.h"
#include "graphManager/perceptions/PerceptionNode.h"
#include "mm_graph_manager.h"

void GraphManager::Init()
{
    mmagentflow::MMGraphManager::GetInstance().SetConfig(mmagentflow::MMGraphConfig{
            .schedStrategy = mmagentflow::MMGraphScheduleStrategy::SharedMultiThreadStrategy,
    });

    mmagentflow::MMScheduler::GetInstance().SetConfig(mmagentflow::MMScheduleConfig{.workerThreadNum = 16});

    // 静态图
    auto mmGraph = mmagentflow::MMGraphManager::GetInstance().CreateMMGraph("intent_process");
    // 开始
    auto start = NewNode<StartNode>(GRAPH_NODE_STARTNNODE, {.timeOut = std::chrono::seconds(3)});
    // 意图感知
    auto perception = NewNode<PerceptionNode>(GRAPH_NODE_PerceptionNode, {.timeOut = std::chrono::seconds(3)});
    // 规则
    auto rule = NewNode<RuleNode>(GRAPH_NODE_RuleNode, {.timeOut = std::chrono::seconds(3)});

    // 并行意图认知
    auto parallelStart = NewNode<ParallelStartNode>("ParallelStartNode", {.timeOut = std::chrono::seconds(3)});
    auto smParallel = NewNode<SModelParallelNode>("SModelParallelNode", {.timeOut = std::chrono::seconds(10)});
    auto lmReWriteQueryNode = NewNode<ReWriteQueryNode>("lmReWriteQueryNode", {.timeOut = std::chrono::seconds(10)});
    auto lmExtractToolParallel = NewNode<ExtractToolNode>("LMExtractToolNode",{.timeOut = std::chrono::seconds(10)});
    auto lmParallel = NewNode<LModelParallelNode>("LModelParallelNode", {.timeOut = std::chrono::seconds(10)});
    auto decision = NewNode<DecisionNode>("DecisionNode", {.timeOut = std::chrono::seconds(10)});

    // 统一意图认知
    auto unifyIntentStart = NewNode<UnifyIntentStartNode>("UnifyIntentStartNode",
                                                          {.timeOut = std::chrono::seconds(3)});
    auto unifyReWriteQueryNode = NewNode<ReWriteQueryNode>("UnifyReWriteQueryNode",
                                                           {.timeOut = std::chrono::seconds(10)});
    auto unifyFastIntentNode = NewNode<UnifyFastIntentNode>("UnifyFastIntentNode",
                                                            {.timeOut = std::chrono::seconds(10)});
    auto unifyExtractToolNode = NewNode<ExtractToolNode>("UnifyExtractToolNode",
                                                         {.timeOut = std::chrono::seconds(10)});
    auto unifyIntentDecisionNode = NewNode<UnifyIntentDecisionNode>("UnifyIntentDecisionNode",
                                                                    {.timeOut = std::chrono::seconds(10)});

    // 规划
    auto plan = NewNode<PlanNode>(GRAPH_NODE_PlanNode, {.timeOut = std::chrono::seconds(3)});

    // 执行
    auto execute = NewNode<ExecuteNode>("ExecuteNode", {.timeOut = std::chrono::seconds(3)});

    // 结束
    auto end = NewNode<EndNode>("EndNode", {.timeOut = std::chrono::seconds(3)});
    mmGraph->AddEdge({NewMMEdge(start, perception), NewMMEdge(perception, rule),
                      NewMMEdge(rule, std::bind(&GraphManager::ConditionFunc, this, std::placeholders::_1)),
                      NewMMEdge(unifyIntentStart, unifyReWriteQueryNode),
                      NewMMEdge(unifyReWriteQueryNode, unifyFastIntentNode),
                      NewMMEdge(unifyReWriteQueryNode, unifyExtractToolNode),
                      NewMMEdge(unifyFastIntentNode, unifyIntentDecisionNode, true),
                      NewMMEdge(unifyExtractToolNode, unifyIntentDecisionNode, true),
                      NewMMEdge(unifyIntentDecisionNode, plan),
                      NewMMEdge(parallelStart, smParallel),
                      NewMMEdge(parallelStart, lmReWriteQueryNode),
                      NewMMEdge(lmReWriteQueryNode, lmExtractToolParallel), NewMMEdge(lmReWriteQueryNode, lmParallel),
                      NewMMEdge(smParallel, decision),
                      NewMMEdge(lmParallel, decision), NewMMEdge(lmExtractToolParallel, decision),
                      NewMMEdge(decision, plan), NewMMEdge(plan, execute),
                      NewMMEdge(execute, end)});

    mmGraph->SetStartNode(start);
    mmGraph->SetEndNode(end);
    static_graphs_[STATIC_GRAPH_INTENT] = mmGraph;

    // 并发意图，小模型子图
    auto subStart = NewNode<StartNode>("SubStartNode", {.timeOut = std::chrono::seconds(3)});
    auto subClassifying = NewNode<ClassifyingNode>("ClassifyingNode", {.timeOut = std::chrono::seconds(10)});
    auto subExtractParam = NewNode<ExtractParamNode>("ExtractParamNode", {.timeOut = std::chrono::seconds(10)});
    auto subAdjudication = NewNode<AdjudicationNode>("AdjudicationNode", {.timeOut = std::chrono::seconds(10)});
    auto subEnd = NewNode<SubEndNode>("SubEndNode", {.timeOut = std::chrono::seconds(3)});

    auto subGraph = mmagentflow::MMGraphManager::GetInstance().CreateMMGraph("sub_intent_process");
    subGraph->AddEdge({NewMMEdge(subStart, subClassifying), NewMMEdge(subStart, subExtractParam),
                       NewMMEdge(subClassifying, subAdjudication, true),
                       NewMMEdge(subExtractParam, subAdjudication, true), NewMMEdge(subAdjudication, subEnd)});
    subGraph->SetStartNode(subStart);
    subGraph->SetEndNode(subEnd);
    static_graphs_[STATIC_GRAPH_SUB_INTENT] = subGraph;

    // 创建并调度动态图
    auto intentGraph =
            mmagentflow::MMGraphManager::GetInstance().CreateMMAgentGraph(static_graphs_[STATIC_GRAPH_INTENT]);
    auto subIntentGraph =
            mmagentflow::MMGraphManager::GetInstance().CreateMMAgentGraph(static_graphs_[STATIC_GRAPH_SUB_INTENT]);
    auto scheduleParam = mmagentflow::MMScheduleParam{.priority = 1};
    agent_graphs_[AGENT_GRAPH_INTENT] = intentGraph;
    agent_graphs_[AGENT_GRAPH_SUB_INTENT] = subIntentGraph;
    mmagentflow::MMScheduler::GetInstance().Sched(intentGraph, scheduleParam);
    mmagentflow::MMScheduler::GetInstance().Sched(subIntentGraph, scheduleParam);
}

template <typename T>
std::shared_ptr<mmagentflow::MMNode> GraphManager::NewNode(std::string name, mmagentflow::MMNodeConfig config)
{
    auto node = std::make_shared<T>(name, config);
    GraphManager::GetInstance().RegisterNode(name, node);
    return node;
}

void GraphManager::RegisterNode(std::string name, std::shared_ptr<mmagentflow::MMNode> node)
{
    nodes_.emplace(name, node);
}

GraphManager::MMAgentGraphPtr GraphManager::GetAgentGraph(const std::string& name)
{
    auto it = agent_graphs_.find(name);
    if (it != agent_graphs_.end()) {
        return it->second;
    }
    LOG_ERR_EX("next node not found: %s", string(name).c_str());
    return nullptr;
}

std::vector<std::shared_ptr<mmagentflow::MMNode>> GraphManager::ConditionFunc(
        std::shared_ptr<mmagentflow::MMMessage> msg) const
{
    auto nextNode = GraphUtils::GetMessageData<NextNode>(msg, GRAPH_DATA_NEXTNODE);
    LOG_INFO_EX("next node: %d", nextNode.next.size());

    std::vector<std::shared_ptr<mmagentflow::MMNode>> nexts;
    for (const auto& node : nextNode.next) {
        auto it = nodes_.find(node);
        if (it == nodes_.end()) {
            LOG_ERR_EX("next node not found: %s", node.c_str());
            continue;
        }
        nexts.push_back(it->second);
    }
    return nexts;
};