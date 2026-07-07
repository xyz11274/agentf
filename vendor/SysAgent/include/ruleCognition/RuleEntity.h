//
// Created by z00913804 on 25-7-30.
//

#ifndef SYSAGENT_RULEENTITY_H
#define SYSAGENT_RULEENTITY_H
#include "aisf_platform/parser/aisf_msg_parse.h"
#include "aisf_platform/parser/aisf_msg_ctrl.h"
#include <regex>
class Condition {
public:
    std::string conditionName;
    std::string conditionKey;
    std::string conditionValue;
    std::string conditionType;
public:
    bool match(const string& handlerProperty)
    { // 传入的是handle里面Header的某个字段的值
        if (conditionType == "1") { // Sender，相等
            return handlerProperty == conditionValue;
        }
        if (conditionType == "3") { // UserName，子字符串
            return handlerProperty.find(conditionValue)!= std::string::npos;
        }
        if (conditionType == "2") { //  正则
            std::regex pattern(conditionValue); // 条件多就需要优化，暂时不需要
            if (std::regex_search(handlerProperty, pattern)) { // search or match
                return true;
            }
        }
        return false;
    }
};

class AbstractRuleEntity {
public:
    std::string ruleName;
    std::string ruleType;
    std::string nextNode;
    std::string toAgent;
    vector<Condition> conditions;
    virtual bool match(const shared_ptr<SYSAGENT::AccessCtrl>& accessCtrl) = 0;
    virtual ~AbstractRuleEntity() = default;
    std::string getValueFromAccessCtrl(const std::shared_ptr<SYSAGENT::AccessCtrl>& accessCtrl,
                                       const std::string& conditionKey)
    {
        if (conditionKey == "Sender") {
            return accessCtrl->header.sender;
        }
        if (conditionKey == "UserName") {
            return accessCtrl->header.userName;
        }
        if (conditionKey == "UserInputText") { // 可以优化，目前只是任务创建
            if (accessCtrl->eventType == SYSAGENT::EventType::TASKASSIGNMENT) {
                return accessCtrl->taskAssignment.data.userInputText;
            }
        }
        if (conditionKey == "DeviceCategory") {
            return accessCtrl->header.deviceCategory;
        }
        if (conditionKey == "UserIntent") {
            if (accessCtrl->eventType == SYSAGENT::EventType::TASKASSIGNMENT) {
                return accessCtrl->taskAssignment.data.userIntent;
            }
        }
        return "";
    }
};
#endif // SYSAGENT_RULEENTITY_H
