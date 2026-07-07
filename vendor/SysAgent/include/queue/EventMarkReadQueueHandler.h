//
// Created by z00913804 on 25-12-5.
//

#ifndef SYSAGENT_EVENTMARKREADQUEUEHANDLER_H
#define SYSAGENT_EVENTMARKREADQUEUEHANDLER_H
#include "QueueEntity.h"
#include "BlockingMessageQueue.h"
#include "graphManager/GraphData.h"

struct MarkingMsg {
    std::string serviceName;
    std::string eventName;
    std::string status;
    std::string details;
};

struct EventMarkingMsg {
    std::string timeStamp;
    std::string logLevel;
    std::string userID;
    std::string taskIdr;
    std::string eventType;
    MarkingMsg msg;
    std::string toJsonStr() {
        Json::Value root;
        root["TimeStamp"] = timeStamp;
        root["LogLevel"] = logLevel;
        root["UserID"] = userID;
        root["TaskIdr"] = taskIdr;
        root["EventType"] = eventType;

        // 嵌套 MarkingMsg
        Json::Value markingMsg;
        markingMsg["ServiceName"] = msg.serviceName;
        markingMsg["EventName"] = msg.eventName;
        markingMsg["Status"] = msg.status;
        markingMsg["Details"] = msg.details;
        root["Message"] = markingMsg;
        // 设置 JSON 的输出格式为漂亮格式（PrettyFormat）
        Json::StreamWriterBuilder writer;
        writer["indentation"] = "  ";
        std::ostringstream oss;
        std::unique_ptr<Json::StreamWriter> jsonWriter(Json::StreamWriterBuilder().newStreamWriter());
        jsonWriter->write(root, &oss);
        return oss.str();
    }
};

class EventMarkMsgReadQueueHandler {
public:
    void start();
    void postData(const EventMarkingMsg&);
private:
    BlockingMessageQueue<EventMarkingMsg> msgQueue;
};
#endif // SYSAGENT_EVENTMARKREADQUEUEHANDLER_H
