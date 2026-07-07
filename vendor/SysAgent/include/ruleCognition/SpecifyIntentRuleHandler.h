//
// Created by z00913804 on 25-7-28.
//

#ifndef SYSAGENT_SPECIFYHANDLERRULEHANDLER_H
#define SYSAGENT_SPECIFYHANDLERRULEHANDLER_H
#include "AbstractRuleHandler.h"
class SpecifyIntentRuleHandler : public AbstractRuleHandler {
public:
    shared_ptr<Rule> doChain(const std::shared_ptr<AccessCtrl>& accessCtrl) override;
    AgentData userSpecifiedAgentRecognition(const std::string& userSpecifiedAgent);
};
#endif // SYSAGENT_SPECIFYHANDLERRULEHANDLER_H
