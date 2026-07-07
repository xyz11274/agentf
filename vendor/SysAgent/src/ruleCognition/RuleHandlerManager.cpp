// Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.

#include "ruleCognition/RuleHandlerManager.h"
#include "ruleCognition/RAGRuleHandler.h"
#include "ruleCognition/TaskRuleHandler.h"
#include "ruleCognition/CustomIntentRuleHandler.h"
#include "ruleCognition/SpecifyIntentRuleHandler.h"
#include "ruleCognition/MemorizeRuleHandler.h"
#include "ruleCognition/SecurityFenceRuleHandler.h"
#include "ruleCognition/RateLimitRuleHandler.h"
#include "ruleCognition/NewFrameworkRuleHandler.h"

void RuleHandlerManager::init()
{
    // 新建安全围栏的handler，进行前围栏拦截
    std::shared_ptr<RateLimitRuleHandler> rateLimitRuleHandler = std::make_shared<RateLimitRuleHandler>();
    ruleHandlers.push_back(rateLimitRuleHandler);
    std::shared_ptr<SecurityFenceRuleHandler> securifyFenceRuleHandler = std::make_shared<SecurityFenceRuleHandler>();
    ruleHandlers.push_back(securifyFenceRuleHandler);
    std::shared_ptr<RAGRuleHandler> ragRuleHandler = std::make_shared<RAGRuleHandler>();
    ruleHandlers.push_back(ragRuleHandler);
    // 关闭 MemorizeRuleHandler（内部 SendAsync），新流程不使用
    // std::shared_ptr<MemorizeRuleHandler> memorizeRuleHandler = std::make_shared<MemorizeRuleHandler>();
    // ruleHandlers.push_back(memorizeRuleHandler);
    std::shared_ptr<TaskRuleHandler> taskRuleHandler = std::make_shared<TaskRuleHandler>();
    ruleHandlers.push_back(taskRuleHandler);
    std::shared_ptr<SpecifyIntentRuleHandler> specifyIntentRuleHandler = std::make_shared<SpecifyIntentRuleHandler>();
    ruleHandlers.push_back(specifyIntentRuleHandler);
    std::shared_ptr<CustomIntentRuleHandler> customIntentRuleHandler = std::make_shared<CustomIntentRuleHandler>();
    ruleHandlers.push_back(customIntentRuleHandler);
    std::shared_ptr<NewFrameworkRuleHandler> newFrameworkRuleHandler = std::make_shared<NewFrameworkRuleHandler>();
    ruleHandlers.push_back(newFrameworkRuleHandler);
    LOG_INFO_EX("RuleHandlerManager init successfully");
}

shared_ptr<Rule> RuleHandlerManager::doChain(shared_ptr<SYSAGENT::AccessCtrl>& accessCtrl)
{
    shared_ptr<Rule> rule = nullptr;
    for (const auto& ruleHandler : ruleHandlers) {
        rule = ruleHandler->doChain(accessCtrl);
        if (rule != nullptr) {
            LOG_INFO_EX("get rule %d", rule->ruleName);
            return rule;
        }
    }
    LOG_INFO_EX("no handler match, will go Unify Intent");
    rule = make_shared<Rule>();
    rule->ruleName = RuleName::CUSTOM_INTENT_RULE_HANDLER;
    rule->nextNode = "UnifyIntentStartNode";
    return rule;
}
