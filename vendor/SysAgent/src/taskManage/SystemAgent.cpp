//
// Created by l00889076 on 2026/3/30.
//

#include "taskManage/SystemAgent.h"
#include "sys_agent/AgentFlowManager.h"
#include "queue/QueueManager.h"
#include "utils/Constants.h"
#include "taskManage/TeHelper.h"

using namespace SysAgent;
bool SystemAgent::sendRestfulRequest(const shared_ptr<SYSAGENT::AccessCtrl>& accessCtrl) const
{
    QueueManager::getInstance().EventMark("info", "Agent.SendRestfulRequestBegin", "success",
        "start to send accessCtrl to agent : " + agentName_, accessCtrl);
    std::string messageBody = accessCtrl->toString();
    LOG_INFO_EX("start, agent agent name: %s", agentName_.c_str());
    LOG_INFO_EX("messageBody send to agent: %s", messageBody.c_str());

    if (agentName_ != SysAgent::Constant::AgentName::SYS_AGENT) {
        LOG_ERR("agentName %s is not system agent", agentName_.c_str());
        return false;
    }

    std::string dialogueId;
    std::string sessionId;
    if (accessCtrl->eventType == SYSAGENT::EventType::TASKSTATUSMANAGEMENT) {
        auto workflow = TEHelper::GetWorkFlow(accessCtrl);
        if (workflow != nullptr) {
            dialogueId = workflow->GetAgentFlowDialogueId();
            sessionId = workflow->GetAgentFlowSessionId();
            LOG_INFO_EX("get dialogueId %s and sessionId: %s", dialogueId.c_str(), sessionId.c_str());
        } else {
            LOG_ERR("workflow is nullptr");
        }
    }
    return AgentFlowManager::GetAgentFlowManager()->SendMsg(accessCtrl, dialogueId, sessionId);
}
