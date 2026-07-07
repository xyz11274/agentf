//
// Created by z00913804 on 25-12-8.
//
#include "registerManage/AgentManager.h"
#include "registerManage/RequestEntity.h"
#include "aisf_platform/utils/file_utils.h"

#include <thread>
void AgentManager::onRecvAgentRegisterRequest(const std::shared_ptr<CMFrm::ServiceRouter::Context>& context) {
    std::string body = context->GetRequestBody();
    if(body.empty()) {
        LOG_ERR_EX("parse req failed, body is empty");
        std::string response = buildResp("body is empty", 400);
        WriteHttpOKResponse(context, response);
        return;
    }
    LOG_INFO_EX("received body : %s", body.c_str());
    InstallParams installParams;
    if (!RequestEntity::getInstance().parseInstallParams(body, installParams)) {
        std::string response = buildResp("parse body failed, check body json", 400);
        WriteHttpOKResponse(context, response);
        return;
    }
    AgentInfo agentInfo {
            .type = installParams.type,
            .ids = installParams.ids,
            .name = installParams.name,
            .description = installParams.description,
            .port = installParams.port,
            .baseUrl = installParams.baseUrl,
            .serviceName = installParams.serviceName,
            .example = installParams.example
    };
    if (!agentInfo.isValid()) {
        LOG_ERR_EX("wrong agentInfo");
        WriteHttpOKResponse(context, buildResp("wrong agentInfo", 400));
        return;
    }
    // flash change to the .json file on the disc
    if (!addAgentInfoConfigFile(agentInfo)) {
        LOG_INFO_EX("新增 agent : %s 失败", agentInfo.name.c_str());
        WriteHttpOKResponse(context, buildResp("add failed", 400));
        return;
    }
    WriteHttpOKResponse(context, buildResp("Add Success", 200));
    LOG_INFO_EX("新增 agent : %s 成功", agentInfo.name.c_str());
    return;
}

void AgentManager::onRecvAgentUpdateRequest(const std::shared_ptr<CMFrm::ServiceRouter::Context>& context) {
    std::string agentName = context->GetParam("name");
    if (agentName.empty()) {
        LOG_ERR_EX("parse param failed, update agent name is null");
        std::string response = buildResp("update failed, check param", 400);
        WriteHttpOKResponse(context, response);
        return;
    }
    LOG_INFO_EX("receive update agent name : %s", agentName.c_str());
    std::string body = context->GetRequestBody();
    if (body.empty()) {
        LOG_ERR_EX("parse param failed, update agent body is null");
        std::string response = buildResp("update failed, check body", 400);
        WriteHttpOKResponse(context, response);
        return;
    }
    LOG_INFO_EX("received body : %s", body.c_str());
    UpdateParams updateParams;
    if (!RequestEntity::getInstance().parseUpdateParams(body, updateParams)) {
        std::string response = buildResp("parse body failed, check body json", 400);
        WriteHttpOKResponse(context, response);
        return;
    }

    if (!updateAgentInfoConfigFileByName(agentName, updateParams)) {
        LOG_ERR_EX("更新agent : %s 失败", agentName.c_str());
        std::string response = buildResp("update failed, agent not found", 400);
        return;
    }
    std::string response = buildResp("Update Success", 200);
    LOG_INFO_EX("更新agent : %s 成功", agentName.c_str());
    WriteHttpOKResponse(context, response);
}

void AgentManager::onRecvAgentUninstallRequest(const std::shared_ptr<CMFrm::ServiceRouter::Context>& context) {
    std::string name = context->GetParam("name");
    if (name.empty()) {
        LOG_ERR_EX("delete failed, agent name is null");
        WriteHttpOKResponse(context, buildResp("delete failed, name is null", 400));
        return;
    }
    LOG_INFO_EX("receive delete agent name : %s", name.c_str());
    const AgentInfo* agent = agentInfoList.findFirst([name](const AgentInfo& agentInfo) {
        return agentInfo.name == name;
    });
    if (agent == nullptr) {
        LOG_ERR_EX("未找到 name : %s 的agent", name.c_str());
        WriteHttpOKResponse(context, buildResp("删除失败, 没有该agent", 400));
        return;
    }

    if (!deleteAgentInfoByName(name)) {
        LOG_ERR_EX("删除 agent : %s 失败", name.c_str());
        WriteHttpOKResponse(context, buildResp("删除失败", 500));
        return;
    }
    WriteHttpOKResponse(context, buildResp("删除成功", 200));
    LOG_INFO_EX("删除 agent : %s 成功", name.c_str());
    return;
}
std::string AgentManager::buildResp(const std::string& content, int code) {
    Json::Value root;
    root["RetCode"] = code;
    root["RetMsg"] = content;
    Json::StreamWriterBuilder writerBuilder;
    writerBuilder["indentation"] = "";
    writerBuilder["emitUTF8"] = true;
    std::string resp = Json::writeString(writerBuilder,root);
    return resp;
}
void AgentManager::WriteHttpOKResponse(const std::shared_ptr<CMFrm::ServiceRouter::Context> &context,
                                   const std::string &body)
{
    context->WriteStatueCode(CMFrm::COM::HttpStatus::HTTP200);
    context->WriteJSONContentType();
    context->WriteResponseBody(body);
}

bool AgentManager::addAgentInfoConfigFile(AgentInfo agentInfo) {
    std::string name = agentInfo.name;
    AgentInfo* agent = agentInfoList.findFirst([name](const AgentInfo& agentInfo) {
        return agentInfo.name == name;
    });
    if (agent != nullptr) {
        LOG_ERR_EX("已经存在 name : %s 的agent，无法新增", name.c_str());
        return false;
    }
    agentInfoList.push_back(agentInfo);
    saveToFile(agentInfoList);
    return true;
}

bool AgentManager::updateAgentInfoConfigFileByName(std::string& agentName, UpdateParams updateParams) {
    AgentInfo* agent = agentInfoList.findFirst([agentName](const AgentInfo& agentInfo) {
        return agentInfo.name == agentName;
    });
    if (agent == nullptr) {
        LOG_ERR_EX("未找到 name : %s 的agent", agentName.c_str());
        return false;
    }
    agent->ids = updateParams.ids;
    agent->description = updateParams.description;
    if (agent->type == "3") {
        agent->baseUrl = updateParams.baseUrl;
    }
    agent->serviceName = agent->serviceName;
    // 需要在type不为3且baseUrl不为空的情况下返回提示吗 ?
    saveToFile(agentInfoList);
    return true;
}

bool AgentManager::saveToFile(ThreadSafeVector<AgentInfo>& agentList) {
    // 从线程安全 vector 中获取所有元素
    std::vector<AgentInfo> configList;
    agentList.forEach([&configList](const AgentInfo& item) {
        configList.push_back(item);
    });
    // 转换为JSON数组
    Json::Value jsonArray(Json::arrayValue);
    for (auto& agent : configList) {
        jsonArray.append(agent.toJsonValue());
    }
    Json::StreamWriterBuilder writer;
    writer.settings_["emitUTF8"] = true;
    std::string jsonStr = Json::writeString(writer, jsonArray);
    // 写入文件
    const int maxRetries = 3;
    for (int retry = 0; retry < maxRetries; ++retry) {
        {
            std::lock_guard<std::mutex> lock(fileMutex_);
            std::ofstream file(agentFilePath);
            if (!file) {
                LOG_ERR_EX("第 %d 次尝试打开文件失败: %s", retry + 1, agentFilePath);
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                continue;
            }
            file << jsonStr;
            if (file.fail()) {
                LOG_ERR_EX("第 %d 次写入文件失败: %s", retry + 1, agentFilePath);
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                continue;
            }

            LOG_INFO_EX("成功写入文件: %s", agentFilePath);
            return true;
        }
    }
    LOG_INFO_EX("写入文件: %s 失败", agentFilePath);
    return false;
}

bool AgentManager::loadAgentInfoCache(ThreadSafeVector<AgentInfo>& agentList) {
    std::ifstream file(AisfPlat::FileUtils::RealPath(agentFilePath), std::ifstream::binary);
    if (!file.is_open()) {
        LOG_ERR_EX("open %s failed", agentFilePath);
        return false;
    }
    // 读取JSON文件到Json::Value
    Json::Value jsonData;
    Json::CharReaderBuilder rbuilder;
    std::string errs;
    if (!Json::parseFromStream(rbuilder, file, &jsonData, &errs)) {
        LOG_ERR_EX("Failed to parse agents.json");
        return false;
    }
    file.close();
    for (const auto& item : jsonData) {
        if (item.isObject()) {
            AgentInfo agent;
            AisfPlat::JsonParser::GetString(item, "type", agent.type);
            AisfPlat::JsonParser::GetStringArray(item, "ids", agent.ids);
            AisfPlat::JsonParser::GetString(item, "name", agent.name);
            AisfPlat::JsonParser::GetString(item, "description", agent.description);
            AisfPlat::JsonParser::GetUInt(item, "port", agent.port);
            AisfPlat::JsonParser::GetString(item, "baseUrl", agent.baseUrl);
            AisfPlat::JsonParser::GetString(item, "serviceName", agent.serviceName);
            AisfPlat::JsonParser::GetString(item, "example", agent.example);
            if (!agent.isValid()) {
                Json::StreamWriterBuilder writer;
                writer.settings_["emitUTF8"] = true;
                std::string jsonStr = Json::writeString(writer, agent.toJsonValue());
                LOG_ERR_EX("wrong agent info, jsonStr: %s", jsonStr.c_str());
                continue;
            }
            agentList.push_back(agent);
        } else {
            LOG_ERR_EX("agents.json has invalid non-string element");
            return false;
        }
    }
    LOG_INFO_EX("Parsed agent information from: %s", agentFilePath);
    agentList.forEach([](const AgentInfo& agentInfo) {
        LOG_INFO_EX("agent : %s", agentInfo.name.c_str());
    });
    return true;
}

bool AgentManager::deleteAgentInfoByName(std::string &name) {
    bool res = agentInfoList.removeIf([name](const AgentInfo& agentInfo) {
        return agentInfo.name == name;
    });
    if (!res) {
        LOG_ERR_EX("删除 name: %s 失败", name.c_str());
        return false;
    }
    saveToFile(agentInfoList); // 不加判断
    return true;
}