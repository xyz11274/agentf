//
// Created by z00913804 on 25-7-31.
//
#include "ruleCognition/CustomIntentRuleHandler.h"
#include "config/Config.h"
#include "ruleCognition/RuleConfigManager.h"
shared_ptr<Rule> CustomIntentRuleHandler::doChain(const std::shared_ptr<SYSAGENT::AccessCtrl>& accessCtrl)
{
    LOG_INFO_EX("CustomIntentRuleHandler enter");
    if (accessCtrl == nullptr) {
        LOG_ERR_EX("accessCtrl is nullptr");
        return nullptr;
    }
    return RuleConfigManager::getInstance().match(accessCtrl);
}
