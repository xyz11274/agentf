// Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.

#ifndef SYSAGENT_EXECUTENODE_H
#define SYSAGENT_EXECUTENODE_H

#include "TEManager.h"
#include "aisf_platform/common_define.h"
#include "graphManager/GraphData.h"
#include "graphManager/GraphUtils.h"
#include "graphManager/base/BaseNode.h"
#include "mm_node.h"
#include "taskManage/WorkFlow.h"

class ExecuteNode : public BaseNode {
public:
    ExecuteNode(const std::string& nodeName, const mmagentflow::MMNodeConfig& nodeConfig)
        : BaseNode(nodeName, nodeConfig)
    {
    }
    std::shared_ptr<mmagentflow::MMNode> Clone() { return std::make_shared<ExecuteNode>(name_, config_); }

    /**
     * @param msg
     * @return
     */
    mmagentflow::MMNodeFunctionStatus Run(std::shared_ptr<mmagentflow::MMMessage> msg)
    {
        LOG_INFO_EX("RequestId: %d, Run: %s", msg->GetRequestId(), name_.c_str());
        GraphUtils::AddDebugInfo(msg, name_);

        auto access = GraphUtils::GetMessageData<AccessCtrlPtr>(msg, GRAPH_DATA_ACCESSCTRL);
        QueueManager::getInstance().EventMark("info", "ExecuteNode.enter", "success", "enter ExecuteNode", access);

        GraphUtils::AddMessageData(msg, "ExecuteNode", "sucess");

        auto teamEnv = TEManager::getInstance().getTEById(access->header.teamEnvironmentID);
        if (!teamEnv) {
            LOG_ERR_EX("get TeamEnv failed: %s", access->header.teamEnvironmentID.c_str());
            return mmagentflow::MMNodeFunctionStatus::FUNCTION_ERROR;
        }

        auto workFlow = teamEnv->getWorkFlow();  // 直接拿第一个
        if (!workFlow) {
            LOG_ERR_EX("get workFlow failed: %s", access->header.teamEnvironmentID.c_str());
            return mmagentflow::MMNodeFunctionStatus::FUNCTION_ERROR;
        }

        // 操控类 子意图识别，manipulative_intent_identification
        if (workFlow->HasTaskNotFinished()) {
            if (LLMRequestForSubIntent(msg, workFlow)) {
                return mmagentflow::MMNodeFunctionStatus::FUNCTION_WAIT_ASYNC;
            } else {
                LOG_INFO_EX("sub intent infer failed!");
                return mmagentflow::MMNodeFunctionStatus::FUNCTION_ERROR;
            }
        }

        // 执行任务
        workFlow->ExecTask(access->header.taskID, access);
        GraphUtils::PrintDebugInfo(msg);
        QueueManager::getInstance().EventMark("info", "ExecuteNode.exit", "success", "exit ExecuteNode", access);
        return mmagentflow::MMNodeFunctionStatus::FUNCTION_COMPLETE;
    }

    /**
     *
     * @param requestId 处理请求id
     * @param event 节点请求id的异步事件
     * @return
     */
    mmagentflow::MMNodeFunctionStatus AsyncRun(std::shared_ptr<mmagentflow::MMEvent> event)
    {
        auto data = std::dynamic_pointer_cast<BaseData<ReactLLMTaskEvent>>(event->GetData())->getValue();
        auto msg = event->GetMessage();
        LOG_INFO_EX("ExecuteNode RequestId: %d, AsyncRun: %s", msg->GetRequestId(), name_.c_str());

        switch (data.type) {
            case LLMEventType::OnLLMError: {
                auto& res = std::get<OnLLMErrorEvent>(data.data);
                LOG_INFO_EX("llm error stage: %s, error msg: %s", res.taskId.c_str(), res.errorMsg.c_str());
                return mmagentflow::MMNodeFunctionStatus::FUNCTION_ERROR;
            }
            case LLMEventType::OnLLMFinalAnswer: {
                auto& res = std::get<OnLLMFinalAnswerEvent>(data.data);
                LOG_INFO_EX("llm request OnLLMFinalAnswer, stage: %s llm final answer msg: %s",
                            res.taskId.c_str(), res.answer.c_str());
                // 解析子意图识别的结果
                ParseLLMResponse(msg, res.answer);
                auto access = GraphUtils::GetMessageData<AccessCtrlPtr>(msg, GRAPH_DATA_ACCESSCTRL);
                auto teamEnv = TEManager::getInstance().getTEById(access->header.teamEnvironmentID);
                CHECK_NULL_LOG_ERROR_RETURN(teamEnv, mmagentflow::MMNodeFunctionStatus::FUNCTION_ERROR, "te is empty!");
                auto workFlow = teamEnv->getWorkFlow();
                auto taskNode = workFlow->GetTopTaskNode();
                CHECK_TRUE_LOG_ERROR_RETURN(taskNode == nullptr,
                                            mmagentflow::MMNodeFunctionStatus::FUNCTION_ERROR, "taskNodes is empty!");
                access->setTaskID(taskNode->task->findTaskID());
                workFlow->ExecTask(access->header.taskID, access);
                return mmagentflow::MMNodeFunctionStatus::FUNCTION_COMPLETE;
            }
            default:
                return mmagentflow::MMNodeFunctionStatus::FUNCTION_COMPLETE;
        }
    }

    /**
     *
     * @param msg
     * @return
     */
    mmagentflow::MMNodeFunctionStatus CancelAsyncRun(std::shared_ptr<mmagentflow::MMMessage> msg)
    {
        return mmagentflow::MMNodeFunctionStatus::FUNCTION_COMPLETE;
    }

    /**
     * 打断（cancel）场景只有在等待RunAsync存在，不存在在Run方法场景。
     * 增加异常
     * @param globalDatas
     * @return
     */
    std::shared_ptr<mmagentflow::MMMessage> DataMsgMerge(
        const std::map<std::string, std::shared_ptr<mmagentflow::MMMessage>>& inputmsgs)
    {
        return inputmsgs.begin()->second;
    }

    bool LLMRequestForSubIntent(std::shared_ptr<mmagentflow::MMMessage> msg, std::shared_ptr<WorkFlow> workFlow)
    {
        std::string request;
        workFlow->GetIntentForTopTask(request);
        std::string modelServiceName = ConfigManager::getInstance().getEnvInfo("INTENT_IDENTITY_MODEL_SERVICE_NAME",
                                                                               "llmlargeservice");
        mmsdk::MMReasonParam param = {
            .modelServiceName = modelServiceName,
            .modelName = "Qwen3-32B-Intent",
            .temperature = 0.001,
            .maxToken = 1024
        };
        // 操控类意图识别
        auto prompt = PromptManager::GetInstance().GetPormptTmpl("manipulative_intent_identification");
        auto intention = mmsdk::MMUserIntention{.query = request,
                                                .modaltype = mmsdk::MMReasonModalType::MMLLM_MODAL_TYPE_TEXT};
        auto req = mmsdk::MMLLMRequest{.intentions = {intention}, .prompt = {prompt}};

        auto retCode = LLMomponent->RequestAsync(param, req,
        [msg](uint32_t errorCode, const std::string& response) -> uint32_t {
            std::string contextLog = "";
            contextLog += "======= ExecuteNode result =======\n" + response;
            auto decisionStat = GraphUtils::GetMessageData<std::shared_ptr<DecisionStat>>(msg, GRAPH_DATA_ISDECISION);
            auto accessCtrl = GraphUtils::GetMessageData<AccessCtrlPtr>(msg, GRAPH_DATA_ACCESSCTRL);
            std::string workflowID = (decisionStat == nullptr) ? "" : decisionStat->workflowID;
            UserInfo userInfo = TEHelper::extractUserInfo(accessCtrl);
            CommonFunc::AppendDebugInfoToFile(userInfo.userName, workflowID, contextLog);
            LOG_INFO_EX("ExecuteNode NotifyAsyncEvent: %s, RequestAsync: %s", Utils::GetThreadName().c_str(),
                        response.c_str());
            auto result = trimThinkFlag(response);
            if (errorCode != mmsdk::MMErrorCode::SUCCESS) {
                LOG_ERR_EX("LLMRequestForSubIntent error, errorCode: %d", errorCode);
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

    const std::unordered_map<std::string, std::string> agentTypeMap = {
        {"GeneralShoppingAgent", "AnyShoppingAgent"},
        {"TravelBookingAgent", "LifeAssistantAgent"},
        {"InstantRetailAgent", "FoodieAgent"},
    };

    void ParseLLMResponse(std::shared_ptr<mmagentflow::MMMessage> msg, const std::string& response)
    {
        auto access = GraphUtils::GetMessageData<AccessCtrlPtr>(msg, GRAPH_DATA_ACCESSCTRL);
        auto teamEnv = TEManager::getInstance().getTEById(access->header.teamEnvironmentID);
        CHECK_NULL_LOG_ERROR_RETURN_VOID(teamEnv, "teamEnv is null");
        auto workFlow = teamEnv->getWorkFlow();
        CHECK_NULL_LOG_ERROR_RETURN_VOID(workFlow, "workFlow is null");
        std::string trimResponse = CommonFunc::ExtractJsonString(response);
        Json::Value root;
        JsonParser::Parse(trimResponse, root);
        for (const auto& item : root) {
            if (JsonParser::IsMember(item, "assistant_name") && JsonParser::IsMember(item, "context")) {
                std::string agentName;
                std::string requestContent;
                JsonParser::GetString(item, "assistant_name", agentName);
                JsonParser::GetString(item, "context", requestContent);
                std::string taskID = workFlow->GenNewTaskID();
                Data data;
                std::string convertAgentName = agentTypeMap.count(agentName) ? agentTypeMap.at(agentName) : "GUIAgent";
                auto agent = Agent::getAgentInstance(convertAgentName);
                LOG_INFO_EX("Agent task add taskID:%s, convertAgentName:%s, agentName:%s", taskID.c_str(),
                            convertAgentName.c_str(), agentName.c_str());
                auto task = std::make_shared<ATATask>(taskID, agent, convertAgentName, requestContent, data);
                if (convertAgentName == "GUIAgent") {
                    task->isGUIGeneralization = true;
                }
                workFlow->AddNewTask(std::make_shared<TaskNode>(task));
            }
        }
    }
};

#endif  // SYSAGENT_EXECUTENODE_H