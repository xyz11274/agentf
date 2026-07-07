// Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.

#include "aisf_platform/common_define.h"
#include "memory/History.h"
#include "utils/http_client.h"
#include "utils/env_util.h"
#include "json/json.h"

#define MEMORY_REQ_TIMEOUT 10

void AddMemoryHistory(std::string userid, const ChatMessage& message)
{
    std::string endpointIP = SYSAGENT::HttpClient::GetServiceIP("KMMService");
    if (endpointIP.empty()) {
        LOG_ERR_EX("[MEMORY] add user memory history failed, kmmservice instance empty");
        return;
    }

    auto http2Client = CMFrm::COM::HttpClientFactory::GetHttpClientFactory()->GetHttp2Client("KMMService");
    auto req = std::make_shared<CMFrm::COM::Http2ClientRequest>();
    req->SetMethod(CMFrm::COM::HttpMethod::POST);
    req->SetReqUrl("https://" + endpointIP + ":27203" + URL_MEMORY_HISTORY_ADD.data());
    req->AddHeader("userId", userid);
    req->AddBody(message.toString());
    req->SetTimeoutSecond(MEMORY_REQ_TIMEOUT);
    LOG_INFO_EX("content : %s", message.toString().c_str());
    http2Client->SendAsync(req, [](std::shared_ptr<CMFrm::COM::Http2ClientResponse> resp, void*) {
        // 只记录结果
        LOG_INFO_EX("add memory result: %d, %s", resp->GetStatusCode(), resp->GetResponseBody().c_str());
    });
    return;
}

void addBatchMemory(BatchMemory& batchMemory)
{
    std::string endpointIP = SYSAGENT::HttpClient::GetServiceIP("KMMService");
    if (endpointIP.empty()) {
        LOG_ERR_EX("[MEMORY] add user memory history failed, kmmService instance empty");
        return;
    }
    auto http2Client = CMFrm::COM::HttpClientFactory::GetHttpClientFactory()->GetHttp2Client("KMMService");
    auto req = std::make_shared<CMFrm::COM::Http2ClientRequest>();
    req->SetMethod(CMFrm::COM::HttpMethod::POST);
    req->SetReqUrl("https://" + endpointIP + ":27203" + URL_MEMORY_HISTORY_BATCH_ADD.data());
    req->AddBody(batchMemory.toString());
    req->SetTimeoutSecond(MEMORY_REQ_TIMEOUT);
    http2Client->SendAsync(req, [](std::shared_ptr<CMFrm::COM::Http2ClientResponse> resp, void*) {
        // 只记录结果
        LOG_INFO_EX("add memory result: %d, %s", resp->GetStatusCode(), resp->GetResponseBody().c_str());
    });
    return;
}

void UpdateMemory(std::string userid, const MemorizeMessage& message, std::string& url, std::string& taskID)
{
    std::string endpointIP = SYSAGENT::HttpClient::GetServiceIP("KMMService");
    if (endpointIP.empty()) {
        LOG_ERR_EX("[MEMORY] add user memory history failed, kmmService instance empty");
        return;
    }
    std::vector<std::string> indexs = getIndexArray(taskID);
    std::string workflowID;
    if (!indexs.empty()) {
        workflowID = indexs[0];
    }
    std::string taskIdr = "-" + workflowID + "-" + taskID;
    auto http2Client = CMFrm::COM::HttpClientFactory::GetHttpClientFactory()->GetHttp2Client("KMMService");
    auto req = std::make_shared<CMFrm::COM::Http2ClientRequest>();
    req->SetMethod(CMFrm::COM::HttpMethod::POST);
    req->SetReqUrl("https://" + endpointIP + ":27203" + url);
    req->AddHeader("userId", userid);
    req->AddHeader("taskIdr", taskIdr);
    req->AddBody(message.toString());
    req->SetTimeoutSecond(MEMORY_REQ_TIMEOUT);
    LOG_INFO_EX("content : %s", message.toString().c_str());
    http2Client->SendAsync(req, [](std::shared_ptr<CMFrm::COM::Http2ClientResponse> resp, void*) {
        LOG_INFO_EX("remember or forget memory result: %d, %s", resp->GetStatusCode(), resp->GetResponseBody().c_str());
    });
    return;
}

bool buildRecallRequest(std::shared_ptr<CMFrm::COM::Http2ClientRequest> &outReq, const std::string& query,
                        const std::string& userId, int topK)
{
    std::string endpointIP = SYSAGENT::HttpClient::GetServiceIP("KMMService");
    if (endpointIP.empty()) {
        LOG_ERR_EX("endpointIP is null");
        return false;
    }

    outReq->SetMethod(CMFrm::COM::HttpMethod::POST);
    outReq->SetReqUrl("https://" + endpointIP + ":27203" + URL_GET_MEMORIES.data());
    outReq->AddHeader("Content-Type", "application/json;charset=utf-8");
    outReq->AddHeader("userId", userId);

    Json::Value requestText;
    requestText["query"] = query;
    requestText["topK"] = topK;

    Json::StreamWriterBuilder builder;
    builder["indentation"] = "";
    builder["emitUTF8"] = true;
    std::string body = Json::writeString(builder, requestText);
    outReq->AddBody(body);
    outReq->SetTimeoutSecond(60); //  暂定60s

    LOG_INFO_EX("content : %s", body.c_str());
    return true;
}

bool ParseRecallMemoryInfo(const std::string& respBody, std::vector<std::string>& memories)
{
    Json::Value jsonObject;
    if (!JsonParser::Parse(respBody.c_str(), jsonObject) || !JsonParser::IsMember(jsonObject, "RetCode")) {
        LOG_ERR("Parse response to json failed.");
        return false;
    }

    int retCode = jsonObject["RetCode"].asInt();
    if (retCode != CMFrm::COM::HttpStatus::HTTP200 || !JsonParser::IsMember(jsonObject, "data")) {
        LOG_ERR("Parse response to json failed.");
        return false;
    }

    for (auto& memoryBody : jsonObject["data"]) {
        if (JsonParser::IsMember(memoryBody, "content") && memoryBody["content"].isString()) {
            memories.push_back(memoryBody["content"].asString());
        }
    }

    return memories.empty() ? false : true;
}

bool RecallMemoriesSync(std::vector<std::string>& outMemories, const std::string& query, const std::string& userId,
                        int topK)
{
    auto req = std::make_shared<CMFrm::COM::Http2ClientRequest>();
    if (!buildRecallRequest(req, query, userId, topK)) {
        LOG_ERR_EX("buildRecallRequest is failed");
        return false;
    }

    auto http2Client = CMFrm::COM::HttpClientFactory::GetHttpClientFactory()->GetHttp2Client("KMMService");
    auto resp = http2Client->Send(req);
    if (resp == nullptr) {
        LOG_ERR("http request failed");
        return false;
    }

    if (!resp->Is2XXStatusCode()) {
        LOG_ERR("http resp failed:%d", resp->GetStatusCode());
        return false;
    }

    auto bodyStr = resp->GetResponseBody();
    if (bodyStr.empty()) {
        LOG_ERR_EX("response body is null");
        return false;
    }

    auto ret = ParseRecallMemoryInfo(bodyStr, outMemories);
    if (!ret || outMemories.empty()) {
        LOG_ERR_EX("parse memory body failed");
        return false;
    }

    return true;
}

void RecallMemoriesAsync(const std::string& query, const GetMemoriesCallback& callback, const std::string& userId,
                         const std::string& taskID, int topK)
{
    auto req = std::make_shared<CMFrm::COM::Http2ClientRequest>();
    if (!buildRecallRequest(req, query, userId, topK)) {
        LOG_ERR_EX("buildRecallRequest is failed");
        callback(-1, {});
        return;
    }
    std::vector<std::string> indexs = getIndexArray(taskID);
    std::string workflowID;
    if (!indexs.empty()) {
        workflowID = indexs[0];
    }
    std::string taskIdr = "-" + workflowID + "-" + taskID;
    req->AddHeader("taskIdr", taskIdr);
    auto http2Client = CMFrm::COM::HttpClientFactory::GetHttpClientFactory()->GetHttp2Client("KMMService");
    http2Client->SendAsync(req, [callback](std::shared_ptr<CMFrm::COM::Http2ClientResponse> resp, void*) {
        LOG_INFO_EX("recall memory result: %d, %s", resp->GetStatusCode(), resp->GetResponseBody().c_str());
        if (callback == nullptr) {
            LOG_ERR_EX("[MEMORY] call back is null, return");
            return;
        }

        if (!resp->Is2XXStatusCode()) {
            callback(resp->GetStatusCode(), {});
            LOG_ERR_EX("recall memory failed, return");
            return;
        }

        auto bodyStr = resp->GetResponseBody();
        if (bodyStr.empty()) {
            LOG_ERR_EX("response body is null");
            callback(-1, {});
            return;
        }

        std::vector<std::string> memories;
        auto ret = ParseRecallMemoryInfo(bodyStr, memories);
        if (!ret || memories.empty()) {
            LOG_ERR_EX("parse memory body failed");
            callback(-1, {});
            return;
        }

        callback(0, memories); // code = 0 成功
    });
    return;
}

bool buildQueryMemoryRequest(std::shared_ptr<CMFrm::COM::Http2ClientRequest> &outReq, const std::string& senceType,
                             const std::string& userId)
{
    std::string endpointIP = SYSAGENT::HttpClient::GetServiceIP("KMMService");
    if (endpointIP.empty()) {
        LOG_ERR_EX("endpointIP is null");
        return false;
    }

    outReq->SetMethod(CMFrm::COM::HttpMethod::GET);
    outReq->SetReqUrl("https://" + endpointIP + ":27203" + URL_QUERY_MEMORIES.data());
    outReq->AddHeader("userId", userId);
    outReq->AddHeader("type", senceType);

    outReq->SetTimeoutSecond(60); //  暂定60s
    return true;
}

bool QueryMemoryListSync(std::string& bodyStr, const std::string& senceType, const std::string& userId)
{
    auto req = std::make_shared<CMFrm::COM::Http2ClientRequest>();
    if (!buildQueryMemoryRequest(req, senceType, userId)) {
        LOG_ERR_EX("buildRecallRequest is failed");
        return false;
    }

    auto http2Client = CMFrm::COM::HttpClientFactory::GetHttpClientFactory()->GetHttp2Client("KMMService");
    auto resp = http2Client->Send(req);
    if (resp == nullptr) {
        LOG_ERR("http request failed");
        return false;
    }

    if (!resp->Is2XXStatusCode()) {
        LOG_ERR("http resp failed:%d", resp->GetStatusCode());
        return false;
    }

    bodyStr = resp->GetResponseBody();
    return true;
}

bool buildRecallRelevantMemRequest(std::shared_ptr<CMFrm::COM::Http2ClientRequest> &outReq, const std::string& query,
                                   const std::string& userId, int topK)
{
    std::string endpointIP = SYSAGENT::HttpClient::GetServiceIP("KMMService");
    if (endpointIP.empty()) {
        LOG_ERR_EX("endpointIP is null");
        return false;
    }

    outReq->SetMethod(CMFrm::COM::HttpMethod::POST);
    outReq->SetReqUrl("https://" + endpointIP + ":27203" + URL_RECALL_RELEVANT_MEMORIES.data());
    outReq->AddHeader("Content-Type", "application/json;charset=utf-8");
    outReq->AddHeader("userId", userId);

    Json::Value requestText;
    requestText["query"] = query;
    requestText["topK"] = topK;

    Json::StreamWriterBuilder builder;
    builder["indentation"] = "";
    builder["emitUTF8"] = true;
    std::string body = Json::writeString(builder, requestText);
    outReq->AddBody(body);
    outReq->SetTimeoutSecond(60); //  暂定60s

    LOG_INFO_EX("request content : %s", body.c_str());
    return true;
}

void FillUserMemInfo(std::string& field, const Json::Value& memObj, const std::string& key)
{
    if (JsonParser::IsMember(memObj, key) && memObj[key].isString()) {
        field = memObj[key].asString();
    }
}

bool ParseRelevantMemoryInfo(const std::string& respBody, std::vector<UserMemoryInfo>& memories)
{
    Json::Value jsonObject;
    if (!JsonParser::Parse(respBody.c_str(), jsonObject) || !JsonParser::IsMember(jsonObject, "RetCode")) {
        LOG_ERR("Parse response to json failed, body:%s.", respBody.c_str());
        return false;
    }

    int retCode = jsonObject["RetCode"].asInt();
    if (retCode != CMFrm::COM::HttpStatus::HTTP200 || !JsonParser::IsMember(jsonObject, "data")) {
        LOG_ERR("Parse response to json failed.");
        return false;
    }

    auto memData = jsonObject["data"];
    if (!memData.isObject() || !JsonParser::IsMember(memData, "memList") || !memData["memList"].isArray()) {
        LOG_ERR("Parse data memList failed, body:%s.", respBody.c_str());
        return false;
    }

    int index = 0;
    for (auto& memItem : memData["memList"]) {
        if (!memItem.isObject()) {
            LOG_ERR("memList item is not object, body:%s.", respBody.c_str());
            continue;
        }

        UserMemoryInfo memInfo;
        FillUserMemInfo(memInfo.memContent, memItem, "content");
        FillUserMemInfo(memInfo.memDate, memItem, "date");
        FillUserMemInfo(memInfo.memSrc, memItem, "source");
        if (memInfo.memContent.empty()) {
            LOG_ERR("memList item content is empty");
            continue;
        }

        memInfo.index = ++index;
        memories.push_back(memInfo);
    }

    return memories.empty() ? false : true;
}


bool RecallRelevantMemorySync(std::vector<UserMemoryInfo>& outMemories, const std::string& query,
                              const std::string& userId, int topK)
{
    auto req = std::make_shared<CMFrm::COM::Http2ClientRequest>();
    if (!buildRecallRelevantMemRequest(req, query, userId, topK)) {
        LOG_ERR_EX("buildRecallRequest is failed");
        return false;
    }

    auto http2Client = CMFrm::COM::HttpClientFactory::GetHttpClientFactory()->GetHttp2Client("KMMService");
    auto resp = http2Client->Send(req);
    if (resp == nullptr) {
        LOG_ERR("http request failed");
        return false;
    }

    if (!resp->Is2XXStatusCode()) {
        LOG_ERR("http resp failed:%d", resp->GetStatusCode());
        return false;
    }

    auto bodyStr = resp->GetResponseBody();
    if (bodyStr.empty()) {
        LOG_ERR_EX("response body is null");
        return false;
    }

    auto ret = ParseRelevantMemoryInfo(bodyStr, outMemories);
    if (!ret || outMemories.empty()) {
        LOG_ERR_EX("parse relevant memory failed");
        return false;
    }

    return true;
}

bool QueryPersonaInfoSync(std::string& bodyStr, const std::string& userId)
{
    auto req = std::make_shared<CMFrm::COM::Http2ClientRequest>();
    std::string endpointIP = SYSAGENT::HttpClient::GetServiceIP("KMMService");
    if (endpointIP.empty()) {
        LOG_ERR_EX("endpointIP is null");
        return false;
    }

    req->SetMethod(CMFrm::COM::HttpMethod::GET);
    req->SetReqUrl("https://" + endpointIP + ":27203" + URL_PERSONA_QUERY.data());
    req->AddHeader("userId", userId);

    req->SetTimeoutSecond(60); //  暂定60s

    auto http2Client = CMFrm::COM::HttpClientFactory::GetHttpClientFactory()->GetHttp2Client("KMMService");
    auto resp = http2Client->Send(req);
    if (resp == nullptr) {
        LOG_ERR("http request failed");
        return false;
    }

    if (!resp->Is2XXStatusCode()) {
        LOG_ERR("http resp failed:%d", resp->GetStatusCode());
        return false;
    }

    bodyStr = resp->GetResponseBody();
    return true;
}