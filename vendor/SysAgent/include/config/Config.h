// Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.

#ifndef SYSAGENT_CONFIG_H
#define SYSAGENT_CONFIG_H

#include "aisf_platform/common_define.h"
#include "ruleCognition/RuleEntity.h"
#include "ruleCognition/DefaultRuleEntity.h"
#include <map>
#include <string>
#include <utility>
#include <vector>
#include "AbstractTransformRule.h"
#include "IntentPlaySpeed.h"
#include "IntentPlayForward.h"
#include "IntentContentComment.h"


struct ToolInfo {
    std::string id;
    std::string name;
};

struct IntentParam {
    std::string name;
    std::string type;
    std::string description;
    std::string required;
    shared_ptr<AbstractTransformRule> abstractTransformRule;
};
struct Intent {
    std::string intent;
    std::string category;
    std::string categoryId;
    std::string description;
    std::string args;
    bool noArgs{false};
    std::vector<IntentParam> params;

    Intent(std::string category, std::string categoryId)
        : category(std::move(category)),
          categoryId(std::move(categoryId))
    {
    }
};
struct PromptInfo {
    std::string name;
    std::string description;
    std::string version;
    std::string promptTemplate;
};

struct ToolParam {
    std::string type;
    std::string required;
    std::string description;
    std::string source;
    bool needModelExtract{false};
};

struct ToolInterface {
    std::string apiName;
    std::string apiDescription;
    std::map<std::string, ToolParam> apiParams;
};

struct ToolMessage {
    std::string type;
    std::string name;
    std::string id;
    std::string description;
    std::vector<ToolInterface> toolInterfaces;
    std::string msgCallBack;
    Json::Value ToJson() const;
    std::string ToString() const;
};

struct LongMemory {
    std::string regularString;
    std::string longMemory;
};

bool parseModelInfoData(std::string filePath, std::map<std::string, std::string>& modelInfoMap);
bool parseToolInfoData(std::string filePath, std::vector<ToolInfo>& toolList);

bool parseIntentCategoryData(std::string filePath, std::map<std::string, Intent>& intents,
                             std::map<std::string, std::string>& intentToParamsString);

bool parsePrompt(std::string filePath, std::string& prompt);
bool parseRuleInfoData(std::string filePath, vector<shared_ptr<AbstractRuleEntity>>& ruleList);
bool parsePromptInfoData(std::string filePath, std::vector<PromptInfo>& promptInfoList);
bool parseLLMExtractParamPromptInfoData(std::string filePath, std::map<std::string, std::string>& categoryToPromptMap);
bool parseToolApi(std::string filePath, std::map<std::string, ToolMessage>& apiList, std::string& apiString);
bool parseEnvInfo(std::string filePath, std::map<std::string, std::string>& envInfoMap);
LongMemory parseLongMemory(const std::string& userId, const std::string& subUserId);

class ConfigManager {
public:
    static ConfigManager& getInstance()
    {
        static ConfigManager instance = ConfigManager();
        return instance;
    }

    bool LoadConfig()
    {
        if (!parseModelInfoData("/opt/coremind/conf/model_info.json", modelInfoMap)) {
            LOG_ERR_EX("[ConfigManager] parseModelInfoData failed");
            return false;
        }
        if (!parseToolInfoData(string("/opt/coremind/conf/tool_info.json"), toolList)) {
            LOG_ERR_EX("[ConfigManager] parseToolInfoData failed");
            return false;
        }
        if (!parseRuleInfoData(string("/opt/coremind/conf/rule.json"), ruleList)) {
            LOG_ERR_EX("[ConfigManger] parseRuleInfoData failed");
            return false;
        }
        if (!parsePromptInfoData(string("/opt/coremind/conf/prompts.json"), promptInfoList)) {
            LOG_ERR_EX("[ConfigManager] parsePromptInfoData failed");
            return false;
        }

        if (!parseIntentCategoryData(string("/opt/coremind/conf/intent_category.json"),
                                     intents, intentToParamsString)) {
            LOG_ERR_EX("[ConfigManager] parseIntentCategoryInfoData failed");
            return false;
        }
        if (!parseLLMExtractParamPromptInfoData(string("/opt/coremind/conf/llm_prompt.json"),
                                                categoryToPromptMap)) {
            LOG_ERR_EX("[ConfigManager] parseLLMExtractParamPromptInfoData failed");
            return false;
        }
        if (!parseToolApi(string("/opt/coremind/conf/tool_api.json"), apiMap, apiString)) {
            LOG_ERR_EX("[ConfigManager] parseToolApi failed");
            return false;
        }
        if (!parseEnvInfo("/opt/coremind/conf/env_info.json", envInfoMap)) {
            LOG_ERR_EX("[ConfigManager] parseEnvInfo failed");
            return false;
        }
        longMemory = parseLongMemory(userId, subUserId);
        return true;
    }
    std::map<std::string, std::string>& getModelInfo() { return modelInfoMap; }

    std::vector<ToolInfo>& getToolList() { return toolList; }

    std::string& GetApiString() { return apiString; }

    std::map<std::string, ToolMessage>& GetApiMap() { return apiMap; }

    std::map<std::string, std::string>& getAPPInfoMap() { return appInfoMap; }

    std::vector<shared_ptr<AbstractRuleEntity>>& getRuleList() { return ruleList; }

    const std::map<std::string, Intent>& getIntents() { return intents; }

    std::map<std::string, std::string>& getIntentToParamsString()
    {
        return intentToParamsString;
    }

    std::vector<PromptInfo>& getPromptInfoList()
    {
        return promptInfoList;
    }

    std::map<std::string, std::string>& getCategoryToPromptMap()
    {
        return categoryToPromptMap;
    }

    std::string getEnvInfo(const std::string &key, const std::string &defaultValue = "");

private:
    std::map<std::string, std::string> modelInfoMap;
    std::vector<ToolInfo> toolList;
    std::map<std::string, Intent> intents;
    std::map<std::string, std::string> appInfoMap;
    std::vector<shared_ptr<AbstractRuleEntity>> ruleList;
    std::map<std::string, std::string> intentToParamsString;
    std::vector<PromptInfo> promptInfoList;
    std::map<std::string, std::string> categoryToPromptMap;
    std::map<std::string, ToolMessage> apiMap;
    std::string apiString;
    std::string userId;
    std::string subUserId;
    LongMemory longMemory;
    std::map<std::string, std::string> envInfoMap;
};

#endif  // SYSAGENT_CONFIG_H