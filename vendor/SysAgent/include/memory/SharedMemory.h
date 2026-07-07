//
// Created by c00888021 on 2025/11/15.
//

#ifndef SYSAGENT_SHAREDMEMORY_H
#define SYSAGENT_SHAREDMEMORY_H

#include <vector>
#include <map>
#include <string>
#include "json/value.h"
#include "unordered_map"
#include <mutex>
#include <future>

template <typename T>
void AddFilterField(std::unordered_map<std::string, std::string>& map, const T& filter, const std::string& fieldName,
                    const std::string T::*fieldPtr)
{
    const auto& value = filter.*fieldPtr;
    if (!value.empty()) {
        map[fieldName] = value;
    }
}

struct ShortMemory {
    std::string userName;
    std::string workFlowId;
    std::string taskId;
    std::string role;
    std::string createTime;
    std::string source;
    std::string content;
};

class SharedMemory {
public:
    static void AddSharedMemory(std::vector<ShortMemory>& context, std::string& taskID);
    static void QuerySharedMemory(std::vector<ShortMemory>& context, ShortMemory shortMemory);
    static void RefreshSharedMemory(std::vector<ShortMemory>& context, ShortMemory shortMemory);
    static void GetContext(std::vector<std::map<std::string, std::string>>& context, ShortMemory shortMemory);
    static std::vector<std::string> getIndexArray(const std::string& taskID);
    static std::map<std::string, uint32_t> workflowCounts_;
    static std::mutex mutex_;

    static uint32_t totalSize_;
    static uint32_t summarySize_;
    static uint32_t roundSize_;
    static std::unordered_map<std::string, std::string> BuildQueryFilter(ShortMemory filter);
    static void ConvertToVector(Json::Value& data, std::vector<ShortMemory>& context);
    static void ConvertToJSON(const std::vector<ShortMemory>& context, Json::Value& dataArray);
    static std::vector<ShortMemory> ParseJsonToShortMemory(const std::string& json, ShortMemory shortMemory);
    static std::vector<uint32_t> GetSharedMemoryQuantity(std::shared_ptr<std::vector<ShortMemory>> memory);
    static bool CheckContextSize(std::shared_ptr<std::vector<ShortMemory>> memoryList);
};

#endif // SYSAGENT_SHAREDMEMORY_H
