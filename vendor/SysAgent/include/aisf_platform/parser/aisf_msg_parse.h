#pragma once

#include "aisf_platform/utils/utils.h"

namespace AisfPlat {

struct MsgAisfHeader {
    string messageType; // 必选
    string sender; // 必选
    string receiver; // 必选
    string userName; // 必选
    string deviceId; // 必选
    string teamEnvironmentID; // Optional
    string deviceCategory; // 必选
    string uuid; // 必选
    string clientSessionId; // 必选
    string acmSessionId; // Optional
    string taskID; // Optional
    string eventTimeStamp; // 必选
    string sysAgentEntity; // Optional SystemAgent实例IP信息
    string clientExField; // Optional LTServer 和 AISF restful通信消息关联标识

    [[nodiscard]] bool Parse(const Json::Value& root);

    void ToJson(Json::Value& jsonObj) const;
};

struct MsgPayload {
    // 保存原始的Payload JSON对象
    Json::Value originalPayload;

    string function; // 必选
    string action; // Optional

    [[nodiscard]] bool Parse(const Json::Value& root);

    void ToJson(Json::Value& jsonObj) const;
};

struct TaskAssignment {
    string messageType{"TaskAssignment"}; // 必选
    string userInputText; // Optional
    string userInputObject; // Optional
    string userIntent; // Optional
    string userSpecifiedAgent; // Optional
    string APPName; // Optional
    string RewriteQ;
    string LongHistory;
    string toolName;
    std::string isHaveSendHuoShan;

    [[nodiscard]] bool Parse(const Json::Value& root);

    void ToJson(Json::Value& jsonObj) const;
};

struct TaskStop {
    string messageType{"TaskStop"}; // 必选
    string userInputText; // Optional
    string userInputObject; // Optional
    string userIntent; // Optional
    string userSpecifiedAgent; // Optional

    [[nodiscard]] bool Parse(const Json::Value& root);

    void ToJson(Json::Value& jsonObj) const;
};

struct UserInput {
    string messageType{"UserInput"}; // 必选
    string userInputText; // Optional
    string userInputObject; // Optional
    string userIntent; // Optional
    bool implicitInputFlag; // Optional

    [[nodiscard]] bool Parse(const Json::Value& root);

    void ToJson(Json::Value& jsonObj) const;
};


#define UserOutput_Completed "Completed"
struct UserOutput {
    string messageType{"UserOutput"}; // 必选
    string userOutputText; // Optional
    string userOutputObject; // Optional
    string streamStatus; //Optional
    int userOutPutFlag{-1};
    bool userManualInterventionFlag; // Optional
    bool implicitOutputFlag; // Optional
    int isVoiceBroadcast{0}; // Optional

    [[nodiscard]] bool Parse(const Json::Value& root);

    void ToJson(Json::Value& jsonObj) const;
};

struct DataExchange {
    string messageType{"DataExchange"}; // 必选
    string userOutputText; // Optional

    [[nodiscard]] bool Parse(const Json::Value& root);

    void ToJson(Json::Value& jsonObj) const;
};

struct QueryTaskStatus {
    string messageType{"QueryTaskStatus"}; // 必选
    string taskStatusCode; // Optional
    string taskStatusReport; // Optional

    bool Parse(const Json::Value& root);

    void ToJson(Json::Value& jsonObj) const;
};

#define TaskStatusReport_TaskCompleted "TaskCompleted"
struct TaskStatusReport {
    string messageType{"TaskStatusReport"}; // 必选
    string taskStatusCode; // Optional
    string taskStatusReport; // Optional

    bool Parse(const Json::Value& root);

    void ToJson(Json::Value& jsonObj) const;
};

struct TaskStatusManagement {
    string messageType; // 必选
    int taskStatusCode; // Optional
    string taskStatusReport; // Optional
    string userOutputText; // Optional — 外部 SysAgent 合并帧时嵌入

    [[nodiscard]] bool Parse(const Json::Value& root);

    void ToJson(Json::Value& jsonObj) const;
};

struct LoginOut {
    string messageType; // 必选: TokenRefresh/LoginOut

    [[nodiscard]] bool Parse(const Json::Value& root);

    void ToJson(Json::Value& jsonObj) const;
};

struct LocationInfo {
    string messageType{"LocationInfo"};
    string latitude;
    string longitude;
    string instanceNum;
    [[nodiscard]] bool Parse(const Json::Value& root);

    void ToJson(Json::Value& jsonObj) const;
};

struct UserPortrait {
    string messageType{"UserPortraitConf"};
    Json::Value params;
    [[nodiscard]] bool Parse(const Json::Value& root);

    void ToJson(Json::Value& jsonObj) const;
};

struct UserFeedback {
    string messageType{"UserFeedback"};
    bool satisfied{true};
    std::string details;
    [[nodiscard]] bool Parse(const Json::Value& root);

    void ToJson(Json::Value& jsonObj) const;
};

struct MsgAisfTotal {
    MsgAisfHeader header;
    Json::Value rootJson;
    const Json::Value* dataJson{nullptr};
    const Json::Value* payloadJson{nullptr};
    string dataMsgType;

    [[nodiscard]] bool Parse(const char* in, size_t len);

    [[nodiscard]] string ToJson() const;
};

template<typename DATA, typename PAYLOAD>
class MessageAisf final {
DISALLOW_COPY_AND_ASSIGN(MessageAisf)

public:
    MessageAisf() = default;

    ~MessageAisf() = default;

    [[nodiscard]] bool Parse(const string& req, const MsgAisfHeader& msgHeader, const Json::Value* dataJson,
                             const Json::Value* payloadJson)
    {
        CHECK_NULL_RETURN(dataJson, false);

        reqStr = req;
        header = msgHeader;

        CHECK_FALSE_RETURN(data.Parse(*dataJson), false);

        CHECK_NULL_RETURN(payloadJson, true);
        return payload.Parse(*payloadJson);
    }

    [[nodiscard]] string ToJson() const
    {
        Json::Value root;

        Json::Value jsonHeader;
        header.ToJson(jsonHeader);
        root["Header"] = jsonHeader;

        Json::Value jsonData;
        data.ToJson(jsonData);
        root["Data"] = jsonData;

        // 修改判断条件，检查originalPayload是否为空
        if (!payload.originalPayload.isNull()) {
            Json::Value jsonPayload;
            payload.ToJson(jsonPayload);
            root["Payload"] = jsonPayload;
        }

        return JsonParser::ToRawString(root);
    }

public:
    string reqStr{};
    MsgAisfHeader header{};
    DATA data{};
    PAYLOAD payload{};
};
}