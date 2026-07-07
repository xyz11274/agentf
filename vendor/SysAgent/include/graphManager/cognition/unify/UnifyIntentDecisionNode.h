// Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.

#ifndef SYSAGENT_UNIFYINTENTDECISIONNODE_H
#define SYSAGENT_UNIFYINTENTDECISIONNODE_H

#include "graphManager/base/BaseNode.h"
#include "taskManage/WorkFlow.h"
#include "mm_node.h"
#include "graphManager/GraphData.h"
#include <memory>

class UnifyIntentDecisionNode : public BaseNode {
public:
    UnifyIntentDecisionNode(const std::string& nodeName, const mmagentflow::MMNodeConfig& nodeConfig);

    /**
     * 复制
     * @return
     */
    std::shared_ptr<mmagentflow::MMNode> Clone();

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
    mmagentflow::MMNodeFunctionStatus AsyncRun(std::shared_ptr<mmagentflow::MMEvent> event);

    /**
     *
     * @param msg
     * @return
     */
    mmagentflow::MMNodeFunctionStatus CancelAsyncRun(std::shared_ptr<mmagentflow::MMMessage> msg);

    /**
     * 打断（cancel）场景只有在等待RunAsync存在，不存在在Run方法场景。
     * 增加异常
     * @param globalDatas
     * @return
     */
    std::shared_ptr<mmagentflow::MMMessage> DataMsgMerge(
        const std::map<std::string, std::shared_ptr<mmagentflow::MMMessage>>& inputmsgs);
private:
    bool HandleQATask(std::shared_ptr<mmagentflow::MMMessage> msg);
};

#endif // SYSAGENT_UNIFYINTENTDECISIONNODE_H
