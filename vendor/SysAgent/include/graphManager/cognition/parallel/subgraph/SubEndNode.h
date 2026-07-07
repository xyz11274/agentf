// Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.

#ifndef SYSAGENT_SUBENDNODE_H
#define SYSAGENT_SUBENDNODE_H

#include "aisf_platform/common_define.h"
#include "graphManager/GraphData.h"
#include "graphManager/GraphUtils.h"
#include "graphManager/base/BaseNode.h"
#include "mm_message_factory.h"
#include "mm_node.h"
#include "model/ModelUtils.h"

class SubEndNode : public BaseNode {
public:
    SubEndNode(const std::string& nodeName, const mmagentflow::MMNodeConfig& nodeConfig)
        : BaseNode(nodeName, nodeConfig)
    {
    }
    std::shared_ptr<mmagentflow::MMNode> Clone() { return std::make_shared<SubEndNode>(name_, config_); }

    /**
     * @param msg
     * @return
     */
    mmagentflow::MMNodeFunctionStatus Run(std::shared_ptr<mmagentflow::MMMessage> msg)
    {
        LOG_INFO_EX("RequestId: %d, Run: %s", msg->GetRequestId(), name_.c_str());
        GraphUtils::AddDebugInfo(msg, name_);
        auto parentMsg = GraphUtils::GetMessageData<MMMessagePtr>(msg, GRAPH_DATA_PARENTGRAPHMSG);

        // 构造小模型输出
        auto adjudication = GraphUtils::GetMessageData<Adjudication>(msg, GRAPH_DATA_Adjudication);

        if (adjudication.match) {
            auto classifying = GraphUtils::GetMessageData<std::string>(msg, GRAPH_DATA_Classifying);
            auto extraParams = GraphUtils::GetMessageData<BertReault>(msg, GRAPH_DATA_ExtractParam);
            GraphUtils::NotifyAsyncEvent(
                parentMsg, SModelResult{adjudication.match, adjudication.paramsMatch,
                                        classifying, extraParams.intent, extraParams.params});
            return mmagentflow::MMNodeFunctionStatus::FUNCTION_COMPLETE;
        }

        // 通知父图执行结束
        GraphUtils::NotifyAsyncEvent(parentMsg, SModelResult{.match = adjudication.match});
        GraphUtils::PrintDebugInfo(msg);
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

#endif  // SYSAGENT_SUBENDNODE_H