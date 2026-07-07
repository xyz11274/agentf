// Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.

#ifndef SYSAGENT_UNIFYFASTINTENTNODE_H
#define SYSAGENT_UNIFYFASTINTENTNODE_H

#include "WorkFlow.h"
#include "aisf_platform/common_define.h"
#include "config/Config.h"
#include "queue/QueueManager.h"
#include "graphManager/GraphUtils.h"
#include "graphManager/base/BaseNode.h"
#include "graphManager/GraphData.h"
#include "mm_node.h"
#include "mm_error_code.h"
#include "model/ModelUtils.h"
#include "model/PromptManager.h"
#include "model/ReactLLMTask.h"
#include "utils/env_util.h"

class UnifyFastIntentNode : public BaseNode {
public:
    UnifyFastIntentNode(const std::string& nodeName, const mmagentflow::MMNodeConfig& nodeConfig);

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

    bool LLMRequestForIntentAnalysis(std::shared_ptr<mmagentflow::MMMessage> msg, const std::string& request);

    void ParseLLMResponse(const std::string& llmOutput, shared_ptr<DecisionStat> decision);
};

#endif // SYSAGENT_UNIFYFASTINTENTNODE_H
