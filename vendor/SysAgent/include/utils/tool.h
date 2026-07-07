//
// Created by c00888021 on 2025/12/12.
//

#ifndef SYSAGENT_TOOL_H
#define SYSAGENT_TOOL_H

#include <string>
#include <mutex>
#include "mmagentflow/mm_tool_manager.h"
#include "json/json.h"

namespace SYSAGENT {

class Tool {
public:
    static bool CallTool(const std::string &toolName, const Json::Value &param, Json::Value& result);
private:
    static std::shared_ptr<mmsdk::MMToolClient> GetToolClient();
    static std::shared_ptr<mmsdk::MMToolClient> toolClientInstance;
    static std::once_flag toolClientOnce;
};

}

#endif //SYSAGENT_TOOL_H
