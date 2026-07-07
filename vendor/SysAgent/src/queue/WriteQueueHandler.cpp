//
// Created by y00601565 on 2025/2/24.
//

#include "queue/WebSocketWriteQueueHandler.h"
#include "aisf_platform/common_define.h"
#include "message/ServerEntity.h"

bool is_wsi_usable(struct lws *wsi)
{
    CHECK_NULL_RETURN(wsi, false);
    auto context = lws_get_context(wsi);    // 获取连接的上下文
    CHECK_NULL_LOG_ERROR_RETURN(context, false, "wsi:%p context is nullptr", wsi); // 检查上下文是否有效（例如未被销毁）

    auto fd = lws_get_socket_fd(wsi);
    CHECK_TRUE_LOG_ERROR_RETURN(fd == LWS_SOCK_INVALID, false, "wsi:%p socket is invalid", wsi);

    int sockErr = 0;
    socklen_t len = sizeof(sockErr);
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, (char*) &sockErr, &len) < 0 || sockErr != 0) {
        LOG_ERR("wsi:%p, socket error:%s", wsi, strerror(sockErr));
        return false;
    }

    return true;
}

void WebSocketWriteQueueHandler::start() {
    while (true) {
        QueueMessage msg = msgQueue.GetMessage();
        std::string msgData = msg.data->toString();
        if (msgData.size() == 0) {
            LOG_INFO("msgData is NULL.");
            continue;
        }

        if (!msg.session || !is_wsi_usable(msg.session->wsi) || msgData.empty()) {
            LOG_INFO_EX("msg.session->wsi is not available.");
            continue;
        }
        try {
            std::lock_guard<std::mutex> lock(queuesMutex);
            sessionQueues[msg.session->wsi].push(std::make_shared<std::string>(msgData));
            if (msg.session->wsi && is_wsi_usable(msg.session->wsi)) {
                lws_callback_on_writable(msg.session->wsi);
            } else {
                LOG_INFO_EX("msg.session->wsi is not available.");
            }
        } catch (const std::exception& e) {
            LOG_ERR("Exception occurred: %s", e.what());
        }
    }
}
void WebSocketWriteQueueHandler::writeData(const QueueMessage message) {
     msgQueue.PostMessage(message);
}

