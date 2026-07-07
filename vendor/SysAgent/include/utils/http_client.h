/* ******************************************************************************
 *
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
 *
 * -------------------------------------------------------------------------------
 *
 * @file http_client.h
 *
 * Project Code: SysAgent
 * Module Name: utils
 * Date Created: 2025-05-27
 * Author:
 * Description: Definitions of Http client
 *
 * -------------------------------------------------------------------------------
 * Modification History
 * DATE          NAME                       DESCRIPTION
 * -----------------------------------------------------------------------------
 *
 * ***************************************************************************** */

#ifndef SYSAGENT_HTTPCLIENT_H
#define SYSAGENT_HTTPCLIENT_H

#include <list>
#include <shared_mutex>
#include <unordered_map>

#include "framework_com/http_2_client.h"
#include "framework_om/microservice/microservice_model.h"

namespace SYSAGENT {

constexpr char KMM_SERVICE[] = "KMMService";

constexpr char KMM_SERVICE_PORT[] =  ":27203";

constexpr char KMM_ADD_HISTORY[] = "/kmm/v1/user/memory/history/add";

constexpr char KMM_ADD_MEMORY[] = "/kmm/v1/user/memory/add";

constexpr char KMM_GET_MEMORIES[] = "/kmm/v1/user/memories/get";

constexpr char AISF_MGR[] = "AISFMgr";

constexpr char AISF_MGR_PORT[] = ":27042";

constexpr char AISF_MGR_EVENT_MARKING[] = "/aisf/v1/data/event_marking/add";

constexpr int KEEPALIVE_TIMEOUT_SECOND = 30;

constexpr int MAX_CONNECT_NUMBER = 2;

constexpr int TIMEOUT_SECOND = 30;

class HttpClient {
public:

    static bool QueryServiceInstance(const std::string& serviceName);

    static bool GetAllInstanceInfo(const std::string &serviceName, CMFrm::OM::QueryInstanceExtResp &resp);

    static std::string GetServiceIP(const std::string& serviceName);

    static std::vector<std::string> GetServiceInstance(const std::string &serviceName);

    static std::string GetSystemAgentUrl(const std::string& endpoint);

    static std::string GetKmmServerUrl(const std::string path);

    static bool RegisterHttp1ClientConfig();

    static bool Http1SendRequest(const std::string& url, const std::string& body);

    static bool Http2SendRequest(const std::string& url, const std::string& body,
                                 const std::map<std::string, std::string>& headers,
                                 CMFrm::COM::ClientHandleFunc clientHandleFunc, int timeout);

    static bool Http2SendRequest(std::shared_ptr<CMFrm::COM::Http2ClientRequest>& request,
                                      CMFrm::COM::ClientHandleFunc clientHandleFunc, void* arg = nullptr);

    static bool Htt2SendRequest(std::shared_ptr<CMFrm::COM::Http2ClientRequest>& request, std::string& response);

    static bool Http2SendStreamRequest(const std::string& url, const std::string& body,
                                       const std::map<std::string, std::string>& headers,
                                       CMFrm::COM::ClientHandleFunc clientHandleFunc,
                                       const int& timeoutSecond, void* arg);

    static bool Http2SendStreamRequest(std::shared_ptr<CMFrm::COM::Http2ClientRequest>& request,
                                       CMFrm::COM::ClientHandleFunc clientHandleFunc, void* arg);

    static bool Http2SendRequest(const std::string& url, const std::map<std::string, std::string>& headers,
                                 CMFrm::COM::ClientHandleFunc clientHandleFunc, int timeout);

    static void UpdateInstanceInfo(const std::string& serviceName, const std::string& ip);

    static void DeleteInstanceInfo(const std::string& serviceName);

private:
    static std::shared_mutex lock;

    static std::unordered_map<std::string, std::vector<std::string>> serviceInstanceMap;
};

}  // namespace SYSAGENT

#endif  // SYSAGENT_HTTPCLIENT_H
