//
// Created by z00913804 on 25-7-28.
//

#ifndef SYSAGENT_TASKRULEHANDLER_H
#define SYSAGENT_TASKRULEHANDLER_H
#include "AbstractRuleHandler.h"
class TaskRuleHandler : public AbstractRuleHandler {
public:
    shared_ptr<Rule> doChain(const std::shared_ptr<AccessCtrl>& accessCtrl) override;
};
#endif // SYSAGENT_TASKRULEHANDLER_H
