//
// Created by c00888021 on 2026/1/4.
//
#include "graphManager/cognition/context/ReWriteQ.h"
#include "graphManager/GraphUtils.h"
#include "graphManager/GraphData.h"
#include "taskManage/TeHelper.h"
#include "memory/memoryMgr.h"
#include "framework_common/constants/number_constant.h"
#include "model/PromptManager.h"
#include "utils/env_util.h"
#include "queue/QueueManager.h"
#include "framework_common/timermanager/timer_manager_model.h"
#include "framework_common/timermanager/timer_manager.h"

#define CURRENT_DATE "CURRENT_DATE"
#define REWRITE_QUERY_TIMER "REWRITE_QUERY_TIMER"

ReWriteQ::ReWriteQ(const std::string& nodeName,
                   const mmagentflow::MMNodeConfig& nodeConfig) : BaseNode(nodeName, nodeConfig)
{
}

std::shared_ptr<mmagentflow::MMNode> ReWriteQ::Clone()
{
    return std::make_shared<ReWriteQ>(name_, config_);
}

mmagentflow::MMNodeFunctionStatus ReWriteQ::CancelAsyncRun(std::shared_ptr<mmagentflow::MMMessage> msg)
{
    return mmagentflow::MMNodeFunctionStatus::FUNCTION_COMPLETE;
}

std::shared_ptr<mmagentflow::MMMessage> ReWriteQ::DataMsgMerge(
        const std::map<std::string, std::shared_ptr<mmagentflow::MMMessage>>& inputmsgs)
{
    return inputmsgs.begin()->second;
}

mmagentflow::MMNodeFunctionStatus ReWriteQ::Run(std::shared_ptr<mmagentflow::MMMessage> msg)
{
    auto accessCtrl = GraphUtils::GetMessageData<AccessCtrlPtr>(msg, GRAPH_DATA_ACCESSCTRL);
    std::string userInput = accessCtrl->extractInput();
    UserInfo userInfo = TEHelper::extractUserInfo(accessCtrl);
    auto currentDate = CommonFunc::GetCurrentDateWithWeekday();
    GraphUtils::AddMessageData(msg, CURRENT_DATE, currentDate);
    QueueManager::getInstance().EventMark("info", "ReWriteNode.Enter", "success", "rewriteQ run", accessCtrl);
    MemoryMgr::GetShortMemories(
            userInfo.userName,
            "",
            CMFrm::Constants::Const5,
                [msg, userInput](bool isSuccess, const std::vector<std::map<string, string>>& memories) {
                    GraphUtils::PrintDebugInfo(msg, "Recall Short Memory");
                    if (memories.empty()) {
                        LOG_ERR_EX("LModelSerial recall short memory is empty!");
                        GraphUtils::NotifyAsyncEvent(msg, std::pair<std::string, bool>("", true));
                        return;
                    }
                    auto timeoutTask = std::make_shared<CMFrm::Timer::TimerTask>(
                            3 * CMFrm::Constants::Const1000,
                            [msg]() {
                                LOG_ERR_EX("ReWriteQ timeout!");
                                GraphUtils::NotifyAsyncEvent(msg, std::pair<std::string, bool>("", true));
                            }
                    );
                    CMFrm::Timer::TimerManager::GetTimerManager()->AddTimerTask(timeoutTask);
                    GraphUtils::AddMessageData(msg, REWRITE_QUERY_TIMER, timeoutTask);
                    std::string modelServiceName = CMFrm::Utils::GetEnv("INTENT_REWRITE_QUERY_SERVICE_NAME", "llmmediumservice");
                    mmsdk::MMReasonParam param = {
                        .modelServiceName = modelServiceName,
                        .modelName = "Qwen3-8B",
                        .maxToken = 1024
                    };
                    std::string context = "";
                    for (auto& memory : memories) {
                        auto it = memory.find("content");
                        if (it != memory.end()) {
                            context += memory.at("content") + "\n";
                        }
                    }
                    auto userPrompt = PromptManager::GetInstance().GetPormptTmpl("rewrite_query_user");
                    std::string currentDate = GraphUtils::GetMessageData<std::string>(msg, CURRENT_DATE);
                    userPrompt.RenderPrompt({{"current_date", currentDate}, {"user_input", userInput}, {"context_input", context}});
                    LOG_INFO_EX("userPrompt is:%s", userPrompt.GetContent().c_str());
                    auto intention = mmsdk::MMUserIntention{.query = userPrompt.GetContent() + "/nothink",
                        .modaltype = mmsdk::MMReasonModalType::MMLLM_MODAL_TYPE_TEXT};
                    auto req = mmsdk::MMLLMRequest{
                        .intentions = {intention},
                        .prompt = {PromptManager::GetInstance().GetPormptTmpl("rewrite_query_system")}};
                    mmsdk::MMReasonParam toolParam = {};
                    LLMomponent->RequestAsync(param, req, [msg](uint32_t errorCode, const std::string& response) -> uint32_t {
                        GraphUtils::PrintDebugInfo(msg, "ReWrite Query");
                        GraphUtils::NotifyAsyncEvent(msg, std::pair<std::string, bool>(response, true));
                        LOG_INFO_EX("rewrite query result is:%s", response.c_str());
                        return 0;
                    });
            });
    return mmagentflow::MMNodeFunctionStatus::FUNCTION_WAIT_ASYNC;
}

mmagentflow::MMNodeFunctionStatus ReWriteQ::AsyncRun(std::shared_ptr<mmagentflow::MMEvent> event)
{
    auto data = std::dynamic_pointer_cast<BaseData<std::pair<std::string, bool>>>(event->GetData())->getValue();
    auto msg = event->GetMessage();
    LOG_INFO_EX("ReWriteQ AsyncRun data is:%d", data.second);
    auto accessCtrl = GraphUtils::GetMessageData<AccessCtrlPtr>(msg, GRAPH_DATA_ACCESSCTRL);
    QueueManager::getInstance().EventMark("info", "ReWriteNode.Exit", "success", "rewriteQ run", accessCtrl);
    auto decisionStat = GraphUtils::GetMessageData<std::shared_ptr<DecisionStat>>(msg, GRAPH_DATA_ISDECISION);
    decisionStat->reWriteQ = data;
    auto timeoutTask = GraphUtils::GetMessageData<std::shared_ptr<CMFrm::Timer::TimerTask>>(msg, REWRITE_QUERY_TIMER);
    if (timeoutTask != nullptr && timeoutTask->GetTask() != nullptr) {
        GraphUtils::AddMessageData(msg, REWRITE_QUERY_TIMER, nullptr);
        CMFrm::Timer::TimerManager::GetTimerManager()->DeleteTimerTask(timeoutTask->GetTaskId());
        LOG_INFO_EX("Delete TimeoutTask success!");
    } else {
        LOG_ERR_EX("No need to delete TimeoutTask");
    }
    return mmagentflow::MMNodeFunctionStatus::FUNCTION_COMPLETE;
}