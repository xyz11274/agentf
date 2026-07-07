// Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.

#ifndef SYSAGENT_ENDNODE_H
#define SYSAGENT_ENDNODE_H

#include "aisf_platform/common_define.h"
#include "graphManager/base/BaseNode.h"
#include "mm_node.h"

class EndNode : public BaseNode {
public:
    EndNode(const std::string& nodeName, const mmagentflow::MMNodeConfig& nodeConfig) : BaseNode(nodeName, nodeConfig)
    {
    }
    std::shared_ptr<mmagentflow::MMNode> Clone()
    {
        return std::make_shared<EndNode>(name_, config_);
    }

    /**
     * @param msg
     * @return
     */
    mmagentflow::MMNodeFunctionStatus Run(std::shared_ptr<mmagentflow::MMMessage> msg)
    {
        LOG_INFO_EX("RequestId: %d, Run: %s", msg->GetRequestId(), name_.c_str());

        msg->GetData()["EndNode"] = std::make_shared<BaseData<std::string>>("sucess");

        for (const auto& [key, value] : msg->GetData()) {
            LOG_INFO_EX("key: %s, value: %d", key.c_str(), value.get());
        }

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

#endif // SYSAGENT_ENDNODE_H