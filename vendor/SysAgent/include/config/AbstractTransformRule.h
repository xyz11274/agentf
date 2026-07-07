//
// Created by z00913804 on 25-8-16.
//

#ifndef SYSAGENT_ABSTRACTTRANSFORMRULE_H
#define SYSAGENT_ABSTRACTTRANSFORMRULE_H
#include <utility>
#include <iostream>
#include <map>
#include "model/ModelUtils.h"
class AbstractTransformRule {
public:
    std::string getName()
    {
        return name;
    }
    const std::map<std::string, std::string>& getRuleMap()
    {
        return ruleMap;
    }
    void insertToRuleMap(const std::string& key, const std::string& value)
    {
        ruleMap[key] = value;
    }
    void setName(const string& ruleName)
    {
        this->name = ruleName;
    }
    virtual void doTransformParam(BertReault&) = 0;
    virtual ~AbstractTransformRule() = default;
private:
    std::string name;
    std::map<std::string, std::string> ruleMap;
};
#endif // SYSAGENT_ABSTRACTTRANSFORMRULE_H
