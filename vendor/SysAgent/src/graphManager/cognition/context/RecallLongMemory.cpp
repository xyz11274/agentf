//
// Created by c00888021 on 2026/1/4.
//
#include "graphManager/cognition/context/RecallLongMemory.h"
#include "graphManager/GraphUtils.h"
#include "graphManager/GraphData.h"
#include "taskManage/TeHelper.h"
#include "memory/memoryMgr.h"
#include "framework_common/constants/number_constant.h"
#include "queue/QueueManager.h"
#include "framework_common/timermanager/timer_manager_model.h"
#include "framework_common/timermanager/timer_manager.h"

#define RECALL_MEMORY_QUERY_TIMER "RECALL_MEMORY_QUERY_TIMER"

RecallLongMemory::RecallLongMemory(const std::string& nodeName,
                                   const mmagentflow::MMNodeConfig& nodeConfig) : BaseNode(nodeName, nodeConfig)
{
}

std::shared_ptr<mmagentflow::MMNode> RecallLongMemory::Clone()
{
    return std::make_shared<RecallLongMemory>(name_, config_);
}

mmagentflow::MMNodeFunctionStatus RecallLongMemory::CancelAsyncRun(std::shared_ptr<mmagentflow::MMMessage> msg)
{
    return mmagentflow::MMNodeFunctionStatus::FUNCTION_COMPLETE;
}

std::shared_ptr<mmagentflow::MMMessage> RecallLongMemory::DataMsgMerge(
        const std::map<std::string, std::shared_ptr<mmagentflow::MMMessage>>& inputmsgs)
{
    return inputmsgs.begin()->second;
}

mmagentflow::MMNodeFunctionStatus RecallLongMemory::Run(std::shared_ptr<mmagentflow::MMMessage> msg)
{
    LOG_INFO_EX("RequestId: %d, Run: %s", msg->GetRequestId(), name_.c_str());
    auto accessCtrl = GraphUtils::GetMessageData<AccessCtrlPtr>(msg, GRAPH_DATA_ACCESSCTRL);
    std::string userInput = accessCtrl->extractInput();
    UserInfo userInfo = TEHelper::extractUserInfo(accessCtrl);
    QueueManager::getInstance().EventMark("info", "RecallMemoryNode.Enter", "success", "recall memory run", accessCtrl);
    auto timeoutTask = std::make_shared<CMFrm::Timer::TimerTask>(
            8 * CMFrm::Constants::Const1000,
            [msg]() {
                LOG_ERR_EX("Recall Long Memory timeout!");
                GraphUtils::NotifyAsyncEvent(msg, std::pair<std::string, bool>("", true));
            });
    CMFrm::Timer::TimerManager::GetTimerManager()->AddTimerTask(timeoutTask);
    GraphUtils::AddMessageData(msg, RECALL_MEMORY_QUERY_TIMER, timeoutTask);
    MemoryMgr::GetLongMemories(userInfo.userName, userInput, CMFrm::Constants::Const1,
                               [msg, accessCtrl](bool isSuccess, const std::vector<std::string>& memories) {
                                   if (isSuccess) {
                                       QueueManager::getInstance().EventMark("info", "RecallMemoryNode.Exit", "success",
                                                                             "recall memory success", accessCtrl);
                                   } else {
                                       QueueManager::getInstance().EventMark("info", "RecallMemoryNode.Exit", "failed",
                                                                             "recall memory failed", accessCtrl);
                                   }
                                   GraphUtils::PrintDebugInfo(msg, "Recall Long Memory");
                                   std::string memory = memories.empty() ? "" : memories[0];
                                   LOG_INFO_EX("Recall long memory is: %s, isSuccess:%d", memory.c_str(), isSuccess);
                                   GraphUtils::NotifyAsyncEvent(msg, std::pair<std::string, bool>(memory, true));
                               });
    return mmagentflow::MMNodeFunctionStatus::FUNCTION_WAIT_ASYNC;
}

mmagentflow::MMNodeFunctionStatus RecallLongMemory::AsyncRun(std::shared_ptr<mmagentflow::MMEvent> event)
{
    auto data = std::dynamic_pointer_cast<BaseData<std::pair<std::string, bool>>>(event->GetData())->getValue();
    auto msg = event->GetMessage();
    LOG_INFO_EX("Recall Memory AsyncRun data is:%d", data.second);
    auto decisionStat = GraphUtils::GetMessageData<std::shared_ptr<DecisionStat>>(msg, GRAPH_DATA_ISDECISION);
    decisionStat->recallMemory = data;
    auto timeoutTask = GraphUtils::GetMessageData<std::shared_ptr<CMFrm::Timer::TimerTask>>(msg, RECALL_MEMORY_QUERY_TIMER);
    if (timeoutTask != nullptr && timeoutTask->GetTask() != nullptr) {
        GraphUtils::AddMessageData(msg, RECALL_MEMORY_QUERY_TIMER, nullptr);
        CMFrm::Timer::TimerManager::GetTimerManager()->DeleteTimerTask(timeoutTask->GetTaskId());
        LOG_INFO_EX("Delete TimeoutTask success!");
    } else {
        LOG_ERR_EX("No need to delete TimeoutTask");
    }
    return mmagentflow::MMNodeFunctionStatus::FUNCTION_COMPLETE;
}