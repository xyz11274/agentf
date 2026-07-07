#include <thread>
#include "event2/thread.h"
#include "framework_common/utils/file_util.h"

#include "aisf_platform/websockets/ws_client.h"
#include "aisf_platform/websockets/common.h"

namespace AisfPlat {
using namespace std;
using namespace WebSocket;
using namespace StringUtils;

WebSocketClient::~WebSocketClient()
{
    Stop();
}

void WebSocketClient::MainWorker()
{
    Utils::SetThreadName(m_info.name + "_ws");

    auto context = m_context;
    auto isRunning = m_isRunning;
    const auto name = m_info.name;

    RecordStatus(name, "WebSocketClient Thread Run");
    // 主线程运行事件循环
    while (isRunning->load()) {
        lws_service(context, MILLI_IN_SECOND); // 主线程处理监听
    }
    lws_context_destroy(context);  // 清理资源

    RecordStatus(name, "WebSocketClient Thread Exit");
}

bool WebSocketClient::Start()
{
    CHECK_TRUE_RETURN(m_isRunning->load(), true);

    if (m_info.threadCnt > 0) {
        CHECK_FALSE_LOG_ERROR_RETURN(m_pool.Start(), false, "%s thread m_pool start error", m_info.name.c_str());
        m_isThreadPoolRun = true;
    }

    RecordStatus(m_info.name, "WebSocketClient Start");

    // 初始化 libevent 多线程支持
    evthread_use_pthreads();

    // 初始化 libwebsockets 上下文
    if (m_context == nullptr) {
        lws_context_creation_info info{};
        info.port = CONTEXT_PORT_NO_LISTEN; // 客户端不需要监听端口
        info.protocols = m_protocols;
        info.options = LWS_SERVER_OPTION_LIBEVENT;
        info.max_http_header_pool = 1024; // 头内存池大小
        info.pt_serv_buf_size = BUFFER_SIZE_512KB;      // 每个线程缓冲区
        info.timeout_secs = 5;              // I/O 超时

        CertInfo certInfo{};
        if (m_info.enableSsl) {
            SetSslInfo(info, certInfo);
        }

        m_context = lws_create_context(&info);
        CHECK_NULL_LOG_ERROR_RETURN(m_context, false, "lws_create_context failed");
    }

    m_isRunning->store(true);
    thread worker([this] { MainWorker(); });
    worker.detach();

    return true;
}

bool WebSocketClient::Connect(const vector<WebSocketServerInfo>& addrs)
{
    m_srvInfo = {};
    // 创建 WebSocket 客户端连接
    for (auto& addr : addrs) {

        lws_client_connect_info connectInfo{};
        connectInfo.context = m_context;
        connectInfo.address = addr.ip.c_str();
        connectInfo.port = addr.port;
        connectInfo.path = addr.path.c_str();
        connectInfo.host = m_localIp.c_str();
        connectInfo.origin = m_localIp.c_str();
        connectInfo.protocol = m_info.protocol.c_str();
        connectInfo.ssl_connection = m_info.enableSsl ? LCCSCF_USE_SSL : 0; // 是否启用 SSL

        auto lws = lws_client_connect_via_info(&connectInfo);
        if (lws != nullptr) {
            LOG_ERR("connect to server succeed:%s", addr.ToString().c_str());
            m_srvInfo = addr;
            return true;
        }

        LOG_ERR("connect to server error:%s", addr.ToString().c_str());
    }

    return false;
}

void WebSocketClient::SetSslInfo(lws_context_creation_info& info, CertInfo& certInfo)
{
    GetCertPath(certInfo);

    info.ssl_ca_filepath = certInfo.sslCaFilePath.c_str(); // 服务器设备证书
    info.options |= LWS_SERVER_OPTION_REQUIRE_VALID_OPENSSL_CLIENT_CERT; // 强制验证服务器证书
    info.options |= LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT;
}

void WebSocketClient::Stop()
{
    CHECK_FALSE_RETURN_VOID(m_isRunning->load());
    RecordStatus(m_info.name, "WebSocketClient Stop");
    m_isRunning->store(false);
    std::this_thread::sleep_for(std::chrono::milliseconds(MILLI_IN_SECOND)); // 等待一秒，等待线程都退出
}

void WebSocketClient::MessageCallback(const WebSocketCbMsg& msg)
{
    CHECK_NULL_RETURN_VOID(msg.wsi);
    auto itr = m_info.callbacks.find(msg.reason);
    if (itr != m_info.callbacks.end()) {
        itr->second(msg); // 需要处理的消息，调用回调
    }
}

bool WebSocketClient::ProcInWebsocketThread(lws* wsi, lws_callback_reasons reason, void* user, void* in, size_t len)
{
    switch (reason) {
        case LWS_CALLBACK_TIMER: // 定时触发时
            CheckSendQueueStatus();
            lws_set_timer_usecs(wsi, 10 * LWS_US_PER_MS);  // 重置定时器
            return true; // 定时器处理，后续不处理

        case LWS_CALLBACK_CLIENT_ESTABLISHED:
            lws_set_timer_usecs(wsi, 10 * LWS_US_PER_MS);  // 与服务端新建链，以此链接，建立定时器
            m_wsi.store(wsi);
            PRINT_AND_LOG_ERR("Connected succeed to server:%s", m_srvInfo.ToString().c_str());
            WebSocket::RecordStatus(m_info.name, "server established: " + m_srvInfo.ToString());
            break;

        case LWS_CALLBACK_CLIENT_WRITEABLE:
            ProcSendMsgToServer(wsi);
            return true;  // 给服务端发送消息，后续不处理

        case LWS_CALLBACK_CLIENT_CONNECTION_ERROR: {
            m_wsi.store(nullptr);
            auto server = m_srvInfo.ToString();
            auto cause = in == nullptr ? "unknown" : string((const char*) in, len);
            PRINT_AND_LOG_ERR("%s Connection error:%s", server.c_str(), cause.c_str());
            WebSocket::RecordStatus(m_info.name, server + ", error: " + cause);
            break;
        }

        case LWS_CALLBACK_CLIENT_CLOSED: {
            m_wsi.store(nullptr);
            auto server = m_srvInfo.ToString();
            LOG_ERR("%s Connection closed", server.c_str());
            WebSocket::RecordStatus(m_info.name, "server closed:" + server);
            break;
        }

        default:
            break;
    }

    return false;
}

int WebSocketClient::WsClientCallback(lws* wsi, lws_callback_reasons reason, void* user, void* in, size_t len)
{
    auto protocol = lws_get_protocol(wsi);
    CHECK_NULL_RETURN(protocol, 0); // 前期接入流程不处理，例如reason是：21 22 31 等等

    auto* self = (WebSocketClient*) protocol->user;
    CHECK_FALSE_RETURN(self->m_isRunning->load(), 1); // 客户端已经被关闭了

    CHECK_TRUE_RETURN(self->ProcInWebsocketThread(wsi, reason, user, in, len), 0); // 如果返回true，说明不需要后续线程池继续处理

    auto msg = WebSocket::NewWebSocketCbMsg(wsi, reason, user, in, len);
    if (self->m_isThreadPoolRun) {
        self->m_pool.Put(msg); // 加入处理队列
    } else {
        self->MessageCallback(*msg);
    }

    return 0;
}

void WebSocketClient::CheckSendQueueStatus()
{
    m_toSvrMsgQueues.first.lock();
    bool isEmpty = m_toSvrMsgQueues.second.empty();
    m_toSvrMsgQueues.first.unlock();

    auto wsi = m_wsi.load();
    if (!isEmpty && wsi != nullptr) {
        lws_callback_on_writable(wsi);
    }
}

bool WebSocketClient::AddSendMsgToServer(const SafeLwsStringPtr& msg)
{
    CHECK_NULL_LOG_ERROR_RETURN(msg, false, "msg is nullptr");

    auto wsi = m_wsi.load();
    CHECK_FALSE_LOG_ERROR_RETURN(IsLwsValid(wsi), false, "wsi is invalid");

    m_toSvrMsgQueues.first.lock();
    m_toSvrMsgQueues.second.emplace(msg);
    m_toSvrMsgQueues.first.unlock();
    return true;
}

void WebSocketClient::ProcSendMsgToServer(lws* wsi)
{
    const static int ONE_BATCH_SEND_MSG_CNT = 16;

    CHECK_NULL_RETURN_VOID(wsi);

    vector<SafeLwsStringPtr> msgs{};
    bool isEmpty = false;

    m_toSvrMsgQueues.first.lock();
    auto& sendQue = m_toSvrMsgQueues.second;
    for (int i = 0; i < ONE_BATCH_SEND_MSG_CNT && !sendQue.empty(); ++i) {
        msgs.emplace_back(sendQue.front());
        sendQue.pop();
    }
    isEmpty = sendQue.empty();
    m_toSvrMsgQueues.first.unlock();

    for (auto& msg : msgs) { // 批量发送
        if (msg != nullptr) {
            lws_write(wsi, msg->Data(), msg->Length(), LWS_WRITE_TEXT);
        }
    }

    if (!isEmpty) {
        lws_callback_on_writable(wsi);
    }
}

void WebSocketClient::MakeAllReport()
{
    CHECK_FALSE_RETURN_VOID(CMFrm::Utils::CheckFileExist("/tmp/writeWebSocketClient"));
    CHECK_FALSE_RETURN_VOID(remove("/tmp/WebSocketClient") == 0);

    instances.first.lock();
    auto ins = instances.second;
    instances.first.unlock();

    stringstream ss;
    for (auto one : ins) {
        CHECK_TRUE_CONTINUE(one == nullptr)
        ss << "---------------------" << endl;
        one->Report(ss);
        ss << endl;
    }

    auto content = ss.str();
    FileUtils::WriteTextFile("/tmp/WebSocketClient.txt", content.c_str(), content.size());
}

void WebSocketClient::Report(stringstream& ss)
{
    /*
  const string m_localIp{Utils::GetBaseIp()};
    const Info m_info;
    const lws_protocols m_protocols[2]{   // libwebsockets 协议列表
        {m_info.protocol.c_str(), WsClientCallback, 0, BUFFER_SIZE_16MB, 0, this},
        {nullptr, nullptr, 0, 0, 0, nullptr} // 结束标记
    };

    pair<mutex, std::queue<SafeLwsStringPtr>> m_toSvrMsgQueues{};
    ThreadPool<WebSocketCbMsg> m_pool{{m_info.name, m_info.queueCap, m_info.threadCnt, ThreadPoolCallback, this, true}};
    shared_ptr<atomic_bool> m_isRunning{make_shared<atomic_bool>(false)};
    atomic<lws*> m_wsi{nullptr}; // 记录与服务端的链接

    WebSocketServerInfo m_srvInfo{}; // 仅主线程操作
    lws_context* m_context{nullptr}; // 仅主线程操作
    bool m_isThreadPoolRun{false};  // 仅主线程操作, 提升判断是否开启线程池的效率
     * */


}
}