// Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.

#include "graphManager/cognition/unify/UnifyIntentDecisionNode.h"

#include "aisf_platform/common_define.h"

#include "graphManager/GraphData.h"
#include "graphManager/GraphUtils.h"
#include "queue/QueueManager.h"
#include "model/PromptManager.h"
#include "graphManager/cognition/parallel/ExtractToolNode.h"

UnifyIntentDecisionNode::UnifyIntentDecisionNode(const std::string& nodeName,
                                                 const mmagentflow::MMNodeConfig& nodeConfig) : BaseNode(nodeName, nodeConfig)
{
}

std::shared_ptr<mmagentflow::MMNode> UnifyIntentDecisionNode::Clone()
{
    return std::make_shared<UnifyIntentDecisionNode>(name_, config_);
}

/**
 *
 * @param requestId 处理请求id
 * @param event 节点请求id的异步事件
 * @return
 */
mmagentflow::MMNodeFunctionStatus UnifyIntentDecisionNode::AsyncRun(std::shared_ptr<mmagentflow::MMEvent> event)
{
    return mmagentflow::MMNodeFunctionStatus::FUNCTION_COMPLETE;
}

/**
 *
 * @param msg
 * @return
 */
mmagentflow::MMNodeFunctionStatus UnifyIntentDecisionNode::CancelAsyncRun(std::shared_ptr<mmagentflow::MMMessage> msg)
{
    return mmagentflow::MMNodeFunctionStatus::FUNCTION_COMPLETE;
}

/**
 * 打断（cancel）场景只有在等待RunAsync存在，不存在在Run方法场景。
 * 增加异常
 * @param globalDatas
 * @return
 */
std::shared_ptr<mmagentflow::MMMessage> UnifyIntentDecisionNode::DataMsgMerge(
        const std::map<std::string, std::shared_ptr<mmagentflow::MMMessage>>& inputmsgs)
{
    return inputmsgs.begin()->second;
}

std::map<std::string, std::string> intentEventMap = {
        {"FoodieAgent", "event-food"},
        {"AnyShoppingAgent", "event-shopping"},
        {"LifeAssistantAgent", "event-hotel-airticket-cab"},
        {"QAAgent", "event-QA"},
        {"SystemAgent", "event-deepResearch"},
        {"CustomAgent", "event-custom"},
        {"ThirdProxyAgent", "event-thirdProxy"}
};

/**
 * Run
 * @param msg
 * @return
 */
mmagentflow::MMNodeFunctionStatus UnifyIntentDecisionNode::Run(std::shared_ptr<mmagentflow::MMMessage> msg)
{
    LOG_INFO_EX("RequestId: %d, Run: %s", msg->GetRequestId(), name_.c_str());
    GraphUtils::AddDebugInfo(msg, name_);
    auto access = GraphUtils::GetMessageData<AccessCtrlPtr>(msg, GRAPH_DATA_ACCESSCTRL);
    QueueManager::getInstance().EventMark("info", "UnifyIntentDecisionNode.Enter", "success",
                                          "enter UnifyIntentDecisionNode run", access);
    auto decisionStat = GraphUtils::GetMessageData<std::shared_ptr<DecisionStat>>(msg, GRAPH_DATA_ISDECISION);
    std::string toolSelect = CommonFunc::ExtractJsonString(decisionStat->extractTool.first);
    std::vector<IntentTask> intentTasks = decisionStat->additionalIntentTasks;
    // AgentTasks 有QAAgent，才会用工具选择的结果
    if (decisionStat->containQA) {
        auto qaTasks = ExtractToolNode::ParseToolSelection(toolSelect);
        intentTasks.insert(intentTasks.end(), qaTasks.begin(), qaTasks.end());
    }
    if (intentTasks.empty() && decisionStat->agentTasks.empty()) {
        // 兜底走问答
        LOG_INFO_EX("QATask and AgentTask is empty");
        IntentTask intentTask{"QAAgent", access->extractInput(), ""};
        intentTasks.push_back(intentTask);
    }
    auto agentData = IntentTask::GetAgentData(intentTasks);
    GraphUtils::AddMessageData(msg, GRAPH_DATA_PLANEXAMPLE, PlanExample("UnifyFastIntent", intentTasks, agentData));
    QueueManager::getInstance().EventMark("info", "UnifyIntentDecisionNode.Exit", "success",
                                          "exit UnifyIntentDecisionNode run", access);
    return mmagentflow::MMNodeFunctionStatus::FUNCTION_COMPLETE;
}