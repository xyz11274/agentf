// Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.

#include "memory/Location.h"
#include "memory/GaussClient.h"
#include "aisf_platform/common_define.h"
#include "aisf_platform/utils/utils.h"
#include "framework_common/constants/number_constant.h"

using namespace DM::RAG;
void Location::Insert() {
    GaussClient* client = GaussClient::Instance();
    client->Insert("Location", {"id", "deviceID","latitude", "longitude", "city", "address", "instanceNum", "district",
                                "town"},
                   {
                        std::make_shared<DM::RAG::TextValue>(AisfPlat::Utils::GenerateUniqueId()),
                        std::make_shared<DM::RAG::TextValue>(this->deviceID),
                        std::make_shared<DM::RAG::TextValue>(this->latitude),
                        std::make_shared<DM::RAG::TextValue>(this->longitude),
                        std::make_shared<DM::RAG::TextValue>(this->city),
                        std::make_shared<DM::RAG::TextValue>(this->address),
                        std::make_shared<DM::RAG::TextValue>(this->instanceNum),
                        std::make_shared<DM::RAG::TextValue>(this->district),
                        std::make_shared<DM::RAG::TextValue>(this->town),
                   }, InsertOption::Builder().WithSchemaName(AISF_LOCATION_SCEMA_NAME).Build());
    {
        std::lock_guard<std::mutex> lock(cacheMapMutex);
        LocalLocationInfo locationInfo = {this->latitude, this->longitude, this->city, this->address, this->instanceNum,
                                          this->district, this->town};
        auto it = cacheMap.find(this->deviceID);
        if (it != cacheMap.end()) {
            it->second = locationInfo;
            TouchAsNewest(this->deviceID);
        } else {
            cacheMap.emplace(this->deviceID, locationInfo);
            deviceIDOrder.push_back(this->deviceID);
            deviceID2Index[this->deviceID] = std::prev(deviceIDOrder.end());
        }
        KeepCacheMapSize();
    }
}

void Location::Update() {
    GaussClient* client = GaussClient::Instance();
    client->Update("Location",
                   {
                        {"latitude", make_shared<DM::RAG::VarCharValue>(this->latitude)},
                        {"longitude", make_shared<DM::RAG::VarCharValue>(this->longitude)},
                        {"city", make_shared<DM::RAG::VarCharValue>(this->city)},
                        {"address", make_shared<DM::RAG::VarCharValue>(this->address)},
                        {"instanceNum", make_shared<DM::RAG::VarCharValue>(this->instanceNum)},
                        {"district", make_shared<DM::RAG::VarCharValue>(this->district)},
                        {"town", make_shared<DM::RAG::VarCharValue>(this->town)},
                   },
                   UpdateOption::Builder()
                   .WithFilters(FilterOption::Builder()
                   .WithFieldName("deviceID")
                   .WithEQ()
                   .WithValue(make_shared<TextValue>(this->deviceID))
                   .Build())
                   .WithSchemaName(AISF_LOCATION_SCEMA_NAME)
                   .Build());
    {
        std::lock_guard<std::mutex> lock(cacheMapMutex);
        LocalLocationInfo locationInfo = {this->latitude, this->longitude, this->city, this->address, this->instanceNum};
        auto it = cacheMap.find(this->deviceID);
        if (it != cacheMap.end()) {
            it->second = locationInfo;
            TouchAsNewest(this->deviceID);
            return;
        }
        cacheMap.emplace(this->deviceID, locationInfo);
        deviceIDOrder.push_back(this->deviceID);
        deviceID2Index[this->deviceID] = std::prev(deviceIDOrder.end());
        KeepCacheMapSize();
    }
}

void Location::Delete() {
    GaussClient* client = GaussClient::Instance();
    client->Delete("Location", DeleteOption::Builder()
    .WithSchemaName(AISF_LOCATION_SCEMA_NAME)
    .WithFilters(FilterOption::Builder()
    .WithFieldName("deviceID")
    .WithEQ()
    .WithValue(make_shared<VarCharValue>(this->deviceID))
    .Build())
    .Build());
    {
        std::lock_guard<std::mutex> lock(cacheMapMutex);
        auto it = cacheMap.find(this->deviceID);
        if (it == cacheMap.end()) {
            return;
        }
        auto posIt = deviceID2Index.find(deviceID);
        if (posIt != deviceID2Index.end()) {
            deviceIDOrder.erase(posIt->second);
            deviceID2Index.erase(posIt);
        }
        cacheMap.erase(it);
    }
}

std::vector<std::string> Location::Query() {
    GaussClient* client = GaussClient::Instance();
    std::vector<std::vector<std::string>> result;
    client->ScalarSearch("Location",
                       QueryOption::Builder(TableQueryType::SCALAR)
                       .WithBaseSchemaName(AISF_LOCATION_SCEMA_NAME)
                       .WithBaseFilters(FilterOption::Builder()
                       .WithFieldName("deviceID")
                       .WithEQ()
                       .WithValue(make_shared<VarCharValue>(this->deviceID))
                       .Build())
                       .Build(),
                       result
                       );
    if (!result.empty()) { // 查的是特定一条记录，不空返回第一条
        LOG_INFO_EX("查询到数据了");
        for (uint32_t i = 0; i < result[0].size(); ++i) {
            LOG_INFO_EX("Location表查询deviceID [%s] 结果第 [%d] 列: [%s]", this->deviceID.c_str(), i, result[0][i].c_str());
        }
        return result[0];
    }
    LOG_INFO_EX("没查到deviceID= %s 的数据", this->deviceID.c_str());
    return {};
}

void Location::TouchAsNewest(const std::string& deviceID)
{
    auto posIt = deviceID2Index.find(deviceID);
    if (posIt == deviceID2Index.end()) return;
    deviceIDOrder.erase(posIt->second);
    deviceIDOrder.push_back(deviceID);
    deviceID2Index[deviceID] = std::prev(deviceIDOrder.end());
}

void Location::KeepCacheMapSize()
{
    while (cacheMap.size() > capacity) {
        const std::string& oldest = deviceIDOrder.front();
        cacheMap.erase(oldest);
        deviceID2Index.erase(oldest);
        deviceIDOrder.pop_front();
    }
}