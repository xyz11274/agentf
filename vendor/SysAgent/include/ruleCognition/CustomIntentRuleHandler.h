//
// Created by z00913804 on 25-7-31.
//

#ifndef SYSAGENT_CUSTOMINTENTRULEHANDLER_H
#define SYSAGENT_CUSTOMINTENTRULEHANDLER_H
#include "AbstractRuleHandler.h"

class CustomIntentRuleHandler : public AbstractRuleHandler {
public:
    shared_ptr<Rule> doChain(const std::shared_ptr<AccessCtrl>& accessCtrl) override;
};
#endif // SYSAGENT_CUSTOMINTENTRULEHANDLER_H
