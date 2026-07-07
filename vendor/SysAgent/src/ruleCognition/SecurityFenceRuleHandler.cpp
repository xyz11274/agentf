//
// Created by l00909985 on 2025/12/20.
//
#include "framework_common/utils/env_util.h"

#include "ruleCognition/SecurityFenceRuleHandler.h"
#include "TEManager.h"
#include "TeHelper.h"
#include "graphManager/GraphData.h"
#include "memory/SafetyFence.h"
#include "queue/QueueManager.h"
#include "ruleCognition/Rule.h"
#include "taskManage/TeHelper.h"
#include "ruleCognition/CommonDefinition.h"
#include "securityFence/SecFenceMessage.h"
#include "securityFence/SecurityFence.h"

shared_ptr<Rule> SecurityFenceRuleHandler::doChain(const std::shared_ptr<AccessCtrl>& accessCtrl)
{
    LOG_INFO_EX("SecurityFenceRuleHandler enter");
    if (accessCtrl == nullptr) {
        LOG_INFO_EX("[SecurityFenceRuleHandler] accessCtrl nullptr");
        return nullptr;  // ragHandler通过
    }

    string request = accessCtrl->extractInput();
    if (request.empty()) {
        LOG_INFO_EX("input is null");
        return nullptr;
    }
    auto frontFenceResult = getFrontFenceDetection(accessCtrl);
    if (frontFenceResult != FENCE_RISK_BLOCK)
    {
        return nullptr;
    }

    shared_ptr<Rule> rule;
    if (accessCtrl->eventType == EventType::TASKASSIGNMENT)
    {
        if (frontFenceResult == FENCE_RISK_BLOCK)
        {
            string rejectMsg = getRejectMsgZh();
            LOG_INFO_EX("[MMGuardService]Front security fence block, reject content: %s", rejectMsg.c_str());
            TEHelper::sendUserOutputMsg(accessCtrl, rejectMsg, UserOutput_Completed, 0);
            TEHelper::sendNewTaskStatusReport(accessCtrl);
            rule = make_shared<Rule>();
            rule->ruleName = RuleName::RAG_RULE_HANDLER;
            rule->nextNode = GRAPH_NODE_EndNode;
            return rule;
        }
    }
    else if (accessCtrl->eventType == EventType::USERINPUT)
    {
        if (frontFenceResult == FENCE_RISK_BLOCK)
        {

            string rejectMsg = getRejectMsgZh();
            LOG_INFO_EX("[MMGuardService]Front security fence block, reject content: %s", rejectMsg.c_str());
            TEHelper::sendUserOutputMsg(accessCtrl, rejectMsg, UserOutput_Completed, 0);
            TEHelper::sendNewTaskStatusReport(accessCtrl);
            rule = make_shared<Rule>();
            rule->ruleName = RuleName::RAG_RULE_HANDLER;
            rule->nextNode = GRAPH_NODE_EndNode;
            return rule;
        }
    }
    return rule;  // ragHandler通过，//要返回共享指针！
}