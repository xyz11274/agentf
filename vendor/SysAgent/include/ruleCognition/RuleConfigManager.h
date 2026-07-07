//
// Created by z00913804 on 25-7-30.
//

#ifndef SYSAGENT_RULECONFIGMANAGER_H
#define SYSAGENT_RULECONFIGMANAGER_H
#include "RuleEntity.h"
#include "Rule.h"
#include "config/Config.h"
#include "UserInputToAgentRuleEntity.h"
#include "CommonDefinition.h"
#include "registerManage/AgentManager.h"
class RuleConfigManager {
public:
    static RuleConfigManager& getInstance()
    {
        static RuleConfigManager instance = RuleConfigManager();
        return instance;
    }
    AgentData getUserInputToAgentData(const std::string& userInputToAgent)
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
    shared_ptr<Rule> match(const shared_ptr<SYSAGENT::AccessCtrl>& accessCtrl)
    {
        vector<shared_ptr<AbstractRuleEntity>> ruleList = ConfigManager::getInstance().getRuleList();
        shared_ptr<MsgAisfHeader> header = make_shared<MsgAisfHeader>(accessCtrl->header);
        for (const auto& ruleItem : ruleList) {
            if (ruleItem->match(accessCtrl)) { // 这个规则匹配上了
                shared_ptr<Rule> rule = make_shared<Rule>();
                rule->ruleName = RuleName::CUSTOM_INTENT_RULE_HANDLER;
                rule->nextNode = ruleItem->nextNode;
                LOG_INFO_EX("rule Name is:%s", ruleItem->ruleName.c_str());
                if (ruleItem->ruleType == USER_INPUT_TEXT_RULE_TYPE) {
                    std::string userInputText = accessCtrl->taskAssignment.data.userInputText;
                    rule->intentTasks.push_back(IntentTask(ruleItem->toAgent, userInputText, ""));
                    rule->agentDatas.push_back(getUserInputToAgentData(ruleItem->toAgent));
                }
                return rule;
            }
        }
        return nullptr;
    }
};
#endif // SYSAGENT_RULECONFIGMANAGER_H
