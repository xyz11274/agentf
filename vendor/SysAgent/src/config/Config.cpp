

#include "config/Config.h"
#include "aisf_platform/common_define.h"
#include "aisf_platform/common_macro.h"
#include "aisf_platform/parser/json_parser.h"
#include "aisf_platform/utils/utils.h"
#include "config/Config.h"
#include <cstddef>
#include <fstream>
#include <json/json.h>
#include "config/Config.h"
#include "json/value.h"
#include "ruleCognition/DefaultRuleEntity.h"
#include "config/IntentPlaySpeed.h"
#include "config/IntentPlayForward.h"
#include "ruleCognition/UserInputToAgentRuleEntity.h"

Json::Value ToolMessage::ToJson() const
{
    Json::Value root;

    // 基本信息
    root["type"] = type;
    root["name"] = name;
    root["id"] = id;
    root["description"] = description;
    root["msgCallBack"] = msgCallBack;

    // toolInterfaces
    Json::Value interfaces(Json::arrayValue);
    for (const auto& toolInterface : toolInterfaces) {
        Json::Value interfaceObj;
        interfaceObj["apiName"] = toolInterface.apiName;
        interfaceObj["apiDescription"] = toolInterface.apiDescription;

        // apiParams
        Json::Value params(Json::objectValue);
        for (const auto& [key, param] : toolInterface.apiParams) {
            Json::Value paramObj;
            paramObj["type"] = param.type;
            paramObj["required"] = param.required;
            paramObj["description"] = param.description;
            paramObj["source"] = param.source;
            params[key] = paramObj;
        }
        interfaceObj["apiParams"] = params;

        interfaces.append(interfaceObj);
    }
    root["toolInterfaces"] = interfaces;

    return root;
}

std::string ToolMessage::ToString() const
{
    return JsonParser::ToRawString(ToJson());
}

bool parseEnvInfo(std::string filePath, std::map<std::string, std::string>& envInfoMap)
{
    // 创建输入文件流
    std::ifstream configFile(AisfPlat::FileUtils::RealPath(filePath), std::ifstream::binary);
    // 检查文件是否打开成功
    if (!configFile.is_open()) {
        LOG_ERR_EX("[parseEnvInfo]Unable to open env_info.json file");
        return false;
    }

    // 创建 JSON 根对象
    Json::Value root;
    Json::CharReaderBuilder readerBuilder;
    std::string errs;

    // 解析 JSON 文件到 root
    if (!Json::parseFromStream(readerBuilder, configFile, &root, &errs)) {
        LOG_ERR_EX("[parseEnvInfo]Failed to parse JSON");
        return false;
    }

    for (const auto& item : root) {
        std::string key = item["key"].asString();
        std::string value = item["value"].asString();
        envInfoMap[key] = value;
    }

    for (const auto& [key, value] : envInfoMap) {
        LOG_INFO_EX("[parseEnvInfo]Key: %s, Value:%s", key.c_str(), value.c_str());
    }
    return true;
}

bool parseModelInfoData(std::string filePath, std::map<std::string, std::string>& modelInfoMap)
{
    // 创建输入文件流
    std::ifstream configFile(AisfPlat::FileUtils::RealPath(filePath), std::ifstream::binary);
    // 检查文件是否打开成功
    if (!configFile.is_open()) {
        LOG_ERR_EX("[parseModelInfoData]Unable to open config.json file");
        return false;
    }

    // 创建 JSON 根对象
    Json::Value root;
    Json::CharReaderBuilder readerBuilder;
    std::string errs;

    // 解析 JSON 文件到 root
    if (!Json::parseFromStream(readerBuilder, configFile, &root, &errs)) {
        LOG_ERR_EX("[parseModelInfoData]Failed to parse JSON");
        return false;
    }

    for (const auto& item : root) {
        std::string name = item["name"].asString();
        std::string url = item["url"].asString();
        modelInfoMap[name] = url;
    }

    for (const auto& [name, url] : modelInfoMap) {
        LOG_INFO_EX("[parseModelInfoData]Name: %s, URL:%s", name.c_str(), url.c_str());
    }
    return true;
}

bool parseToolInfoData(std::string filePath, std::vector<ToolInfo>& toolList)
{
    // 打开JSON文件
    std::ifstream file(AisfPlat::FileUtils::RealPath(filePath), std::ifstream::binary);
    if (!file.is_open()) {
        LOG_ERR_EX("open %s failed", filePath.c_str());
        return false;
    }

    // 读取JSON文件到Json::Value
    Json::Value jsonData;
    Json::CharReaderBuilder rbuilder;
    std::string errs;
    if (!Json::parseFromStream(rbuilder, file, &jsonData, &errs)) {
        LOG_ERR_EX("Failed to parse tool_info.json");
        return false;
    }

    file.close();  // 明确关闭文件

    // 检查JSON是否为一个数组
    if (!jsonData.isArray()) {
        LOG_ERR_EX("tool_info.json is not array");
        return false;
    }

    // 解析数组中的字符串
    for (const auto& item : jsonData) {
        if (item.isObject()) {
            ToolInfo tool;
            AisfPlat::JsonParser::GetString(item, "id", tool.id);
            AisfPlat::JsonParser::GetString(item, "name", tool.name);
            toolList.push_back(tool);
        } else {
            LOG_ERR_EX("tool_info.json has invalid non-string element");
        }
    }

    LOG_INFO_EX("Parsed tool information from ", filePath.c_str());
    // 输出解析后的字符串
    for (const auto& tool : toolList) {
        LOG_INFO_EX("Tool: %s", tool.id.c_str());
    }

    return true;
}


shared_ptr<AbstractTransformRule> getPtrByRuleName(std::string& name, Json::Value transformRule)
{
    shared_ptr<AbstractTransformRule> abstractTransformRule;
    if (name == "TO_KEY_VALUE") {
        abstractTransformRule = make_shared<IntentPlaySpeed>();
    } else if (name == "STR_CLEAN_TO_INT") {
        abstractTransformRule = make_shared<IntentContentComment>();
    } else if (name == "STR_TO_TIME") {
        abstractTransformRule = make_shared<IntentPlayForward>();
    }
    if (abstractTransformRule != nullptr) {
        abstractTransformRule->setName(name);
        for (const auto& ruleName : transformRule["rule"].getMemberNames()) {
            abstractTransformRule->insertToRuleMap(ruleName, transformRule["rule"][ruleName].asString());
        }
    }
    return abstractTransformRule;
}

bool parseIntentCategoryData(std::string filePath, std::map<std::string, Intent>& intents,
                             std::map<std::string, std::string>& intentToParamsString)
{
    std::ifstream file(AisfPlat::FileUtils::RealPath(filePath), std::ifstream::binary);
    if (!file.is_open()) {
        LOG_ERR_EX("open %s failed", filePath.c_str());
        return false;
    }

    // 读取JSON文件到Json::Value
    Json::Value jsonData;
    Json::CharReaderBuilder rbuilder;
    std::string errs;
    if (!Json::parseFromStream(rbuilder, file, &jsonData, &errs)) {
        LOG_ERR_EX("Failed to parse tool_info.json");
        return false;
    }

    // 解析数组中的字符串
    for (const auto& item : jsonData) {
        if (item.isObject()) {
            std::string category;
            std::string categoryId;
            AisfPlat::JsonParser::GetString(item, "category", category);
            AisfPlat::JsonParser::GetString(item, "categoryId", categoryId);
            auto intentsJson = AisfPlat::JsonParser::GetNode(item, "intents");
            CHECK_NULL_LOG_ERROR_RETURN(intentsJson, false, "parse intents failed");
            for (const auto& intentJson : *intentsJson) {
                Intent intent(category, categoryId);
                AisfPlat::JsonParser::GetString(intentJson, "intent", intent.intent);
                AisfPlat::JsonParser::GetString(intentJson, "description", intent.description);
                AisfPlat::JsonParser::GetBool(intentJson, "noArgs", intent.noArgs);
                std::string paramsJsonStr = "[]"; // 配置文件里面无参的时候，没有params这个成员，但是prompt无参数的要有个[]空数组
                if (!intent.noArgs) {
                    auto paramsJson = AisfPlat::JsonParser::GetNode(intentJson, "params");
                    CHECK_NULL_LOG_ERROR_RETURN(paramsJson, false, "parse params failed");
                    for (const auto& paramJson : *paramsJson) {
                        IntentParam param;
                        AisfPlat::JsonParser::GetString(paramJson, "name", param.name);
                        AisfPlat::JsonParser::GetString(paramJson, "type", param.type);
                        AisfPlat::JsonParser::GetString(paramJson, "description", param.description);
                        AisfPlat::JsonParser::GetString(paramJson, "required", param.required);
                        auto transformRule = paramJson["transformRule"];
                        if (transformRule.isObject()) {
                            std::string name = transformRule["name"].asString();
                            param.abstractTransformRule = getPtrByRuleName(name, transformRule);
                        }
                        intent.params.push_back(param);
                    }
                    Json::Value p = *paramsJson; // 上面的都是 const，不好修改，这里统一整改成符合prompt要求的
                    for (auto& pJson : p) {
                        pJson.removeMember("transformRule");
                    }
                    paramsJsonStr =  AisfPlat::JsonParser::ToRawString(p);
                }
                intentToParamsString[intent.intent] = paramsJsonStr; // 存在参数的，放入map意图名称参数字符串
                LOG_INFO_EX("category: %s, categoryId: %s, intent: %s", category.c_str(), categoryId.c_str(),
                            intent.intent.c_str());
                intents.emplace(intent.intent, intent);
            }
        } else {
            LOG_ERR_EX("tool_info.json has invalid non-string element");
        }
    }

    LOG_INFO_EX("Parsed intentCategory from %s, count: %d", filePath.c_str(), intents.size());
    return true;
}

bool parsePrompt(std::string filePath, std::string& prompt)
{
    std::ifstream file(AisfPlat::FileUtils::RealPath(filePath), std::ifstream::binary);
    if (!file.is_open()) {
        LOG_ERR_EX("open %s failed", filePath.c_str());
        return "";
    }

    // 读取JSON文件到Json::Value
    Json::Value jsonData;
    Json::CharReaderBuilder rbuilder;
    std::string errs;
    if (!Json::parseFromStream(rbuilder, file, &jsonData, &errs)) {
        LOG_ERR_EX("Failed to parse tool_info.json");
        return "";
    }

    file.close();  // 明确关闭文件
    return AisfPlat::JsonParser::GetString(jsonData["template"], "zh", prompt);
}

bool parseRuleInfoData(std::string filePath, vector<shared_ptr<AbstractRuleEntity>>& ruleList)
{
    std::ifstream file(AisfPlat::FileUtils::RealPath(filePath), std::ifstream::binary);
    if (!file.is_open()) {
        LOG_ERR_EX("open %s failed", filePath.c_str());
        return false;
    }
    // 读取JSON文件到Json::Value
    Json::Value jsonData;
    Json::CharReaderBuilder rbuilder;
    std::string errs;
    if (!Json::parseFromStream(rbuilder, file, &jsonData, &errs)) {
        LOG_ERR_EX("Failed to parse rule.json");
        return false;
    }
    file.close();  // 明确关闭文件
    // 解析数组中的字符串
    for (const auto& item : jsonData) {
        shared_ptr<AbstractRuleEntity> ruleEntity;
        if (item.isObject()) {
            // 要根据 ruleType 来创建不同的实现类
            string ruleType = item["ruleType"].asString();
            if (ruleType == "1") {  // 这里类型 "1" 是 DefaultRuleEntity 的标识
                ruleEntity = make_shared<DefaultRuleEntity>();
            } else if (ruleType == "2") {
                ruleEntity = make_shared<UserInputToAgentRuleEntity>();
            }
            if (ruleEntity == nullptr) {  // ruleType和所有实现类都不匹配，输出错误日志并跳过当前循环
                LOG_ERR_EX("Failed to create rule entity for type %s", ruleType.c_str());
                continue;
            }
            AisfPlat::JsonParser::GetString(item, "ruleName", ruleEntity->ruleName);
            AisfPlat::JsonParser::GetString(item, "ruleType", ruleEntity->ruleType);
            AisfPlat::JsonParser::GetString(item, "nextNode", ruleEntity->nextNode);
            AisfPlat::JsonParser::GetString(item, "toAgent", ruleEntity->toAgent);
            if (!item["conditions"].empty()) {
                for (const auto& conditionItem : item["conditions"]) {
                    if (conditionItem.isObject()) {
                        Condition condition;
                        AisfPlat::JsonParser::GetString(conditionItem, "conditionName", condition.conditionName);
                        AisfPlat::JsonParser::GetString(conditionItem, "conditionKey", condition.conditionKey);
                        AisfPlat::JsonParser::GetString(conditionItem, "conditionValue", condition.conditionValue);
                        AisfPlat::JsonParser::GetString(conditionItem, "conditionType", condition.conditionType);
                        ruleEntity->conditions.push_back(condition);
                    }
                }
            }
            ruleList.push_back(ruleEntity);
        } else {
            LOG_ERR_EX("rule.json has invalid non-string element");
        }
    }
    LOG_INFO_EX("Parsed rule information from [%s]", filePath.c_str());
    return true;
}


bool parsePromptInfoData(std::string filePath, std::vector<PromptInfo>& promptInfoList)
{
    std::ifstream file(AisfPlat::FileUtils::RealPath(filePath), std::ifstream::binary);
    if (!file.is_open()) {
        LOG_ERR_EX("open %s failed", filePath.c_str());
        return false;
    }
    // 读取JSON文件到Json::Value
    Json::Value jsonData;
    Json::CharReaderBuilder rbuilder;
    std::string errs;
    if (!Json::parseFromStream(rbuilder, file, &jsonData, &errs)) {
        LOG_ERR_EX("Failed to parse tool_info.json");
        return false;
    }
    file.close(); // 明确关闭文件
    // 解析数组中的字符串
    for (const auto& item : jsonData) {
        if (item.isObject()) {
            PromptInfo promptInfo;
            AisfPlat::JsonParser::GetString(item, "name", promptInfo.name);
            AisfPlat::JsonParser::GetString(item, "description", promptInfo.description);
            AisfPlat::JsonParser::GetString(item, "version", promptInfo.version);
            if (item["template"].isObject()) {
                AisfPlat::JsonParser::GetString(item["template"], "zh", promptInfo.promptTemplate);
            }
            LOG_INFO_EX("promptInfo.name [%s], promptInfo.promptTemplate [%s]", promptInfo.name.c_str(), promptInfo.promptTemplate.c_str());
            promptInfoList.push_back(promptInfo);
        } else {
            LOG_ERR_EX("prompt.json has invalid non-string element");
        }
    }
    LOG_INFO_EX("Parsed PromptInfo information from ", filePath.c_str());
    return true;
}

bool parseLLMExtractParamPromptInfoData(std::string filePath, std::map<std::string, std::string>& categoryToPromptMap)
{
    std::ifstream file(AisfPlat::FileUtils::RealPath(filePath), std::ifstream::binary);
    if (!file.is_open()) {
        LOG_ERR_EX("open %s failed", filePath.c_str());
        return false;
    }
    // 读取JSON文件到Json::Value
    Json::Value jsonData;
    Json::CharReaderBuilder rbuilder;
    std::string errs;
    if (!Json::parseFromStream(rbuilder, file, &jsonData, &errs)) {
        LOG_ERR_EX("Failed to parse tool_info.json");
        return false;
    }
    file.close(); // 明确关闭文件
    for (const auto& item : jsonData) {
        if (item.isObject()) {
            std::string category;
            AisfPlat::JsonParser::GetString(item, "category", category);
            std::string prompt;
            AisfPlat::JsonParser::GetString(item, "prompt", prompt);
            std::string categoryName;
            AisfPlat::JsonParser::GetString(item, "categoryName", categoryName);
            LOG_INFO_EX("category [%s], categoryName [%s], prompt [%s]", category.c_str(), categoryName.c_str(), prompt.c_str());
            categoryToPromptMap[category] = prompt;
        } else {
            LOG_ERR_EX("llm_prompt.json has invalid non-string element");
        }
    }
    return true;
}

std::map<std::string, ToolParam> ParseToolParam(const Json::Value& toolParam)
{
    std::map<std::string, ToolParam> apiParams;
    if (toolParam.isObject()) {
        for (const auto& paramName : toolParam.getMemberNames()) {
            ToolParam param;
            auto paramObj = toolParam[paramName];
            AisfPlat::JsonParser::GetString(paramObj, "type", param.type);
            AisfPlat::JsonParser::GetString(paramObj, "required", param.required);
            AisfPlat::JsonParser::GetString(paramObj, "description", param.description);
            AisfPlat::JsonParser::GetString(paramObj, "source", param.source);
            apiParams.insert({paramName, param});
        }
    }
    return apiParams;
}

ToolInterface ParseInterface(const Json::Value& interface)
{
    ToolInterface toolInterface{};
    if (interface.isObject()) {
        AisfPlat::JsonParser::GetString(interface, "apiName", toolInterface.apiName);
        AisfPlat::JsonParser::GetString(interface, "apiDescription", toolInterface.apiDescription);
        if (interface.isMember("apiParams") && interface["apiParams"].isObject()) {
            toolInterface.apiParams = ParseToolParam(interface["apiParams"]);
        }
    }
    return toolInterface;
}

bool parseToolApi(std::string filePath, std::map<std::string, ToolMessage>& apiList, std::string& apiString)
{
    std::ifstream file(AisfPlat::FileUtils::RealPath(filePath), std::ifstream::binary);
    if (!file.is_open()) {
        LOG_ERR_EX("open %s failed", filePath.c_str());
        return false;
    }
    // 读取JSON文件到Json::Value
    Json::Value jsonData;
    Json::CharReaderBuilder rbuilder;
    std::string errs;
    if (!Json::parseFromStream(rbuilder, file, &jsonData, &errs)) {
        LOG_ERR_EX("Failed to parse tool_info.json");
        return false;
    }
    file.close(); // 明确关闭文件
    apiString = JsonParser::ToRawString(jsonData);
    for (const auto& item : jsonData) {
        if (item.isObject()) {
            ToolMessage toolMessage;
            AisfPlat::JsonParser::GetString(item, "type", toolMessage.type);
            AisfPlat::JsonParser::GetString(item, "name", toolMessage.name);
            AisfPlat::JsonParser::GetString(item, "id", toolMessage.id);
            AisfPlat::JsonParser::GetString(item, "description", toolMessage.description);
            AisfPlat::JsonParser::GetString(item, "msgCallBack", toolMessage.msgCallBack);
            // 解析interface
            if (item.isMember("interface") && item["interface"].isArray()) {
                for (const auto& interfaceItem : item["interface"]) {
                    auto interface = ParseInterface(interfaceItem);
                    toolMessage.toolInterfaces.push_back(interface);
                }
            }
            apiList.insert({toolMessage.name, toolMessage});
        }
    }
    return true;
}

std::string ConfigManager::getEnvInfo(const string &key, const string &defaultValue) {
    auto it = ConfigManager::envInfoMap.find(key);
    if (it != ConfigManager::envInfoMap.end()) {
        return it->second;
    }
    return defaultValue;
}

LongMemory parseLongMemory(const std::string& userId, const std::string& subUserId)
{
    LongMemory result;
    result.regularString = R"(.*肚子.*痛.*|.*治疗.*|.*病.*|.*挂.*号.*|.*门诊.*|.*医院.*)";
    result.longMemory = "用户有肠炎病史，用户经常去的医院是东方医院南院";
    std::string filePath = "/opt/coremind/conf/" + userId + "__memory.json";
    std::ifstream file(AisfPlat::FileUtils::RealPath(filePath), std::ifstream::binary);
    if (!file.is_open()) {
        LOG_ERR_EX("[parseLongMemory] Unable to open %s", filePath.c_str());
        return result;
    }

    Json::Value root;
    Json::CharReaderBuilder readerBuilder;
    std::string errs;
    if (!Json::parseFromStream(readerBuilder, file, &root, &errs)) {
        LOG_ERR_EX("[parseLongMemory] Failed to parse JSON from %s", filePath.c_str());
        return result;
    }
    file.close();

    std::string regularString, longMemory;
    if (AisfPlat::JsonParser::GetString(root, "regularString", regularString) && !regularString.empty()) {
        result.regularString = regularString;
    }
    if (AisfPlat::JsonParser::GetString(root, "longMemory", longMemory) && !longMemory.empty()) {
        result.longMemory = longMemory;
    }
    LOG_INFO_EX("[parseLongMemory] regularString: %s, longMemory: %s",
                result.regularString.c_str(), result.longMemory.c_str());
    return result;
}