#pragma once

#include <utility>
#include <functional>
#include "define.h"
#include "common.h"
#include "aisf_platform/thread/thread_pool.h"

namespace AisfPlat {
class WebSocketServer final {
DISALLOW_COPY_AND_ASSIGN(WebSocketServer)

    struct Info {
        string name; // 服务器名称，已经线程名称的抬头
        uint16_t port; // 监听的端口；IP是固定的Base口IP，无法修改
        bool enableSsl{true};

        uint32_t queueCap{BUFFER_SIZE_64KB}; // 内部线程池的队列深度；只有threadCnt大于0才能生效
        uint16_t threadCnt{0}; // 内部线程池，线程的个数；如果配置为0，则不启动线程池，使用服务器本身的线程
        unordered_map<lws_callback_reasons, WebSocketCallbackFunc> callbacks; // 服务器各种使用的处理回调函数，多线程调用，注意并发
    };

    struct ClientRecord {
        const long onlineTime{Utils::GetTimeMs()};
        const string ip;
        const uint16_t port;

        ClientRecord(string i, uint16_t p) : ip(std::move(i)), port(p) {}

        string ToString()
        {
            return StringUtils::Format(BUFFER_SIZE_4KB, "online @ %s,ip:%s,port:%d",
                                       Utils::MillisecondToString(onlineTime).c_str(), ip.c_str(), port);
        }
    };

public:
    explicit WebSocketServer(Info info) : m_info(std::move(info)) {
        instances.first.lock();
        instances.second.emplace(this);
        instances.first.unlock();
    };

    ~WebSocketServer();

    bool Start();

    void Stop();

    void AddSendMsgToClient(const lws* wsi, const SafeLwsStringPtr& msg);

    static void MakeAllReport();

    void Report(stringstream& ss);

private:
    static void ReadCustomHeader(const char* name, int nlen, void* opaque);

    static void SetSslInfo(lws_context_creation_info& info, WebSocket::CertInfo& certInfo);

    static int WsServerCallback(lws* wsi, lws_callback_reasons reason, void* user, void* in, size_t len);

    static void ThreadPoolCallback(void* self, const WebSocketCbMsg& msg)
    {
        CHECK_NULL_RETURN_VOID(self);
        ((WebSocketServer*) self)->MessageCallback(msg);
    }

    void MainWorker();

    void ProcClientClosed(lws* wsi);

    void ProcClientConnected(lws* wsi);

    bool ProcInWebsocketThread(lws* wsi, lws_callback_reasons reason, void* user, void* in, size_t len);

    void MessageCallback(const WebSocketCbMsg& msg);

    void ProcSendMsgToClient(lws* wsi);

    void CheckSendQueueStatus();

private:
    static inline pair<mutex, unordered_set<WebSocketServer*>> instances{};
    const Info m_info;

    pair<mutex, vector<lws*>> m_needToSendMsgToClients{};
    pair<mutex, unordered_map<lws*, std::queue<SafeLwsStringPtr>>> m_toClientMsgQueues{};
    ThreadPool<WebSocketCbMsg> m_pool{{m_info.name, m_info.queueCap, m_info.threadCnt, ThreadPoolCallback, this, true}};
    shared_ptr<atomic_bool> m_isRunning{make_shared<atomic_bool>(false)};

    unordered_map<lws*, ClientRecord> m_clientRecords; // 仅主线程操作
    lws_context* m_context{nullptr}; // 仅主线程操作
    lws* m_timerUsingWsi{nullptr}; // 仅主线程操作
    bool m_isThreadPoolRun{false}; // 仅主线程操作，提升判断是否开启线程池的效率
};
}