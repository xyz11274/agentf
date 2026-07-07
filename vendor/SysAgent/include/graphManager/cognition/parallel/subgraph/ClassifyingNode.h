// Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.

#ifndef SYSAGENT_CLASSIFYINGNODE_H
#define SYSAGENT_CLASSIFYINGNODE_H

#include "aisf_platform/common_define.h"
#include "aisf_platform/string/string_utils.h"
#include "aisf_platform/utils/utils.h"
#include "config/Config.h"
#include "graphManager/GraphData.h"
#include "graphManager/GraphUtils.h"
#include "graphManager/base/BaseNode.h"
#include "mm_llm_component.h"
#include "mm_node.h"
#include "model/PromptManager.h"
#include "model/ReactLLMTask.h"

constexpr std::string_view ClassifyingUserPrompt = "\n##用户输入\n%s\n";

class ClassifyingNode : public BaseNode {
public:
    ClassifyingNode(const std::string& nodeName, const mmagentflow::MMNodeConfig& nodeConfig)
        : BaseNode(nodeName, nodeConfig)
    {
    }
    std::shared_ptr<mmagentflow::MMNode> Clone() { return std::make_shared<ClassifyingNode>(name_, config_); }

    /**
     * @param msg
     * @return
     */
    mmagentflow::MMNodeFunctionStatus Run(std::shared_ptr<mmagentflow::MMMessage> msg)
    {
        LOG_INFO_EX("RequestId: %d, Run: %s", msg->GetRequestId(), name_.c_str());
        GraphUtils::AddDebugInfo(msg, name_);
        auto access = GraphUtils::GetMessageData<AccessCtrlPtr>(msg, GRAPH_DATA_ACCESSCTRL);
        QueueManager::getInstance().EventMark("info", "ClassifyingNode.Enter", "success",
                                              "enter ClassifyingNode Run", access);
        auto request = access->extractInput();
        std::string modelServiceName = ConfigManager::getInstance().getEnvInfo("INTENT_CLASSIFY_MODEL_SERVICE_NAME",
                                                                               "llmsmallservice");
        mmsdk::MMReasonParam reasonParam = {
            .modelServiceName = modelServiceName,
            .modelName = "qwen3-4B",
            .temperature = 0.001,
            .maxToken = 1024
        };

        auto query = AisfPlat::StringUtils::Format(512, ClassifyingUserPrompt.data(), request.c_str(), request.c_str());
        auto intention = mmsdk::MMUserIntention{.query = query + "/nothink",
                                                .modaltype = mmsdk::MMReasonModalType::MMLLM_MODAL_TYPE_TEXT};
        auto req = mmsdk::MMLLMRequest{.intentions = {intention},
                                       .prompt = {PromptManager::GetInstance().GetPormptTmpl("scene_classification")}};
        QueueManager::getInstance().EventMark("info", "ClassifyingNode.ModelBegin", "success",
                              "ClassifyingNode invoke model begin", access);
        auto ret = LLMomponent->RequestAsync(reasonParam, req,
                                             [msg, access](uint32_t errorCode, const std::string& response) -> uint32_t {
                                                 LOG_INFO_EX("ClassifyingNode NotifyAsyncEvent: %s, RequestAsync: %s",
                                                             Utils::GetThreadName().c_str(), response.c_str());
                                                 std::string result = errorCode == 0 ? "success" : "failed";
                                                 QueueManager::getInstance().EventMark("info", "ClassifyingNode.ModelEnd", result,
                                                                       "ClassifyingNode invoke model end, invoke " + result, access);
                                                 GraphUtils::NotifyAsyncEvent(msg, response);
                                                 return 0;
                                             });
        LOG_INFO_EX("RequestId: %d, Classifying Res: %d", msg->GetRequestId(), ret);
        GraphUtils::PrintDebugInfo(msg);
        QueueManager::getInstance().EventMark("info", "ClassifyingNode.Exit", "success",
                              "exit ClassifyingNode Run", access);
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
        auto data = std::dynamic_pointer_cast<BaseData<std::string>>(event->GetData())->getValue();
        auto msg = event->GetMessage();
        LOG_INFO_EX("RequestId: %d, AsyncRun: %s", msg->GetRequestId(), name_.c_str());
        GraphUtils::PrintDebugInfo(msg);

        // 去掉前后空白
        auto result = AisfPlat::StringUtils::Strip(trimThinkFlag(data));
        // 添加分类结果
        GraphUtils::AddMessageData(msg, GRAPH_DATA_Classifying, result);
        GraphUtils::PrintDebugInfo(msg);
        return mmagentflow::MMNodeFunctionStatus::FUNCTION_COMPLETE;
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
};

#endif  // SYSAGENT_CLASSIFYINGNODE_H