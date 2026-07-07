//
// Created by l00909985 on 2025/12/21.
//

#ifndef SECURITYFENCERULEHANDLER_H
#define SECURITYFENCERULEHANDLER_H
#include "AbstractRuleHandler.h"
using namespace SYSAGENT;
class SecurityFenceRuleHandler : public AbstractRuleHandler {
public:
    shared_ptr<Rule> doChain(const std::shared_ptr<AccessCtrl>& accessCtrl) override;
};
#endif //SECURITYFENCERULEHANDLER_H
