// Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.

#ifndef SYSAGENT_PARALLEL_DECISIONNODE_H
#define SYSAGENT_PARALLEL_DECISIONNODE_H

#include "graphManager/base/BaseNode.h"
#include "taskManage/WorkFlow.h"
#include "mm_node.h"
#include <memory>

class DecisionNode : public BaseNode {
public:
    DecisionNode(const std::string& nodeName, const mmagentflow::MMNodeConfig& nodeConfig)
        : BaseNode(nodeName, nodeConfig)
    {
    }
    std::shared_ptr<mmagentflow::MMNode> Clone() { return std::make_shared<DecisionNode>(name_, config_); }

    /**
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

    mmagentflow::MMNodeFunctionStatus HandleRAGResult(const ParallelReault& result,
                                                      std::shared_ptr<mmagentflow::MMMessage> msg,
                                                      const std::string& request);

    mmagentflow::MMNodeFunctionStatus HandleSModelResult(const ParallelReault& result,
                                                         std::shared_ptr<mmagentflow::MMMessage> msg,
                                                         const std::string& request);

    mmagentflow::MMNodeFunctionStatus HandleLModelResult(const ParallelReault& result,
                                                         std::shared_ptr<mmagentflow::MMMessage> msg,
                                                         std::shared_ptr<DecisionStat> data);

    mmagentflow::MMNodeFunctionStatus HandleQAToolExtractResult(const ParallelReault& result,
                                                                std::shared_ptr<mmagentflow::MMMessage> msg,
                                                                std::shared_ptr<DecisionStat> data);


private:
    std::string getAgentName(const std::string& categoryId);
    void updateTask(IntentTask& task);
};

#endif  // SYSAGENT_PARALLEL_DECISIONNODE_H