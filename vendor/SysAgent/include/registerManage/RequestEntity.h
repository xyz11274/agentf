/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 */
//
// Created by z00913804 on 25-12-8.
//

#ifndef SYSAGENT_REQUESTENTITY_H
#define SYSAGENT_REQUESTENTITY_H
#include <string>
#include <vector>
#include "aisf_platform/parser/json_parser.h"
struct InstallParams {
    std::string name;
    std::string type;
    std::vector<std::string> ids;       // 非必选，不能冲突，由SysAgent统一分配
    std::string description;            // agent描述，对agent的功能、职责进行说明。
    uint port;                          // 非必选，agent业务监听端口，isLocal为Y时必选。
    std::string baseUrl;                // 非必选，type为3时填完整的url，type为2时只需要填url path，type为1时不需要填写
    std::string serviceName;            // 实际注册中心上服务的名字
    std::string example;
};

struct UpdateParams {
    std::string name;                   // agent名称，该参数不允许变更
    std::vector<std::string> ids;       // 非必选，指定意图场景需要传，且id不能冲突，由AISF统一分配
    std::string description;
    std::string baseUrl;                // type为3时，填完整url；type为2时，只需要填url path；type为1时，不需要填写。仅为3时需要更新。
    std::string serviceName;
};

class RequestEntity {
public:
    static RequestEntity& getInstance()
    {
        static RequestEntity parser;
        return parser;
    }
    bool parseInstallParams(std::string& body, InstallParams& installParams)
    {
        Json::Value root;
        if (!AisfPlat::JsonParser::Parse(body.c_str(), root)) {
            LOG_ERR_EX("parse body failed, check body json");
            return false;
        }
        AisfPlat::JsonParser::GetString(root, "name", installParams.name);
        AisfPlat::JsonParser::GetString(root, "type", installParams.type);
        AisfPlat::JsonParser::GetStringArray(root, "ids", installParams.ids);
        AisfPlat::JsonParser::GetString(root, "description", installParams.description);
        AisfPlat::JsonParser::GetString(root, "baseUrl", installParams.baseUrl);
        AisfPlat::JsonParser::GetUInt(root, "port", installParams.port);
        AisfPlat::JsonParser::GetString(root, "serviceName", installParams.serviceName);
        if (installParams.type == "3" && installParams.baseUrl.substr(0, 4) != "http") {
            LOG_INFO_EX("type is 3, baseUrl should start with http");
            return false;
        }
        return true;
    }

    bool parseUpdateParams(std::string& body, UpdateParams& updateParams)
    {
        Json::Value root;
        if (!AisfPlat::JsonParser::Parse(body.c_str(), root)) {
            LOG_ERR_EX("parse body failed, check body json");
            return false;
        }
        AisfPlat::JsonParser::GetString(root, "name", updateParams.name);
        AisfPlat::JsonParser::GetStringArray(root, "ids", updateParams.ids);
        AisfPlat::JsonParser::GetString(root, "description", updateParams.description);
        AisfPlat::JsonParser::GetString(root, "baseUrl", updateParams.baseUrl);
        AisfPlat::JsonParser::GetString(root, "serviceName", updateParams.serviceName);
        return true;
    }
private:
    RequestEntity() {}
    ~RequestEntity() = default;
    RequestEntity(const RequestEntity&) = delete;
    RequestEntity& operator=(const RequestEntity&) = delete;
};
#endif // SYSAGENT_REQUESTENTITY_H
