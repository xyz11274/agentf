/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 */

#include "graphManager/cognition/context/ReWriteQueryNode.h"
#include "memory/ContextManager.h"
#include "graphManager/GraphUtils.h"
#include "graphManager/GraphData.h"
#include "model/PromptManager.h"
#include "model/ReactLLMTask.h"
#include "queue/QueueManager.h"
#include "config/Config.h"
#include "taskManage/TeamEnvironment.h"
#include "taskManage/TeHelper.h"
#include "memory/Location.h"
#include "queue/Lifecycle/LocationHandler.h"

#define CURRENT_DATE "CURRENT_DATE"
#define REWRITE_QUERY_NODE_TIMER "REWRITE_QUERY_NODE_TIMER"
#define REWRITE_QUERY_TASK_ID "REWRITE_QUERY_TASK_ID"

using namespace ContextManager;

ReWriteQueryNode::ReWriteQueryNode(const std::string& nodeName,
                                   const mmagentflow::MMNodeConfig& nodeConfig) : BaseNode(nodeName, nodeConfig)
{
}

std::shared_ptr<mmagentflow::MMNode> ReWriteQueryNode::Clone()
{
    return std::make_shared<ReWriteQueryNode>(name_, config_);
}

mmagentflow::MMNodeFunctionStatus ReWriteQueryNode::CancelAsyncRun(std::shared_ptr<mmagentflow::MMMessage> msg)
{
    return mmagentflow::MMNodeFunctionStatus::FUNCTION_COMPLETE;
}

std::shared_ptr<mmagentflow::MMMessage> ReWriteQueryNode::DataMsgMerge(
        const std::map<std::string, std::shared_ptr<mmagentflow::MMMessage>>& inputmsgs)
{
    return inputmsgs.begin()->second;
}

mmagentflow::MMNodeFunctionStatus ReWriteQueryNode::Run(std::shared_ptr<mmagentflow::MMMessage> msg)
{
    SetTimeoutTask(msg, REWRITE_QUERY_NODE_TIMER, CMFrm::Constants::Const8, [msg]() {
        LOG_ERR_EX("ReWriteQNode timeout!");
        GraphUtils::NotifyAsyncEvent(msg, std::pair<std::string, bool>("", true));
    });
    auto accessCtrl = GraphUtils::GetMessageData<AccessCtrlPtr>(msg, GRAPH_DATA_ACCESSCTRL);
    auto userInput = accessCtrl->extractInput();
    UserInfo userInfo = TEHelper::extractUserInfo(accessCtrl);
    ContextRequest request {
            .userId = userInfo.userName,
            .sessionId = "",
            .currentAgent = "sysAgent",
            .queryType = "all",
            .query = userInput,
            .rewriteQuery = userInput,
            .limitQARounds = 20,
            .maxQATokens = 2048
    };
    auto decisionStat = GraphUtils::GetMessageData<std::shared_ptr<DecisionStat>>(msg, GRAPH_DATA_ISDECISION);
    std::string workflowID = decisionStat ? decisionStat->workflowID : "";
    ContextManager::GetContext(request, [=](bool isSuccess, std::shared_ptr<ContextResult> result) {
        GraphUtils::PrintDebugInfo(msg, "Recall Short Memory");
        if (!result) {
            LOG_ERR_EX("Recall short memory is empty!");
            GraphUtils::NotifyAsyncEvent(msg, std::pair<std::string, bool>("", true));
            return;
        }
        std::string model = ConfigManager::getInstance().getEnvInfo("REWRITE_QUERY_MODEL", "");
        std::string modelServiceName = ConfigManager::getInstance().getEnvInfo("REWRITE_QUERY_MODEL_TYPE",
                                                                               "llmlargeservice");
        mmsdk::MMReasonParam param = {
                .modelServiceName = modelServiceName,
                .modelName = model,
                .temperature = 0.001,
                .maxToken = 1024
        };
        auto sysPrompt = PromptManager::GetInstance().GetPormptTmpl("rewrite_query_system");
        LOG_INFO_EX("workflowID:%s", workflowID.c_str());
        auto intention = mmsdk::MMUserIntention{.query = userInput + "/nothink",
                .modaltype = mmsdk::MMReasonModalType::MMLLM_MODAL_TYPE_TEXT};
        auto req = mmsdk::MMLLMRequest{
                .intentions = {intention},
                .prompt = {sysPrompt}};
        LOG_INFO_EX("user prompt is:%s", intention.query.c_str());
        std::string contextLog = "[" + AisfPlat::Utils::GetCurrentTimestampInCustomFormat() + "]Question:" + userInput;
        contextLog += "\n======= ReWriteQuery context =======\n";
        auto mixedContext = GetMixedContext(result);
        for (auto &memory : mixedContext) {
            contextLog += memory["role"] + ": " + memory["content"] + "\n";
        }
        std::string timeStr = workflowID;
        CommonFunc::AppendDebugInfoToFile(userInfo.userName, timeStr, contextLog);
        auto task = std::make_shared<ReactLLMTask>();
        task->RegisterCallback(LLMEventType::OnLLMFinalAnswer, [=](const LLMEventData& data) {
            auto answer = std::get<OnLLMFinalAnswerEvent>(data);
            LOG_INFO_EX("ReWriteQuery answer is:%s", answer.answer.c_str());
            std::string finalAnswer = trimThinkFlag(answer.answer);
            finalAnswer = CommonFunc::ExtractJsonString(finalAnswer);
            LOG_INFO_EX("Trim ReWriteQuery answer is:%s", finalAnswer.c_str());
            GraphUtils::NotifyAsyncEvent(msg, std::pair<std::string, bool>(finalAnswer, true));
            std::string prefix = "======= ReWriteQuery result =======\n";
            CommonFunc::AppendDebugInfoToFile(userInfo.userName, timeStr, prefix + finalAnswer);
        });
        task->RegisterCallback(LLMEventType::OnLLMError, [=](const LLMEventData& data) {
            LOG_INFO_EX("ReWrite Query error");
            GraphUtils::NotifyAsyncEvent(msg, std::pair<std::string, bool>("", true));
        });
        reactLLMomponent->CreateTask(param, task, [msg, req, mixedContext](uint32_t errorCode, const std::string& taskId) -> uint32_t {
            auto decisionStat = GraphUtils::GetMessageData<std::shared_ptr<DecisionStat>>(msg, GRAPH_DATA_ISDECISION);
            decisionStat->reWriteQTaskID = taskId;
            CustomedLLMComponent->UpdateUserContext(taskId, mixedContext, [=](uint32_t errorCode) -> uint32_t {
                reactLLMomponent->RequestAsync(taskId, req, {});
                return 0;
            });
            return 0;
        });
    });
    return mmagentflow::MMNodeFunctionStatus::FUNCTION_WAIT_ASYNC;
}

mmagentflow::MMNodeFunctionStatus ReWriteQueryNode::AsyncRun(std::shared_ptr<mmagentflow::MMEvent> event)
{
    auto data = std::dynamic_pointer_cast<BaseData<std::pair<std::string, bool>>>(event->GetData())->getValue();
    auto msg = event->GetMessage();
    LOG_INFO_EX("ReWriteQ AsyncRun data is:%s, result is:%d", data.first.c_str(), data.second);
    auto accessCtrl = GraphUtils::GetMessageData<AccessCtrlPtr>(msg, GRAPH_DATA_ACCESSCTRL);
    QueueManager::getInstance().EventMark("info", "ReWriteNode.Exit", "success", "rewriteQ run", accessCtrl);
    auto decisionStat = GraphUtils::GetMessageData<std::shared_ptr<DecisionStat>>(msg, GRAPH_DATA_ISDECISION);
    auto taskId = decisionStat->reWriteQTaskID;
    if (!taskId.empty()) {
        reactLLMomponent->DestroyTask(taskId,
                                      [](uint32_t errorCode, std::string taskId) -> uint32_t {
                                          LOG_INFO_EX("destroy rewriteQuery task: %s, %d", taskId.c_str(), errorCode);
                                          return 0;
                                      });
    }
    std::string result = accessCtrl->extractInput();
    if (!data.first.empty()) {
        Json::Value root;
        JsonParser::Parse(data.first, root);
        int needReWrite = 0;
        JsonParser::GetInt(root, "need_rewrite", needReWrite);
        LOG_INFO_EX("needReWrite is:%d", needReWrite);
        if (needReWrite == 1) {
            JsonParser::GetString(root, "rewritten_query", result);
        }
        decisionStat->reWriteQ.first = result;
    }
    UserInfo userInfo = TEHelper::extractUserInfo(accessCtrl);
    auto input = decisionStat->reWriteQ.first.empty() ? accessCtrl->extractInput() : decisionStat->reWriteQ.first;
    ContextRequest request {
        .userId = userInfo.userName,
        .currentAgent = "sysAgent",
        .query = input,
        .rewriteQuery = input,
        .limitQARounds = 20,
        .needStartGetMemory = "true"
    };
    GetContext(request, [](bool isSuccess, std::shared_ptr<ContextResult> result) {
        LOG_INFO_EX("Relationship Query result:%d", isSuccess);
    });
    auto timeoutTask = GraphUtils::GetMessageData<std::shared_ptr<CMFrm::Timer::TimerTask>>(msg, REWRITE_QUERY_NODE_TIMER);
    if (timeoutTask != nullptr && timeoutTask->GetTask() != nullptr) {
        GraphUtils::AddMessageData(msg, REWRITE_QUERY_NODE_TIMER, nullptr);
        CMFrm::Timer::TimerManager::GetTimerManager()->DeleteTimerTask(timeoutTask->GetTaskId());
        LOG_INFO_EX("Delete TimeoutTask success!");
    } else {
        LOG_ERR_EX("No need to delete TimeoutTask");
    }
    return mmagentflow::MMNodeFunctionStatus::FUNCTION_COMPLETE;
}