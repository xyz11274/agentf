//
// Created by z00913804 on 25-7-28.
//

#ifndef SYSAGENT_RULEHANDLERMANAGER_H
#define SYSAGENT_RULEHANDLERMANAGER_H
#include <vector>
#include "ruleCognition/AbstractRuleHandler.h"
class RuleHandlerManager {
public:
    static RuleHandlerManager& getInstance()
    {
        static RuleHandlerManager ruleHandlerManager;
        return ruleHandlerManager;
    }
    std::vector<std::shared_ptr<AbstractRuleHandler>> ruleHandlers; // 底层就是数组
    void init();
    shared_ptr<Rule> doChain(shared_ptr<SYSAGENT::AccessCtrl>& accessCtrl);
};
#endif // SYSAGENT_RULEHANDLERMANAGER_H
