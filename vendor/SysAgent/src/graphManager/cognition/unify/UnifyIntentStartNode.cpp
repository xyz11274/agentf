// Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.

#include "graphManager/cognition/unify/UnifyIntentStartNode.h"
#include "queue/QueueManager.h"

UnifyIntentStartNode::UnifyIntentStartNode(const std::string& nodeName, const mmagentflow::MMNodeConfig& nodeConfig)
    : BaseNode(nodeName, nodeConfig)
{
}

/**
 * 拷贝方法
 * @return
 */
std::shared_ptr<mmagentflow::MMNode> UnifyIntentStartNode::Clone()
{
    return std::make_shared<UnifyIntentStartNode>(name_, config_);
}

/**
 * @param msg
 * @return
 */
mmagentflow::MMNodeFunctionStatus UnifyIntentStartNode::Run(std::shared_ptr<mmagentflow::MMMessage> msg)
{
    LOG_INFO_EX("RequestId: %d, AsyncRun: %s", msg->GetRequestId(), name_.c_str());
    auto workflowID = WorkFlow::GenerateRandomWorkFlowID();
    GraphUtils::AddMessageData(msg, GRAPH_DATA_ISDECISION, std::make_shared<DecisionStat>(false, workflowID));
    auto access = GraphUtils::GetMessageData<AccessCtrlPtr>(msg, GRAPH_DATA_ACCESSCTRL);
    QueueManager::getInstance().EventMark("info", "UnifyIntentStartNode.Enter", "success",
                                          "enter UnifyFastIntentNode run", access);
    auto now = std::chrono::system_clock::now();
    auto timeNow = std::chrono::time_point_cast<std::chrono::milliseconds>(now).time_since_epoch().count();
    LOG_ERR_EX("UnifyIntentStartNode start time: %ld", timeNow);
    return mmagentflow::MMNodeFunctionStatus::FUNCTION_COMPLETE;
}

/**
 *
 * @param event 节点请求id的异步事件
 * @return
 */
mmagentflow::MMNodeFunctionStatus UnifyIntentStartNode::AsyncRun(std::shared_ptr<mmagentflow::MMEvent> event)
{
    return mmagentflow::MMNodeFunctionStatus::FUNCTION_COMPLETE;
}

/**
 *
 * @param msg
 * @return
 */
mmagentflow::MMNodeFunctionStatus UnifyIntentStartNode::CancelAsyncRun(std::shared_ptr<mmagentflow::MMMessage> msg)
{
    return mmagentflow::MMNodeFunctionStatus::FUNCTION_COMPLETE;
}

/**
 * 打断（cancel）场景只有在等待RunAsync存在，不存在在Run方法场景。
 * 增加异常
 * @param globalDatas
 * @return
 */
std::shared_ptr<mmagentflow::MMMessage> UnifyIntentStartNode::DataMsgMerge(
    const std::map<std::string, std::shared_ptr<mmagentflow::MMMessage>>& inputmsgs)
{
    return inputmsgs.begin()->second;
}
