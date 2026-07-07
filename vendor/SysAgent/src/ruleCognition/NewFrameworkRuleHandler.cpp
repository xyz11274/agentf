//
// Created by l00889076 on 2026/3/30.
//

#include "ruleCognition/NewFrameworkRuleHandler.h"
#include "utils/Constants.h"
#include "graphManager/GraphData.h"
#include "config/Config.h"

using namespace SysAgent;

const char ENABLE_NEW_FRAMEWORK_KEY[] = "ENABLE_NEW_FRAMEWORK";

namespace {
    AgentData GetUserInputToAgentData(const std::string& userInputToAgent)
    {
        LOG_INFO_EX("Start to recognize UserInputToAgent");
        auto agents = AgentManager::getInstance().getAgentList();
        AgentData agentData;
        // 但是需要构建agent实例给后面的任务规划节点
        for (const auto& agent : agents) {
            if (agent.name == userInputToAgent) {
                agentData.name = agent.name;
                agentData.serviceName = agent.serviceName;
                agentData.type = agent.type;
                agentData.baseUrl = agent.baseUrl;
                agentData.port = agent.port;
                return agentData;
            }
        }
        return agentData;
    }
}


shared_ptr<Rule> NewFrameworkRuleHandler::doChain(const std::shared_ptr<AccessCtrl>& accessCtrl)
{
    if (ConfigManager::getInstance().getEnvInfo(ENABLE_NEW_FRAMEWORK_KEY, "false") != "true") {
        LOG_INFO("Not use new framework");
        return nullptr;
    }
    LOG_WARN("Use New Framework");
    IntentTask intentTask;
    intentTask.agentName = Constant::AgentName::SYS_AGENT;
    intentTask.requestContent = accessCtrl->extractInput();
    shared_ptr<Rule> rule = std::make_shared<Rule>();
    std::vector<IntentTask> intentTasks = {intentTask};
    std::vector<AgentData> agentDatas = {GetUserInputToAgentData(Constant::AgentName::SYS_AGENT)};
    rule->intentTasks = intentTasks;
    rule->nextNode = GRAPH_NODE_PlanNode;
    rule->ruleName = RuleName::NEW_FRAMEWORK_HANDLER;
    rule->agentDatas = agentDatas;
    return rule;
}
