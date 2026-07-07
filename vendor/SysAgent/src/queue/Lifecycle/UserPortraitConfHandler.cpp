//
// Created by c00888021 on 2025/8/25.
//
#include "queue/Lifecycle/UserPortraitConfHandler.h"
#include "memory/History.h"

void UserPortraitConfHandler::handle(const shared_ptr<SYSAGENT::AccessCtrl>& accessCtrl)
{
    LOG_INFO_EX("enter user handle, userName is:%s", accessCtrl->header.userName.c_str());
    std::string userId = accessCtrl->header.userName.c_str();
    auto data = accessCtrl->userPortrait.data.params;
    string role = "";
    JsonParser::GetString(data, "voiceId", role);
    role = role.empty() ? "" : std::string("_" + role);
    std::string url(URL_MEMORY_SCENE);
    UpdateMemory(userId + role, MemorizeMessage{.category = "movie", .custom_info = data}, url, accessCtrl->header.taskID);
}
