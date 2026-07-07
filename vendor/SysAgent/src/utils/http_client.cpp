//
// Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
//

#include "framework_common/utils/env_util.h"
#include "framework_common/utils/net_util.h"
#include "framework_common/utils/random_util.h"
#include "framework_common/utils/retry_util.h"
#include "framework_com/http_client_factory.h"
#include "framework_om/microservice/microservice_manager.h"

#include <sstream>
#include <thread>

#include "utils/http_client.h"
#include "utils/logger.h"

using namespace SYSAGENT;

std::shared_mutex HttpClient::lock;
std::unordered_map<std::string, std::vector<std::string>> HttpClient::serviceInstanceMap{};

bool HttpClient::QueryServiceInstance(const std::string& serviceName)
{
    LOG_INFO("QueryServiceInstance, serviceName: %s.", serviceName.c_str());

    auto microserviceManager = CMFrm::OM::MicroserviceManager::GetMicroserviceManager();
    CMFrm::OM::QueryInstanceReq queryInstanceReq;
    queryInstanceReq.serviceDiscovery = CMFrm::OM::BusinessServiceDiscovery;
    queryInstanceReq.appId = CMFrm::Utils::GetEnv("APPID", "0");
    queryInstanceReq.serviceName = serviceName;
    queryInstanceReq.filter = CMFrm::OM::All;

    CMFrm::Utils::Retry(
        [&microserviceManager, &queryInstanceReq]() -> bool {
            CMFrm::OM::QueryInstanceResp queryInstanceResp;
            auto ret = microserviceManager->QueryInstanceList(queryInstanceReq, queryInstanceResp);
            if (!ret || queryInstanceResp.instanceList.empty()) {
                LOG_ERR("Query instance failed, ret: %d.", ret);
                return false;
            }

            std::vector<std::string> endpoints;
            for (auto& instance : queryInstanceResp.instanceList) {
                LOG_INFO("Endpoint is: %s", instance.endpoint.c_str());
                auto ipAndPort = CMFrm::Utils::ParseEndpoint(instance.endpoint);
                endpoints.push_back(std::get<0>(ipAndPort));
            }

            std::unique_lock<std::shared_mutex> guard(lock);
            serviceInstanceMap[queryInstanceReq.serviceName] = endpoints;
            return true;
        },
        CMFrm::Constants::RetryCount2, CMFrm::Constants::RetryBackOffSecond2);

    return true;
}

bool HttpClient::GetAllInstanceInfo(const std::string &serviceName, CMFrm::OM::QueryInstanceExtResp &resp)
{
    CMFrm::OM::QueryInstanceReq req = CMFrm::OM::QueryInstanceReq();
    req.appId = CMFrm::Utils::GetEnv("APPID", "0");
    req.serviceName = serviceName;
    if (!CMFrm::OM::MicroserviceManager::GetMicroserviceManager()->QueryInstanceListExt(req, resp)) {
        LOG_ERR("query instance list failed");
        return false;
    }
    return true;
}

std::string HttpClient::GetServiceIP(const std::string& serviceName)
{
    auto endpoints = GetServiceInstance(serviceName);
    if (endpoints.empty()) {
        if (!QueryServiceInstance(serviceName)) {
            LOG_ERR("Query %s instance failed.", serviceName.c_str());
            return "";
        }
        endpoints = GetServiceInstance(serviceName);
    }

    return CMFrm::Utils::GetRandomElem(endpoints);
}

std::vector<std::string> HttpClient::GetServiceInstance(const std::string& serviceName)
{
    std::shared_lock<std::shared_mutex> guard(lock);
    auto it = serviceInstanceMap.find(serviceName);
    if (it == serviceInstanceMap.end()) {
        return {};
    }
    return it->second;
}

std::string HttpClient::GetKmmServerUrl(const std::string path)
{
    return "https://" + GetServiceIP(KMM_SERVICE) + KMM_SERVICE_PORT + path;
}

bool HttpClient::RegisterHttp1ClientConfig()
{
    auto httpClientFactory = CMFrm::COM::HttpClientFactory::GetHttpClientFactory();
    auto http1ClientConfig = std::make_shared<CMFrm::COM::Http1ClientConfig>();
    http1ClientConfig->name = "default";
    http1ClientConfig->keepAlive = true;
    http1ClientConfig->keepAliveTimeoutSecond = KEEPALIVE_TIMEOUT_SECOND;
    http1ClientConfig->maxConnectNum = MAX_CONNECT_NUMBER;
    http1ClientConfig->timeoutSecond = TIMEOUT_SECOND;
    httpClientFactory->RegisterHttp1ClientConfig(http1ClientConfig);
    return true;
}

bool HttpClient::Http1SendRequest(const std::string& url, const std::string& body)
{
    LOG_INFO("Send http request start, url: %s, body: %s", url.c_str(), body.c_str());

    auto httpClientFactory = CMFrm::COM::HttpClientFactory::GetHttpClientFactory();
    auto http1Client = httpClientFactory->GetHttp1Client("default");
    if (!http1Client) {
        LOG_ERR("Get http1 client failed.");
        return false;
    }

    auto http1ClientRequest = std::make_shared<CMFrm::COM::Http1ClientRequest>();
    http1ClientRequest->SetMethod(CMFrm::COM::POST);
    http1ClientRequest->SetReqUrl(url);
    http1ClientRequest->AddBody(body);
    http1ClientRequest->AddJSONContentTypeHeader();

    auto http1Response = http1Client->Send(http1ClientRequest);
    if (!http1Response->Is2XXStatusCode()) {
        LOG_ERR("Http1 send failed, status code: %d", http1Response->GetStatusCode());
        return false;
    }

    LOG_INFO("Send http request success.");
    return true;
}

bool HttpClient::Http2SendRequest(const std::string& url, const std::string& body,
                                  const std::map<std::string, std::string>& headers,
                                  CMFrm::COM::ClientHandleFunc clientHandleFunc, int timeout)
{
    LOG_INFO("Send http request start, url: %s, body: %s", url.c_str(), body.c_str());

    auto http2ClientRequest = std::make_shared<CMFrm::COM::Http2ClientRequest>();
    http2ClientRequest->SetMethod(CMFrm::COM::POST);
    http2ClientRequest->SetReqUrl(url);
    http2ClientRequest->AddBody(body);
    http2ClientRequest->AddJSONContentTypeHeader();
    http2ClientRequest->SetTimeoutSecond(timeout);
    for (const auto& header : headers) {
        http2ClientRequest->AddHeader(header.first, header.second);
    }

    return Http2SendRequest(http2ClientRequest, clientHandleFunc);
}

bool HttpClient::Http2SendRequest(std::shared_ptr<CMFrm::COM::Http2ClientRequest>& request,
                                  CMFrm::COM::ClientHandleFunc clientHandleFunc, void* arg)
{
    auto httpClientFactory = CMFrm::COM::HttpClientFactory::GetHttpClientFactory();
    auto http2Client = httpClientFactory->GetHttp2Client("default");
    if (!http2Client) {
        LOG_ERR("Get http2 client failed.");
        return false;
    }

    std::ostringstream tid_oss;
    tid_oss << std::this_thread::get_id();
    LOG_INFO("[HTTP2] SendAsync BEGIN client=default tid=%s", tid_oss.str().c_str());

    auto result = http2Client->SendAsync(request, clientHandleFunc, arg);
    if (!result) {
        LOG_ERR("[HTTP2] SendAsync FAILED result=%d tid=%s", result, tid_oss.str().c_str());
        return false;
    }
    LOG_INFO("[HTTP2] SendAsync END tid=%s", tid_oss.str().c_str());
    return true;
}

bool HttpClient::Http2SendRequest(const std::string& url, const std::map<std::string, std::string>& headers,
                                  CMFrm::COM::ClientHandleFunc clientHandleFunc, int timeout)
{
    LOG_INFO("Send http GET request start, url: %s", url.c_str());

    auto http2ClientRequest = std::make_shared<CMFrm::COM::Http2ClientRequest>();
    http2ClientRequest->SetMethod(CMFrm::COM::GET);
    http2ClientRequest->SetReqUrl(url);
    http2ClientRequest->SetTimeoutSecond(timeout);
    for (const auto& header : headers) {
        http2ClientRequest->AddHeader(header.first, header.second);
    }

    return Http2SendRequest(http2ClientRequest, clientHandleFunc);
}

void HttpClient::UpdateInstanceInfo(const std::string &serviceName, const std::string &ip)
{
    std::shared_lock<std::shared_mutex> guard(lock);
    LOG_INFO_EX("update service:%s", ip.c_str());
    serviceInstanceMap[serviceName] = std::vector<std::string>{ip};
}

void HttpClient::DeleteInstanceInfo(const std::string& serviceName)
{
    std::shared_lock<std::shared_mutex> guard(lock);
    if (serviceInstanceMap.find(serviceName) == serviceInstanceMap.end()) {
        return;
    }
    LOG_INFO_EX("release service:%s", serviceName.c_str());
    serviceInstanceMap.erase(serviceName);
}

bool HttpClient::Htt2SendRequest(std::shared_ptr<CMFrm::COM::Http2ClientRequest>& request, std::string& response)
{
    auto httpClientFactory = CMFrm::COM::HttpClientFactory::GetHttpClientFactory();
    auto http2Client = httpClientFactory->GetHttp2Client("default");
    if (!http2Client) {
        LOG_ERR_EX("Get http2 client failed");
        return false;
    }

    auto result = http2Client->Send(request);
    if (!result) {
        LOG_ERR_EX("http request failed!");
        return false;
    }

    if (!result->Is2XXStatusCode()) {
        LOG_ERR_EX("http resp failed:%d", result->GetStatusCode());
    }

    auto res = result->GetResponseBody();
    if (res.empty()) {
        LOG_ERR_EX("http resp body is empty!");
        return false;
    }

    response = res;
    return true;
}

bool HttpClient::Http2SendStreamRequest(const std::string& url, const std::string& body,
                                       const std::map<std::string, std::string>& headers,
                                       CMFrm::COM::ClientHandleFunc clientHandleFunc,
                                       const int& timeoutSecond, void* arg)
{
    if (body.size() > 1024) {
        LOG_INFO_SAFE("Send http request start, url: %s, body size: %d", url.c_str(), body.size());
    } else {
        LOG_INFO_SAFE("Send http request start, url: %s, body: %s", url.c_str(), body.c_str());
    }

    auto http2ClientRequest = std::make_shared<CMFrm::COM::Http2ClientRequest>();
    http2ClientRequest->SetMethod(CMFrm::COM::POST);
    http2ClientRequest->SetReqUrl(url);
    http2ClientRequest->AddBody(body);
    http2ClientRequest->AddJSONContentTypeHeader();
    http2ClientRequest->SetTimeoutSecond(timeoutSecond);
    for (const auto& header : headers) {
        http2ClientRequest->AddHeader(header.first, header.second);
    }

    return Http2SendStreamRequest(http2ClientRequest, clientHandleFunc, arg);
}

bool HttpClient::Http2SendStreamRequest(std::shared_ptr<CMFrm::COM::Http2ClientRequest>& request,
                                       CMFrm::COM::ClientHandleFunc clientHandleFunc, void* arg)
{
    auto httpClientFactory = CMFrm::COM::HttpClientFactory::GetHttpClientFactory();
    auto http2Client = httpClientFactory->GetHttp2Client("stream");
    if (!http2Client) {
        LOG_ERR("Get http2 stream client failed.");
        return false;
    }

    auto result = http2Client->SendStreamAsync(request, clientHandleFunc, arg);
    if (!result) {
        LOG_ERR_SAFE("Http2 send failed, result: %d", result);
        return false;
    }
    LOG_INFO("Send http stream request success.");
    return true;
}