// Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.

#ifndef SYSAGENT_HISTORY_H
#define SYSAGENT_HISTORY_H

#include "aisf_platform/utils/utils.h"

// libwebsockets Expand宏定义和rapidjson 冲突
#ifdef Expand
#undef Expand
#endif

#include "http_client_factory.h"

using namespace AisfPlat;
using GetMemoriesCallback = std::function<void(int errorCode, const std::vector<std::string>& memories)>;

constexpr std::string_view URL_MEMORY_HISTORY_ADD = "/kmm/v1/user/memory/history/add";
constexpr std::string_view URL_MEMORY_UPDATE = "/kmm/v1/user/memory/user_input";
constexpr std::string_view URL_MEMORY_SCENE = "/kmm/v1/user/memory/custom_input";
constexpr std::string_view URL_MEMORY_HISTORY_BATCH_ADD = "/kmm/v1/user/memory/history/batch";
constexpr std::string_view URL_GET_MEMORIES = "/kmm/v1/user/memories/get";
constexpr std::string_view URL_GET_SHORT_MEMORIES = "/kmm/v1/user/memory/short/qa_mgr/get";
constexpr std::string_view URL_ADD_SHORT_MEMORIES = "/kmm/v1/user/memory/short/qa_mgr/operate";
constexpr std::string_view URL_QUERY_MEMORIES = "/kmm/v1/user/memories/query";
constexpr std::string_view URL_RECALL_RELEVANT_MEMORIES = "/kmm/v1/user/memories/relevant/recall";
constexpr std::string_view URL_MEMORY_QUERY = "/kmm/v1/user/memory/query";
constexpr std::string_view URL_PERSONA_QUERY = "/kmm/v1/user/memories/persona/query";
constexpr std::string_view URL_SHARED_MEMORY_MANAGER = "/kmm/v1/user/memory/short/manager";
constexpr std::string_view URL_STEP_TEACH_SKILL = "/kmm/v1/user/stepTeachSkill";

struct ChatMessageContent {
    std::string role;
    std::string content;
    std::string date;

    void ToJson(Json::Value& jsonObj) const
    {
        jsonObj["role"] = role;
        jsonObj["content"] = content;
        jsonObj["date"] = date;
    }
    std::string toString() const
    {
        Json::Value root;
        root["role"] = role;
        root["content"] = content;
        root["date"] = date;
        return JsonParser::ToRawString(root);
    }
};

struct ChatMessage {
    std::string matedata;
    std::string sessionId;
    ChatMessageContent content;

    void ToJson(Json::Value& jsonObj) const
    {
        jsonObj["matedata"] = matedata;
        jsonObj["sessionId"] = sessionId;
        Json::Value jsonContent;
        content.ToJson(jsonContent);
        jsonObj["content"] = jsonContent;
    }
    std::string toString() const
    {
        Json::Value root;
        Json::Value jsonContent;
        root["matedata"] = matedata;
        root["sessionId"] = sessionId;
        // 转成string发送
        root["content"] = content.toString();
        return JsonParser::ToRawString(root);
    }
};
struct Memory {
    std::string userId;
    std::string sessionId;
    std::string role;
    std::string content;
    std::string date;
};
struct BatchMemory {
    std::string metadata;
    std::vector<Memory> contents;
    std::string toString() const
    {
        Json::Value root;
        root["metadata"] = metadata;
        Json::Value contentArray;
        for (const auto &mem: contents) {
            Json::Value item;
            item["userId"] = mem.userId;
            item["sessionId"] = mem.sessionId;
            item["role"] = mem.role;
            item["content"] = mem.content;
            item["date"] = mem.date;
            contentArray.append(item);
        }
        root["content"] = contentArray;
        return JsonParser::ToRawString(root);
    }
};

struct MemorizeMessage {
    std::string metadata;
    std::string sessionId;
    std::string opType;
    std::string category;
    std::string content;
    Json::Value custom_info;

    std::string toString() const
    {
        Json::Value root;
        root["metadata"] = metadata;
        root["sessionId"] = sessionId;
        root["opType"] = opType;
        root["category"] = category;
        root["content"] = content;
        root["custom_info"] = custom_info;
        return JsonParser::ToRawString(root);
    }
};

struct UserMemoryInfo {
    std::string memContent;
    std::string memSrc;
    std::string memDate;
    int index;
};

void AddMemoryHistory(std::string userid, const ChatMessage& message);

void addBatchMemory(BatchMemory& batchMemory);

void UpdateMemory(std::string userid, const MemorizeMessage& message, std::string& url, std::string& taskID);

std::vector<std::string> getIndexArray(const std::string& taskID);

void RecallMemoriesAsync(const std::string& query, const GetMemoriesCallback& callback, const std::string& userId,
                         const std::string& taskID, int topK = 1);

bool RecallMemoriesSync(std::vector<std::string>& outMemories, const std::string& query, const std::string& userId,
                        int topK = 1);

bool QueryMemoryListSync(std::string& bodyStr, const std::string& senceType, const std::string& userId);

bool RecallRelevantMemorySync(std::vector<UserMemoryInfo>& outMemories, const std::string& query,
                              const std::string& userId, int topK = 1);

bool QueryPersonaInfoSync(std::string& bodyStr, const std::string& userId);
#endif // SYSAGENT_HISTORY_H