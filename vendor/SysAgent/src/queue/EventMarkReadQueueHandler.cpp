//
// Created by z00913804 on 25-12-5.
//
#include "queue/EventMarkReadQueueHandler.h"

void EventMarkMsgReadQueueHandler::start()
{
    LOG_INFO("EventMarkMsgReadQueueHandler started processing message queue...");
    while (true) {
        try {
            // 等待接收消息
            EventMarkingMsg msg = msgQueue.GetMessage();
            CMFrm_Info("Send msg to aisfmgr to marking event, event name: %s.", msg.msg.eventName.c_str());
            std::string ip = SYSAGENT::HttpClient::GetServiceIP(SYSAGENT::AISF_MGR);
            std::string url = "https://" + ip + SYSAGENT::AISF_MGR_PORT + SYSAGENT::AISF_MGR_EVENT_MARKING;
            std::string body = msg.toJsonStr();
            auto req = std::make_shared<CMFrm::COM::Http2ClientRequest>();
            req->SetReqUrl(url);
            req->SetBody(body);
            req->SetTimeoutSecond(CMFrm::Constants::Const8);
            SYSAGENT::HttpClient::Http2SendRequest(req, [](std::shared_ptr<CMFrm::COM::Http2ClientResponse> resp, void*) {
                LOG_INFO_EX("send aisf_mgr result: %d, %s", resp->GetStatusCode(), resp->GetResponseBody().c_str());
            });
        } catch (const std::exception& e) {
            LOG_ERR_EX("Exception occurred while processing message: %s", e.what());
        }
    }
}

void EventMarkMsgReadQueueHandler::postData(const EventMarkingMsg& message)
{
    msgQueue.PostMessage(message);
}