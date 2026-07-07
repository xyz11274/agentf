// Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.

#ifndef SYSAGENT_BERTUTILS_H
#define SYSAGENT_BERTUTILS_H

#include <map>
#include <string>
#include "aisf_platform/parser/json_parser.h"
#include "json/value.h"
#include "taskManage/WorkFlow.h"

struct BertReault {
    std::string intent;
    std::map<std::string, std::string> params;
    std::string ToString()
    {
        Json::Value root;
        root["intent"] = intent;
        Json::Value paramsJson;
        for (const auto& [key, value] : params) {
            paramsJson[key] = value;
        }
        root["params"] = paramsJson;
        return JsonParser::ToRawString(root);
    }
};

struct BertParamTag {
    std::string text;
    std::string type;
    std::string name;
};

std::vector<BertParamTag> parseBertParamTag(const std::string& input);

std::vector<BertReault> parseBertAnswer(const std::string& query);

std::vector<IntentTask> parseIntentTasks(const std::string& result, const std::string& request,
                                         const std::string& intentPath);

/**
 * 解析26A统一意图识别输出结果
 * @param result
 * @param request
 * @param intentPath
 * @return
 */
std::vector<IntentTask> parseUnifyIntentTask(const std::string& result, const std::string& request,
    const std::string& intentPath);

#endif  // SYSAGENT_BERTUTILS_H