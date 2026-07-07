// Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.

#include <future>
#include "queue/Lifecycle/LocationHandler.h"
#include "memory/Location.h"
#include "mm_tool_manager.h"
#include "utils/tool.h"

void LocationHandler::handle(const shared_ptr<SYSAGENT::AccessCtrl>& accessCtrl)
{
    shared_ptr<Location> location = make_shared<Location>(accessCtrl->header.deviceId,
                                                          accessCtrl->locationInfo.data.latitude,
                                                          accessCtrl->locationInfo.data.longitude,
                                                          accessCtrl->locationInfo.data.instanceNum);
    shared_ptr<std::vector<std::string>> res = make_shared<std::vector<std::string>>(location->Query());
    if (!res->empty() && res->size() >= NUMBER_SEVEN) { // + 判断size，否则越界
        // 返回的查询结果，默认按照表的字段顺序
        string id = (*res)[0];
        string deviceID = (*res)[1];
        string latitude = (*res)[2];
        string longitude = (*res)[3];
        string city = (*res)[4];
        string address = (*res)[5];
        string instanceNum = (*res)[6]; // 新增的instanceNum
        LOG_INFO_EX("id : %s , deviceID : %s , latitude : %s , longitude : %s , city : %s , address : %s , instanceNum : %s",
            id.c_str(), deviceID.c_str(), latitude.c_str(), longitude.c_str(), city.c_str(), address.c_str(), instanceNum.c_str());
        // check if changed
        if (!city.empty() && !address.empty() && latitude == location->getLatitude() && longitude == location->getLongitude()) {
            LOG_INFO_EX("deviceID : %s , location did not change", deviceID.c_str());
            if (instanceNum == location->getInstanceNum()) {
                LOG_INFO_EX("deviceID: %s, instanceNum: %s did not change", deviceID.c_str(), instanceNum.c_str());
                return;
            }
            location->Update();
            return;
        }
    }
    LOG_INFO_EX("location have changed or did not exist");

    // 构造入参
    Json::Value toolInfo;
    toolInfo["latitude"] = location->getLatitude();
    toolInfo["longitude"] = location->getLongitude();
    std::string toolName = "liantong_mcp_server_get_location_information";
    std::async([res, toolName, toolInfo, location](){
        Json::Value result;
        if (!SYSAGENT::Tool::CallTool(toolName, toolInfo, result) || !result.isMember("errCode")
            || !result["errCode"].isInt() || result["errCode"].asInt()) {
            LOG_ERR_EX("Invoke location tool failed:%s", toolName.c_str());
        }
        if (result.isMember("data") && result["data"].isString() && location) {
            Json::Value data;
            JsonParser::Parse(result["data"].asString(), data);
            JsonParser::GetString(data, "city", location->city);
            JsonParser::GetString(data, "formatted_address", location->address);
            JsonParser::GetString(data, "district", location->district);
            JsonParser::GetString(data, "town", location->town);
            if (res->empty()) {
                LOG_INFO_EX("deviceID location did not exist, insert location");
                location->Insert();
                return;
            }
            location->Update();
        }
    });
}