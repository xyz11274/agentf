/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 */
//
// Created by z00913804 on 25-12-8.
//

#ifndef SYSAGENT_TOOL_H
#define SYSAGENT_TOOL_H
#include <cstdint>
#include <string>
#include "mmagentflow/mm_tool_manager.h"
#include "aisf_platform/parser/json_parser.h"
class Tool {
public:
    static bool CallTool(const std::string &toolName, const Json::Value &param, Json::Value& result);
    static bool ListTools(std::vector<mmsdk::MMListToolResult> &tools);
private:
    static std::shared_ptr<mmsdk::MMToolClient> GetToolClient();
    static std::shared_ptr<mmsdk::MMToolClient> toolClientInstance;
    static std::once_flag toolClientOnce;
};
#endif // SYSAGENT_TOOL_H
