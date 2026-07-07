// Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.

#ifndef SYSAGENT_EXTRACTPARAMNODE_H
#define SYSAGENT_EXTRACTPARAMNODE_H

#include "aisf_platform/common_define.h"
#include "aisf_platform/parser/json_parser.h"
#include "config/Config.h"
#include "graphManager/GraphUtils.h"
#include "graphManager/base/BaseNode.h"
#include "graphManager/GraphData.h"
// libwebsockets Expand宏定义和rapidjson 冲突
#ifdef Expand
#undef Expand
#endif
#include "http_client_factory.h"
#include "http_method.h"
#include "json/value.h"
#include "mm_node.h"
#include "model/ModelUtils.h"

#include <memory>

class ExtractParamNode : public BaseNode {
public:
    ExtractParamNode(const std::string& nodeName, const mmagentflow::MMNodeConfig& nodeConfig)
        : BaseNode(nodeName, nodeConfig)
    {
    }
    std::shared_ptr<mmagentflow::MMNode> Clone() { return std::make_shared<ExtractParamNode>(name_, config_); }

    /**
     * @param msg
     * @return
     */
    mmagentflow::MMNodeFunctionStatus Run(std::shared_ptr<mmagentflow::MMMessage> msg)
    {
        LOG_INFO_EX("RequestId: %d, Run: %s", msg->GetRequestId(), name_.c_str());
        GraphUtils::AddDebugInfo(msg, name_);
        auto access = GraphUtils::GetMessageData<AccessCtrlPtr>(msg, GRAPH_DATA_ACCESSCTRL);
        QueueManager::getInstance().EventMark("info", "ExtractParamNode.Enter", "success",
                                              "enter ExtractParamNode Run", access);
        auto request = access->extractInput();

        Json::Value texts(Json::arrayValue);
        texts.append(request);
        Json::Value body;
        body["texts"] = texts;

        // 是否可以使用http2客户端给http1发消息
        auto client = CMFrm::COM::HttpClientFactory::GetHttpClientFactory()->GetHttp1Client("http1_default");
        auto req = std::make_shared<CMFrm::COM::Http1ClientRequest>();
        req->SetMethod(CMFrm::COM::HttpMethod::POST);
        req->SetReqUrl("http://9.15.87.225:8001/predict");
        req->AddHeader("Content-Type", "application/json");
        req->AddBody(JsonParser::ToRawString(body));
        QueueManager::getInstance().EventMark("info", "ExtractParamNode.BertBegin","success",
                                              "send request to bert model", access);
        auto resp = client->Send(req);
        if (resp->GetStatusCode() != CMFrm::COM::HTTP200) {
            LOG_INFO_EX("RequestId: %d, connect bert failed", resp->GetStatusCode());
            QueueManager::getInstance().EventMark("error", "ExtractParamNode.BertFailed","success",
                                  "send request to bert model failed", access);
            return mmagentflow::MMNodeFunctionStatus::FUNCTION_ERROR;
        }
        LOG_INFO_EX("RequestId: %d, Run: %s", resp->GetStatusCode(), resp->GetResponseBody().c_str());
        QueueManager::getInstance().EventMark("info", "ExtractParamNode.BertEnd","success",
                              "receive response from bert model", access);
        auto result = parseBertAnswer(resp->GetResponseBody());
        LOG_INFO_EX("RequestId: %d, Run: %s, BertResult: %s", resp->GetStatusCode(), resp->GetResponseBody().c_str(),
                    result[0].ToString().c_str()); // result[0]里面的是 bert result {"intent" : "xxx", "params" : {"":""}}
        auto intentMap = ConfigManager::getInstance().getIntents();
        if (intentMap.find(result[0].intent) != intentMap.end()) {
            Intent intent = intentMap.at(result[0].intent);
            LOG_INFO_EX("find intent : [%s]", intent.intent.c_str());
            if (!intent.noArgs) {
                for (const auto& param : intent.params) {
                    if (param.abstractTransformRule != nullptr) {
                        param.abstractTransformRule->doTransformParam(result[0]);
                        LOG_INFO_EX("开始转换bert的结果了 doTransformParam to [%s]", result[0].ToString().c_str());
                    }
                }
            }
        } else {
            LOG_INFO_EX("have no intent [%s] in intentMap", result[0].intent.c_str());
        }
        GraphUtils::AddMessageData(msg, GRAPH_DATA_ExtractParam, result[0]);
        GraphUtils::PrintDebugInfo(msg);
        QueueManager::getInstance().EventMark("info", "ExtractParamNode.Exit", "success",
                                              "exit ExtractParamNode Run", access);
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
        auto data = std::dynamic_pointer_cast<BaseData<std::string>>(event->GetData())->getValue();
        auto msg = event->GetMessage();
        LOG_INFO_EX("RequestId: %d, AsyncRun: %s", msg->GetRequestId(), name_.c_str());

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

#endif  // SYSAGENT_EXTRACTPARAMNODE_H