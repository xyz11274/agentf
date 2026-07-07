//
// Created by z00913804 on 25-7-28.
//

#ifndef SYSAGENT_RAGRULEHANDLER_H
#define SYSAGENT_RAGRULEHANDLER_H
#include "AbstractRuleHandler.h"
using namespace SYSAGENT;
class RAGRuleHandler : public AbstractRuleHandler {
public:
    shared_ptr<Rule> doChain(const std::shared_ptr<AccessCtrl>& accessCtrl) override;
    static std::string extractInput(const std::shared_ptr<AccessCtrl>& accCtr);
};
#endif // SYSAGENT_RAGRULEHANDLER_H
