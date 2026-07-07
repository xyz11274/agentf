//
// Created by z00913804 on 25-8-16.
//

#ifndef SYSAGENT_INTENTCONTENTCOMMENT_H
#define SYSAGENT_INTENTCONTENTCOMMENT_H
#include "AbstractTransformRule.h"
#include <regex>
class IntentContentComment : public AbstractTransformRule {
public:
    // 从字符串中提取数字部分（整数或小数）
    std::string extractNumber(const std::string &input)
    {
        // 正则表达式：匹配正整数或小数
        std::regex numberRegex(R"((\d+(?:\.\d+)?))");
        std::smatch match;
        if (std::regex_search(input, match, numberRegex)) {
            return match[1].str(); // 返回第一个匹配到的数字
        }
        return ""; // 如果没有匹配到，返回空字符串
    }

    void doTransformParam(BertReault &bertResult) override
    {
        auto params = bertResult.params;
        std::string star = params["star"];
        std::string numbPart = extractNumber(star);
        bertResult.params["star"] = numbPart;
    }
};
#endif // SYSAGENT_INTENTCONTENTCOMMENT_H
