/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
 */
//
// Created by l00909985 on 2025/11/14.
//

#include "securityFence/SecurityFence.h"
#include "securityFence/SecFenceMessage.h"
#include "aisf_platform/common_define.h"
#include "utils/http_client.h"
#include "utils/env_util.h"
#include "http_client_factory.h"
#include "TeHelper.h"
#include <aisf_platform/parser/json_parser.h>
#include "framework_om/service_router/router_manager.h"

#include <vector>
#include <cstdlib>
#include <ctime>
#include <climits>
#include <shared_mutex>

std::shared_mutex securityFenceFlagMutex;
bool securityFenceFlag = false;

std::string buildFrontFenceBody(const std::shared_ptr<SYSAGENT::AccessCtrl>& accessCtrl)
{
    string taskID = accessCtrl->header.taskID;
    auto userInfo = TEHelper::extractUserInfo(accessCtrl);
    std::string userID = userInfo.userName + "_" + userInfo.deviceCategory;
    std::shared_ptr<TeamEnvironment> te = TEManager::getInstance().getTEById(accessCtrl->header.teamEnvironmentID);
    unsigned int businessId = generateBusinessId();
    CHECK_NULL_LOG_ERROR_RETURN(te, "te is nullptr", "");
    te->fenceBusinessId = businessId;
    te->rearFenceSeqNum = 1;
    te->frontFenceLevel = FENCE_NO_RISK_PASS;

    Json::Value root;
    root["text"] = accessCtrl->extractInput();
    root["sessionId"] = userID;
    root["requestType"] = TYPE_FRONT_FENCE;
    root["businessId"] = businessId;
    return JsonParser::ToRawString(root);
}

std::string buildRearFenceBody(const std::shared_ptr<SYSAGENT::AccessCtrl>& accessCtrl)
{
    string taskID = accessCtrl->header.taskID;
    auto userInfo = TEHelper::extractUserInfo(accessCtrl);
    std::string userID = userInfo.userName + "_" + userInfo.deviceCategory;
    std::shared_ptr<TeamEnvironment> te = TEManager::getInstance().getTEById(accessCtrl->header.teamEnvironmentID);
    CHECK_TRUE_LOG_ERROR_RETURN(!te, "te is empty", "");
    Json::Value root;
    root["sessionId"] = userID;
    root["text"] = accessCtrl->userOutput.data.userOutputText;
    root["requestType"] = TYPE_REAR_FENCE;
    root["status"] = te->rearFenceSeqNum == 1 ? FENCE_START_STATUS : FENCE_MIDDLE_STATUS;
    root["seqNumber"] = te->rearFenceSeqNum++;
    root["businessId"] = te->fenceBusinessId;
    return JsonParser::ToRawString(root);
}

int getFrontFenceDetection(const std::shared_ptr<SYSAGENT::AccessCtrl>& accessCtrl)
{
    if (!securityFenceFlag) {
        LOG_INFO_EX("[MMGuardService]Security fence is currently disabled");
        return FENCE_NO_RISK_PASS;
    }
    const std::string endpointIP = getSecFenceServiceEndPoint();
    if (endpointIP.empty()) {
        LOG_ERR_EX("[MMGuardService]Send front fence failed, MMGuardservice instance is empty");
        return FENCE_NO_RISK_PASS;
    }
    LOG_INFO_EX("[MMGuardService]Endpoint: %s, fence_url: %s", endpointIP.c_str(), security_fence_url.data());
    auto req = std::make_shared<CMFrm::COM::Http2ClientRequest>();
    auto http2Client = CMFrm::COM::HttpClientFactory::GetHttpClientFactory()->GetHttp2Client("default");
    CHECK_NULL_LOG_ERROR_RETURN(http2Client, FENCE_NO_RISK_PASS, "Get http2 client failed.");
    std::string frontFenceBody = buildFrontFenceBody(accessCtrl);
    req->SetMethod(CMFrm::COM::HttpMethod::POST);
    req->SetReqUrl("https://" + endpointIP + ":27142" + security_fence_url.data());
    req->AddHeader("Content-Type", "application/json");
    req->AddHeader("Accept", "application/json");
    req->AddBody(frontFenceBody);
    req->SetTimeoutSecond(FENCE_REQ_TIMEOUT);
    LOG_INFO_EX("[MMGuardService]Body: %s", frontFenceBody.c_str());
    auto resp = http2Client->Send(req);
    if (!resp) {
        LOG_INFO_EX("[MMGuardService]Front fence error");
        return FENCE_INTERNAL_ERROR;
    }
    LOG_INFO_EX("[MMGuardService]Front fence result: %d, %s",
                resp->GetStatusCode(), resp->GetResponseBody().c_str());
    if (resp->GetStatusCode() != CMFrm::COM::HTTP200)  {
        LOG_INFO_EX("[MMGuardService]Front fence error, error code: %d", resp->GetStatusCode());
        return FENCE_INTERNAL_ERROR;
    }
    secFenceResp frontFenceResp;
    auto parseFrontResp = ParseSecFenceResp(resp->GetResponseBody(), frontFenceResp);
    if (!parseFrontResp) {
        LOG_INFO_EX("[MMGuardService]Parse front fence response error");
        return FENCE_PARSE_ERROR;
    }
    LOG_INFO_EX("[MMGuardService]Front fence response result: %d", frontFenceResp.result);
    int frontFenceStatus = checkFenceResp(frontFenceResp);
    std::shared_ptr<TeamEnvironment> te = TEManager::getInstance().getTEById(accessCtrl->header.teamEnvironmentID);
    CHECK_NULL_LOG_ERROR_RETURN(te, FENCE_NO_RISK_PASS, "te is nullptr");
    te->frontFenceLevel = frontFenceStatus;
    LOG_INFO_EX("[MMGuardService]Front fence detection result: %d", te->frontFenceLevel);
    return frontFenceStatus;
}

int getRearFenceDetection(const std::shared_ptr<SYSAGENT::AccessCtrl>& accessCtrl)
{
    if (!securityFenceFlag) {       // 判断开关
        LOG_INFO_EX("[MMGuardService]Security fence is currently disabled");
        return FENCE_NO_RISK_PASS;
    }
    if (accessCtrl->eventType != SYSAGENT::EventType::USEROUTPUT) {
        LOG_INFO_EX("[MMGuardService]Security fence is currently disabled");
        return FENCE_NO_RISK_PASS;
    }
    const std::string endpointIP = getSecFenceServiceEndPoint();
    if (endpointIP.empty()) {
        LOG_ERR_EX("[MMGuardService]Send front fence failed, MMGuardservice instance is empty");
        return FENCE_NO_RISK_PASS;
    }
    LOG_INFO_EX("[MMGuardService]Endpoint: %s, fence_url: %s", endpointIP.c_str(), security_fence_url.data());
    auto req = std::make_shared<CMFrm::COM::Http2ClientRequest>();
    auto http2Client = CMFrm::COM::HttpClientFactory::GetHttpClientFactory()->GetHttp2Client("default");
    if (!http2Client) {
        LOG_ERR("Get http2 client failed.");
        return FENCE_NO_RISK_PASS;
    }
    std::string rearFenceBody = buildRearFenceBody(accessCtrl);
    req->SetMethod(CMFrm::COM::HttpMethod::POST);
    req->SetReqUrl("https://" + endpointIP + ":27142" + security_fence_url.data());
    req->AddHeader("Content-Type", "application/json");
    req->AddHeader("Accept", "application/json");
    req->AddBody(rearFenceBody);
    req->SetTimeoutSecond(FENCE_REQ_TIMEOUT);
    LOG_INFO_EX("[MMGuardService]Body: %s", rearFenceBody.c_str());
    auto resp = http2Client->Send(req);
    if (!resp) {
        LOG_INFO_EX("[MMGuardService]Rear fence error");
        return FENCE_INTERNAL_ERROR;
    }
    LOG_INFO_EX("[MMGuardService]Rear fence result: %d, %s",
                resp->GetStatusCode(), resp->GetResponseBody().c_str());
    if (resp->GetStatusCode() != CMFrm::COM::HTTP200)  {
        LOG_INFO_EX("[MMGuardService]Rear fence error, error code: %d", resp->GetStatusCode());
        return FENCE_INTERNAL_ERROR;
    }
    secFenceResp rearFenceResp;
    auto parseFrontResp = ParseSecFenceResp(resp->GetResponseBody(), rearFenceResp);
    if (!parseFrontResp) {
        LOG_INFO_EX("[MMGuardService]Parse rear fence response error");
        return FENCE_PARSE_ERROR;
    }
    LOG_INFO_EX("[MMGuardService]Rear fence response result: %d", rearFenceResp.result);
    int rearFenceStatus = checkFenceResp(rearFenceResp);
    LOG_INFO_EX("[MMGuardService]Rear fence detection result: %d", rearFenceStatus);
    return rearFenceStatus;
}

bool ParseSecFenceResp(const std::string& secFenceRespStr, secFenceResp& resp)
{
    Json::Value root;
    if (!JsonParser::Parse(secFenceRespStr, root)) {
        LOG_INFO_EX("[MMGuardService]Parse fence response root string error");
        return false;
    }
    // 2. 解析基础字段
    if (!JsonParser::GetString(root, "text", resp.text)) {
        LOG_INFO_EX("[MMGuardService]Parse fence response text error");
        return false;
    }
    if (!JsonParser::GetInt(root, "result", resp.result)) {
        LOG_INFO_EX("[MMGuardService]Parse fence response result error");
        return false;
    }

    std::vector<const Json::Value*> evidenceList;
    if (!JsonParser::GetJsonValueArray(root, "evidenceList", evidenceList)) {
        LOG_INFO_EX("[MMGuardService]Parse fence response evidenceList error");
        return false;
    }

    // 解析证据
    resp.evidenceList.clear();
    for (const auto& evidenceNode : evidenceList) {
        evidence newEvidence;
        if (!JsonParser::GetString(*evidenceNode, "detectionType", newEvidence.detectionType)) {
            LOG_INFO_EX("[MMGuardService]Parse fence response detectionType error");
            return false;
        }
        std::vector<const Json::Value*> detailsList;
        if (!JsonParser::GetJsonValueArray(*evidenceNode, "detail", detailsList)) {
            LOG_INFO_EX("[MMGuardService]Parse fence response details error");
            return false;
        }
        newEvidence.details.clear();
        for (const auto& detailNode : detailsList) {
            riskDetail newDetail;
            if (!JsonParser::GetString(*detailNode, "riskLabel1", newDetail.riskLabel1)) {
                LOG_INFO_EX("[MMGuardService]Parse fence response riskLabel1 error");
                return false;
            }
            if (!JsonParser::GetString(*detailNode, "evidence", newDetail.evidence)) {
                LOG_INFO_EX("[MMGuardService]Parse fence response evidence error");
                return false;
            }
            // 添加到风险详情数组
            newEvidence.details.push_back(newDetail);
        }
        // 添加到证据列表
        resp.evidenceList.push_back(newEvidence);
    }
    LOG_INFO_EX("[MMGuardService]Parse fence response success");
    return true;
}

std::string getSecFenceServiceEndPoint()
{
    std::string endpointIP = SYSAGENT::HttpClient::GetServiceIP("MMGuardService");
    if (endpointIP.empty()) {
        LOG_INFO_EX("[MMGuardService]Get MMGuard Service failed, endpoint is empty!");
        return {};
    }
    LOG_INFO_EX("[MMGuardService]Service endpoint: %s", endpointIP.c_str());
    return endpointIP;
}

std::string getRejectMsgZh()
{
    return R"(非常抱歉，该问题暂时无法继续回答。请您换个话题，我们继续交流。)";
}

std::string getRejectMsgEn()
{
    return R"(Sorry, this question cannot be answered at the moment.
Please change the topic and we'll continue the conversation.)";
}

// 解析响应报文中的检测类型和风险等级
int checkFenceResp(const secFenceResp& resp)
{
    int riskLevel = FENCE_NO_RISK_PASS;
    if (resp.result == FENCE_RESULT_ONE) {
        LOG_INFO_EX("[MMGuardService]Risk detected!");
    }

    for (auto& evidence: resp.evidenceList) {
        if (evidence.detectionType == "Keyword") {
            riskLevel = FENCE_RISK_BLOCK;
            LOG_INFO_EX("[MMGuardService]Keyword risk detected, risk status is: %d", riskLevel);
            return riskLevel;
        }
        for (auto& detail: evidence.details) {
            if (detail.riskLabel1 == POLITICAL_RED_LINES)  {
                riskLevel = FENCE_RISK_BLOCK;
                LOG_INFO_EX("[MMGuardService]Level 1 risk detected, risk status is: %d", riskLevel);
                return riskLevel;
            }
            if (!detail.riskLabel1.empty() && detail.riskLabel1 != POLITICAL_RED_LINES)  {
                riskLevel = FENCE_RISK_PASS;
                LOG_INFO_EX("[MMGuardService]Level 2 or 3 risk detected, risk status is: %d", riskLevel);
                return riskLevel;
            }
        }
    }
    LOG_INFO_EX("[MMGuardService]No risk detected, risk status is: %d", riskLevel);
    return riskLevel;
}

void initRandomSeed()
{
    srand(static_cast<unsigned int>(time(nullptr)));
}

unsigned int generateBusinessId()
{
    static bool is_seeded = false;
    if (!is_seeded) {
        srand(static_cast<unsigned int>(time(nullptr)));
        is_seeded = true;
    }
    unsigned int high = static_cast<unsigned int>(rand()) & 0x7FFF;
    unsigned int low = static_cast<unsigned int>(rand()) & 0x7FFF;

    unsigned int random_value = (high << 15) | low;
    return random_value == 0 ? 1 : random_value % (INT_MAX - 1) + 1;
}

void WriteFenceRespSucc(const std::shared_ptr<CMFrm::ServiceRouter::Context> &context)
{
    Json::Value resp;
    resp["result"] = "OK";
    std::string fenceFlag = securityFenceFlag ? "Enable" : "Disable";
    resp["FenceStatus"] = fenceFlag;
    context->WriteStatueCode(CMFrm::COM::HttpStatus::HTTP200);
    context->WriteJSONContentType();
    context->WriteResponseBody(JsonParser::ToRawString(resp));
    context->WriteAsyncResponse();
}

void WriteFenceRespErr(const std::shared_ptr<CMFrm::ServiceRouter::Context> &context)
{
    Json::Value resp;
    resp["err"] = "body params error!";
    context->WriteStatueCode(CMFrm::COM::HttpStatus::HTTP400);
    context->WriteJSONContentType();
    context->WriteResponseBody(JsonParser::ToRawString(resp));
    context->WriteAsyncResponse();
}

void OnRecvSecurityStatusRest(const std::shared_ptr<CMFrm::ServiceRouter::Context>& context)
{
    std::string reqBody = context->GetRequestBody();
    LOG_INFO_EX("Run security status callback, reqBody: %s", reqBody.c_str());
    Json::Value root;
    if (!AisfPlat::JsonParser::Parse(reqBody, root)) {
        LOG_INFO_EX("[MMGuardService]Security fence parse json error.");
        WriteFenceRespErr(context);
        return;
    }
    std::string type;
    std::string param;
    if (!AisfPlat::JsonParser::GetString(root, "type", type) ||
        !AisfPlat::JsonParser::GetString(root, "param", param)) {
        LOG_INFO_EX("[MMGuardService]Security fence json params error.");
        WriteFenceRespErr(context);
        return;
    }

    if (type == "get") {
        std::shared_lock<std::shared_mutex> lock(securityFenceFlagMutex);
        WriteFenceRespSucc(context);
        return;
    }
    if (type == "set") {
        std::unique_lock<std::shared_mutex> lock(securityFenceFlagMutex);
        securityFenceFlag = (param == "Enable") ? true : false;
        WriteFenceRespSucc(context);
    }
}

