//
// Created by z00913804 on 25-7-31.
//

#ifndef SYSAGENT_RULE_H
#define SYSAGENT_RULE_H
#include "string"
#include "taskManage/WorkFlow.h"
enum class RuleName {
    RAG_RULE_HANDLER,
    TASK_RULE_HANDLER,
    SPECIFY_INTENT_RULE_HANDLER,
    CUSTOM_INTENT_RULE_HANDLER,
    MEMORIZE_RULE_HANDLER,
    RATELIMIT_RULE_HANDLER,
    NEW_FRAMEWORK_HANDLER
};
class Rule {
public:
    RuleName ruleName;
    std::string nextNode;
    // nextNodeParam 需要确定
    std::vector<IntentTask> intentTasks;
    std::vector<AgentData> agentDatas;
    Rule() = default;
};
#endif // SYSAGENT_RULE_H
