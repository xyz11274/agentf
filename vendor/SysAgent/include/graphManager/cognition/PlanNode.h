// Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.

#ifndef SYSAGENT_PLANNODE_H
#define SYSAGENT_PLANNODE_H

#include "TEManager.h"
#include "TeHelper.h"
#include "aisf_platform/common_define.h"
#include "graphManager/GraphData.h"
#include "graphManager/GraphUtils.h"
#include "graphManager/base/BaseNode.h"
#include "mm_message.h"
#include "mm_node.h"
#include <memory>
#include "queue/QueueManager.h"
class PlanNode : public BaseNode {
public:
    PlanNode(const std::string& nodeName, const mmagentflow::MMNodeConfig& nodeConfig) : BaseNode(nodeName, nodeConfig)
    {
    }
    std::shared_ptr<mmagentflow::MMNode> Clone() { return std::make_shared<PlanNode>(name_, config_); }

    /**
     * @param msg
     * @return
     */
    mmagentflow::MMNodeFunctionStatus Run(std::shared_ptr<mmagentflow::MMMessage> msg)
    {
        LOG_INFO_EX("RequestId: %d, Run: %s", msg->GetRequestId(), name_.c_str());
        GraphUtils::AddDebugInfo(msg, name_);
        auto access = GraphUtils::GetMessageData<AccessCtrlPtr>(msg, GRAPH_DATA_ACCESSCTRL);
        QueueManager::getInstance().EventMark("info", "PlanNode.enter", "success",
                                              "enter PlanNode", access);
        auto tasks = GraphUtils::GetMessageData<PlanExample>(msg, GRAPH_DATA_PLANEXAMPLE);
        auto teamEnv = TEManager::getInstance().getTEById(access->header.teamEnvironmentID);
        if (!teamEnv) {
            LOG_ERR_EX("get TeamEnv failed: %s", access->header.teamEnvironmentID.c_str());
            return mmagentflow::MMNodeFunctionStatus::FUNCTION_ERROR;
        }

        auto decisionStat = GraphUtils::GetMessageData<std::shared_ptr<DecisionStat>>(msg, GRAPH_DATA_ISDECISION);
        std::string workflowID = decisionStat ? decisionStat->workflowID : "";
        auto workFlow = TEHelper::CreateWorkFlow(teamEnv, access->header.userName, workflowID);
        CHECK_TRUE_LOG_ERROR_RETURN(!workFlow, mmagentflow::MMNodeFunctionStatus::FUNCTION_COMPLETE,
                                    "workflow or decision is empty!");
        if (decisionStat) {
            workFlow->SetAgentTasks(decisionStat->agentTasks);
        }
        // 创建任务
        if (!workFlow->CreateWorkFlowTask(tasks.tasks, access, tasks.agentDatas)) {
            LOG_ERR_EX("CreateWorkFlowTask failed");
            // 创建失败，删除workFlow
            teamEnv->deleteWorkFlow();
            return mmagentflow::MMNodeFunctionStatus::FUNCTION_ERROR;
        }

        // 设置TaskID
        auto taskNode = workFlow->GetTopTaskNode();
        if (taskNode != nullptr) {
            access->setTaskID(taskNode->task->findTaskID());
        }
        GraphUtils::PrintDebugInfo(msg);
        QueueManager::getInstance().EventMark("info", "PlanNode.exit", "success", "exit PlanNode", access);
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

#endif  // SYSAGENT_PLANNODE_H