// Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.

#include "graphManager/cognition/unify/UnifyFastIntentNode.h"

#include <future>
#include "utils/common_func.h"
#include "TeHelper.h"

UnifyFastIntentNode::UnifyFastIntentNode(const std::string& nodeName,
    const mmagentflow::MMNodeConfig& nodeConfig) : BaseNode(nodeName, nodeConfig)
{
}

std::shared_ptr<mmagentflow::MMNode> UnifyFastIntentNode::Clone()
{
    return std::make_shared<UnifyFastIntentNode>(name_, config_);
}

mmagentflow::MMNodeFunctionStatus UnifyFastIntentNode::CancelAsyncRun(std::shared_ptr<mmagentflow::MMMessage> msg)
{
    return mmagentflow::MMNodeFunctionStatus::FUNCTION_COMPLETE;
}

std::shared_ptr<mmagentflow::MMMessage> UnifyFastIntentNode::DataMsgMerge(
    const std::map<std::string, std::shared_ptr<mmagentflow::MMMessage>>& inputmsgs)
{
    return inputmsgs.begin()->second;
}

mmagentflow::MMNodeFunctionStatus UnifyFastIntentNode::Run(std::shared_ptr<mmagentflow::MMMessage> msg)
{
    LOG_INFO_EX("RequestId: %d, Run: %s", msg->GetRequestId(), name_.c_str());
    GraphUtils::AddDebugInfo(msg, name_);
    auto access = GraphUtils::GetMessageData<AccessCtrlPtr>(msg, GRAPH_DATA_ACCESSCTRL);
    QueueManager::getInstance().EventMark("info", "UnifyFastIntentRecognize.Enter", "success",
                                          "intent recognize run", access);
    auto decision = GraphUtils::GetMessageData<std::shared_ptr<DecisionStat>>(msg, GRAPH_DATA_ISDECISION);
    auto request = decision->reWriteQ.first.empty() ? access->extractInput() : decision->reWriteQ.first;
    // 意图识别：(GUI、云盘、IOT)
    if (!LLMRequestForIntentAnalysis(msg, request)) {
        LOG_ERR_EX("LLMRequestForIntentAnalysis failed, reqeust %s", request.c_str());
        QueueManager::getInstance().EventMark("info", "UnifyFastIntentRecognize.Exit", "failed",
                                              "intent recognize failed", access);
        return mmagentflow::MMNodeFunctionStatus::FUNCTION_ERROR;
    }
    return mmagentflow::MMNodeFunctionStatus::FUNCTION_WAIT_ASYNC;
}

mmagentflow::MMNodeFunctionStatus UnifyFastIntentNode::AsyncRun(std::shared_ptr<mmagentflow::MMEvent> event)
{
    auto data = std::dynamic_pointer_cast<BaseData<ReactLLMTaskEvent>>(event->GetData())->getValue();
    auto msg = event->GetMessage();
    LOG_INFO_EX("UnifyFastIntentNode RequestId: %d, AsyncRun: %s", msg->GetRequestId(), name_.c_str());

    auto access = GraphUtils::GetMessageData<AccessCtrlPtr>(msg, GRAPH_DATA_ACCESSCTRL);
    auto decision = GraphUtils::GetMessageData<std::shared_ptr<DecisionStat>>(msg, GRAPH_DATA_ISDECISION);
    auto request = decision->reWriteQ.first.empty() ? access->extractInput() : decision->reWriteQ.first;
    auto& res = std::get<OnLLMFinalAnswerEvent>(data.data);
    std::string log = "======= UnifyFastIntent =======\n" + res.answer;
    UserInfo userInfo = TEHelper::extractUserInfo(access);
    CommonFunc::AppendDebugInfoToFile(userInfo.userName, decision->workflowID, log);
    switch (data.type) {
        case LLMEventType::OnLLMError: {
            LOG_ERR_EX("llm request error, error stage: %s", res.taskId.c_str());
            return mmagentflow::MMNodeFunctionStatus::FUNCTION_ERROR;
        }
        case LLMEventType::OnLLMFinalAnswer: {
            LOG_INFO_EX("llm request OnLLMFinalAnswer, stage: %s llm final answer msg: %s", res.taskId.c_str(),
                res.answer.c_str());
            QueueManager::getInstance().EventMark("info", "UnifyFastIntentRecognize.Exit", "success",
                                                  "intent recognize success", access);
            ParseLLMResponse(res.answer, decision);
            return mmagentflow::MMNodeFunctionStatus::FUNCTION_COMPLETE;
        }
        default:
            return mmagentflow::MMNodeFunctionStatus::FUNCTION_COMPLETE;
    }
}

bool UnifyFastIntentNode::LLMRequestForIntentAnalysis(std::shared_ptr<mmagentflow::MMMessage> msg,
    const std::string& request)
{
    std::string modelServiceName = ConfigManager::getInstance().getEnvInfo("INTENT_IDENTITY_MODEL_SERVICE_NAME",
                                                                           "llmlargeservice");
    mmsdk::MMReasonParam param = {
        .modelServiceName = modelServiceName,
        .modelName = "Qwen3-32B-Intent",
        .temperature = 0.001,
        .maxToken = 1024
    };
    // 操控类意图识别
    auto prompt = PromptManager::GetInstance().GetPormptTmpl("intent_classification");
    auto intention = mmsdk::MMUserIntention{.query = request + "/nothink",
        .modaltype = mmsdk::MMReasonModalType::MMLLM_MODAL_TYPE_TEXT};
    auto req = mmsdk::MMLLMRequest{.intentions = {intention}, .prompt = {prompt}};

    auto retCode = LLMomponent->RequestAsync(param, req,
        [msg](uint32_t errorCode, const std::string& response) -> uint32_t {
            LOG_INFO_EX("UnifyFastIntentNode NotifyAsyncEvent: %s, RequestAsync: %s", Utils::GetThreadName().c_str(),
                response.c_str());
            auto result = trimThinkFlag(response);
            if (errorCode != mmsdk::MMErrorCode::SUCCESS) {
                LOG_ERR_EX("LLMRequestForIntentAnalysis error, errorCode: %d", errorCode);
                GraphUtils::NotifyAsyncEvent(msg, ReactLLMTaskEvent{LLMEventType::OnLLMError,
                        OnLLMErrorEvent{"", "", result}});
            }
            GraphUtils::NotifyAsyncEvent(msg, ReactLLMTaskEvent{LLMEventType::OnLLMFinalAnswer,
                    OnLLMFinalAnswerEvent{"", "", result}});
            return 0;
        });

    if (retCode != mmsdk::MMErrorCode::SUCCESS) {
        LOG_INFO_EX("LLMRequestForIntentAnalysis RequestAsync failed with error code %u", retCode);
        return false;
    }
    return true;
}

void UnifyFastIntentNode::ParseLLMResponse(const std::string& llmOutput, std::shared_ptr<DecisionStat> decision)
{
    std::string trimOutput = CommonFunc::ExtractJsonString(llmOutput);
    LOG_INFO_EX("trim agentTask res is:%s", trimOutput.c_str());
    Json::Value root;
    JsonParser::Parse(trimOutput, root);
    std::vector<AgentTask> guiAgentTasks;
    for (const auto& item : root) {
        if (JsonParser::IsMember(item, "assistant_name") && JsonParser::IsMember(item, "context")) {
            AgentTask task;
            JsonParser::GetString(item, "assistant_name", task.type);
            JsonParser::GetString(item, "context", task.intent);
            if (task.type == "QAAgent") {
                decision->containQA = true;
            } else if (task.type == "InstallAgent") {
                IntentTask intentTask{"ToolAgent", task.intent, ""};
                decision->additionalIntentTasks.push_back(intentTask);
            } else if (task.type == "CloudAgent") {
                IntentTask intentTask{"CloudAgent", task.intent, ""};
                decision->additionalIntentTasks.push_back(intentTask);
            } else if (task.type == "IOTControlAgent") {
                decision->agentTasks.push_back(std::make_shared<AgentTask>("GUIAgent", task.intent));
            } else if (task.type == "GUIAgent") {
                guiAgentTasks.push_back(task);
            }
        }
    }
    auto guiAgent = std::make_shared<AgentTask>("GUIAgent", "");
    for (auto& task : guiAgentTasks) {
        guiAgent->intent += task.intent + ";";
    }
    if (!guiAgent->intent.empty()) {
        decision->agentTasks.push_back(guiAgent);
    }
}