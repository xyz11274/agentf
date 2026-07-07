#include "memory/memoryMgr.h"

#include <condition_variable>
#include <map>
#include <mutex>
#include <sstream>

#include "aisf_platform/parser/json_parser.h"
#include "aisf_platform/common_macro.h"
#include "memory/History.h"
#include "model/PromptManager.h"
#include "utils/env_util.h"
#include "utils/logger.h"

using namespace SysAgent;
using namespace AisfPlat;
using namespace mmsdk;

static const std::string USERID = "userId";
static const std::string SCENE_TYPE = "type";

MemoryMgr::MemoryMgr()
{
    m_threadPool = std::make_shared<CMFrm::ThreadPool::ThreadPool>(NUMBER_TEN);

    m_procMap.emplace(RequestType::RECALL_MEMORY, &OnRecvMemoryRecall);
    m_procMap.emplace(RequestType::QUERY_MEMORY, &OnRecvMemoryQuery);
    m_procMap.emplace(RequestType::QUERY_PERSONA, &OnRecvPersonaQuery);
}

void WriteResponseHttpErr(const std::shared_ptr<CMFrm::ServiceRouter::Context> &context, const std::string &content)
{
    context->WriteStatueCode(CMFrm::COM::HttpStatus::HTTP400);
    context->WriteResponseBody(content);
    context->WriteAsyncResponse();
}

void WriteResponseErr(const std::shared_ptr<CMFrm::ServiceRouter::Context> &context, int retCode,
                      const std::string &errMsg)
{
    std::ostringstream oss;
    oss << R"({"RetCode": )" << retCode << R"(, "RetMsg": ")" << errMsg << R"("})";

    context->WriteStatueCode(CMFrm::COM::HttpStatus::HTTP200);
    context->WriteJSONContentType();
    context->WriteResponseBody(oss.str());
    context->WriteAsyncResponse();
}

void WriteResponseSucc(const std::shared_ptr<CMFrm::ServiceRouter::Context> &context, const std::string &body)
{
    context->WriteStatueCode(CMFrm::COM::HttpStatus::HTTP200);
    context->WriteJSONContentType();
    context->WriteResponseBody(body);
    context->WriteAsyncResponse();
}

void MemoryMgr::OnRecvRestRequestAsync(RequestType reqType,
                                       const std::shared_ptr<CMFrm::ServiceRouter::Context>& context)
{
    // 通过type找处理函数，使用线程池方式
    const auto &it = m_procMap.find(reqType);
    if (m_procMap.find(reqType) == m_procMap.end()) {
        LOG_ERR_EX("unknown reqType: %d", static_cast<int>(reqType));
        WriteResponseHttpErr(context, "unknown request");
        return;
    }

    auto handler = it->second;
    // 线程池异步操作
    m_threadPool->Submit([handler, context]() {
        try {
            handler(context);
        } catch (const std::exception& e) {
            LOG_ERR_EX("Exception in request handler: %s", e.what());
            WriteResponseHttpErr(context, "internal server error");
        } catch (...) {
            LOG_ERR_EX("Unknown exception in request handler");
            WriteResponseHttpErr(context, "internal server error");
        }
    });
}

bool MemoryMgr::ParseMemRequestParam(const std::shared_ptr<CMFrm::ServiceRouter::Context>& context,
                                     MemoryReqParam& param)
{
    param.userId = context->GetHeader(USERID);
    if (param.userId.empty()) {
        LOG_ERR_EX("userId is empty");
        return false;
    }

    param.sceneType = context->GetHeader(SCENE_TYPE);
    param.body = context->GetRequestBody();

    LOG_INFO("request header: userId:%s, sceneType:%s", param.userId.c_str(), param.sceneType.c_str());

    if (param.body.empty()) {
        return true;
    }

    // 如果有请求body，尝试解一层字段
    Json::Value jsonObject;
    if (!JsonParser::Parse(param.body.c_str(), jsonObject)) {
        return true;
    }

    JsonParser::GetString(jsonObject, "query", param.query);
    if (!JsonParser::GetInt(jsonObject, "topK", param.topK)) {
        param.topK = 1;
    }
    if (!JsonParser::GetInt(jsonObject, "needMemList", param.needMemList)) {
        param.needMemList = 0;
    }

    LOG_INFO("request body: query:%s, topK:%d, needMemList:%d", param.query.c_str(), param.topK, param.needMemList);
    return true;
}

bool RecallUserMemory(std::string& rspMemory, MemoryReqParam& reqParam)
{
    std::vector<std::string> memories;
    auto ret = RecallMemoriesSync(memories, reqParam.query, reqParam.userId, reqParam.topK);
    // 当前只返回一个总结后的结果
    if (!ret || memories.empty()) {
        return false;
    }

    rspMemory = memories[0];
    return true;
}

std::string BuildRecallMemRspBody(int retCode, const std::string &retMsg, const std::string &queryRet,
    const std::vector<UserMemoryInfo>& memList, const std::vector<MemoryRelativeInfo>& relateMemList)
{
    Json::Value root;
    root["RetCode"] = retCode;
    root["RetMsg"] = retMsg;

    Json::Value data;
    data["queryRet"] = queryRet;

    Json::Value memArray(Json::arrayValue);
    for (const auto& it : memList) {
        Json::Value memObj;
        memObj["index"] = it.index;
        memObj["content"] = it.memContent;
        memObj["source"] = it.memSrc;
        memObj["date"] = it.memDate;
        memArray.append(memObj);
    }
    if (!memArray.empty()) {
        data["memList"] = memArray;
    }

    Json::Value relativeArray(Json::arrayValue);
    for (const auto& it : relateMemList) {
        Json::Value relativeObj;
        relativeObj["highlight"] = it.keyWord;
        relativeObj["memoryIndex"] = it.index;
        relativeArray.append(relativeObj);
    }
    if (!relativeArray.empty()) {
        data["relativeMap"] = relativeArray;
    }

    root["data"] = data;

    Json::StreamWriterBuilder writer;
    writer["indentation"] = "";
    writer["emitUTF8"] = true;
    return Json::writeString(writer, root);
}

std::string BuildExtractMemoryRelationQuery(std::string& content, std::vector<UserMemoryInfo>& memList)
{
    Json::Value memArray(Json::arrayValue);
    for (const auto& it : memList) {
        Json::Value memObj;
        memObj["index"] = it.index;
        memObj["content"] = it.memContent;
        memArray.append(memObj);
    }
    Json::StreamWriterBuilder writer;
    std::string memBody = Json::writeString(writer, memArray);

    // 构建模型query
    std::ostringstream oss;
    oss << "原始内容:" << "\n" << content << "\n";
    oss << "可能相关的记忆或个人画像信息:" << "\n" << memBody;
    oss << "\n" << "/nothink"; // 不带思考

    return oss.str();
}

std::string LLMModelInferSync(const std::string &query, const std::string &promptName,
                              const std::string modelName, const std::string& modelServiceName)
{
    MMReasonParam reasonParam = {.modelServiceName = modelServiceName, .modelName = modelName,
                                 .temperature = 0.001, .maxToken = 1024
    };
    auto intention = MMUserIntention{.query = query, .modaltype = MMLLM_MODAL_TYPE_TEXT};
    auto req = MMLLMRequest{.intentions = {intention},
                            .prompt = {PromptManager::GetInstance().GetPormptTmpl(promptName)}}; // 关联萃取
    MMLlmComponent llmComponent = MMLlmComponent();

    std::mutex mutex;
    std::unique_lock<std::mutex> lock(mutex);
    std::condition_variable cond;
    std::atomic<bool> done{false};
    std::string queryRsp;
    int32_t ret = -1;
    llmComponent.RequestAsync(reasonParam, req, [&](int32_t errCode, const std::string &response) -> uint32_t {
        try {
            ret = errCode;
            queryRsp = response;
            done.store(true);
            cond.notify_one();
        } catch (...) {
            LOG_ERR("llmComponent RequestAsync exception.");
            ret = -1;
        }
        return 0;
    });
    cond.wait_for(lock, std::chrono::seconds(NUMBER_SIXTY), [&] { return done.load(); });
    if (ret != 0) {
        LOG_ERR("llmComponent.RequestAsync failed, ret=%u", ret);
        return "";
    }

    return queryRsp;
}

std::string FilterOutStr(const std::string &input, char startKey, char endKey)
{
    size_t start = input.find(startKey);
    if (start == std::string::npos) {
        LOG_INFO("input.find failed: %s", input.c_str());
        return "";
    }

    int braceCount = 0;
    size_t end = start;

    for (size_t i = start; i < input.size(); ++i) {
        if (input[i] == startKey) {
            ++braceCount;
        } else if (input[i] == endKey) {
            --braceCount;
            if (braceCount == 0) {
                end = i;
                break;
            }
        }
    }

    if (braceCount == 0) {
        return input.substr(start, end - start + 1);
    } else {
        LOG_INFO("cannot find endKey: %s", input.c_str());
        return "";
    }
}

// 萃取记忆与内容的关联关系
bool ExtractMemoryRelationship(std::string& content, std::vector<UserMemoryInfo>& memList,
    std::vector<MemoryRelativeInfo>& outRelateList)
{
    if (content.empty() || memList.empty()) {
        LOG_ERR_EX("input param err, content:%s, memList size:%d", content.c_str(), static_cast<int>(memList.size()));
        return false;
    }

    auto query = BuildExtractMemoryRelationQuery(content, memList);
    std::string modelName = ConfigManager::getInstance().getEnvInfo("SUMM_MODEL_NAME", "Qwen3-32B");
    std::string modelServiceName = ConfigManager::getInstance().getEnvInfo("SUMM_MODEL_SERVICE_NAME", "llmlargeservice");
    auto response = LLMModelInferSync(query, "extract_memory_relevance", modelName, modelServiceName);
    if (response.empty()) {
        LOG_ERR_EX("extract memory relevance invoke model failed");
        return false;
    }

    // 结果解析
    auto outStr = FilterOutStr(response, '[', ']');
    if (outStr.empty()) {
        LOG_ERR_EX("rsp data is unexpect, rsp:%s", response.c_str());
        return false;
    }

    Json::Value jsonObject;
    if (!JsonParser::Parse(outStr.c_str(), jsonObject) || !jsonObject.isArray()) {
        LOG_ERR("Parse response to json failed, body:%s", outStr.c_str());
        return false;
    }

    // 去重使用
    std::map<std::string, int> relateMap;

    for (auto& item : jsonObject) {
        if (!item.isObject()) {
            continue;
        }

        if (!JsonParser::IsMember(item, "index") || !item["index"].isInt() ||
            !JsonParser::IsMember(item, "keyword") || !item["keyword"].isString()) {
            LOG_ERR("unexpect item date.");
            continue;
        }

        relateMap[item["keyword"].asString()] = item["index"].asInt();
    }

    for (const auto& it : relateMap) {
        MemoryRelativeInfo info;
        info.keyWord = it.first;
        info.index = it.second;
        outRelateList.push_back(info);
    }

    return true;
}

void MemoryMgr::OnRecvMemoryRecall(const std::shared_ptr<CMFrm::ServiceRouter::Context>& context)
{
    MemoryReqParam reqParam = {};
    if (!ParseMemRequestParam(context, reqParam)) {
        LOG_ERR_EX("parse req param failed");
        WriteResponseErr(context, CMFrm::COM::HttpStatus::HTTP400, "parse req param failed");
        return;
    }

    LOG_ERR("whl_test rest recall memory, userId:%s, body:%s", reqParam.userId.c_str(), reqParam.body.c_str());
    if (reqParam.query.empty()) {
        WriteResponseErr(context, CMFrm::COM::HttpStatus::HTTP400, "parse user query is failed");
        return;
    }

    // 召回记忆
    std::string queryRet;
    bool ret = RecallUserMemory(queryRet, reqParam);
    if (!ret || queryRet.empty()) {
        WriteResponseErr(context, CMFrm::COM::HttpStatus::HTTP400, "recall user memory failed");
        return;
    }

    // 不需要相关相关记忆信息
    if (reqParam.needMemList != 1) {
        auto body = BuildRecallMemRspBody(CMFrm::COM::HttpStatus::HTTP200, "query success", queryRet, {}, {});
        WriteResponseSucc(context, body);
        return;
    }

    // 查询相关记忆列表
    std::vector<UserMemoryInfo> memList;
    ret = RecallRelevantMemorySync(memList, reqParam.query, reqParam.userId, 5); // topK 模型5， 可能不需要
    // 如果没有关联记忆，只返回记忆召回结果
    if (!ret || memList.empty()) {
        LOG_INFO("query relevant memory failed");
        auto body = BuildRecallMemRspBody(CMFrm::COM::HttpStatus::HTTP200, "query success", queryRet, {}, {});
        WriteResponseSucc(context, body);
        return;
    }

    // 模型调用，生成关联信息
    std::vector<MemoryRelativeInfo> relateList;
    ExtractMemoryRelationship(queryRet, memList, relateList);

    auto rspBody = BuildRecallMemRspBody(CMFrm::COM::HttpStatus::HTTP200, "query success",
                                         queryRet, memList, relateList);
    WriteResponseSucc(context, rspBody);
}

void MemoryMgr::OnRecvMemoryQuery(const std::shared_ptr<CMFrm::ServiceRouter::Context>& context)
{
    MemoryReqParam reqParam;
    if (!ParseMemRequestParam(context, reqParam)) {
        LOG_ERR_EX("parse req param failed");
        WriteResponseErr(context, CMFrm::COM::HttpStatus::HTTP400, "parse req param failed");
        return;
    }

    if (reqParam.sceneType.empty()) {
        LOG_ERR_EX("userId(%s) scene type is empty", reqParam.userId.c_str());
        WriteResponseErr(context, CMFrm::COM::HttpStatus::HTTP400, "userId or type is null");
        return;
    }

    LOG_ERR("whl_test rest query memory list, userId:%s, type:%s", reqParam.userId.c_str(), reqParam.sceneType.c_str());

    // 调用查询记忆接口
    std::string respBody;
    if (!QueryMemoryListSync(respBody, reqParam.sceneType, reqParam.userId)) {
        LOG_ERR("query memory list from kmm failed");
        WriteResponseErr(context, CMFrm::COM::HttpStatus::HTTP400, "query memory list from kmm failed");
        return;
    }

    // 直接透传body
    WriteResponseSucc(context, respBody);
    return;
}

void MemoryMgr::OnRecvPersonaQuery(const std::shared_ptr<CMFrm::ServiceRouter::Context>& context)
{
    MemoryReqParam reqParam;
    if (!ParseMemRequestParam(context, reqParam)) {
        LOG_ERR_EX("parse req param failed");
        WriteResponseErr(context, CMFrm::COM::HttpStatus::HTTP400, "parse req param failed");
        return;
    }

    LOG_ERR("whl_test rest query persona list, userId:%s, type:%s",
            reqParam.userId.c_str(), reqParam.sceneType.c_str());

    // 调用画像查询接口
    std::string respBody;
    if (!QueryPersonaInfoSync(respBody, reqParam.userId)) {
        LOG_ERR("query memory list from kmm failed");
        WriteResponseErr(context, CMFrm::COM::HttpStatus::HTTP400, "query persona info from kmm failed");
        return;
    }

    // 直接透传body
    WriteResponseSucc(context, respBody);
    return;
}

bool MemoryMgr::GetLongMemories(const std::string &userName, const std::string &query, int topK,
                                const LongMemoriesCallback &callback)
{
    std::string endpointIP = SYSAGENT::HttpClient::GetServiceIP("KMMService");
    std::string url = "https://" + endpointIP + ":27203" + URL_GET_MEMORIES.data();
    std::map<std::string, std::string> headers{};
    headers.insert(std::make_pair("userId", userName));

    Json::Value root;
    root["query"] = query;
    root["topK"] = topK;
    std::string body = JsonParser::ToRawString(root);

    auto retCode = SYSAGENT::HttpClient::Http2SendRequest(
            url, body, headers,
            [callback](const std::shared_ptr<CMFrm::COM::Http2ClientResponse>& http2Response, void* arg) {
                if (!http2Response) {
                    callback(false, {});
                    return;
                }

                std::string bodyString = http2Response->GetResponseBody();
                LOG_INFO_EX("recall long memory is:%s", bodyString.c_str());
                if (!http2Response->Is2XXStatusCode()) {
                    CMFrm_Error("Received response failed, status code: %d, response body: %s.",
                                http2Response->GetStatusCode(), bodyString.c_str());
                    callback(false, {});
                    return;
                }
                CMFrm_Info("Received response success, body: %s", bodyString.c_str());

                Json::Value root;
                if (!JsonParser::Parse(bodyString.c_str(), root) || !JsonParser::IsMember(root, "data")) {
                    CMFrm_Error("Failed to parser json, or no data member.");
                    callback(false, {});
                    return;
                }
                std::vector<std::string> memories{};
                for (auto& item : root["data"]) {
                    std::string memory;
                    JsonParser::GetString(item, "content", memory);
                    if (!memory.empty()) {
                        memories.push_back(memory);
                    }
                }
                callback(true, memories);
            }, 8);
    if (!retCode) {
        callback(false, {});
    }
    return retCode;
}

bool MemoryMgr::GetShortMemories(const std::string &userName, const std::string &taskId, int count,
                                 const ShortMemoriesCallback &callback)
{
    std::string endpointIP = SYSAGENT::HttpClient::GetServiceIP("KMMService");
    std::string url = "https://" + endpointIP + ":27203" + URL_GET_SHORT_MEMORIES.data();
    std::map<std::string, std::string> headers{};
    headers.insert(std::make_pair("userId", userName));
    headers.insert(std::make_pair("taskIdr", taskId));

    Json::Value root;
    root["limit"] = count;
    std::string body = JsonParser::ToRawString(root);

    auto retCode = SYSAGENT::HttpClient::Http2SendRequest(
            url, body, headers,
            [callback](const std::shared_ptr<CMFrm::COM::Http2ClientResponse>& http2Response, void* arg) {
                if (!http2Response) {
                    callback(false, {});
                    return;
                }

                std::string bodyString = http2Response->GetResponseBody();
                LOG_INFO_EX("recall short memory is:%s", bodyString.c_str());
                if (!http2Response->Is2XXStatusCode()) {
                    CMFrm_Error("Received response failed, status code: %d, response body: %s.",
                                http2Response->GetStatusCode(), bodyString.c_str());
                    callback(false, {});
                    return;
                }
                CMFrm_Info("Received response success, body: %s", bodyString.c_str());

                Json::Value root;
                if (!JsonParser::Parse(bodyString.c_str(), root) || !JsonParser::IsMember(root, "data")) {
                    CMFrm_Error("Failed to parser json, or no data member.");
                    callback(false, {});
                    return;
                }
                std::vector<std::map<std::string, std::string >> memories{};
                for (auto& item : root["data"]) {
                    if (item.isMember("originalQA") && item["originalQA"].isArray()) {
                        for (auto& originalQA : item["originalQA"]) {
                            std::map<std::string, std::string> qaPair;
                            JsonParser::GetString(originalQA, "role", qaPair["role"]);
                            JsonParser::GetString(originalQA, "content", qaPair["content"]);
                            memories.push_back(qaPair);
                        }
                    }
                }
                callback(true, memories);
            }, 5);
    if (!retCode) {
        callback(false, {});
    }
    return retCode;
}

bool MemoryMgr::AddShortMemories(const std::string &taskIdr, const std::string &opType,
                                 std::shared_ptr<QAObject> qaObject, const OperateMemoriesCallbback& callback)
{
    std::string endpointIP = SYSAGENT::HttpClient::GetServiceIP("KMMService");
    std::string url = "https://" + endpointIP + ":27203" + URL_ADD_SHORT_MEMORIES.data();
    std::map<std::string, std::string> headers{};
    headers.insert(std::make_pair("taskIdr", taskIdr));
    headers.insert(std::make_pair("userId", qaObject->userId));

    Json::Value agingPolicy;
    agingPolicy["maxRecords"] = 10;
    agingPolicy["agingTime"] = 30;
    Json::Value bodyJson;
    bodyJson["content"] = qaObject->ToJson();
    bodyJson["agingPolicy"] = agingPolicy;
    bodyJson["opType"] = opType;
    bodyJson["id"] = qaObject->id;

    std::string body = JsonParser::ToRawString(bodyJson);
    auto retCode = SYSAGENT::HttpClient::Http2SendRequest(
            url, body, headers,
            [qaObject, callback](const std::shared_ptr<CMFrm::COM::Http2ClientResponse>& http2Response, void* arg) {
                if (!http2Response) {
                    CMFrm_Error("insert short memory failed!");
                    callback(false, "");
                    return;
                }
                std::string bodyString = http2Response->GetResponseBody();
                if (!http2Response->Is2XXStatusCode()) {
                    CMFrm_Error("Received response failed, status code: %d, response body: %s.",
                                http2Response->GetStatusCode(), bodyString.c_str());
                    CMFrm_Error("insert short memory failed!");
                    callback(false, "");
                    return;
                }
                CMFrm_Info("insert short memor, body: %s", bodyString.c_str());
                callback(true, bodyString);
            }, 8);
    if (!retCode) {
        CMFrm_Error("insert short memory failed, retcode is :%d", retCode);
    }
    return retCode;
}

// 获取远程服务数据
Json::Value MemoryMgr::GetRemoteMemories(const std::string& userName, const std::string& taskID, int count)
{
    std::string endpointIP = SYSAGENT::HttpClient::GetServiceIP("KMMService");
    std::string url = "https://" + endpointIP + ":27203" + URL_GET_SHORT_MEMORIES.data();
    Json::Value root;
    root["limit"] = count;
    std::string response = "";
    auto request = std::make_shared<CMFrm::COM::Http2ClientRequest>();
    request->SetMethod(CMFrm::COM::POST);
    request->SetReqUrl(url);
    request->AddBody(JsonParser::ToRawString(root));
    request->AddJSONContentTypeHeader();
    request->SetTimeoutSecond(CMFrm::Constants::Const8);
    request->AddHeader("userId", userName);
    request->AddHeader("taskIdr", taskID);
    auto isSuccess = HttpClient::Htt2SendRequest(request, response);
    if (!isSuccess || !JsonParser::Parse(response.c_str(), root) || !JsonParser::IsMember(root, "data")) {
        return Json::Value::null;
    }
    return root["data"];
}
