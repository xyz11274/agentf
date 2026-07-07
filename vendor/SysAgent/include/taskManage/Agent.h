//
// Created by z00913804 on 25-5-20.
//

#ifndef SYSAGENT_AGENT_H
#define SYSAGENT_AGENT_H

#include <string>
// libwebsockets Expand宏定义和rapidjson 冲突
#ifdef Expand
#undef Expand
#endif
#include "aisf_platform/common_define.h"
#include "utils/http_client.h"
#include "aisf_platform/parser/aisf_msg_ctrl.h"

// 实例化需要的少数Agent信息
struct AgentData {
    std::string type;
    std::string name;
    std::string serviceName;
    std::string baseUrl;
    uint32_t port{23767};
    AgentData(){};
    AgentData(std::string type, std::string name, std::string serviceName,
        std::string baseUrl, uint32_t port) : type(std::move(type)), name(std::move(name)),
        serviceName(std::move(serviceName)), baseUrl(std::move(baseUrl)), port(std::move(port)) {};
};

class Agent {
public:
    Agent(std::string type, std::string name, std::string desc, std::string url, std::string port, std::string baseUrl = "")
        : type_(std::move(type)), agentName_(std::move(name)), description_(std::move(desc)), url_(std::move(url)), port_(std::move(port)), base_url_(std::move(baseUrl))
    {
    }

    virtual ~Agent() = default;

    [[nodiscard]] const std::string& getType() const
    {
        return type_;
    }

    [[nodiscard]] const std::string& getName() const
    {
        return agentName_;
    }

    [[nodiscard]] const std::string& getDescription() const
    {
        return description_;
    }

    [[nodiscard]] const std::string& getUrl() const
    {
        return url_;
    }

    [[nodiscard]] const std::string& getPort() const
    {
        return port_;
    }

    [[nodiscard]] const std::string& getBaseUrl() const
    {
        return base_url_;
    }

    void setBaseUrl(const std::string& baseUrl)
    {
        base_url_ = baseUrl;
    }

    // 获取完整的URL，包括IP地址
    [[nodiscard]] std::string getFullUrl() const
    {
        std::string endpointIP = SYSAGENT::HttpClient::GetServiceIP(type_);
        if (endpointIP.empty()) {
            LOG_ERR_EX("[getFullUrl] get %s IP failed.", type_.c_str());
            return "";
        }

        return "https://" + endpointIP + ":" + port_ + base_url_;
    }

    // 发送RESTful请求
    virtual bool sendRestfulRequest(const shared_ptr<SYSAGENT::AccessCtrl>& accessCtrl) const;

    static shared_ptr<Agent> getAgentInstance(std::string& agentName);

    static shared_ptr<Agent> getAgentInstance(std::string agentName, std::vector<AgentData>& agentDatas);

protected:
    std::string type_;          // 1：虚拟Agent；2：本地Agent；3：三方Agent
    std::string agentName_;     // agent名称，eg: FoodieAgent、GUIAgent，可以获取实例ip，等价原来的type
    std::string description_;
    std::string url_;
    std::string port_;
    std::string base_url_;      // 基础URL路径 agent对接的url，type为3时，填完整的url，type为2时，只需要填url path，type为1时、不需要填写。
};
#endif // SYSAGENT_AGENT_H
