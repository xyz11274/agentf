//
// Created by l00889076 on 2026/3/30.
//

#ifndef SYSAGENT_NEWFRAMEWORKRULEHANDLER_H
#define SYSAGENT_NEWFRAMEWORKRULEHANDLER_H

#include "AbstractRuleHandler.h"
using namespace SYSAGENT;
class NewFrameworkRuleHandler : public AbstractRuleHandler {
public:
    shared_ptr<Rule> doChain(const std::shared_ptr<AccessCtrl>& accessCtrl) override;
};

#endif //SYSAGENT_NEWFRAMEWORKRULEHANDLER_H
