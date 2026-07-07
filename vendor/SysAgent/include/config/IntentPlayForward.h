//
// Created by z00913804 on 25-8-16.
//

#ifndef SYSAGENT_INETENTPLAYFORWARD_H
#define SYSAGENT_INETENTPLAYFORWARD_H
#include "AbstractTransformRule.h"
#include <unordered_set>
#define SIXTY 60
class IntentPlayForward : public AbstractTransformRule {
public:
    void splitTimeString(const std::string& input, std::string& numberPart, std::string& textPart)
    {
        size_t i = 0;
        while (i < input.size() && std::isdigit(input[i])) {
            i++;
        }
        numberPart = input.substr(0, i);
        textPart = input.substr(i);
    }
    std::string doubleToIntString(double value)
    {
        int integerPart = static_cast<int>(value); // 只要整数部分
        return std::to_string(integerPart);
    }
    void replaceAll(std::string& str, const std::string& from, const std::string& to)
    {
        size_t start_pos = 0u;
        while ((start_pos = str.find(from, start_pos)) != std::string::npos) {
            str.replace(start_pos, from.length(), to);
            start_pos += to.length();  // 在替换后的位置继续查找
        }
    }
    std::string convertTime(std::string input)
    {
        replaceAll(input, "小时", "时");
        replaceAll(input, "分钟", "分");
        std::regex timePattern(R"((\d+时)?(\d+分)?(\d+秒)?)");
        std::smatch matches;
        if (std::regex_match(input, matches, timePattern)) {
            LOG_INFO_EX("匹配成功");
            int time = 0;
            if (matches[NUMBER_ONE].matched) {
                time += SIXTY * SIXTY * std::stoi(matches[NUMBER_ONE].str().
                        substr(0, matches[NUMBER_ONE].str().length() - std::string("时").length()));
            }
            if (matches[NUMBER_TWO].matched) {
                time += SIXTY * std::stoi(matches[NUMBER_TWO].str().
                        substr(0, matches[NUMBER_TWO].str().length() - std::string("分").length()));
            }
            if (matches[NUMBER_THREE].matched) {
                time += std::stoi(matches[NUMBER_THREE].str().
                        substr(0, matches[NUMBER_THREE].str().length() - std::string("秒").length()));
            }
            return doubleToIntString(time);
        } else {
            LOG_INFO_EX("匹配失败");
            return input;
        }
    }
    void doTransformParam(BertReault& bertResult) override
    {
        auto params = bertResult.params;
        std::unordered_set<std::string> keys = {"relativeTime", "absoluteTime", "time"};
        for (auto& key : keys) {
            if (params.find(key) != params.end()) {
                std::string time = params[key];
                // relativeTime 如果以分钟结尾，按rule里面的规则转换
                std::string numberPart;
                std::string textPart;
                splitTimeString(time, numberPart, textPart);
                if (textPart.empty()) {
                    LOG_INFO_EX("textPart is empty");
                    return;
                }
                auto rule = getRuleMap();
                if (rule.find("toUnit") == rule.end()) {
                    LOG_INFO_EX("have no key [toUnit] in the ruleMap");
                    return;
                }
                std::string toUnit = rule["toUnit"];
                LOG_INFO_EX("textPart : [%s], toUnit : [%s]", textPart.c_str(), toUnit.c_str());
                if (toUnit == "s") {
                    bertResult.params[key] = convertTime(time);
                }
                return;
            }
        }
        LOG_INFO_EX("params have no key in the keys");
    }
};
#endif // SYSAGENT_INETENTPLAYFORWARD_H
