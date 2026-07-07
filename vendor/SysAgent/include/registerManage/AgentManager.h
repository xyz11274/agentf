/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 */
//
// Created by z00913804 on 25-12-8.
//

#ifndef SYSAGENT_AGENTMANAGER_H
#define SYSAGENT_AGENTMANAGER_H
#include <vector>
#include "aisf_platform/thread/thread_safe_vector.h"
#include <fstream>
#include "http_client_factory.h"
#include "service_router/router_manager.h"
#include "RequestEntity.h"
#include "json/json.h"
#include "utils/logger.h"
#include "taskManage/Agent.h"
// constexpr char agentFilePath[] = "/opt/AISF/SysAgent/conf/agent.json";
constexpr char agentFilePath[] = "/opt/coremind/conf/agents.json";
struct AgentInfo {
    std::string type;
    std::vector<std::string> ids;
    std::string name;
    std::string description;
    uint32_t port;
    std::string baseUrl;
    std::string serviceName;
    std::string example;
    Json::Value toJsonValue()
    {
        Json::Value jsonAgent;
        jsonAgent["type"] = type;
        jsonAgent["ids"] = Json::Value(Json::arrayValue);
        for (const auto& id : ids) {
            jsonAgent["ids"].append(id);
        }
        jsonAgent["name"] = name;
        jsonAgent["description"] = description;
        jsonAgent["port"] = port;
        jsonAgent["baseUrl"] = baseUrl;
        jsonAgent["serviceName"] = serviceName;
        jsonAgent["example"] = example;
        return jsonAgent;
    }
    Json::Value toAgentDescriptionValue()
    {
        Json::Value jsonAgent;
        jsonAgent["assistant_name"] = name;
        jsonAgent["description"] = description;
        jsonAgent["example"] = example;
        return jsonAgent;
    }
    bool isValid()
    {
        if (!checkUrl(type, baseUrl)) {
            LOG_ERR_EX("wrong url");
            return false;
        }
        if (name.empty() || type.empty() || description.empty()) {
            LOG_ERR_EX("name or type or description is empty");
            return false;
        }
        if (!isValidPort(type, port)) {
            LOG_ERR_EX("wrong port");
            return false;
        }
        if (!isValidServiceName(type, serviceName)) {
            LOG_ERR_EX("wrong serviceName");
            return false;
        }
        return true;
    }
    static bool isValidPort(std::string& type, uint32_t port)
    {
        if (type != "2") {
            return true;
        }
        if (port < 1 || port > 65535) {
            LOG_ERR_EX("invalid port: %u", port);
            return false;
        }
        return true;
    }
    static bool isValidServiceName(std::string& type, std::string& serviceName)
    {
        if (type == "2") {
            if (serviceName.empty()) {
                return false;  // type 为 "2" 时，服务名称不能为空
            }
        }
        return true;
    }
    static bool startWithHttp(std::string& url)
    {
        if (url.length() >= 4) {
            return url.compare(0, 4, "http") == 0;
        }
        return false;
    }

    static bool checkUrl(std::string& type, std::string baseUrl)
    {
        if (type == "1") {
            return baseUrl.empty();
        }
        if (type == "2") {
            return !startWithHttp(baseUrl);
        }
        if (type == "3") {
            return startWithHttp(baseUrl);
        }
        LOG_ERR_EX("checkUrl failed, type: %s, baseUrl: %s", type.c_str(), baseUrl.c_str());
        return false;
    }
};

class AgentManager {
public:
    static AgentManager& getInstance()
    {
        static AgentManager agentMgr;
        return agentMgr;
    }
    void init()
    {
        // recover agent info from .json file on the disc
        if (!loadAgentInfoCache(agentInfoList)) {
            LOG_ERR_EX("failed to recover agent info from .json file on the disc");
            return;
        }
        LOG_INFO_EX("recovered agent info from .json file on the disc");
    }
    std::vector<AgentInfo> getAgentList()
    {
        std::vector<AgentInfo> configList;
        agentInfoList.forEach([&configList](const AgentInfo& item) {
            configList.push_back(item);
        });
        return configList;
    }
    std::map<std::string, AgentInfo> getAgentMap()
    {
        std::map<std::string, AgentInfo> agentMap;
        agentInfoList.forEach([&agentMap](const AgentInfo& item) {
            agentMap.insert(std::make_pair(item.name, item));
        });
        return agentMap;
    }
    std::string getAgentDescriptionString()
    {
        Json::Value root(Json::arrayValue);
        std::vector<AgentInfo> agents = getAgentList();
        for (AgentInfo agent : agents) {
            if (agent.name == "GUIAgent" || agent.name == "StudyBuddyAgent" || agent.name == "MovieAgent") {
                continue;
            }
            Json::Value jsonAgent = agent.toAgentDescriptionValue();
            root.append(jsonAgent);
        }

        Json::StreamWriterBuilder builder;
        builder["emitUTF8"] = true;
        std::string jsonStr = Json::writeString(builder, root);
        return jsonStr;
    }

    void onRecvAgentRegisterRequest(const std::shared_ptr<CMFrm::ServiceRouter::Context>& context);

    void onRecvAgentUpdateRequest(const std::shared_ptr<CMFrm::ServiceRouter::Context>& context);

    void onRecvAgentUninstallRequest(const std::shared_ptr<CMFrm::ServiceRouter::Context>& context);

    std::string buildResp(const std::string& content, int code);

    void WriteHttpOKResponse(const std::shared_ptr<CMFrm::ServiceRouter::Context> &context,
                             const std::string &body);

private:
    AgentManager() {};

    ~AgentManager() = default;

    AgentManager(const AgentManager&) = delete;

    AgentManager& operator=(const AgentManager&) = delete;

    ThreadSafeVector<AgentInfo> agentInfoList;

    std::mutex fileMutex_;

    bool addAgentInfoConfigFile(AgentInfo agentInfo);

    bool updateAgentInfoConfigFileByName(std::string& name, UpdateParams updateParams);

    bool deleteAgentInfoByName(std::string& name);

    bool saveToFile(ThreadSafeVector<AgentInfo>& agentList);

    bool loadAgentInfoCache(ThreadSafeVector<AgentInfo>& agentList);
};
#endif // SYSAGENT_AGENTMANAGER_H
