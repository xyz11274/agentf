// Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.

#ifndef SYSAGENT_PARALLEL_NODE_H
#define SYSAGENT_PARALLEL_NODE_H

#include "aisf_platform/common_define.h"

#include "graphManager/GraphData.h"
#include "graphManager/GraphUtils.h"
#include "graphManager/base/BaseNode.h"
#include "mm_node.h"
#include <memory>

class ParallelStartNode : public BaseNode {
public:
    ParallelStartNode(const std::string& nodeName, const mmagentflow::MMNodeConfig& nodeConfig)
        : BaseNode(nodeName, nodeConfig)
    {
    }
    std::shared_ptr<mmagentflow::MMNode> Clone() { return std::make_shared<ParallelStartNode>(name_, config_); }

    /**
     * @param msg
     * @return
     */
    mmagentflow::MMNodeFunctionStatus Run(std::shared_ptr<mmagentflow::MMMessage> msg)
    {
        LOG_INFO_EX("RequestId: %d, AsyncRun: %s", msg->GetRequestId(), name_.c_str());

        GraphUtils::AddMessageData(msg, GRAPH_DATA_ISDECISION, std::make_shared<DecisionStat>(false));

        auto workflowID = WorkFlow::GenerateRandomWorkFlowID();
        GraphUtils::AddMessageData(msg, GRAPH_DATA_ISDECISION, std::make_shared<DecisionStat>(false, workflowID));

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

#endif  // SYSAGENT_PARALLELSTARTNODE_H