/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 */

#include "memory/ContextManager.h"
#include "utils/http_client.h"
#include "aisf_platform/parser/json_parser.h"
#include "config/Config.h"
#include <set>
#include <regex>

using namespace AisfPlat;

namespace ContextManager
{

std::vector<std::map<std::string, std::string>> ParseQAEntries(const Json::Value& data, const std::string& key1, const std::string& key2)
{
    std::vector<std::map<std::string, std::string>> entries;

    auto parseEntry = [&](const std::string& key) {
        if (!data.isNull() && data.isMember(key) && data[key].isObject()) {
            std::map<std::string, std::string> entry;
            std::string role, content;
            if (AisfPlat::JsonParser::GetString(data[key], "role", role) &&
                AisfPlat::JsonParser::GetString(data[key], "content", content)) {
                entry["role"] = role;
                entry["content"] = content;
                entries.push_back(entry);
            }
        }
    };
    parseEntry(key1);
    parseEntry(key2);

    return entries;
}

bool GetContext(const ContextRequest& request, const ContextQueryCallback &callback)
{
    std::string endpointIP = SYSAGENT::HttpClient::GetServiceIP("DMContextService");
    std::string url = "https://" + endpointIP + ":27203" + URL_CONTEXT_QUERY.data();
    std::string body = "";
    Json::Value bodyJson;
    request.ToJson(bodyJson);
    auto retCode = SYSAGENT::HttpClient::Http2SendRequest(
            url, AisfPlat::JsonParser::ToRawString(bodyJson).c_str(), {},
            [callback, request](const std::shared_ptr<CMFrm::COM::Http2ClientResponse>& http2Response, void* arg) {
                if (!http2Response) {
                    callback(false, nullptr);
                    return;
                }
                std::string bodyString = http2Response->GetResponseBody();
                if (!http2Response->Is2XXStatusCode()) {
                    CMFrm_Error("GetContext Received response failed, status code: %d", http2Response->GetStatusCode());
                    callback(false, nullptr);
                    return;
                }
                CMFrm_Info("GetContext Received response success, body: %s", bodyString.c_str());

                Json::Value root;
                if (!JsonParser::Parse(bodyString.c_str(), root) || !root.isMember("context")) {
                    CMFrm_Error("Failed to parser json, or no data member.");
                    callback(false, nullptr);
                    return;
                }
                auto contextResult = std::make_shared<ContextResult>();
                Json::Value qaList = root["context"]["context_msg"];
                for (const auto& item : qaList) {
                    contextResult->originQAList.push_back(OriginQA::Parse(item));
                }

                LongMemory lm = parseLongMemory(request.userId, request.subUserId);
                CMFrm_Info("query: %s, regularString: %s, longMemory: %s", request.query.c_str(), lm.regularString.c_str(), lm.longMemory.c_str());

                // 优先用 DMContextService 返回的长期记忆，没有再兜底硬编码
                if (root.isMember("long_memory") && root["long_memory"].isArray() && !root["long_memory"].empty()) {
                    CMFrm_Info("use long_memory from DMContextService, size=%u content=%s",
                        root["long_memory"].size(), JsonParser::ToRawString(root["long_memory"]).c_str());
                } else if (!lm.longMemory.empty()) {
                    root["long_memory"] = Json::Value(Json::arrayValue);
                    root["long_memory"].append(lm.longMemory);
                    CMFrm_Info("use hardcoded long_memory: %s", lm.longMemory.c_str());
                }
                contextResult->result = JsonParser::ToRawString(root);
                callback(true, contextResult);
            }, 5);
    if (!retCode) {
        callback(false, nullptr);
    }
    return retCode;
}

bool AddContext(const std::string& userId, const std::string& subUserId, const std::string& sessionId,
                const std::string& agentName, const std::string& label, Json::Value& context, const ContextAddCallback &callback)
{
    std::string endpointIP = SYSAGENT::HttpClient::GetServiceIP("DMContextService");
    std::string url = "https://" + endpointIP + ":27203" + URL_CONTEXT_ADD.data();
    Json::Value bodyJson;
    bodyJson["user_id"] = userId;
    bodyJson["sub_user_id"] = subUserId;
    bodyJson["session_id"] = sessionId;
    bodyJson["agent_name"] = agentName;
    bodyJson["label"] = label;
    bodyJson["context"] = context;
    auto retCode = SYSAGENT::HttpClient::Http2SendRequest(
            url, AisfPlat::JsonParser::ToRawString(bodyJson).c_str(), {},
            [callback](const std::shared_ptr<CMFrm::COM::Http2ClientResponse>& http2Response, void* arg) {
                if (!http2Response) {
                    callback(false, {});
                    return;
                }
                std::string bodyString = http2Response->GetResponseBody();
                if (!http2Response->Is2XXStatusCode()) {
                    CMFrm_Error("AddContext Received response failed, status code: %d", http2Response->GetStatusCode());
                    callback(false, {});
                    return;
                }
                CMFrm_Info("AddContext Received response success, body: %s", bodyString.c_str());
                Json::Value root;
                if (!JsonParser::Parse(bodyString.c_str(), root)) {
                    CMFrm_Error("Failed to parser json, or no data member.");
                    callback(false, {});
                    return;
                }
                std::vector<std::string> contextIds;
                if (root.isMember("context_id") && root["context_id"].isArray()) {
                    for (const auto& id : root["context_id"]) {
                        contextIds.push_back(id.asString());
                    }
                }
                callback(true, contextIds);
            }, 3);
    if (!retCode) {
        callback(false, {});
    }
    return retCode;
}

bool DelContext(const std::string& userId, const ContextDelCallback &callback)
{
    std::string endpointIP = SYSAGENT::HttpClient::GetServiceIP("DMContextService");
    std::string url = "https://" + endpointIP + ":27203" + URL_CONTEXT_DELETE.data();
    Json::Value bodyJson;
    ContextRequest request{.userId = userId};
    request.ToJson(bodyJson);
    auto retCode = SYSAGENT::HttpClient::Http2SendRequest(
            url, AisfPlat::JsonParser::ToRawString(bodyJson).c_str(), {},
            [callback](const std::shared_ptr<CMFrm::COM::Http2ClientResponse>& http2Response, void* arg) {
                if (!http2Response) {
                    callback(false);
                    return;
                }
                std::string bodyString = http2Response->GetResponseBody();
                if (!http2Response->Is2XXStatusCode()) {
                    CMFrm_Error("DeleteContext Received response failed, status code: %d", http2Response->GetStatusCode());
                    callback(false);
                    return;
                }
                CMFrm_Info("AddContext Received response success, body: %s", bodyString.c_str());
                Json::Value root;
                if (!JsonParser::Parse(bodyString.c_str(), root)) {
                    CMFrm_Error("Failed to parser json, or no data member.");
                    callback(false);
                    return;
                }
                callback(true);
            }, 3);
    if (!retCode) {
        callback(false);
    }
    return retCode;
}

bool CheckContextValid(std::map<std::string, std::string> context)
{
    if (context["content"].empty()) {
        return false;
    }
    return true;
}

std::vector<std::map<std::string, std::string>> GetMixedContext(std::shared_ptr<ContextResult> result)
{
    std::vector<std::map<std::string, std::string>> memories;
    if (!result) {
        return memories;
    }
    for (uint32_t i = 0; i < result->originQAList.size(); i ++) {
        for (auto qa : result->originQAList[i].contextQA) {
            if (CheckContextValid(qa)) {
                memories.push_back(qa);
            }
        }
    }
    return memories;
}

}