// Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.

#ifndef SYSAGENT_LOCATION_H
#define SYSAGENT_LOCATION_H
#ifdef Expand
#undef Expand
#endif
#include <vector>
#include "string"
#include <mutex>
#include <unordered_map>
#include <list>

#define LOCATION_POS 4

class Location {
public:
    std::string id;
    std::string deviceID;
    std::string latitude;
    std::string longitude;
    std::string address;
    std::string city;
    std::string instanceNum;
    std::string district;
    std::string town;
    Location(std::string deviceID, std::string latitude, std::string longitude) {
        this->deviceID = deviceID;
        this->latitude = latitude;
        this->longitude = longitude;
    }
    Location(std::string deviceID, std::string latitude, std::string longitude, std::string instanceNum) {
        this->deviceID = deviceID;
        this->latitude = latitude;
        this->longitude = longitude;
        this->instanceNum = instanceNum;
    }
    std::string getDeviceID() {
        return deviceID;
    }
    std::string getLatitude() {
        return latitude;
    }
    std::string getLongitude() {
        return longitude;
    }
    std::string getInstanceNum() {
        return instanceNum;
    }
    void setAddress(std::string mcpAddress) {
        this->address = mcpAddress;
    }
    void setCity(std::string city) {
        this->city = city;
    }
    void Insert();
    void Delete();
    void Update();
    std::vector<std::string> Query();
//    std::vector<std::string> Query(std::string deviceID);

private:
    static inline  std::mutex cacheMapMutex;
    struct LocalLocationInfo{
        std::string latitude;
        std::string longitude;
        std::string city;
        std::string address;
        std::string instanceNum;
        std::string district;
        std::string town;
    };
    static inline size_t capacity = 1000;
    static inline std::unordered_map<std::string, LocalLocationInfo> cacheMap;
    static inline std::list<std::string> deviceIDOrder;
    static inline std::unordered_map<std::string, std::list<std::string>::iterator> deviceID2Index;

    static void TouchAsNewest(const std::string& deviceID);
    static void KeepCacheMapSize();
};
#endif //SYSAGENT_LOCATION_H
