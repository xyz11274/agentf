// Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.

#ifndef SYSAGENT_ADJUDICATIONNODE_H
#define SYSAGENT_ADJUDICATIONNODE_H

#include <string>
#include <string_view>
#include "aisf_platform/common_define.h"
#include "aisf_platform/string/string_utils.h"
#include "config/Config.h"
#include "graphManager/GraphData.h"
#include "graphManager/GraphUtils.h"
#include "graphManager/base/BaseNode.h"
#include "mm_llm_component.h"
#include "mm_message_factory.h"
#include "mm_node.h"
#include "model/ModelUtils.h"
#include "model/PromptManager.h"
#include "model/ReactLLMTask.h"

constexpr std::string_view AdjudicationUserPrompt = "\n##bert输出\n%s\n##用户输入\n%s\n";

class AdjudicationNode : public BaseNode {
public:
    AdjudicationNode(const std::string& nodeName, const mmagentflow::MMNodeConfig& nodeConfig)
        : BaseNode(nodeName, nodeConfig)
    {
    }
    std::shared_ptr<mmagentflow::MMNode> Clone() { return std::make_shared<AdjudicationNode>(name_, config_); }

    Json::Value getJsonStrFromParamsAndIntent(BertReault& bertReault)
    {
        // 根据intent_category.json 里面的 intent参数的类型，和当前data里面的params组装指定的字符串
        auto intentMap = ConfigManager::getInstance().getIntents();
        const auto& intent = intentMap.at(bertReault.intent);
        auto params = intent.params;
        std::map<std::string, std::string> paramNameToType;
        for (const auto& intentParam : params) {
            paramNameToType[intentParam.name] = intentParam.type;
        }
        Json::Value paramsJson;
        for (const auto& [paramName, paramValue] : bertReault.params) {
            if (paramNameToType.find(paramName) == paramNameToType.end()) {
                LOG_ERR_EX("paramName [%s] is not in intent params", paramName.c_str());
                continue;
            }
            std::string paramType = paramNameToType[paramName];
            if (paramType == "string") {
                paramsJson[paramName] = paramValue;
            } else if (paramType == "int") {
                int intData = 0;
                bool isNumeric = AisfPlat::StringUtils::SafeAtoi(paramValue.data(), intData);
                paramsJson[paramName] = isNumeric ?
                                        Json::Value(intData) :
                                        Json::Value(paramValue);
            }
        }
        return paramsJson;
    }

    /**
     * @param msg
     * @return
     */
    mmagentflow::MMNodeFunctionStatus Run(std::shared_ptr<mmagentflow::MMMessage> msg)
    {
        LOG_INFO_EX("RequestId: %d, Run: %s", msg->GetRequestId(), name_.c_str());
        GraphUtils::AddDebugInfo(msg, name_);
        auto access = GraphUtils::GetMessageData<AccessCtrlPtr>(msg, GRAPH_DATA_ACCESSCTRL);
        QueueManager::getInstance().EventMark("info", "AdjudicationNode.Enter", "success",
                                              "enter AdjudicationNode Run", access);
        auto classifying = GraphUtils::GetMessageData<std::string>(msg, GRAPH_DATA_Classifying);
        auto extractParam = GraphUtils::GetMessageData<BertReault>(msg, GRAPH_DATA_ExtractParam);

        // 需要对Bert提取的结果中的参数进行转换，5分钟 -》 300，Bert里面的
        LOG_INFO_EX("RequestId: %d, Run: %s, Classifying: %s", msg->GetRequestId(), name_.c_str(), classifying.c_str());
        LOG_INFO_EX("RequestId: %d, Run: %s, BertReault: %s", msg->GetRequestId(), name_.c_str(),
                    extractParam.intent.c_str());

        // 检查场景分类，场景分类只保留观影
        if (classifying != "4") {
            GraphUtils::AddMessageData(msg, GRAPH_DATA_Adjudication, Adjudication{.match = false});
            LOG_INFO_EX("RequestId: %d, Run: %s， classifying is other", msg->GetRequestId(), name_.c_str());
            GraphUtils::PrintDebugInfo(msg);
            return mmagentflow::MMNodeFunctionStatus::FUNCTION_COMPLETE;
        }

        // todo 获取当前场景所有的意图，检查提惨获取的意图是否当前场景
        auto find = ConfigManager::getInstance().getIntents().find(extractParam.intent);
        if (find == ConfigManager::getInstance().getIntents().end() || find->second.categoryId != classifying) {
            GraphUtils::AddMessageData(msg, GRAPH_DATA_Adjudication, Adjudication{.match = false});
            LOG_INFO_EX("RequestId: %d, Run: %s, not match", msg->GetRequestId(), name_.c_str());
            GraphUtils::PrintDebugInfo(msg);
            return mmagentflow::MMNodeFunctionStatus::FUNCTION_COMPLETE;
        }

        if (classifying == "4") {
            GraphUtils::AddMessageData(msg, GRAPH_DATA_Adjudication, Adjudication{.match = true});
            LOG_INFO_EX("RequestId: %d, Run: %s， classifying is other", msg->GetRequestId(), name_.c_str());
            GraphUtils::PrintDebugInfo(msg);
            return mmagentflow::MMNodeFunctionStatus::FUNCTION_COMPLETE;
        }

        // bert 参数转换
        Json::Value convertBertResult = getJsonStrFromParamsAndIntent(extractParam);
        auto request = access->extractInput();
        std::string modelServiceName = ConfigManager::getInstance().getEnvInfo("INTENT_ADJUDICATION_MODEL_SERVICE_NAME", "llmsmallservice");
        mmsdk::MMReasonParam reasonParam = {
            .modelServiceName = modelServiceName,
            .modelName = "qwen3-4B",
            .temperature = 0.001,
            .maxToken = 1024
        };

        // 根据classifying选择prompt
        // {"relativeTime": "1分30秒"}，拼接意图：{"intent": 'INTENT_INC_VOLUME', "args": [{"relativeTime": "1分30秒"}]}
        Json::Value root;
        root["intent"] = extractParam.intent;
        root["args"] = convertBertResult;
        // 要求intent在前
        std::string result = "{\"intent\":\"" + extractParam.intent + "\",\"args\":" +
                JsonParser::ToRawString(convertBertResult) + "}";
        LOG_INFO_EX("bert result is:%s", result.c_str());
        auto query = AisfPlat::StringUtils::Format(512, AdjudicationUserPrompt.data(), result.c_str(),
                                                   request.c_str());

        auto intention = mmsdk::MMUserIntention{.query = query + "/nothink",
                                                .modaltype = mmsdk::MMReasonModalType::MMLLM_MODAL_TYPE_TEXT};

        // todo 意图裁决prompt
        auto req =
            mmsdk::MMLLMRequest{.intentions = {intention},
                                .prompt = {PromptManager::GetInstance().GetIntentAdjudicatecPrompt(classifying)}};
        std::cout << "-----------------" << std::endl;
        std::cout << "prompt is: " << req.prompt[0].GetContent() << std::endl;
        std::cout << "request is: " << intention.query << std::endl;
        std::cout << "-----------------" << std::endl;
        QueueManager::getInstance().EventMark("info", "AdjudicationNode.ModelBegin", "success",
                                              "AdjudicationNode Model Begin", access);
        auto ret = LLMomponent->RequestAsync(
            reasonParam, req, [msg, access](uint32_t errorCode, const std::string& response) -> uint32_t {
                std::cout << "response is:" << response << std::endl;
                LOG_INFO_EX("AdjudicationNode NotifyAsyncEvent: RequestAsync: %s", response.c_str());
                std::string result = errorCode == 0 ? "success" : "failed";
                QueueManager::getInstance().EventMark("info", "AdjudicationNode.ModelEnd", result,
                                                      "AdjudicationNode Model End, " + result, access);
                GraphUtils::NotifyAsyncEvent(msg, response);
                return 0;
            });
        LOG_INFO_EX("RequestId: %d, Classifying Res: %d", msg->GetRequestId(), ret);
        GraphUtils::PrintDebugInfo(msg);
        QueueManager::getInstance().EventMark("info", "AdjudicationNode.Exit", "success",
                                              "exit AdjudicationNode Run", access);
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
        // 0-意图不匹配，1-参数不匹配，2-都匹配
        if (result == "0") {
            LOG_INFO_EX("RequestId: %d, AsyncRun: %s, Intent recog error", msg->GetRequestId(), name_.c_str());
            GraphUtils::AddMessageData(msg,
                                       GRAPH_DATA_Adjudication,
                                       Adjudication{.match = false, .paramsMatch = false});
        } else if (result == "1") {
            LOG_INFO_EX("RequestId: %d, AsyncRun: %s, params recog error", msg->GetRequestId(), name_.c_str());
            GraphUtils::AddMessageData(msg,
                                       GRAPH_DATA_Adjudication,
                                       Adjudication{.match = true, .paramsMatch = false});
        } else if (result == "2") {
            LOG_INFO_EX("RequestId: %d, AsyncRun: %s, Intent recog success", msg->GetRequestId(), name_.c_str());
            GraphUtils::AddMessageData(msg,
                                       GRAPH_DATA_Adjudication,
                                       Adjudication{.match = true, .paramsMatch = true});
        } else {
            // 意图裁决异常
            LOG_INFO_EX("RequestId: %d, AsyncRun: %s, adjust failed", msg->GetRequestId(), name_.c_str());
            GraphUtils::AddMessageData(msg,
                                       GRAPH_DATA_Adjudication,
                                       Adjudication{.match = false, .paramsMatch = false});
        }
        GraphUtils::PrintDebugInfo(msg);
        auto access = GraphUtils::GetMessageData<AccessCtrlPtr>(msg, GRAPH_DATA_ACCESSCTRL);
        QueueManager::getInstance().EventMark("info", "AdjudicationNode.Exit", "success",
                                              "exit AdjudicationNode AsyncRun", access);
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
        auto context = inputmsgs.begin()->second->GetContext();
        auto graph = std::any_cast<std::shared_ptr<mmagentflow::MMAgentGraph>>(context[MM_KEY_CURRENT_AGENT_GRAPH]);
        auto result_message =
            mmagentflow::MMMessageFactory::GetInstance().NewMessage(graph, inputmsgs.begin()->second->GetRequestId());

        // 合并参数
        // ClassifyingNode      ->
        // ExtractParamNode     ->
        for (auto& [key, msg] : inputmsgs) {
            for (auto& [key, value] : msg->GetData()) {
                result_message->GetData()[key] = value->DeepCopy();
            }
        }

        return result_message;
    }
};

#endif  // SYSAGENT_ADJUDICATIONNODE_H