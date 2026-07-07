//
// Created by l00889076 on 2026/1/5.
//

#include "queue/Lifecycle/UserFeedbackHandler.h"
#include "queue/QueueManager.h"

void UserFeedbackHandler::handle(const shared_ptr<SYSAGENT::AccessCtrl>& accessCtrl)
{
    LOG_INFO_EX("enter user handle, userName is:%s", accessCtrl->header.userName.c_str());
    std::string userId = accessCtrl->header.userName.c_str();
    auto& data = accessCtrl->userFeedback.data;
    string satisfiedStr = data.satisfied ? "satisfied" : "dissatisfied";

    // 按照指定格式进行打点
    QueueManager::getInstance().EventMark("info", "SysAgent.UserFeedback", satisfiedStr,
        data.details, accessCtrl, "event-feedback");
}