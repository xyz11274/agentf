// Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.

#ifndef SYSAGENT_PARALLEL_LMODELCOG_H
#define SYSAGENT_PARALLEL_LMODELCOG_H

#include "WorkFlow.h"
#include "aisf_platform/common_define.h"
#include "config/Config.h"
#include "graphManager/GraphUtils.h"
#include "graphManager/base/BaseNode.h"
#include "graphManager/GraphData.h"
#include "mm_node.h"
#include "model/ModelUtils.h"
#include "model/PromptManager.h"
#include "model/ReactLLMTask.h"
#include "utils/env_util.h"

class LModelParallelNode : public BaseNode {
public:
    LModelParallelNode(const std::string& nodeName, const mmagentflow::MMNodeConfig& nodeConfig)
        : BaseNode(nodeName, nodeConfig)
    {
    }
    std::shared_ptr<mmagentflow::MMNode> Clone() { return std::make_shared<LModelParallelNode>(name_, config_); }

    /**
     * @param msg
     * @return
     */
    mmagentflow::MMNodeFunctionStatus Run(std::shared_ptr<mmagentflow::MMMessage> msg)
    {
        LOG_INFO_EX("RequestId: %d, Run: %s", msg->GetRequestId(), name_.c_str());
        GraphUtils::AddDebugInfo(msg, name_);
        auto access = GraphUtils::GetMessageData<AccessCtrlPtr>(msg, GRAPH_DATA_ACCESSCTRL);
        QueueManager::getInstance().EventMark("info", "LModelParallelNode.Enter", "success",
                                              "enter LModelParallelNode run", access);
        // 非流式带记忆
        std::string modelServiceName = ConfigManager::getInstance().getEnvInfo("INTENT_IDENTITY_MODEL_SERVICE_NAME",
                                                                               "llmlargeservice");
        mmsdk::MMReasonParam param = {
            .modelServiceName = modelServiceName,
            .modelName = "Qwen3-32B-Intent",
            .maxToken = 1024
        };
        auto decision = GraphUtils::GetMessageData<std::shared_ptr<DecisionStat>>(msg, GRAPH_DATA_ISDECISION);
        auto request = decision->reWriteQ.first.empty() ? access->extractInput() : decision->reWriteQ.first;
        LOG_INFO_EX("LModel parallel request is:%s", request.c_str());
        auto intention = mmsdk::MMUserIntention{.query = request + "/nothink",
                                                .modaltype = mmsdk::MMReasonModalType::MMLLM_MODAL_TYPE_TEXT};
        auto req = mmsdk::MMLLMRequest{.intentions = {intention},
                                       .prompt = {PromptManager::GetInstance().GetPormptTmpl("intent_identification")}};
        LLMomponent->RequestAsync(param, req, [msg](uint32_t errorCode, const std::string& response) -> uint32_t {
            LOG_INFO_EX("LModelParallelNode NotifyAsyncEvent: %s, RequestAsync: %s", Utils::GetThreadName().c_str(),
                        response.c_str());
            GraphUtils::NotifyAsyncEvent(
                msg, ReactLLMTaskEvent{LLMEventType::OnLLMFinalAnswer, OnLLMFinalAnswerEvent{"", "", response}});
            return 0;
        });

        GraphUtils::PrintDebugInfo(msg);
        return mmagentflow::MMNodeFunctionStatus::FUNCTION_WAIT_ASYNC;
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
        LOG_INFO_EX("RequestId: %d, AsyncRun: %s", msg->GetRequestId(), name_.c_str());

        auto access = GraphUtils::GetMessageData<AccessCtrlPtr>(msg, GRAPH_DATA_ACCESSCTRL);
        auto decisionStat = GraphUtils::GetMessageData<std::shared_ptr<DecisionStat>>(msg, GRAPH_DATA_ISDECISION);
        auto request = access->extractInput();
        switch (data.type) {
            case LLMEventType::TaskCreated: {
                auto task = std::get<TaskCreatedEvent>(data.data);
                LOG_INFO_EX("AsyncRun TaskCreated: %s", task.taskId.c_str());
                auto intention = mmsdk::MMUserIntention{.query = decisionStat->reWriteQ.first + "/nothink",
                    .modaltype = mmsdk::MMReasonModalType::MMLLM_MODAL_TYPE_TEXT};
                auto req = mmsdk::MMLLMRequest{
                    .intentions = {intention},
                    .prompt = {PromptManager::GetInstance().GetPormptTmpl("intent_identification")}};
                auto ret = reactLLMomponent->RequestAsync(task.taskId, req, {});
                LOG_INFO_EX("AsyncRun RequestAsyncInvoke: %d", ret);
                GraphUtils::PrintDebugInfo(msg);
                return mmagentflow::MMNodeFunctionStatus::FUNCTION_WAIT_ASYNC;
            }
            case LLMEventType::OnLLMFinalAnswer: {
                return CBOnLLMFinalAnswer(data, msg, request);
            }
            case LLMEventType::TaskDestroyed: {
                auto res = std::get<TaskDestroyedEvent>(data.data);
                LOG_INFO_EX("destroy llm task: %s, %d", res.taskId.c_str(), res.code);
                GraphUtils::PrintDebugInfo(msg);
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

private:
    mmagentflow::MMNodeFunctionStatus CBOnLLMFinalAnswer(const ReactLLMTaskEvent& data,
                                                         std::shared_ptr<mmagentflow::MMMessage> msg,
                                                         std::string request)
    {
        auto answer = std::get<OnLLMFinalAnswerEvent>(data.data);
        LOG_INFO_EX("OnLLMFinalAnswerEvent: %s, %s, %s", answer.taskId.c_str(), answer.reqId.c_str(),
                    answer.answer.c_str());
        // 解析意图
        std::string result = trimThinkFlag(answer.answer);

        auto intentTasks = parseIntentTasks(result, request, ParallelLModel);
        if (intentTasks.size() == 0) {
            // todo 需不需要保底
            intentTasks.push_back(IntentTask("QAAgent", request, ""));
        }

        // 设置任务
        GraphUtils::AddMessageData(msg, GRAPH_DATA_ParallelNodeResult, ParallelReault(LModelResult{intentTasks}));
        GraphUtils::PrintDebugInfo(msg);
        return mmagentflow::MMNodeFunctionStatus::FUNCTION_COMPLETE;
    }
};

#endif  // SYSAGENT_PARALLEL_LMODELCOG_H