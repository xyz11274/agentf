#include "aisf_platform/parser/aisf_msg_parse.h"

namespace AisfPlat {
bool MsgAisfTotal::Parse(const char* in, size_t len)
{
    CHECK_FALSE_LOG_ERROR_RETURN(JsonParser::Parse(in, len, rootJson), false, "raw string parse error");

    auto headerJson = JsonParser::GetNode(rootJson, "Header");
    CHECK_NULL_LOG_ERROR_RETURN(headerJson, false, "Header get error");
    CHECK_FALSE_RETURN(header.Parse(*headerJson), false);

    dataJson = JsonParser::GetNode(rootJson, "Data");
    CHECK_NULL_LOG_ERROR_RETURN(dataJson, false, "Data get error");
    CHECK_FALSE_LOG_ERROR_RETURN(JsonParser::GetString(*dataJson, "MessageType", dataMsgType), false,
                                 "MessageType get error");

    payloadJson = JsonParser::GetNode(rootJson, "Payload");

    return true;
}

string MsgAisfTotal::ToJson() const
{
    Json::Value root;

    Json::Value jsonHeader;
    header.ToJson(jsonHeader); // Header 的acmSessionId 可能发生变化
    root["Header"] = jsonHeader;

    if (dataJson != nullptr) {
        root["Data"] = *dataJson; // Data 保持现状
    }
    if (payloadJson != nullptr) {
        root["Payload"] = *payloadJson;  // Payload 保持现状
    }

    return JsonParser::ToRawString(root);
}

bool MsgAisfHeader::Parse(const Json::Value& root)
{
    CHECK_FALSE_LOG_ERROR_RETURN(JsonParser::GetString(root, "MessageType", messageType), false,
                                 "MessageType get error");
    CHECK_FALSE_LOG_ERROR_RETURN(JsonParser::GetString(root, "Sender", sender), false, "Sender get error");
    CHECK_FALSE_LOG_ERROR_RETURN(JsonParser::GetString(root, "Receiver", receiver), false, "Receiver get error");
    CHECK_FALSE_LOG_ERROR_RETURN(JsonParser::GetString(root, "UserName", userName), false, "UserName get error");
    JsonParser::GetString(root, "TeamEnvironmentID", teamEnvironmentID);
    CHECK_FALSE_LOG_ERROR_RETURN(JsonParser::GetString(root, "DeviceId", deviceId), false, "DeviceId get error");
    CHECK_FALSE_LOG_ERROR_RETURN(JsonParser::GetString(root, "DeviceCategory", deviceCategory), false,
                                 "DeviceCategory get error");
    CHECK_FALSE_LOG_ERROR_RETURN(JsonParser::GetString(root, "UUID", uuid), false, "UUID get error");
    CHECK_FALSE_LOG_ERROR_RETURN(JsonParser::GetString(root, "ClientSessionId", clientSessionId), false,
                                 "ClientSessionId get error");
    JsonParser::GetString(root, "AcmSessionId", acmSessionId);
    JsonParser::GetString(root, "TaskID", taskID);
    CHECK_FALSE_LOG_ERROR_RETURN(JsonParser::GetString(root, "EventTimeStamp", eventTimeStamp), false,
                                 "EventTimeStamp get error");
    JsonParser::GetString(root, "SysAgentEntity", sysAgentEntity);
    JsonParser::GetString(root, "ClientExField", clientExField);
    return true;
}

void MsgAisfHeader::ToJson(Json::Value& jsonObj) const
{
    jsonObj["MessageType"] = messageType;
    jsonObj["Sender"] = sender;
    jsonObj["Receiver"] = receiver;
    jsonObj["UserName"] = userName;
    jsonObj["TeamEnvironmentID"] = teamEnvironmentID;
    jsonObj["DeviceId"] = deviceId;
    jsonObj["DeviceCategory"] = deviceCategory;
    jsonObj["UUID"] = uuid;
    jsonObj["ClientSessionId"] = clientSessionId;
    jsonObj["AcmSessionId"] = acmSessionId;
    jsonObj["TaskID"] = taskID;
    jsonObj["EventTimeStamp"] = eventTimeStamp;
    jsonObj["SysAgentEntity"] = sysAgentEntity;
    jsonObj["ClientExField"] = clientExField;
}

bool MsgPayload::Parse(const Json::Value& root)
{
    // 保存整个原始Payload
    originalPayload = root;

    JsonParser::GetString(root, "Function", function);
    JsonParser::GetString(root, "Action", action);
    return true;
}

void MsgPayload::ToJson(Json::Value& jsonObj) const
{
    jsonObj = originalPayload;
}

bool TaskAssignment::Parse(const Json::Value& root)
{
    JsonParser::GetString(root, "UserInputText", userInputText);
    JsonParser::GetString(root, "UserInputObject", userInputObject);
    JsonParser::GetString(root, "UserIntent", userIntent);
    JsonParser::GetString(root, "UserSpecifiedAgent", userSpecifiedAgent);
    JsonParser::GetString(root, "APPName", APPName);
    return true;
}

void TaskAssignment::ToJson(Json::Value& jsonObj) const
{
    jsonObj["MessageType"] = messageType;
    jsonObj["UserInputText"] = userInputText;
    jsonObj["UserInputObject"] = userInputObject;
    jsonObj["UserIntent"] = userIntent;
    jsonObj["UserSpecifiedAgent"] = userSpecifiedAgent;
    jsonObj["APPName"] = APPName;
    jsonObj["RewriteQ"] = RewriteQ;
    jsonObj["LongHistory"] = LongHistory;
    jsonObj["ToolName"] = toolName;
    jsonObj["IsHaveSendHuoShan"] = isHaveSendHuoShan;
}

bool UserInput::Parse(const Json::Value& root)
{
    JsonParser::GetString(root, "UserInputText", userInputText);
    JsonParser::GetString(root, "UserInputObject", userInputObject);
    JsonParser::GetString(root, "UserIntent", userIntent);
    JsonParser::GetBool(root, "ImplicitInputFlag", implicitInputFlag);
    return true;
}

void UserInput::ToJson(Json::Value& jsonObj) const
{
    jsonObj["MessageType"] = messageType;
    jsonObj["UserInputText"] = userInputText;
    jsonObj["UserInputObject"] = userInputObject;
    jsonObj["UserIntent"] = userIntent;
    jsonObj["ImplicitInputFlag"] = implicitInputFlag;
}

bool TaskStop::Parse(const Json::Value& root)
{
    JsonParser::GetString(root, "UserInputText", userInputText);
    JsonParser::GetString(root, "UserInputObject", userInputObject);
    JsonParser::GetString(root, "UserIntent", userIntent);
    JsonParser::GetString(root, "UserSpecifiedAgent", userSpecifiedAgent);
    return true;
}

void TaskStop::ToJson(Json::Value& jsonObj) const
{
    jsonObj["MessageType"] = messageType;
    jsonObj["UserInputText"] = userInputText;
    jsonObj["UserInputObject"] = userInputObject;
    jsonObj["UserIntent"] = userIntent;
    jsonObj["UserSpecifiedAgent"] = userSpecifiedAgent;
}

bool UserOutput::Parse(const Json::Value& root)
{
    JsonParser::GetString(root, "UserOutputText", userOutputText);
    JsonParser::GetString(root, "UserOutputObject", userOutputObject);
    JsonParser::GetBool(root, "UserManualInterventionFlag", userManualInterventionFlag);
    JsonParser::GetBool(root, "ImplicitOutputFlag", implicitOutputFlag);
    JsonParser::GetString(root, "StreamStatus", streamStatus);
    JsonParser::GetInt(root, "UserOutputFlag", userOutPutFlag);
    JsonParser::GetInt(root, "IsVoiceBroadcast", isVoiceBroadcast);
    return true;
}

void UserOutput::ToJson(Json::Value& jsonObj) const
{
    jsonObj["MessageType"] = messageType;
    jsonObj["UserOutputText"] = userOutputText;
    jsonObj["UserOutputObject"] = userOutputObject;
    jsonObj["UserManualInterventionFlag"] = userManualInterventionFlag;
    jsonObj["ImplicitOutputFlag"] = implicitOutputFlag;
    jsonObj["StreamStatus"] = streamStatus;
    jsonObj["UserOutputFlag"] = userOutPutFlag;
    jsonObj["IsVoiceBroadcast"] = isVoiceBroadcast;
}

bool DataExchange::Parse(const Json::Value& root)
{
    JsonParser::GetString(root, "UserOutputText", userOutputText);
    return true;
}

void DataExchange::ToJson(Json::Value& jsonObj) const
{
    jsonObj["MessageType"] = messageType;
    jsonObj["UserOutputText"] = userOutputText;
}

bool TaskStatusManagement::Parse(const Json::Value& root)
{
    CHECK_FALSE_LOG_ERROR_RETURN(JsonParser::GetString(root, "MessageType", messageType), false,
                                 "MessageType get error");
    JsonParser::GetInt(root, "TaskStatusCode", taskStatusCode);
    JsonParser::GetString(root, "TaskStatusReport", taskStatusReport);
    JsonParser::GetString(root, "UserOutputText", userOutputText);
    return true;
}

void TaskStatusManagement::ToJson(Json::Value& jsonObj) const
{
    jsonObj["MessageType"] = messageType;
    jsonObj["TaskStatusCode"] = taskStatusCode;
    jsonObj["TaskStatusReport"] = taskStatusReport;
    jsonObj["UserOutputText"] = userOutputText;
}

bool QueryTaskStatus::Parse(const Json::Value& root)
{
    JsonParser::GetString(root, "TaskStatusCode", taskStatusCode);
    JsonParser::GetString(root, "TaskStatusReport", taskStatusReport);
    return true;
}

void QueryTaskStatus::ToJson(Json::Value& jsonObj) const
{
    jsonObj["TaskStatusCode"] = taskStatusCode;
    jsonObj["TaskStatusReport"] = taskStatusReport;
}

bool TaskStatusReport::Parse(const Json::Value& root)
{
    JsonParser::GetString(root, "TaskStatusCode", taskStatusCode);
    JsonParser::GetString(root, "TaskStatusReport", taskStatusReport);
    return true;
}

void TaskStatusReport::ToJson(Json::Value& jsonObj) const
{
    jsonObj["TaskStatusCode"] = taskStatusCode;
    jsonObj["TaskStatusReport"] = taskStatusReport;
}

bool LoginOut::Parse(const Json::Value& root)
{
    CHECK_FALSE_LOG_ERROR_RETURN(JsonParser::GetString(root, "MessageType", messageType), false,
                                 "MessageType get error");
    return true;
}

void LoginOut::ToJson(Json::Value& jsonObj) const
{
    jsonObj["MessageType"] = messageType;
}

bool LocationInfo::Parse(const Json::Value& root) {
    JsonParser::GetString(root, "MessageType", messageType);
    JsonParser::GetString(root, "Latitude", latitude);
    JsonParser::GetString(root, "Longitude", longitude);
    JsonParser::GetString(root, "instanceNum", instanceNum);
    return true;
}

void LocationInfo::ToJson(Json::Value& jsonObj) const {
    jsonObj["MessageType"] = messageType;
    jsonObj["Latitude"] = latitude;
    jsonObj["Longitude"] = longitude;
    jsonObj["instanceNum"] = instanceNum;
}

bool UserPortrait::Parse(const Json::Value& root) {
    JsonParser::GetString(root, "MessageType", messageType);
    params = root["Params"];
    return true;
}

void UserPortrait::ToJson(Json::Value& jsonObj) const {
    jsonObj["MessageType"] = messageType;
    jsonObj["params"] = params;
}

bool UserFeedback::Parse(const Json::Value& root)
{
    if (!JsonParser::GetString(root, "MessageType", messageType)
    || !JsonParser::GetBool(root, "satisfied", satisfied)
    || !JsonParser::GetString(root, "Details", details)) {
        return false;
    }
    return true;
}

void UserFeedback::ToJson(Json::Value& jsonObj) const
{
    jsonObj["MessageType"] = messageType;
    jsonObj["satisfied"] = satisfied;
    jsonObj["Details"] = details;
}
}
