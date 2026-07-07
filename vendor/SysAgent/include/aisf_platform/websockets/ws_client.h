#pragma once

#include "aisf_platform/thread/thread_pool.h"
#include "define.h"
#include "common.h"

namespace AisfPlat {
class WebSocketClient final {
DISALLOW_COPY_AND_ASSIGN(WebSocketClient)

private:
    struct Info {
        string name; // 服务器名称，已经线程名称的抬头
        string protocol;
        bool enableSsl{true};

        uint32_t queueCap{BUFFER_SIZE_64KB}; // 内部线程池的队列深度；只有threadCnt大于0才能生效
        uint16_t threadCnt{0}; // 内部线程池，线程的个数；如果配置为0，则不启动线程池，使用服务器本身的线程
        unordered_map<lws_callback_reasons, WebSocketCallbackFunc> callbacks; // 服务器各种使用的处理回调函数，多线程调用，注意并发
    };

public:
    explicit WebSocketClient(Info info) : m_info(std::move(info)) {};

    ~WebSocketClient();

    WebSocketClient() = delete;

    bool Connect(const vector<WebSocketServerInfo>& addrs);

    bool Start();

    void Stop();

    [[nodiscard]] auto GetServerInfo() const { return m_srvInfo; }

    bool AddSendMsgToServer(const SafeLwsStringPtr& msg);

    static void MakeAllReport();

    void Report(stringstream& ss);

private:
    static void SetSslInfo(lws_context_creation_info& info, WebSocket::CertInfo& certInfo);

    static int WsClientCallback(lws* wsi, lws_callback_reasons reason, void* user, void* in, size_t len);

    static void ThreadPoolCallback(void* self, const WebSocketCbMsg& msg)
    {
        CHECK_NULL_RETURN_VOID(self);
        ((WebSocketClient*) self)->MessageCallback(msg);
    }

    void MainWorker();

    bool ProcInWebsocketThread(lws* wsi, lws_callback_reasons reason, void* user, void* in, size_t len);

    void MessageCallback(const WebSocketCbMsg& msg);

    void ProcSendMsgToServer(lws* wsi);

    void CheckSendQueueStatus();

private:
    static inline pair<mutex, unordered_set<WebSocketClient*>> instances{};

    const string m_localIp{Utils::GetBaseIp()};
    const Info m_info;
    const lws_protocols m_protocols[2]{   // libwebsockets 协议列表
        {m_info.protocol.c_str(), WsClientCallback, 0, BUFFER_SIZE_1MB, 0, this},
        {nullptr, nullptr, 0, 0, 0, nullptr} // 结束标记
    };

    pair<mutex, std::queue<SafeLwsStringPtr>> m_toSvrMsgQueues{};
    ThreadPool<WebSocketCbMsg> m_pool{{m_info.name, m_info.queueCap, m_info.threadCnt, ThreadPoolCallback, this, true}};
    shared_ptr<atomic_bool> m_isRunning{make_shared<atomic_bool>(false)};
    atomic<lws*> m_wsi{nullptr}; // 记录与服务端的链接

    WebSocketServerInfo m_srvInfo{}; // 仅主线程操作
    lws_context* m_context{nullptr}; // 仅主线程操作
    bool m_isThreadPoolRun{false};  // 仅主线程操作, 提升判断是否开启线程池的效率
};
}