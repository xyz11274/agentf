// Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.

#include "graphManager/cognition/parallel/SModelParallelNode.h"
#include "TeHelper.h"
#include "aisf_platform/common_define.h"
#include "graphManager/GraphData.h"
#include "graphManager/GraphManager.h"
#include "graphManager/GraphUtils.h"
#include "graphManager/GraphData.h"
#include "graphManager/base/BaseNode.h"
#include "mm_agent_graph.h"
#include <cstdlib>
#include <memory>

mmagentflow::MMNodeFunctionStatus SModelParallelNode::Run(std::shared_ptr<mmagentflow::MMMessage> msg)
{
    // 构造一个子图执行，在子图的end节点中通知父图。
    // start    -> 分类
    //          -> 提参    -> 裁决
    LOG_INFO_EX("RequestId: %d, Run: %s", msg->GetRequestId(), name_.c_str());
    GraphUtils::AddDebugInfo(msg, name_);
    auto access = GraphUtils::GetMessageData<AccessCtrlPtr>(msg, GRAPH_DATA_ACCESSCTRL);

    auto graph = GraphManager::GetInstance().GetAgentGraph(AGENT_GRAPH_SUB_INTENT);
    if (!graph) {
        LOG_ERR_EX("RequestId: %d, AsyncRun: %s, sub_intent_process not exist", msg->GetRequestId(), name_.c_str());
        return mmagentflow::MMNodeFunctionStatus::FUNCTION_ERROR;
    }
    auto inputMsg = GraphUtils::NewMessage(graph);
    GraphUtils::AddMessageData(inputMsg, GRAPH_DATA_PARENTGRAPHMSG, msg);
    GraphUtils::AddMessageData(inputMsg, GRAPH_DATA_ACCESSCTRL, access);

    auto config = mmagentflow::MMRequestConfig{.timeout = std::chrono::seconds(30),
                                              .errRunFunc = [=](uint64_t requestId, uint32_t errorCode) -> void {
                                                  LOG_INFO_EX("RequestId: %d, errorCode: %d", requestId, errorCode);
                                              },
                                              .canTrace = true,
                                              .traceFunc = [=](uint64_t requestId, std::string& message) -> void {
                                                  LOG_INFO_EX("RequestId: %d, message: %s", requestId, message.c_str());
                                              }};

    graph->AsyncRun(inputMsg, config);
    GraphUtils::PrintDebugInfo(msg);
    return mmagentflow::MMNodeFunctionStatus::FUNCTION_WAIT_ASYNC;
}

mmagentflow::MMNodeFunctionStatus SModelParallelNode::AsyncRun(std::shared_ptr<mmagentflow::MMEvent> event)
{
    auto sModelResult = std::dynamic_pointer_cast<BaseData<SModelResult>>(event->GetData())->getValue();
    auto msg = event->GetMessage();
    GraphUtils::PrintDebugInfo(msg);
    LOG_INFO_EX("RequestId: %d, AsyncRun: %s", msg->GetRequestId(), name_.c_str());

    GraphUtils::AddMessageData(msg, GRAPH_DATA_ParallelNodeResult, ParallelReault(sModelResult));
    GraphUtils::PrintDebugInfo(msg);
    return mmagentflow::MMNodeFunctionStatus::FUNCTION_COMPLETE;
}