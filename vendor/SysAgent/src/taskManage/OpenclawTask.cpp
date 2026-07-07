//
// Created by c00888021 on 2025/11/22.
//

#include <utility>
#include <exception>

#include "taskManage/OpenclawTask.h"
#include "taskManage/TeHelper.h"
#include "utils/common_func.h"
#include "utils/http_client.h"
#include "utils/env_util.h"

OpenclawTask::OpenclawTask(std::string &taskID, std::string &agentName, std::string &requestContent)
{
    this->taskID = taskID;
    this->agentName = agentName;
    this->requestContent = requestContent;
}

/**
 * 从通通助手发送消息上来，匹配到openclaw后，SystemAgent转发给SDK
 * 从SDK上来消息
 * @param accessCtrl accessCtrl
 */
void OpenclawTask::exec(shared_ptr<SYSAGENT::AccessCtrl> accessCtrl)
{
    auto sender = accessCtrl->extractSender();
    if (sender == "LTServer") {
        LOG_ERR_EX("[OpenclawTask][TLServer]: %s", accessCtrl->toString().c_str());
        accessCtrl->setSender("APP");
        accessCtrl->setReceiver("APP");
        accessCtrl->header.messageType = "Openclaw";
        accessCtrl->setTaskID(this->taskID);
        TEHelper::sendUserOutputMsg(accessCtrl, accessCtrl->extractInput(), UserOutput_Completed, 0);
        return;
    }
    if (sender == "APP") {
        LOG_ERR_EX("[OpenclawTask][APP]: %s", accessCtrl->toString().c_str());
        accessCtrl->setSender("LTServer");
        accessCtrl->setReceiver("LTServer");
        std::string data = accessCtrl->extractInput();
        try {
            auto index = data.find_first_of("|||");
            if (index > 0) {
                data = data.substr(index + CMFrm::Constants::Const3);
            }
        } catch (const std::exception &e) {
            LOG_ERR_EX("OpenclawTask exception: %s", e.what());
        }

        TEHelper::sendUserOutputMsg(accessCtrl, data, UserOutput_Completed, 0);
        return;
    }
    LOG_ERR_EX("[OpenclawTask][ERROR]: %s", accessCtrl->toString().c_str());
}

void OpenclawTask::stop(shared_ptr<SYSAGENT::AccessCtrl> accessCtrl)
{
}
