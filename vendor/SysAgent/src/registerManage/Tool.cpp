//
// Created by z00913804 on 25-12-8.
//
#include "registerManage/Tool.h"

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
    auto retCode = GetToolClient()->CallTool(toolName, AisfPlat::JsonParser::ToPrettyString(param), toolResults);
    if (!retCode) {
        CMFrm_Error("Invoke tool failed, tool name: %s, result size: %d", toolName.c_str(), toolResults.content.size());
        return false;
    }
    if (toolResults.content.empty() || !AisfPlat::JsonParser::Parse(toolResults.content[0].textContent.text.c_str(), result)) {
        CMFrm_Error("Invoke tool failed, tool name: %s, result size: %d", toolName.c_str(), toolResults.content.size());
        return false;
    }
    CMFrm_Info("Invoke tool success, tool name: %s, result: %s",
               toolName.c_str(), toolResults.content[0].textContent.text.c_str());
    return true;
}

bool Tool::ListTools(std::vector<mmsdk::MMListToolResult> &tools)
{
    auto retCode = GetToolClient()->ListTools(tools);
    if (retCode != mmsdk::MMErrorCode::SUCCESS) {
        LOG_INFO_EX("查询所有工具失败, retCode : %d", retCode);
        return false;
    }
    if (tools.empty()) {
        LOG_INFO_EX("查询成功，但工具列表为空");
    }
    return true;
}
