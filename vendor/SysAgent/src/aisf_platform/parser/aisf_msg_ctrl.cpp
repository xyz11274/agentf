#include "framework_common/utils/env_util.h"

#include "aisf_platform/parser/aisf_msg_ctrl.h"
#include "aisf_platform/utils/utils.h"

namespace SYSAGENT {
using namespace std;

string AccessCtrl::Parse(const char* in, size_t len)
{
    CHECK_NULL_LOG_ERROR_RETURN(in, "input is null", "input is null");
    CHECK_TRUE_LOG_ERROR_RETURN(len == 0, "input is zero", "input is zero");
    string req(in, len);

    MsgAisfTotal msgTotal{};
    CHECK_FALSE_RETURN(msgTotal.Parse(in, len), "MsgTotal error");
    header = msgTotal.header;

    const auto& headerMsgType = msgTotal.header.messageType;
    const auto& dataMsgType = msgTotal.dataMsgType;
    auto rst = false;

    if (headerMsgType == "UserTaskManagement") {
        if (dataMsgType == "TaskAssignment") {
            eventType = EventType::TASKASSIGNMENT;
            rst = taskAssignment.Parse(req, msgTotal.header, msgTotal.dataJson, msgTotal.payloadJson);
        } else if (dataMsgType == "TaskStop") {
            eventType = EventType::TASKSTOP;
            rst = taskStop.Parse(req, msgTotal.header, msgTotal.dataJson, msgTotal.payloadJson);
        } else {
            return "UserTaskManagement unknown:" + headerMsgType;
        }
    } else if (headerMsgType == "DataExchange") {
        if (dataMsgType == "UserInput") {
            eventType = EventType::USERINPUT;
            rst = userInput.Parse(req, msgTotal.header, msgTotal.dataJson, msgTotal.payloadJson);
        } else if (dataMsgType == "UserOutput") {
            eventType = EventType::USEROUTPUT;
            rst = userOutput.Parse(req, msgTotal.header, msgTotal.dataJson, msgTotal.payloadJson);
        } else if (dataMsgType == "DataExchange") {
            eventType = EventType::DATAEXCHANGE;
            rst = dataExchange.Parse(req, msgTotal.header, msgTotal.dataJson, msgTotal.payloadJson);
        } else {
            return "DataExchange unknown:" + headerMsgType;
        }
    } else if (headerMsgType == "TaskStatusManagement") {
        eventType = EventType::TASKSTATUSMANAGEMENT;
        rst = taskStatusManagement.Parse(req, msgTotal.header, msgTotal.dataJson, msgTotal.payloadJson);
    } else if (headerMsgType == "LifecycleManagement") {
        if (dataMsgType == "LoginOut") {
            eventType = EventType::LOGINOUT;
            rst = loginOut.Parse(req, msgTotal.header, msgTotal.dataJson, msgTotal.payloadJson);
        } else if (dataMsgType == "LocationInfo") {
            eventType = EventType::LOCATION;
            rst = locationInfo.Parse(req, msgTotal.header, msgTotal.dataJson, msgTotal.payloadJson);
        } else if (dataMsgType == "UserPortraitConf") {
            eventType = EventType::USERPORTRAITCONF;
            rst = userPortrait.Parse(req, msgTotal.header, msgTotal.dataJson, msgTotal.payloadJson);
        } else if (dataMsgType == "UserFeedback") {
            eventType = EventType::USERFEEDBACK;
            rst = userFeedback.Parse(req, msgTotal.header, msgTotal.dataJson, msgTotal.payloadJson);
        }
        else {
            return "LifecycleManagement unknown: " + headerMsgType + ", dataMsgType: " + dataMsgType;
        }
        /* code */
    } else {
        return "unknown:" + headerMsgType;
    }
    if (rst) {
        return "";
    }
    return headerMsgType + "_" + dataMsgType + ": failure";
}

string AccessCtrl::toString() const
{
    string msgData;
    switch (eventType) {
        case EventType::TASKASSIGNMENT: {
            msgData = taskAssignment.ToJson();
            break;
        }
        case EventType::TASKSTOP: {
            msgData = taskStop.ToJson();
            break;
        }
        case EventType::USERINPUT: {
            msgData = userInput.ToJson();
            break;
        }
        case EventType::USEROUTPUT: {
            msgData = userOutput.ToJson();
            break;
        }
        case EventType::DATAEXCHANGE: {
            msgData = dataExchange.ToJson();
            break;
        }
        case EventType::TASKSTATUSMANAGEMENT: {
            msgData = taskStatusManagement.ToJson();
            break;
        }
        case EventType::LOGINOUT: {
            msgData = loginOut.ToJson();
            break;
        }
        case EventType::LOCATION: {
            msgData = locationInfo.ToJson();
            break;
        }
        case EventType::USERPORTRAITCONF: {
            msgData = userPortrait.ToJson();
            break;
        }
        default:
            msgData = "";
            break;
    }
    return msgData;
}

void AccessCtrl::setTaskID(const std::string& taskID)
{
    header.taskID = taskID;
    switch (eventType) {
        case SYSAGENT::EventType::TASKASSIGNMENT:
            taskAssignment.header.taskID = taskID;
            break;
        case SYSAGENT::EventType::USERINPUT:
            userInput.header.taskID = taskID;
            break;
        case SYSAGENT::EventType::DATAEXCHANGE:
            dataExchange.header.taskID = taskID;
            break;
        case SYSAGENT::EventType::TASKSTATUSMANAGEMENT:
            taskStatusManagement.header.taskID = taskID;
            break;
        case SYSAGENT::EventType::USEROUTPUT:
            userOutput.header.taskID = taskID;
            break;
        case SYSAGENT::EventType::TASKSTOP:
            taskStop.header.taskID = taskID;
            break;
        case SYSAGENT::EventType::LOGINOUT:
            loginOut.header.taskID = taskID;
            break;
        case SYSAGENT::EventType::LOCATION:
            locationInfo.header.taskID = taskID;
            break;
        case SYSAGENT::EventType::USERPORTRAITCONF:
            userPortrait.header.taskID = taskID;
            break;
        default:
            LOG_INFO_EX("Could not extract user information from unknown event type: %d, using default values",
                        static_cast<int>(eventType));
    }
}

void AccessCtrl::setSender(const std::string& sender)
{
    header.sender = sender;
    switch (eventType) {
        case SYSAGENT::EventType::TASKASSIGNMENT:
            taskAssignment.header.sender = sender;
            break;
        case SYSAGENT::EventType::USERINPUT:
            userInput.header.sender = sender;
            break;
        case SYSAGENT::EventType::DATAEXCHANGE:
            dataExchange.header.sender = sender;
            break;
        case SYSAGENT::EventType::TASKSTATUSMANAGEMENT:
            taskStatusManagement.header.sender = sender;
            break;
        case SYSAGENT::EventType::USEROUTPUT:
            userOutput.header.sender = sender;
            break;
        case SYSAGENT::EventType::TASKSTOP:
            taskStop.header.sender = sender;
            break;
        case SYSAGENT::EventType::LOGINOUT:
            loginOut.header.sender = sender;
            break;
        case SYSAGENT::EventType::LOCATION:
            locationInfo.header.sender = sender;
            break;
            // 添加其他类
        case SYSAGENT::EventType::USERPORTRAITCONF:
            userPortrait.header.sender = sender;
            break;
        default:
            LOG_INFO_EX("Could not extract user information from unknown event type: %d, using default values",
                        static_cast<int>(eventType));
    }
}

void AccessCtrl::setReceiver(const std::string& receiver)
{
    header.receiver = receiver;
    switch (eventType) {
        case SYSAGENT::EventType::TASKASSIGNMENT:
            taskAssignment.header.receiver = receiver;
            break;
        case SYSAGENT::EventType::USERINPUT:
            userInput.header.receiver = receiver;
            break;
        case SYSAGENT::EventType::DATAEXCHANGE:
            dataExchange.header.receiver = receiver;
            break;
        case SYSAGENT::EventType::TASKSTATUSMANAGEMENT:
            taskStatusManagement.header.receiver = receiver;
            break;
        case SYSAGENT::EventType::USEROUTPUT:
            userOutput.header.receiver = receiver;
            break;
        case SYSAGENT::EventType::TASKSTOP:
            taskStop.header.receiver = receiver;
            break;
        case SYSAGENT::EventType::LOGINOUT:
            loginOut.header.receiver = receiver;
            break;
        case SYSAGENT::EventType::LOCATION:
            locationInfo.header.receiver = receiver;
            break;
            // 添加其他类
        case SYSAGENT::EventType::USERPORTRAITCONF:
            userPortrait.header.receiver = receiver;
            break;
        default:
            LOG_INFO_EX("Could not extract user information from unknown event type: %d, using default values",
                        static_cast<int>(eventType));
    }
}

void AccessCtrl::setClientExField(const std::string& clientExField)
{
    header.clientExField = clientExField;
    switch (eventType) {
        case SYSAGENT::EventType::TASKASSIGNMENT:
            taskAssignment.header.clientExField = clientExField;
            break;
        case SYSAGENT::EventType::USERINPUT:
            userInput.header.clientExField = clientExField;
            break;
        case SYSAGENT::EventType::DATAEXCHANGE:
            dataExchange.header.clientExField = clientExField;
            break;
        case SYSAGENT::EventType::TASKSTATUSMANAGEMENT:
            taskStatusManagement.header.clientExField = clientExField;
            break;
        case SYSAGENT::EventType::USEROUTPUT:
            userOutput.header.clientExField = clientExField;
            break;
        case SYSAGENT::EventType::TASKSTOP:
            taskStop.header.clientExField = clientExField;
            break;
        case SYSAGENT::EventType::LOGINOUT:
            loginOut.header.clientExField = clientExField;
            break;
        case SYSAGENT::EventType::LOCATION:
            locationInfo.header.sender = clientExField;
            break;
            // 添加其他类
        case SYSAGENT::EventType::USERPORTRAITCONF:
            userPortrait.header.sender = clientExField;
            break;
        default:
            LOG_INFO_EX("Could not extract user information from unknown event type: %d, using default values",
                        static_cast<int>(eventType));
    }
}

void AccessCtrl::setSystemAgentIP()
{
    std::string ip = Utils::GetBaseIp();
    switch (eventType) {
        case SYSAGENT::EventType::TASKASSIGNMENT:
            taskAssignment.header.sysAgentEntity = ip;
            break;
        case SYSAGENT::EventType::USERINPUT:
            userInput.header.sysAgentEntity = ip;
            break;
        case SYSAGENT::EventType::DATAEXCHANGE:
            dataExchange.header.sysAgentEntity = ip;
            break;
        case SYSAGENT::EventType::TASKSTATUSMANAGEMENT:
            taskStatusManagement.header.sysAgentEntity = ip;
            break;
        case SYSAGENT::EventType::USEROUTPUT:
            userOutput.header.sysAgentEntity = ip;
            break;
        case SYSAGENT::EventType::TASKSTOP:
            taskStop.header.sysAgentEntity = ip;
            break;
            // 添加其他类
        default:
            LOG_INFO_EX("Could not extract user information from unknown event type: %d, using default values",
                        static_cast<int>(eventType));
    }
}

std::string AccessCtrl::extractInput()
{
    std::string request;
    if (this->eventType == SYSAGENT::EventType::TASKASSIGNMENT) {
        request = this->taskAssignment.data.userInputText;
    } else if (this->eventType == SYSAGENT::EventType::USERINPUT) {
        request = this->userInput.data.userInputText;
    }
    LOG_INFO_EX("[HandlerManager::extractInput] request: %s", request.c_str());
    return request;
}

std::string AccessCtrl::extractTaskID()
{
    std::string taskID;
    if (this->eventType == SYSAGENT::EventType::TASKASSIGNMENT) {
        taskID = this->taskAssignment.header.taskID;
    } else if (this->eventType == SYSAGENT::EventType::USERINPUT) {
        taskID = this->userInput.header.taskID;
    }
    LOG_INFO_EX("taskID: %s", taskID.c_str());
    return taskID;
}

std::string AccessCtrl::extractSender() { return header.sender; }
std::string AccessCtrl::extractReceiver() { return header.receiver; }
std::string AccessCtrl::extractDeviceCategory() { return header.deviceCategory; }

} // namespace SYSAGENT