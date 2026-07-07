//
// Created by z00913804 on 25-7-28.
//

#ifndef SYSAGENT_ABSTRACTRULEHANDLER_H
#define SYSAGENT_ABSTRACTRULEHANDLER_H
#include "aisf_platform/parser/aisf_msg_ctrl.h"
#include "Rule.h"
using namespace SYSAGENT;
class AbstractRuleHandler {
public:
    virtual shared_ptr<Rule> doChain(const std::shared_ptr<AccessCtrl>& accessCtrl) = 0;
    virtual ~AbstractRuleHandler() = default;
};
#endif // SYSAGENT_ABSTRACTRULEHANDLER_H
