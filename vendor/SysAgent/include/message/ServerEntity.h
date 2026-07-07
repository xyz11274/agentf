#pragma once

#include "libwebsockets/libwebsockets.h"
#include <queue>
#include <mutex>
#include <string>
#include "aisf_platform/parser/aisf_msg_ctrl.h"

class PerSession {
public:
    char session_id[64]; // 用于标识会话
    lws *wsi;

    void OnReceive(const char* data, size_t len, bool isFinalFragment)
    {
        if (closed_.load()) {
            return;
        }

        std::string text(data, len);
        LOG_INFO("[WsSession] 收到数据帧(%zu 字节, final=%d): %s", len, isFinalFragment, text.c_str());

        {
            std::lock_guard<std::mutex> lock(recvMtx_);
            recvBuffer_.append(data, len);
        }

        if (isFinalFragment) {
            LOG_INFO("[WsSession] 消息接收完成(%zu 字节): %s", recvBuffer_.size(), recvBuffer_.c_str());
        }
    }

    std::string GetAndClearBuffer()
    {
        std::lock_guard<std::mutex> lock(recvMtx_);
        std::string result = std::move(recvBuffer_);
        recvBuffer_.clear();
        return result;
    }

    void OnClose()
    {
        closed_.store(true);
        LOG_INFO("[WsSession] 连接关闭 sessionId=%s", session_id);
    }

private:
    std::string recvBuffer_;
    std::mutex recvMtx_;
    std::atomic<bool>  closed_{false};
};

extern std::map<lws*, std::queue<std::shared_ptr<std::string>>> sessionQueues;
extern std::mutex queuesMutex;

extern std::unordered_map<lws*, std::shared_ptr<PerSession>> perSessionMap;
extern std::mutex perSessionMapMutex;

