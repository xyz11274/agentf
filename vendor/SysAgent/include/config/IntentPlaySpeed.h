//
// Created by z00913804 on 25-8-16.
//

#ifndef SYSAGENT_INTENTPLAYSPEED_H
#define SYSAGENT_INTENTPLAYSPEED_H
#include "AbstractTransformRule.h"
class IntentPlaySpeed : public AbstractTransformRule {
public:
    void doTransformParam(BertReault &bertResult) override
    {
        if (bertResult.params.count("speed") == 0) {
            LOG_INFO_EX("have no speed in bertResult.params");
            return;
        }
        auto speed = bertResult.params["speed"];
        auto rule = getRuleMap();
        if (rule.count(speed) == 0) {
            LOG_INFO_EX("规则未定义key : [%s]", speed.c_str());
            return;
        }
        bertResult.params["speed"] = rule[speed];
    }
};
#endif // SYSAGENT_INTENTPLAYSPEED_H
