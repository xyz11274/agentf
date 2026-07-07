//
// Created by c00888021 on 2025/9/2.
//

#include "gtest/gtest.h"
#include "mockcpp/mokc.h"
#include "aisf_platform/common_define.h"
#include "ruleCognition/RuleEntity.h"
#include "config/Config.h"
#include <map>
#include <string>
#include <utility>
#include <vector>


std::map<std::string, std::string> modelInfoMap;
std::vector<ToolInfo> toolList;
std::vector<AgentInfo> agentList;
std::map<std::string, Intent> intents;
std::map<std::string, std::string> appInfoMap;
std::vector<shared_ptr<AbstractRuleEntity>> ruleList;
std::map<std::string, std::string> intentToParamsString;
std::vector<PromptInfo> promptInfoList;
std::map<std::string, std::string> categoryToPromptMap;


bool LoadConfig()
{
    if (!parseModelInfoData("./model_info.json", modelInfoMap)) {
        std::cout << "[ConfigManger] parseModelInfoData failed" << std::endl;
    }
    if (!parseToolInfoData(string("./tool_info.json"), toolList)) {
        std::cout << "[ConfigManger] parseToolInfoData failed" << std::endl;
    }

    if (!parseAgentInfoData(string("./agent.json"), agentList, appInfoMap)) {
        std::cout << "[ConfigManger] parseAgentInfoData failed" << std::endl;
    }
    if (!parseRuleInfoData(string("./rule.json"), ruleList)) {
        std::cout << "[ConfigManger] parseRuleInfoData failed" << std::endl;
    }
    if (!parsePromptInfoData(string("./prompts.json"), promptInfoList)) {
        std::cout << "[ConfigManger] parsePromptInfoData failed" << std::endl;
    }

    if (!parseIntentCategoryData(string("./intent_category.json"),
                                 intents, intentToParamsString)) {
        std::cout << "[ConfigManger] parseIntentCategoryInfoData failed" << std::endl;
    }
    if (!parseLLMExtractParamPromptInfoData(string("./llm_prompt.json"),
                                            categoryToPromptMap)) {
        std::cout << "[ConfigManger] parseLLMExtractParamPromptInfoData failed" << std::endl;
    }
    return true;
}

TEST(ConfigTest, Valid) {
    bool flag = LoadConfig();
    EXPECT_EQ(true, flag);
}