
//
// Created by z00913804 on 25-5-20.
//

#include "taskManage/Agent.h"
#include "utils/http_client.h"
#include "queue/QueueManager.h"
#include "utils/Constants.h"
#include "taskManage/SystemAgent.h"
/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
 * Description: This file declares for Agent.cpp
 */

bool isHttp2Client(const std::string& url)
{
    return true;
}

bool Agent::sendRestfulRequest(const shared_ptr<SYSAGENT::AccessCtrl>& accessCtrl) const
{
    QueueManager::getInstance().EventMark("info", "Agent.SendRestfulRequestBegin", "success",
                                          "start to send accessCtrl to agent : " + agentName_, accessCtrl);
    std::string messageBody = accessCtrl->toString();
    LOG_INFO_EX("start, agent agent name: %s", agentName_.c_str());
    LOG_INFO_EX("messageBody send to agent: %s", messageBody.c_str());
    // 获取完整URL（直接使用Agent自己的getFullUrl方法）
    std::string fullUrl = url_;
    if (fullUrl.empty()) {
        LOG_ERR_EX("=========== Failed to get full URL for agent: %s ===========", type_.c_str());
        return false;
    }

    auto request = std::make_shared<CMFrm::COM::Http2ClientRequest>();
    request->SetMethod(CMFrm::COM::POST);
    request->SetReqUrl(fullUrl);
    request->AddHeader("Content-Type", "text/json;charset=utf-8");
    request->AddHeader("X-Tenant-Name", "default");
    request->AddBody(messageBody);
    request->SetTimeoutSecond(CMFrm::Constants::Const8);

    // 同步调用
    std::string response;
    bool sendResult = SYSAGENT::HttpClient::Htt2SendRequest(request, response);
    if (!sendResult) {
        LOG_ERR_EX("SendReq failed with code: %d", sendResult);
        QueueManager::getInstance().EventMark("error", "Agent.SendRestfulRequestEnd", "failed",
                                              "failed to send accessCtrl to agent : " + agentName_, accessCtrl);
        return false;
    }
    LOG_INFO("========== Response: %s =============", response.c_str());
    QueueManager::getInstance().EventMark("info", "Agent.SendRestfulRequestEnd", "success",
                                          "start to send accessCtrl to agent : " + agentName_ + " success", accessCtrl);
    return true;
}

shared_ptr<Agent> Agent::getAgentInstance(std::string& agentName)
{
    auto agents = AgentManager::getInstance().getAgentList();
    if (agents.empty()) {
        LOG_ERR_EX("agentDatas is empty");
    }
    for (const auto& agent : agents) {
        LOG_INFO_EX("agent name: %s, %s", agent.name.c_str(), agent.serviceName.c_str());
        if (agent.name == agentName) {
            std::string agentUrl = agent.baseUrl;
            if (agent.type == "2") { // type 2 本地agent，需要向注册中心获取实例ip
                std::string serviceName = agent.serviceName.empty() ? agent.name : agent.serviceName;
                std::string endpointIP = SYSAGENT::HttpClient::GetServiceIP(serviceName);
                if (endpointIP.empty()) {
                    LOG_ERR_EX("[getFullUrl] get IP failed, appid: %s", agentName.c_str());
                    return nullptr;
                }
                LOG_INFO_EX("agentInstance ip : %s, %s", endpointIP.c_str(), serviceName.c_str());
                agentUrl = "https://" + endpointIP + ":" + std::to_string(agent.port) + agent.baseUrl;
            }
            LOG_INFO_EX("agentInstance url : %s, %s", agentUrl.c_str(), agent.name.c_str());
            if (agent.name == SysAgent::Constant::AgentName::SYS_AGENT) {
                return std::make_shared<SystemAgent>(agent.type, agent.name, agent.description, agentUrl,
                    std::to_string(agent.port), agent.baseUrl);
            }
            return std::make_shared<Agent>(agent.type, agent.name, agent.description, agentUrl,
                                           std::to_string(agent.port), agent.baseUrl);
        }
    }
    return nullptr;
}

shared_ptr<Agent> Agent::getAgentInstance(std::string agentName, std::vector<AgentData>& agentDatas)
{
    if (agentDatas.empty()) {
        LOG_ERR_EX("agentDatas is empty");
    }
    for (const auto& agent : agentDatas) {
        if (agent.name == agentName) {
            std::string agentUrl = agent.baseUrl;
            if (agent.type == "2") { // 1 虚拟agent，baseUrl为空 2 本地agent，url path 3 三方agent，完整的url
                std::string serviceName = agent.serviceName.empty() ? agent.name : agent.serviceName;
                std::string endpointIP = SYSAGENT::HttpClient::GetServiceIP(serviceName);
                if (endpointIP.empty()) {
                    LOG_ERR_EX("[getFullUrl] get %s IP failed, appid: %s", agentName.c_str());
                    return nullptr;
                }
                LOG_INFO_EX("agentInstance ip : %s", endpointIP.c_str());
                agentUrl = "https://" + endpointIP + ":" + std::to_string(agent.port) + agent.baseUrl;
            }
            LOG_INFO_EX("agentInstance url : %s", agentUrl.c_str());
            if (agent.name == SysAgent::Constant::AgentName::SYS_AGENT) {
                return std::make_shared<SystemAgent>(agent.type, agent.name, "", agentUrl,
                    std::to_string(agent.port), agent.baseUrl);
            }
            return std::make_shared<Agent>(agent.type, agent.name, "", agentUrl, std::to_string(agent.port),
                                           agent.baseUrl);
        }
    }
    return nullptr;
}