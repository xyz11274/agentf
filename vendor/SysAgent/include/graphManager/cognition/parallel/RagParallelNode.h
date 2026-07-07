// Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.

#ifndef SYSAGENT_PARALLEL_RAGNODE_H
#define SYSAGENT_PARALLEL_RAGNODE_H

#include "aisf_platform/common_define.h"
#include "graphManager/GraphUtils.h"
#include "graphManager/base/BaseNode.h"
#include "graphManager/GraphData.h"
#include "mm_node.h"

class RagParallelNode : public BaseNode {
public:
    RagParallelNode(const std::string& nodeName, const mmagentflow::MMNodeConfig& nodeConfig)
        : BaseNode(nodeName, nodeConfig)
    {
    }
    std::shared_ptr<mmagentflow::MMNode> Clone()
    {
        return std::make_shared<RagParallelNode>(name_, config_);
    }

    /**
     * 并行节点无栅栏，msg在不同节点互不影响
     * @param msg
     * @return
     */
    mmagentflow::MMNodeFunctionStatus Run(std::shared_ptr<mmagentflow::MMMessage> msg);
    /**
     *
     * @param requestId 处理请求id
     * @param event 节点请求id的异步事件
     * @return
     */
    mmagentflow::MMNodeFunctionStatus AsyncRun(std::shared_ptr<mmagentflow::MMEvent> event);

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

#endif // SYSAGENT_PARALLEL_RAGNODE_H