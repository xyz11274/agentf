#pragma once

#include <string>
#include "aisf_platform/parser/aisf_msg_parse.h"

using namespace AisfPlat;

namespace SYSAGENT {
enum class EventType {
        TASKASSIGNMENT,
        TASKSTOP,
        USERINPUT,
        USEROUTPUT,
        DATAEXCHANGE,
        TASKSTATUSMANAGEMENT,
        LOGINOUT,
        LOCATION,
        USERPORTRAITCONF,
        USERFEEDBACK
};

class AccessCtrl final {

public:
    string Parse(const char* in, size_t len);

    string toString() const;

    void setTaskID(const std::string& taskID);
    void setSender(const std::string& sender);
    void setReceiver(const std::string& receiver);
    void setClientExField(const std::string& clientExField);
    void setSystemAgentIP();

    std::string extractInput();
    std::string extractTaskID();
    std::string extractSender();
    std::string extractReceiver();
    std::string extractDeviceCategory();

    EventType eventType;
    MsgAisfHeader header{};

/*{"Header":{"MessageType":"UserTaskManagement","Sender":"APP","Receiver":"SystemAgent","UserName":"my username",
"DeviceId":"18091818573","DeviceCategory":"3","UUID":"uuid-1122233445566","ClientSessionId":"client session",
"AcmSessionId":"acm sssion","TaskID":"taskid-111","EventTimeStamp":"2025-01-09T14:30:15Z"},"Data":{"MessageType":
"TaskAssignment","UserInputText":"input text","UserInputObject":"input object","UserIntent":"user intent",
"UserSpecifiedAgent":"agent"},"Payload":{"Function":"function","Action":"action"}}*/
    MessageAisf<TaskAssignment, MsgPayload> taskAssignment{}; // 头：UserTaskManagement；体：TaskAssignment

/*{"Header":{"MessageType":"UserTaskManagement","Sender":"APP","Receiver":"SystemAgent","UserName":"my username",
"DeviceId":"18091818573","DeviceCategory":"3","UUID":"uuid-1122233445566","ClientSessionId":"client session",
"AcmSessionId":"acm sssion","TaskID":"taskid-111","EventTimeStamp":"2025-01-09T14:30:15Z"},
"Data":{"MessageType":"TaskStop"},"Payload":{"Function":"function","Action":"action"}}*/
    MessageAisf<TaskStop, MsgPayload> taskStop{}; // 头：UserTaskManagement；体：TaskStop

/*{"Header":{"MessageType":"DataExchange","Sender":"APP","Receiver":"SystemAgent","UserName":"my username",
"DeviceId":"18091818573","DeviceCategory":"3","UUID":"uuid-1122233445566","ClientSessionId":"client session",
"AcmSessionId":"acm sssion","TaskID":"taskid-111","EventTimeStamp":"2025-01-09T14:30:15Z"},"Data":
{"MessageType":"UserInput","UserInputText":"input text","UserInputObject":"input object","UserIntent":"user intent",
"ImplicitInputFlag":true},"Payload":{"Function":"function","Action":"action"}}*/
    MessageAisf<UserInput, MsgPayload> userInput{}; // 头：DataExchange；体：UserInput

/*{"Header":{"MessageType":"DataExchange","Sender":"APP","Receiver":"SystemAgent","UserName":"my username",
"DeviceId":"18091818573","DeviceCategory":"3","UUID":"uuid-1122233445566","ClientSessionId":"client session",
"AcmSessionId":"acm sssion","TaskID":"taskid-111","EventTimeStamp":"2025-01-09T14:30:15Z"},"Data":
{"MessageType":"UserOutput","UserOutputText":"output text","UserOutputObject":"output object",
"UserManualInterventionFlag":true,"ImplicitOutputFlag":true},"Payload":{"Function":"function","Action":"action"}}*/
    MessageAisf<UserOutput, MsgPayload> userOutput{}; // 头：DataExchange；体：UserOutput

/*{"Header":{"MessageType":"DataExchange","Sender":"APP","Receiver":"SystemAgent","UserName":"my username",
"DeviceId":"18091818573","DeviceCategory":"3","UUID":"uuid-1122233445566","ClientSessionId":"client session",
"AcmSessionId":"acm sssion","TaskID":"taskid-111","EventTimeStamp":"2025-01-09T14:30:15Z"},"Data":
{"MessageType":"DataExchage","UserOutputText":"output text"},"Payload":{"Function":"function","Action":"action"}}*/
    MessageAisf<DataExchange, MsgPayload> dataExchange{}; // 头：DataExchange；体：DataExchange

/*{"Header":{"MessageType":"TaskStatusManagement","Sender":"APP","Receiver":"SystemAgent","UserName":"my username",
"DeviceId":"18091818573","DeviceCategory":"3","UUID":"uuid-1122233445566","ClientSessionId":"client session",
"AcmSessionId":"acm sssion","TaskID":"taskid-111","EventTimeStamp":"2025-01-09T14:30:15Z"},"Data":
{"MessageType":"QueryTaskStatus","TaskStatusCode":1,"TaskStatusReport":"TaskStart"},
"Payload":{"Function":"function","Action":"action"}}*/
    MessageAisf<TaskStatusManagement, MsgPayload> taskStatusManagement{}; // 头：TaskStatusManagement；体：无

    MessageAisf<TaskStatusReport, MsgPayload> taskStatusReport{};
    MessageAisf<QueryTaskStatus, MsgPayload> queryTaskStatus{};

    MessageAisf<LoginOut, MsgPayload> loginOut{};

    MessageAisf<LocationInfo, MsgPayload> locationInfo{};

    MessageAisf<UserPortrait, MsgPayload> userPortrait{};

    MessageAisf<UserFeedback, MsgPayload> userFeedback{};
};
}