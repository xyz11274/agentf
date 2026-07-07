#pragma once

#include "aisf_platform/websockets/define.h"
#include <atomic>

namespace SYSAGENT {

    class CallBackCtr final {
    public:
        static int WsServerCallback(struct lws* wsi, enum lws_callback_reasons reason, void* user, void* in, size_t len);

    };

    class WebSocketServer final {
    public:
        WebSocketServer(string name, uint8_t threadNumber, uint16_t port, lws_callback_function* callback)
            : m_name(std::move(name)), m_threadNumber(threadNumber), m_port(port), m_callback(callback) {};

        ~WebSocketServer();

        bool Start();

        void Stop() { m_isRunning.store(false); };

    private:
        static void MainWorker(WebSocketServer* thisPtr);

    private:
        const string m_name;
        const uint8_t m_threadNumber;
        const uint16_t m_port;
        lws_callback_function* m_callback;

        std::atomic_bool m_isRunning{false};
        lws_context* m_context{nullptr};
    };
}
