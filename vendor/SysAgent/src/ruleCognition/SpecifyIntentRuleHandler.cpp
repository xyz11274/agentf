//
// Created by z00913804 on 25-7-28.
//
#include "ruleCognition/SpecifyIntentRuleHandler.h"
#include "config/Config.h"
#include "graphManager/GraphData.h"
#include "taskManage/WorkFlow.h"
#include "registerManage/AgentManager.h"

using namespace SYSAGENT;
AgentData SpecifyIntentRuleHandler::userSpecifiedAgentRecognition(const std::string& userSpecifiedAgent)
{
    LOG_INFO_EX("Start to recognize userSpecifiedAgent: %s", userSpecifiedAgent.c_str());
    auto agents = AgentManager::getInstance().getAgentList();
    AgentData agentData;
    // 但是需要构建agent实例给后面的任务规划节点
    for (const auto& agent : agents) {
        if (std::find(agent.ids.begin(), agent.ids.end(), userSpecifiedAgent) != agent.ids.end()) {
            agentData.name = agent.name;
            agentData.type = agent.type;
            agentData.baseUrl = agent.baseUrl;
            agentData.port = agent.port;
            agentData.serviceName = agent.serviceName;
            return agentData;
        }
    }
    LOG_INFO_EX("Not match agent, return null.");
    return agentData;
}

shared_ptr<Rule> SpecifyIntentRuleHandler::doChain(const std::shared_ptr<AccessCtrl>& accessCtrl)
{
    LOG_INFO_EX("SpecifyIntentRuleHandler enter");
    if (accessCtrl == nullptr) {
        LOG_ERR_EX("[SpecifyIntentRuleHandler] accessCtrl is null");
        return nullptr;
    }
    std::string dUserSpecifiedAgent =
            accessCtrl->eventType == EventType::TASKASSIGNMENT ?
            accessCtrl->taskAssignment.data.userSpecifiedAgent : "";
    if (dUserSpecifiedAgent.empty()) {
        return nullptr;
    }
    auto agentData = userSpecifiedAgentRecognition(dUserSpecifiedAgent);
    if (agentData.name.empty()) {
        LOG_INFO_EX("[SpecifyIntentRuleHandler] Get agent: null, not operating");
        return nullptr;
    }
    LOG_INFO_EX("[SpecifyIntentRuleHandler] Get agent: %s", agentData.name.c_str());
    shared_ptr<Rule> rule = make_shared<Rule>();
    rule->ruleName = RuleName::SPECIFY_INTENT_RULE_HANDLER;
    rule->nextNode = GRAPH_NODE_PlanNode;
    rule->intentTasks.push_back(IntentTask(agentData.name, accessCtrl->taskAssignment.data.userInputText, ""));
    rule->agentDatas.push_back(agentData);
    return rule;
}
