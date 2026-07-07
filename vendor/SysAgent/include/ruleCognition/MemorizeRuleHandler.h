//
// Created by c00888021 on 2025/8/25.
//

#ifndef SYSAGENT_MEMORIZERULEHANDLER_H
#define SYSAGENT_MEMORIZERULEHANDLER_H

#include "AbstractRuleHandler.h"
#include "TeHelper.h"

#define HELP_ME_REMEMBER "帮我记住"
#define HELP_ME_FORGET "帮我忘记"
#define HELP_ME_QUERY "帮我查询记住的记忆"
#define MEMORIZE_UPDATE "已帮您更新记忆"
#define CONTEXT_CLEAR "帮我清空上下文"
#define CONTEXT_CLEAR_FINISHED "已帮您清空上下文"

class MemorizeRuleHandler : public AbstractRuleHandler {
public:
    shared_ptr<Rule> doChain(const std::shared_ptr<AccessCtrl>& accessCtrl) override;
    void queryMemory(const std::shared_ptr<AccessCtrl>& accessCtrl);
    void sendTaskStatusReport(const std::shared_ptr<AccessCtrl>& accessCtrl, shared_ptr<TeamEnvironment> te);
    void AddContextWithoutTask(const std::shared_ptr<AccessCtrl>& accessCtrl, std::string query,
                               std::string answer, std::string userId);
};

#endif // SYSAGENT_MEMORIZERULEHANDLER_H