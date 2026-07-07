/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 */

#ifndef SYSAGENT_REWRITEQUERYNODE_H
#define SYSAGENT_REWRITEQUERYNODE_H

#include "graphManager/base/BaseNode.h"

class ReWriteQueryNode : public BaseNode {
public:
    ReWriteQueryNode(const std::string& nodeName, const mmagentflow::MMNodeConfig& nodeConfig);

    std::shared_ptr<mmagentflow::MMNode> Clone();

    /**
     * @param msg
     * @return 节点入口
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
};

#endif // SYSAGENT_REWRITEQUERYNODE_H
