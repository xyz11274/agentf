/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 */
//
// Created by z00913804 on 25-12-8.
//

#ifndef SYSAGENT_TOOLMANAGER_H
#define SYSAGENT_TOOLMANAGER_H
#include <vector>
#include "aisf_platform/thread/thread_safe_vector.h"
#include "aisf_platform/utils/utils.h"
#include "aisf_platform/thread/timer_task.h"
#include "mm_sdk.h"
#include "config/Config.h"
#define INTERVAL_SECOND 120
struct InputSchema {
    std::string type;
    std::vector<std::string> required;
    std::map<std::string, std::map<std::string, std::string>> properties;
};

struct MCPTool {
    std::string name;
    std::string description;
    InputSchema inputSchema;
};
class ToolManager {
public:
    static ToolManager& getInstance()
    {
        static ToolManager toolManager;
        return toolManager;
    }
    void init()
    {
        timer_ = std::make_unique<TimerTask>(INTERVAL_SECOND, [this]() {
            this->queryToolInfo(this->toolInfoCache);
        });
        timer_->start();
        LOG_INFO_EX("ToolManager init success");
    }
    std::vector<MCPTool> getToolList()
    {
        std::set<std::string> forbiddenTools = {"aisf_mcp_server_ctrip_search", "aisf_mcp_server_generates_image",
                                                "aisf_mcp_server_query_image_url", "aisf_mcp_server_cloud_video",
                                                "aisf_mcp_server_huawei_ai_search", "aisf_mcp_server_query_weather",
                                                "aisf_mcp_server_surround_search"};
        std::vector<MCPTool> toolList;
        toolInfoCache.forEach([&toolList, &forbiddenTools](const MCPTool& item) {
            if (forbiddenTools.find(item.name) == forbiddenTools.end()) {
                toolList.push_back(item);
            }
        });
        return toolList;
    }
    std::map<std::string, MCPTool> getToolMap()
    {
        std::map<std::string, MCPTool> toolMap;
        toolInfoCache.forEach([&toolMap](const MCPTool& item) {
            toolMap.insert(std::make_pair(item.name, item));
        });
        return toolMap;
    }
    std::map<std::string, ToolMessage> ConvertMCPToolsToToolMessages(const std::vector<MCPTool>& tools);

private:
    ThreadSafeVector<MCPTool> toolInfoCache;
    ToolManager(){};
    ~ToolManager() = default;
    ToolManager(const ToolManager&) = delete;
    ToolManager& operator=(const ToolManager&) = delete;
    std::unique_ptr<TimerTask> timer_; // 使用智能指针管理 TimerTask
    void queryToolInfo(ThreadSafeVector<MCPTool>& toolInfos);
};
#endif // SYSAGENT_TOOLMANAGER_H
