// Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.

#include "model/ModelUtils.h"
#include "aisf_platform/parser/json_parser.h"
#include "json/value.h"
#include <regex>
#include <vector>

std::vector<BertParamTag> parseBertParamTag(const std::string& input)
{
    std::vector<BertParamTag> tags;
    std::regex tag_regex(R"(\[([^\]]+)\])");
    std::smatch match;
    std::string::const_iterator search_start(input.cbegin());

    while (std::regex_search(search_start, input.cend(), match, tag_regex)) {
        std::string tag_content = match.str(1);
        std::size_t colon_pos = tag_content.find(':');
        if (colon_pos != std::string::npos) {
            std::string text = tag_content.substr(0, colon_pos);
            std::string left_content = tag_content.substr(colon_pos + 1);
            std::size_t hyphens_pos = left_content.find('-');
            if (hyphens_pos != std::string::npos) {
                std::string type = left_content.substr(0, hyphens_pos);
                std::string name = left_content.substr(hyphens_pos + 1);
                tags.push_back({text, type, name});
            }
        }
        search_start = match.suffix().first;
    }

    return tags;
}

// [{"input":"我想看刘德华的喜剧电影","intent":"INTENT_VIDEO_SEARCH_PLAY",
// "slot_prediction":"O O O O O O O B-type I-type B-category I-category",
// "slot":"我想看刘德华的[喜:B-type][剧:I-type][电:B-category][影:I-category]",
// "confidence":["INTENT_VIDEO_SEARCH_PLAY:99.9%","INTENT_PLAY_SPEED:0.0%",
// "INTENT_PLAY_CONTINUE:0.0%","INTENT_AVCALL_EDIT_CONTACT:0.0%","INTENT_PLAY_BACKWORD:0.0%"]}]
std::vector<BertReault> parseBertAnswer(const std::string& answer)
{
    std::vector<BertReault> results;
    Json::Value root;
    JsonParser::Parse(answer, root);
    if (root.isArray()) {
        for (const auto& item : root) {
            string intent;
            string slot;
            std::map<std::string, std::string> params;
            JsonParser::GetString(item, "intent", intent);
            JsonParser::GetString(item, "slot", slot);
            auto tags = parseBertParamTag(slot);
            for (const auto& tag : tags) {
                if (tag.type == "B") {
                    params.emplace(tag.name, tag.text);
                } else {
                    params[tag.name] += tag.text;
                }
            }
            results.push_back(BertReault{intent, params});
        }
    }

    return results;
}

/**
 * 按指定规则添加新任务到任务列表中
 * 当前规则：
 * 1. 同为路由到美食Agent的任务，且依赖相同
 * @param tasks
 * @param newTask
 */
bool addTaskByRule(std::vector<IntentTask>& tasks, const IntentTask& newTask)
{
    if (newTask.agentName != "FoodieAgent") {
        tasks.push_back(newTask);
        return false;
    }
    for (auto& task : tasks) {
        if (task.agentName == "FoodieAgent" && task.dependsOn == newTask.dependsOn) {
            task.requestContent = task.requestContent + "@@" + newTask.requestContent;
            return true;
        }
    }
    tasks.push_back(newTask);
    return false;
}

void convertTask(IntentTask& task)
{
    if (task.agentName == "intent_summary_report") {
        task.agentName = "MovieAgent";
        Data data;
        data.intent = "intent_summary_report";
        task.data = data;
    } else if (task.agentName == "EmailAgent") {
        task.agentName = "OfficeAgent";
        task.data.params["convert"] = "0";
    }
}

/**
 * 解析26A统一意图识别的模型输出结果
 * @return
 */
std::vector<IntentTask> parseUnifyIntentTask(const std::string& result, const std::string& request,
    const std::string& intentPath)
{
    Json::Value jsonReq;
    if (!JsonParser::Parse(result, jsonReq)) {
        return {};
    }
    if (!jsonReq.isArray()) {
        LOG_ERR_EX("invalid llm answer: %s", result.c_str());
        return {};
    }
    std::vector<IntentTask> intentTasks;
    for (auto& it : jsonReq) {
        IntentTask task;
        task.intentPath = intentPath;
        JsonParser::GetUIntHashSet(it, "depends_on", task.dependsOn);
        if (JsonParser::GetString(it, "assistant_name", task.agentName) &&
            JsonParser::GetString(it, "context", task.requestContent) &&
            JsonParser::GetUInt(it, "step", task.step)) {
            convertTask(task);
            addTaskByRule(intentTasks, task);
        }
    }

    return intentTasks;
}

std::vector<IntentTask> parseIntentTasks(const std::string& result, const std::string& request,
                                         const std::string& intentPath)
{
    Json::Value jsonReq;
    if (!JsonParser::Parse(result, jsonReq)) {
        return {};
    }

    std::vector<IntentTask> intentTasks;
    if (jsonReq.isArray()) {
        for (auto& it : jsonReq) {
            IntentTask task;
            task.intentPath = intentPath;
            if (JsonParser::GetString(it, "agent_name", task.agentName) &&
                JsonParser::GetString(it, "context", task.requestContent)) {
                JsonParser::GetString(it, "APP", task.APP);  // appName 可以没有,没有的话就是空字符串
                convertTask(task);
                intentTasks.push_back(task);
            }
        }
    } else if (jsonReq.isObject()) {
        IntentTask task;
        task.intentPath = intentPath;
        if (JsonParser::GetString(jsonReq, "agent_name", task.agentName) &&
            JsonParser::GetString(jsonReq, "context", task.requestContent)) {
            JsonParser::GetString(jsonReq, "APP", task.APP);  // appName 可以没有,没有的话就是空字符串
            convertTask(task);
            intentTasks.push_back(task);
        }
    } else {
        LOG_ERR_EX("invalid llm answer: %s", result.c_str());
        return {};
    }

    return intentTasks;
}