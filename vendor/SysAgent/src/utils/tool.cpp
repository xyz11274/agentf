//
// Created by c00888021 on 2025/12/12.
//
#include "framework_common/logger/logger.h"
#include "utils/tool.h"
#include "aisf_platform/parser/json_parser.h"

namespace SYSAGENT {

std::shared_ptr<mmsdk::MMToolClient> Tool::toolClientInstance = nullptr;
std::once_flag Tool::toolClientOnce;

std::shared_ptr<mmsdk::MMToolClient> Tool::GetToolClient()
{
    std::call_once(toolClientOnce,
                   []() {
                       toolClientInstance = std::make_shared<mmsdk::MMToolClient>();
                   });
    return toolClientInstance;
}

bool Tool::CallTool(const std::string &toolName, const Json::Value &param, Json::Value &result)
{
    mmsdk::MMCallToolResult toolResults;
    auto retCode = GetToolClient()->CallTool(toolName, AisfPlat::JsonParser::ToRawString(param), toolResults);
    if (retCode || toolResults.content.empty()) {
        CMFrm_Error("Invoke tool failed, tool name: %s, result size: %d", toolName.c_str(), toolResults.content.size());
        return false;
    }
    if (!AisfPlat::JsonParser::Parse(toolResults.content[0].textContent.text.c_str(), result)) {
        CMFrm_Error("Invoke tool failed, tool name: %s, result: %s",
                    toolName.c_str(), toolResults.content[0].textContent.text.c_str());
        return false;
    }
    CMFrm_Info("Invoke tool success, tool name: %s, result: %s",
               toolName.c_str(), toolResults.content[0].textContent.text.c_str());
    return true;
}

}