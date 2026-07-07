//
// Created by z00913804 on 25-9-2.
//

#ifndef SYSAGENT_USERINPUTTOAGENTRULEENTITY_H
#define SYSAGENT_USERINPUTTOAGENTRULEENTITY_H
#include "RuleEntity.h"
#include <regex>
class UserInputToAgentRuleEntity : public AbstractRuleEntity {
public:
    bool match(const std::shared_ptr<SYSAGENT::AccessCtrl>& accessCtrl) override
    {
        bool result = false;
        for (auto condition: conditions) { // 当前是一个规则遍历所有规则，有一个满足即可
            std::string conditionValue = condition.conditionValue;
            std::string accValue = getValueFromAccessCtrl(accessCtrl, condition.conditionKey);
            LOG_INFO_EX("conditionKey : %s, conditionValue : %s, accValue : %s",
                        condition.conditionKey.c_str(), conditionValue.c_str(), accValue.c_str());
            if (condition.match(accValue)) {
                LOG_INFO_EX("UserInputToAgentRuleEntity match true : %s", accValue.c_str());
                result = true;
                return result;
            }
        }
        return result; // 都不满足，当前规则匹配失败
    }
};
#endif // SYSAGENT_USERINPUTTOAGENTRULEENTITY_H
