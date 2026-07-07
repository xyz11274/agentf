/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 */


#ifndef SYSAGENT_CONTEXTMANAGER_H
#define SYSAGENT_CONTEXTMANAGER_H

#include <string>
#include <json/json.h>
#include <future>
#include "aisf_platform/parser/json_parser.h"

using namespace AisfPlat;

constexpr std::string_view URL_CONTEXT_ADD = "/v1/user/context/add";
constexpr std::string_view URL_CONTEXT_QUERY = "/v1/user/context/query";
constexpr std::string_view URL_CONTEXT_DELETE = "/v1/user/context/delete";

namespace ContextManager
{
std::vector<std::map<std::string, std::string>> ParseQAEntries(const Json::Value& data,
                                                               const std::string& key1, const std::string& key2);

struct ContextRequest {
    std::string userId;
    std::string subUserId;
    std::string sessionId;
    std::string currentAgent;
    std::string queryType;
    std::string query;
    std::string rewriteQuery;
    int limitQARounds;
    int maxQATokens;
    std::string needLongMemory{"false"};
    std::string needStartGetMemory{"false"};

    void ToJson(Json::Value& jsonObj) const
    {
        jsonObj["user_id"] = userId;
        jsonObj["sub_user_id"] = subUserId;
        jsonObj["session_id"] = sessionId;
        jsonObj["current_agent"] = currentAgent;
        jsonObj["query_type"] = queryType;
        jsonObj["limit_qa_rounds"] = limitQARounds;
        jsonObj["max_qa_tokens"] = maxQATokens;
        jsonObj["query"] = query;
        jsonObj["rewrite_query"] = rewriteQuery;
        jsonObj["need_long_memory"] = needLongMemory;
        jsonObj["need_start_get_memory"] = needStartGetMemory;
    }
};

struct OriginQA {
    std::string contextId;
    std::string ownerAgent;
    std::vector<std::map<std::string, std::string>> contextQA;
    std::string reWriteQuery;
    std::string customSummary;
    std::string date;

    static OriginQA Parse(Json::Value data)
    {
        OriginQA originQa;
        AisfPlat::JsonParser::GetString(data, "context_id", originQa.contextId);
        AisfPlat::JsonParser::GetString(data, "owner_agent", originQa.ownerAgent);
        AisfPlat::JsonParser::GetString(data, "re_write_query", originQa.reWriteQuery);
        AisfPlat::JsonParser::GetString(data, "custom_summary", originQa.customSummary);
        AisfPlat::JsonParser::GetString(data, "date", originQa.date);
        originQa.contextQA = ParseQAEntries(data, "query", "answer");

        return originQa;
    }
};

struct AbstractQA {
    std::string contextId;
    std::vector<std::map<std::string, std::string>> abstractQA;

    static AbstractQA Parse(Json::Value data)
    {
        AbstractQA abstractQbj;
        AisfPlat::JsonParser::GetString(data, "context_id", abstractQbj.contextId);
        abstractQbj.abstractQA = ParseQAEntries(data, "query", "answer");
        return abstractQbj;
    }
};

struct ContextResult {
    std::vector<OriginQA> originQAList;
    std::vector<AbstractQA> abstractQAList;
    std::vector<std::string> relatedQA;
    std::string result;
};

using ContextQueryCallback = std::function<void(bool isSuccess, std::shared_ptr<ContextResult> result)>;
using ContextAddCallback = std::function<void(bool isSuccess, std::vector<std::string> contextIds)>;
using ContextDelCallback = std::function<void(bool isSuccess)>;

bool GetContext(const ContextRequest& request, const ContextQueryCallback &callback);
bool AddContext(const std::string& userId, const std::string& subUserId, const std::string& sessionId,
                const std::string& agentName, const std::string& label, Json::Value& context, const ContextAddCallback &callback);
bool DelContext(const std::string& userId, const ContextDelCallback &callback);

std::vector<std::map<std::string, std::string>> GetMixedContext(std::shared_ptr<ContextResult> result);
}

#endif // SYSAGENT_CONTEXTMANAGER_H
