// Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.

#include "Task.h"
#include "WorkFlow.h"
#include "aisf_platform/common_define.h"

#include "graphManager/GraphData.h"
#include "graphManager/GraphUtils.h"
#include "graphManager/cognition/parallel/DecisionNode.h"
#include "mm_node.h"
#include <iostream>
#include <memory>
#include "queue/QueueManager.h"
#include "model/ReactLLMTask.h"
#include "graphManager/cognition/parallel/ExtractToolNode.h"

std::vector<IntentTask> mergeQATask(const std::string& userInput,
                                    const std::vector<IntentTask>& originTasks,
                                    const std::string& qaIntentWithTool)
{
    // 构建QATask
    auto qaTasks = ExtractToolNode::ParseToolSelection(qaIntentWithTool);
    std::vector<IntentTask> mergedTasks;
    uint32_t j = 0;
    for (uint32_t i = 0; i < originTasks.size(); i ++) {
        auto originTask = originTasks[i];
        if (originTask.agentName != "QAAgent") {
            mergedTasks.push_back(originTask);
        } else if (j < qaTasks.size() && qaTasks[j].data.toolName != "aisf_movie") {
            auto qaTask = qaTasks[j++];
            mergedTasks.push_back(qaTask);
        }
    }
    while (j < qaTasks.size()) {
        if (qaTasks[j].data.toolName != "aisf_movie") {
            mergedTasks.push_back(qaTasks[j]);
        }
        j ++;
    }
    CHECK_TRUE_LOG_ERROR_RETURN(mergedTasks.empty(), {IntentTask("QAAgent", userInput, "")},
                                "mergedTasks tools is empty!");
    return mergedTasks;
}

mmagentflow::MMNodeFunctionStatus DecisionNode::HandleRAGResult(const ParallelReault& result,
                                                                std::shared_ptr<mmagentflow::MMMessage> msg,
                                                                const std::string& request)
{
    auto ragRes = std::get<RAGResult>(result.result);
    LOG_INFO_EX("RequestId: %d, Run: %s, Using RAG Result", msg->GetRequestId(), name_.c_str());
    if (!ragRes.similar) {
        LOG_INFO_EX("RequestId: %d, Run: %s, RAG not match", msg->GetRequestId(), name_.c_str());
        return mmagentflow::MMNodeFunctionStatus::FUNCTION_WAIT_ASYNC;
    }

    auto ragData = Data(ragRes.category, ragRes.intent, ragRes.params);
    auto task = std::vector<IntentTask>{IntentTask("CommandAgent", request, "", ParallelRAG, ragData)};
    GraphUtils::AddMessageData(msg, GRAPH_DATA_PLANEXAMPLE, PlanExample("parallel", task));
    return mmagentflow::MMNodeFunctionStatus::FUNCTION_COMPLETE;
}

mmagentflow::MMNodeFunctionStatus DecisionNode::HandleSModelResult(const ParallelReault& result,
                                                                   std::shared_ptr<mmagentflow::MMMessage> msg,
                                                                   const std::string& request)
{
    auto smRes = std::get<SModelResult>(result.result);
    LOG_INFO_EX("RequestId: %d, Run: %s, Using Small Model Result", msg->GetRequestId(), name_.c_str());
    if (!smRes.match) {
        LOG_INFO_EX("RequestId: %d, Run: %s, Small Model not match", msg->GetRequestId(), name_.c_str());
        return mmagentflow::MMNodeFunctionStatus::FUNCTION_WAIT_ASYNC;
    }

    auto tasks = {IntentTask(getAgentName(smRes.categoryId), request, "", ParallelSModel,
                             Data(smRes.categoryId, smRes.intent, smRes.params), smRes.paramsMatch)};
    auto agentData = IntentTask::GetAgentData(tasks);
    GraphUtils::AddMessageData(msg, GRAPH_DATA_PLANEXAMPLE, PlanExample("parallel", tasks, agentData));
    return mmagentflow::MMNodeFunctionStatus::FUNCTION_COMPLETE;
}

mmagentflow::MMNodeFunctionStatus DecisionNode::HandleLModelResult(const ParallelReault& result,
                                                                   std::shared_ptr<mmagentflow::MMMessage> msg,
                                                                   std::shared_ptr<DecisionStat> data)
{
    LOG_INFO_EX("extract tool result:%s, value:%d", data->extractTool.first.c_str(), data->extractTool.second);
    auto lmRes = std::get<LModelResult>(result.result);
    LOG_INFO_EX("RequestId: %d, Run: %s, Using LLM Result", msg->GetRequestId(), name_.c_str());
    for (auto& task : lmRes.tasks) {
        updateTask(task);
    }

    if (!data->extractTool.second) {
        LOG_INFO_EX("extract tool not finished");
        data->planExample = PlanExample("parallel", lmRes.tasks);
        return mmagentflow::MMNodeFunctionStatus::FUNCTION_WAIT_ASYNC;
    } else {
        auto accessCtrl = GraphUtils::GetMessageData<AccessCtrlPtr>(msg, GRAPH_DATA_ACCESSCTRL);
        std::string userInput = accessCtrl->extractInput();
        lmRes.tasks = mergeQATask(userInput, lmRes.tasks, data->extractTool.first);
        auto agentData = IntentTask::GetAgentData(lmRes.tasks);
        GraphUtils::AddMessageData(msg, GRAPH_DATA_PLANEXAMPLE, PlanExample("parallel", lmRes.tasks, agentData));
        return mmagentflow::MMNodeFunctionStatus::FUNCTION_COMPLETE;
    }
}

mmagentflow::MMNodeFunctionStatus DecisionNode::HandleQAToolExtractResult(const ParallelReault& result,
                                                                          std::shared_ptr<mmagentflow::MMMessage> msg,
                                                                          std::shared_ptr<DecisionStat> data)
{
    LOG_INFO_EX("extract tool result:%s, value:%d", data->extractTool.first.c_str(), data->extractTool.second);
    if (!data->planExample.tasks.empty()) {
        auto accessCtrl = GraphUtils::GetMessageData<AccessCtrlPtr>(msg, GRAPH_DATA_ACCESSCTRL);
        std::string userInput = accessCtrl->extractInput();
        auto tasks = mergeQATask(userInput, data->planExample.tasks, data->extractTool.first);
        auto agentData = IntentTask::GetAgentData(tasks);
        LOG_INFO_EX("LModelParallel node and extract tool finished!");
        GraphUtils::AddMessageData(msg, GRAPH_DATA_PLANEXAMPLE, PlanExample("parallel", tasks, agentData));
        return mmagentflow::MMNodeFunctionStatus::FUNCTION_COMPLETE;
    } else {
        LOG_INFO_EX("LModelParallel node not finished!");
        return mmagentflow::MMNodeFunctionStatus::FUNCTION_WAIT_ASYNC;
    }
}

mmagentflow::MMNodeFunctionStatus DecisionNode::Run(std::shared_ptr<mmagentflow::MMMessage> msg)
{
    LOG_INFO_EX("RequestId: %d, Run: %s", msg->GetRequestId(), name_.c_str());
    GraphUtils::AddDebugInfo(msg, name_);
    auto access = GraphUtils::GetMessageData<AccessCtrlPtr>(msg, GRAPH_DATA_ACCESSCTRL);
    QueueManager::getInstance().EventMark("info", "DecisionNode.Enter", "success",
                                          "enter DecisionNode run", access);
    auto request = access->extractInput();
    // 并发执行，加锁保护
    auto data = GraphUtils::GetMessageData<std::shared_ptr<DecisionStat>>(msg, GRAPH_DATA_ISDECISION);
    std::lock_guard lock(data->mut);
    if (data->isDecision) {
        // 已决策，退出挂起
        LOG_INFO_EX("decisioned skip: %s", name_.c_str());
        return mmagentflow::MMNodeFunctionStatus::FUNCTION_WAIT_ASYNC;
    }

    auto result = GraphUtils::GetMessageData<ParallelReault>(msg, GRAPH_DATA_ParallelNodeResult);
    mmagentflow::MMNodeFunctionStatus status = mmagentflow::MMNodeFunctionStatus::FUNCTION_ERROR;
    switch(result.type) {
        case ParallelReaultType::RAG:
            status = HandleRAGResult(result, msg, request);
            break;
        case ParallelReaultType::SModel:
            status = HandleSModelResult(result, msg, request);
            break;
        case ParallelReaultType::LModel:
            status = HandleLModelResult(result, msg, data);
            break;
        case ParallelReaultType::QAToolExtract:
            status = HandleQAToolExtractResult(result, msg, data);
            break;
        default:
            LOG_INFO_EX("DecisionNode run result type invalid");
            break;
    }
    if (status == mmagentflow::MMNodeFunctionStatus::FUNCTION_COMPLETE) {
        data->isDecision = true;
        LOG_INFO_EX("decisioned end: %s", name_.c_str());
        GraphUtils::PrintDebugInfo(msg);
        QueueManager::getInstance().EventMark("info", "DecisionNode.Exit", "success", "exit DecisionNode run", access);
    }
    return status;
}

std::string DecisionNode::getAgentName(const std::string& categoryId)
{
    if (categoryId == "1" || categoryId == "2" || categoryId == "3") {
        return "CommandAgent";
    } else if (categoryId == "4") {
        return "MovieAgent";
    } else {
        return "CommandAgent";
    }
}

void DecisionNode::updateTask(IntentTask& task)
{
    if (task.agentName == "CommonAgent") {
        task.agentName = "CommandAgent";
        task.data.category = "1";
    } else if (task.agentName == "AVCallAgent") {
        task.agentName = "CommandAgent";
        task.data.category = "2";
    } else if (task.agentName == "FitnessAgent") {
        task.data.category = "3";
        task.agentName = "CommandAgent";
    } else if (task.agentName == "MovieAgent") {
        task.data.category = "4";
    }
}