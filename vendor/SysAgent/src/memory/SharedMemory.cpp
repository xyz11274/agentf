//
// Created by c00888021 on 2025/11/15.
//
#include "memory/SharedMemory.h"
#include "memory/History.h"
#include "utils/http_client.h"
#include "utils/logger.h"
#include "http_client_factory.h"
#include "model/ReactLLMTask.h"
#include "model/PromptManager.h"
#include "config/Config.h"
#include "utils/env_util.h"
#include <vector>
#include <map>
#include <sstream>
#include <future>
#include <algorithm>

#include "utils/env_util.h"
uint32_t SharedMemory::totalSize_ = 20;
uint32_t SharedMemory::summarySize_ = 3;
uint32_t SharedMemory::roundSize_ = 2;

std::unordered_map<std::string, std::string> SharedMemory::BuildQueryFilter(ShortMemory filter)
{
    std::unordered_map<std::string, std::string> result;

    AddFilterField(result, filter, "userName", &ShortMemory::userName);
    AddFilterField(result, filter, "workFlowId", &ShortMemory::workFlowId);
    AddFilterField(result, filter, "taskId", &ShortMemory::taskId);
    AddFilterField(result, filter, "role", &ShortMemory::role);
    AddFilterField(result, filter, "createTime", &ShortMemory::createTime);
    AddFilterField(result, filter, "source", &ShortMemory::source);

    return result;
}

void SharedMemory::ConvertToVector(Json::Value& data, std::vector<ShortMemory>& context)
{
    if (data.isMember("data") && data["data"].isArray()) {
        for (auto &item: data["data"]) {
            ShortMemory shortMemory{};
            JsonParser::GetString(item, "userName", shortMemory.userName);
            JsonParser::GetString(item, "workFlowId", shortMemory.workFlowId);
            JsonParser::GetString(item, "taskId", shortMemory.taskId);
            JsonParser::GetString(item, "createTime", shortMemory.createTime);
            JsonParser::GetString(item, "source", shortMemory.source);
            JsonParser::GetString(item, "role", shortMemory.role);
            JsonParser::GetString(item, "content", shortMemory.content);
            context.push_back(shortMemory);
        }
    }
}

void SharedMemory::ConvertToJSON(const std::vector<ShortMemory>& context, Json::Value& dataArray)
{
    CHECK_TRUE_LOG_ERROR_RETURN_VOID(context.empty(), "context is empty!");
    for (const auto& memory : context) {
        Json::Value item;
        item["userName"] = memory.userName;
        item["workFlowId"] = memory.workFlowId;
        item["taskId"] = memory.taskId;
        item["createTime"] = memory.createTime;
        item["source"] = memory.source;
        item["role"] = memory.role;
        item["content"] = memory.content;
        dataArray.append(item);  // 添加到数组中
    }
}

void SharedMemory::AddSharedMemory(std::vector<ShortMemory>& context, std::string& taskID)
{
    std::string endpointIP = SYSAGENT::HttpClient::GetServiceIP("KMMService");
    if (endpointIP.empty()) {
        LOG_ERR_EX("[MEMORY] get kmmService instance empty");
        return;
    }
    std::vector<std::string> indexs = getIndexArray(taskID);
    std::string workflowID;
    if (!indexs.empty()) {
        workflowID = indexs[0];
    }
    std::string taskIdr = "-" + workflowID + "-" + taskID;
    Json::Value bodyJson;
    bodyJson["opType"] = "add";
    Json::Value data(Json::arrayValue);
    ConvertToJSON(context, data);
    bodyJson["data"] = data;
    std::string body = JsonParser::ToRawString(bodyJson);
    LOG_INFO_EX("body is:%s", body.c_str());
    auto http2Client = CMFrm::COM::HttpClientFactory::GetHttpClientFactory()->GetHttp2Client("KMMService");
    auto req = std::make_shared<CMFrm::COM::Http2ClientRequest>();
    req->AddHeader("taskIdr", taskIdr);
    req->SetMethod(CMFrm::COM::HttpMethod::POST);
    req->SetReqUrl("https://" + endpointIP + ":27203" + URL_SHARED_MEMORY_MANAGER.data());
    req->SetBody(body);
    LOG_INFO_EX("request url is:%s, request body is:%s", req->GetReqUrl().c_str(), body.c_str());
    req->SetTimeoutSecond(NUMBER_TEN);
    http2Client->SendAsync(req, [&](std::shared_ptr<CMFrm::COM::Http2ClientResponse> resp, void*) {
        LOG_INFO_EX("query result: %d, %s", resp->GetStatusCode(), resp->GetResponseBody().c_str());
    });
}

void SharedMemory::QuerySharedMemory(std::vector<ShortMemory>& context, ShortMemory shortMemory)
{
    std::string endpointIP = SYSAGENT::HttpClient::GetServiceIP("KMMService");
    if (endpointIP.empty()) {
        LOG_ERR_EX("[MEMORY] get kmmService instance empty");
        return;
    }
    std::string taskIdr = "-" + shortMemory.workFlowId + "-" + shortMemory.taskId;
    LOG_INFO_EX("short memory userName is:%s", shortMemory.userName.c_str());
    auto filterMap = BuildQueryFilter(shortMemory);
    Json::Value bodyJson;
    bodyJson["opType"] = "get";
    Json::Value filter;
    for (const auto& item : filterMap) {
        filter[item.first] = item.second;
    }
    bodyJson["filter"] = filter;
    std::string body = JsonParser::ToRawString(bodyJson);
    LOG_INFO_EX("body is:%s", body.c_str());
    auto http2Client = CMFrm::COM::HttpClientFactory::GetHttpClientFactory()->GetHttp2Client("KMMService");
    auto req = std::make_shared<CMFrm::COM::Http2ClientRequest>();
    req->AddHeader("taskIdr", taskIdr);
    req->SetMethod(CMFrm::COM::HttpMethod::POST);
    req->SetReqUrl("https://" + endpointIP + ":27203" + URL_SHARED_MEMORY_MANAGER.data());
    req->SetBody(body);
    LOG_INFO_EX("request url is:%s, request body is:%s", req->GetReqUrl().c_str(), body.c_str());
    req->SetTimeoutSecond(NUMBER_TEN);
    std::mutex mutex;
    std::unique_lock<std::mutex> lock(mutex);
    std::condition_variable condition;
    std::atomic<bool> done{false};
    http2Client->SendAsync(req, [&](std::shared_ptr<CMFrm::COM::Http2ClientResponse> resp, void*) {
        LOG_INFO_EX("query result: %d", resp->GetStatusCode());
        if (resp->GetStatusCode() == CMFrm::COM::HTTP200) {
            Json::Value data;
            JsonParser::Parse(resp->GetResponseBody(), data);
            int retCode = 0;
            JsonParser::GetInt(data, "RetCode", retCode);
            if (retCode == CMFrm::COM::HTTP200) {
                ConvertToVector(data, context);
            }
            done.store(true);
            condition.notify_one();
        }
    });
    condition.wait_for(lock, std::chrono::seconds(NUMBER_TEN), [&done] { return done.load(); });
}

void SharedMemory::RefreshSharedMemory(std::vector<ShortMemory>& context, ShortMemory shortMemory)
{
    std::string endpointIP = SYSAGENT::HttpClient::GetServiceIP("KMMService");
    if (endpointIP.empty() || context.empty()) {
        LOG_ERR_EX("[MEMORY] get kmmService instance empty or context is empty");
        return;
    }
    LOG_INFO_EX("refresh memory size is:%d, userName is:%s", context.size(), shortMemory.userName.c_str());
    auto filterMap = BuildQueryFilter(shortMemory);
    Json::Value bodyJson;
    bodyJson["opType"] = "refresh";
    Json::Value filter;
    for (const auto& item : filterMap) {
        filter[item.first] = item.second;
    }
    std::string taskIdr = "-" + shortMemory.workFlowId + "-" + shortMemory.taskId;
    bodyJson["filter"] = filter;
    Json::Value data(Json::arrayValue);
    ConvertToJSON(context, data);
    bodyJson["data"] = data;
    std::string body = JsonParser::ToRawString(bodyJson);
    auto http2Client = CMFrm::COM::HttpClientFactory::GetHttpClientFactory()->GetHttp2Client("KMMService");
    auto req = std::make_shared<CMFrm::COM::Http2ClientRequest>();
    req->AddHeader("taskIdr", taskIdr);
    req->SetMethod(CMFrm::COM::HttpMethod::POST);
    req->SetReqUrl("https://" + endpointIP + ":27203" + URL_SHARED_MEMORY_MANAGER.data());
    req->SetBody(body);
    req->SetTimeoutSecond(NUMBER_TEN);
    http2Client->SendAsync(req, [&](std::shared_ptr<CMFrm::COM::Http2ClientResponse> resp, void*) {
        LOG_INFO_EX("query result: %d, %s", resp->GetStatusCode(), resp->GetResponseBody().c_str());
    });
}

void SharedMemory::GetContext(std::vector<std::map<std::string, std::string>>& context, ShortMemory shortMemory)
{
    std::vector<ShortMemory> memoryList;
    QuerySharedMemory(memoryList, shortMemory);
    for (auto &item : memoryList) {
        std::map<std::string, std::string> memory;
        if ((item.role == "user" || item.role == "assistant") && !item.content.empty()) {
            memory["role"] = item.role;
            memory["content"] = item.content;
            context.push_back(memory);
        }
    }
}

std::vector<ShortMemory> SharedMemory::ParseJsonToShortMemory(const std::string& json, ShortMemory shortMemory)
{
    Json::Value root;
    std::vector<ShortMemory> memoryList;
    JsonParser::Parse(json, root);
    int idx = 0;
    if (root && root.isArray()) {
        for (auto &item: root) {
            ShortMemory newMemory;
            JsonParser::GetString(item, "role", newMemory.role);
            JsonParser::GetString(item, "content", newMemory.content);
            newMemory.userName = shortMemory.userName;
            newMemory.workFlowId = shortMemory.workFlowId;
            newMemory.taskId = shortMemory.taskId;
            newMemory.createTime = shortMemory.createTime + "." + std::to_string(idx ++);
            newMemory.source = "SystemAgent";
            memoryList.push_back(newMemory);
        }
    }
    return memoryList;
}

std::vector<uint32_t> SharedMemory::GetSharedMemoryQuantity(std::shared_ptr<std::vector<ShortMemory>> memory)
{
    if (memory == nullptr || memory->empty()) {
        return std::vector<uint32_t>{0, 0};
    }
    uint32_t sysCount = 0;
    uint32_t originCount = 0;
    for (const auto& item : *memory) {
        if (item.role != "user") {
            continue;
        }
        if (item.source == "SystemAgent") {
            sysCount += summarySize_;
        } else {
            sysCount += 1;
            originCount += 1;
        }
    }
    while (sysCount > SharedMemory::totalSize_) {
        memory->erase(memory->begin(), memory->begin() + roundSize_);
        sysCount -= SharedMemory::summarySize_;
    }
    return std::vector<uint32_t>{sysCount, originCount};
}

bool SharedMemory::CheckContextSize(std::shared_ptr<std::vector<ShortMemory>> memoryList)
{
    std::string contextThresholdStr = ConfigManager::getInstance().getEnvInfo("SUMMARY_SIZE_THRESHOLD", "10000");
    // 一个汉字占用3个字节
    uint32_t contextThreshold = static_cast<uint32_t>(std::stoi(contextThresholdStr));
    if (!memoryList->empty() && contextThreshold > 0) {
        uint32_t contextSize = 0;
        for (auto &item : *memoryList) {
            if (!item.content.empty()) {
                contextSize += item.content.size();
            }
        }
        LOG_INFO_EX("check context size, content size is:%d, threshold is:%d", contextSize, contextThreshold);
        // 丢弃最早的总结信息
        if (contextSize >= contextThreshold && memoryList->size() > roundSize_ &&
            memoryList->at(0).source == "SystemAgent") {
            LOG_INFO_EX("delete the longest summary context!");
            memoryList->erase(memoryList->begin(), memoryList->begin() + roundSize_);
            return true;
        }
    }
    return false;
}

std::vector<std::string> SharedMemory::getIndexArray(const std::string& taskID)
{
    std::vector<std::string> result;
    size_t start = 0;
    size_t end = 0;
    while ((end = taskID.find('_', start)) != std::string::npos) {
        result.push_back(taskID.substr(start, end - start));
        start = end + 1;
    }
    result.push_back(taskID.substr(start));
    return result;
}
