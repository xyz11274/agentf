// Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.

#include "ruleCognition/RateLimitRuleHandler.h"

#include "framework_common/utils/env_util.h"
#include "framework_common/utils/basetype_util.h"

#include "TEManager.h"
#include "TeHelper.h"
#include "graphManager/GraphData.h"
#include "ruleCognition/Rule.h"
#include "ruleCognition/CommonDefinition.h"
#include "config/Config.h"

#define DEFAULT_RATELIMIT_NUM "1000"
#define RATELIMIT_REJECT_CONTENT "系统繁忙，请稍后再试"

shared_ptr<Rule> RateLimitRuleHandler::doChain(const std::shared_ptr<AccessCtrl>& accessCtrl)
{
    LOG_INFO_EX("RateLimitRuleHandler enter");
    if (accessCtrl == nullptr) {
        LOG_INFO_EX("[RateLimitRuleHandler] accessCtrl nullptr");
        return nullptr;  // RateLimitRuleHandler通过
    }
    string taskID = accessCtrl->header.taskID;
    if (!taskID.empty()) {
        LOG_INFO_EX("taskID %s exist, do not need ratelimit", taskID.c_str());
        return nullptr;
    }
    std::string rateLimitEnv = ConfigManager::getInstance().getEnvInfo(ENV_SYS_TASK_RATELIMIT_NUM, DEFAULT_RATELIMIT_NUM);
    auto rateLimitNum = CMFrm::Utils::ParseInt(rateLimitEnv);
    if (rateLimitNum <= 0) {
        LOG_INFO_EX("rateLimitNum %d do not need ratelimit, pass!", rateLimitNum);
        return nullptr;
    }

    // 新任务到来之后，考虑进行限流
    auto allTeInfo = TEManager::getInstance().getAllTEs();
    auto allWorkFlowNums = std::count_if(allTeInfo.begin(), allTeInfo.end(),
        [](std::shared_ptr<TeamEnvironment> te) { return te->hasWorkFlow(); });
    if (allWorkFlowNums <= rateLimitNum) {
        LOG_INFO_EX("rateLimitNum %d, exist workflow num %d ,do not need ratelimit, pass!",
            rateLimitNum, allWorkFlowNums);
        return nullptr;
    }

    LOG_WARN("task input %s need ratelimit, limit %d" , accessCtrl->extractInput().c_str(), rateLimitNum);
    auto rule = make_shared<Rule>();
    rule->ruleName = RuleName::RATELIMIT_RULE_HANDLER;  // 常量表，定义是这些handler
    rule->nextNode = GRAPH_NODE_EndNode;
    TEHelper::sendUserOutputMsg(accessCtrl, RATELIMIT_REJECT_CONTENT, UserOutput_Completed, 0);
    TEHelper::sendNewTaskStatusReport(accessCtrl);

    return rule;  // ragHandler通过，//要返回共享指针！
}

