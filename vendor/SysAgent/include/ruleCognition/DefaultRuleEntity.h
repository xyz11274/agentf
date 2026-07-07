//
// Created by z00913804 on 25-7-30.
//

#ifndef SYSAGENT_DEFAULTRULEENTITY_H
#define SYSAGENT_DEFAULTRULEENTITY_H
#include "RuleEntity.h"
class DefaultRuleEntity : public AbstractRuleEntity {
public:
    bool match(const shared_ptr<SYSAGENT::AccessCtrl>& accessCtrl) override
    {
        bool result = true;
        for (auto condition: conditions) { // 当前是一个规则遍历所有规则，有一个满足即可
            string conditionKey = condition.conditionKey;
            string conditionValue = condition.conditionValue;
            std::string accValue = getValueFromAccessCtrl(accessCtrl, conditionKey);
            if (!condition.match(accValue)) { // 校验header中的指定属性是否满足
                result = false;
                break;
            }
        }
        return result; // 都不满足，当前规则匹配失败
    }
};
#endif // SYSAGENT_DEFAULTRULEENTITY_H
