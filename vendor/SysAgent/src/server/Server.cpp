
#include <thread>
#include "server/Server.h"
#include "event2/thread.h"
#include "message/ServerEntity.h"
#include "queue/QueueManager.h"
#include "aisf_platform/common_define.h"

using namespace std;

#define SESSIONID_LENGTH 64

namespace SYSAGENT {
    int CallBackCtr::WsServerCallback(struct lws* wsi, enum lws_callback_reasons reason, void* user, void* in, size_t len) {
        void** userData = static_cast<void**>(user);
        char threadName[16] = {0};
        (void) pthread_getname_np(pthread_self(), threadName, 16);
        switch (reason) {
            case LWS_CALLBACK_ESTABLISHED: {
                LOG_INFO("%s LWS_CALLBACK_ESTABLISHED protocol initial", threadName);
                if (userData == nullptr) {
                    printf("ERROR: pss is NULL\n");
                    LOG_ERR("%s LWS_CALLBACK_ESTABLISHED session is NULL", threadName);
                    break;
                }
                auto session = std::make_shared<PerSession>();
                strcpy_s(session->session_id, SESSIONID_LENGTH, "session_1");
                session->wsi = wsi;
                *userData = session.get();
                {
                    std::lock_guard<std::mutex> lk(perSessionMapMutex);
                    perSessionMap[wsi] = session;
                }
                LOG_INFO("====%s LWS_CALLBACK_ESTABLISHED protocol success====", threadName);
                break;
            }
            case LWS_CALLBACK_RECEIVE: {
                // 回显收到的消息
                LOG_INFO("%s LWS_CALLBACK_RECEIVE protocol initial", threadName);
                if (userData == nullptr || (*userData) == nullptr) {
                    LOG_ERR("%s LWS_CALLBACK_RECEIVE session is NULL", threadName);
                    break;
                }

                if (lws_frame_is_binary(wsi)) {
                    LOG_ERR("%s LWS_CALLBACK_RECEIVE is binary", threadName);
                    break;
                }

                auto* perSession = static_cast<PerSession*>(*userData);
                bool isFinal = lws_is_final_fragment(wsi);

                perSession->OnReceive(static_cast<const char*>(in), len, isFinal);

                if (isFinal) {
                    auto access = std::make_shared<SYSAGENT::AccessCtrl>();
                    std::string finalMsg = perSession->GetAndClearBuffer();
                    std::string errMsg = access->Parse(finalMsg.c_str(), finalMsg.size());
                    if (!errMsg.empty()) {
                        LOG_ERR("%s LWS_CALLBACK_RECEIVE parse error : %s", threadName, errMsg.c_str());
                        QueueManager::getInstance().EventMark("error", "WSServer.ReceiveAccessCtrl",
                                                              "failed", "receive accessCtrl from acm failed",
                                                              access);
                        break;
                    }
                    QueueManager::getInstance().EventMark("info", "WSServer.ReceiveAccessCtrl",
                                                          "success", "receive accessCtrl from acm success",
                                                          access);
                    QueueManager::getInstance().postReadQueue(access, perSession);
                }
                LOG_INFO("====%s LWS_CALLBACK_RECEIVE protocol success====", threadName);
                break;
            }
            case LWS_CALLBACK_CLOSED: {
                LOG_INFO("%s LWS_CALLBACK_CLOSED protocol initial", threadName);
                if (userData == nullptr || (*userData) == nullptr) {
                    LOG_ERR("%s LWS_CALLBACK_RECEIVE session is NULL", threadName);
                    break;
                }
                auto* session = static_cast<PerSession*>(*userData);
                if (session->wsi) {
                    {
                        std::lock_guard<std::mutex> lk(perSessionMapMutex);
                        perSessionMap.erase(session->wsi);
                    }
                    std::lock_guard<std::mutex> lock(queuesMutex);
                    auto it = sessionQueues.find(session->wsi);
                    if (it != sessionQueues.end()) {
                        sessionQueues.erase(it);
                        LOG_INFO("%s LWS_CALLBACK_CLOSED: session queue for wsi has been released", threadName);
                    }
                    session->OnClose();
                } else {
                    LOG_WARN("%s LWS_CALLBACK_CLOSED: session->wsi is NULL", threadName);
                }
                LOG_INFO("%s LWS_CALLBACK_CLOSED protocol initial", threadName);
                break;
            }
            case LWS_CALLBACK_SERVER_WRITEABLE: {
                LOG_INFO_EX("%s LWS_CALLBACK_SERVER_WRITEABLE", threadName);
                if (userData == nullptr || (*userData) == nullptr) {
                    LOG_ERR("%s LWS_CALLBACK_RECEIVE session is NULL", threadName);
                    break;
                }
                auto* session = static_cast<PerSession*>(*userData);
                std::lock_guard<std::mutex> lock(queuesMutex);
                auto isQueueEmpty = sessionQueues[session->wsi].empty();
                std::shared_ptr<std::string> msg;
                if (!isQueueEmpty) {
                    msg = sessionQueues[session->wsi].front();
                    sessionQueues[session->wsi].pop();
                }
                if (msg) {
                    LOG_INFO("==== msgData is %s ====", msg->c_str());
                    int ret = lws_write(wsi, (unsigned char*) msg->c_str(), msg->size(), LWS_WRITE_TEXT);
                    if (ret < 0) {
                        LOG_ERR("%s LWS_CALLBACK_SERVER_WRITEABLE write failed, ret is:%d", threadName, ret);
                    }
                }
                if (!sessionQueues[session->wsi].empty()) {
                    lws_callback_on_writable(wsi);
                }
                break;
            }
            default:
                LOG_DEBUG("%s no LWS_CALLBACK_ESTABLISHED/LWS_CALLBACK_RECEIVE/LWS_CALLBACK_CLOSED", threadName);
                break;
        }
        return 0;
    }

    WebSocketServer::~WebSocketServer()
    {
        // 清理资源
        if (m_context != nullptr) {
            lws_context_destroy(m_context);
            m_context = nullptr;
        }
    }

    void WebSocketServer::MainWorker(WebSocketServer* thisPtr)
    {
        CHECK_NULL_LOG_ERROR_RETURN_VOID(thisPtr, "this pointer is nullptr");
        Utils::SetThreadName(thisPtr->m_name + ":main");

        // 主线程运行事件循环
        while (thisPtr->m_isRunning.load()) {
            lws_service(thisPtr->m_context, MILLI_IN_SECOND); // 主线程处理监听
        }
    }

    bool WebSocketServer::Start()
    {
        // 初始化 libevent 多线程支持
        evthread_use_pthreads();

        // libwebsockets 协议列表
        const lws_protocols protocols[] = {
                {"websocket", m_callback, sizeof(void*), BUFFER_SIZE_128KB, },
                {nullptr, nullptr, 0, 0,} // 结束标记
        };

        // 初始化 libwebsockets 上下文
        if (m_context == nullptr) {
            lws_context_creation_info info{};
            info.port = m_port;
            info.options = LWS_SERVER_OPTION_LIBEVENT; // 使用 libevent 作为事件库
            info.protocols = protocols;
            info.count_threads = m_threadNumber;


            m_context = lws_create_context(&info);
            CHECK_NULL_LOG_ERROR_RETURN(m_context, false, "libwebsockets init failed");
        }

        LOG_ERR("%s initial at port:%u, thread number:%u ok", m_name.c_str(), m_port, m_threadNumber);

        m_isRunning = true;
        thread worker(WebSocketServer::MainWorker, this);
        worker.detach();

        return true;
    }
}