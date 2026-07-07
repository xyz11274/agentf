#include <arpa/inet.h>
#include <thread>
#include <fstream>
#include <unistd.h>

#include "framework_common/utils/file_util.h"
#include "event2/thread.h"

#include "aisf_platform/websockets/ws_server.h"
#include "aisf_platform/websockets/common.h"

namespace AisfPlat {
using namespace std;
using namespace WebSocket;

WebSocketServer::~WebSocketServer()
{
    Stop();

    instances.first.lock();
    instances.second.erase(this);
    instances.first.unlock();
}

void WebSocketServer::MainWorker()
{
    Utils::SetThreadName(m_info.name + "_ws");

    auto context = m_context;
    auto isRunning = m_isRunning;
    auto name = m_info.name;

    RecordStatus(name, "WebSocketServer Thread Run");
    // 主线程运行事件循环
    while (isRunning->load()) {
        lws_service(context, MILLI_IN_SECOND); // 主线程处理监听
    }
    lws_context_destroy(context);  // 清理资源

    RecordStatus(name, "WebSocketServer Thread Exit");
}

void WebSocketServer::SetSslInfo(lws_context_creation_info& info, CertInfo& certInfo)
{
    info.options |= LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT;
    info.client_tls_1_3_plus_cipher_list = WSI_CIPER_LIST;
    info.tls1_3_plus_cipher_list = WSI_CIPER_LIST;
    info.client_ssl_cipher_list = WSI_CIPER_LIST;
    info.ssl_cipher_list = WSI_CIPER_LIST;
    info.ka_time = 60; // 30秒
    info.ka_interval = 15; // 15秒
    info.ka_probes = 5; // 5次

    GetCertPath(certInfo);
    info.ssl_private_key_password = certInfo.pwd.Data();
    info.ssl_cert_filepath = certInfo.sslCertFilePath.c_str(); // 服务器证书
    info.ssl_private_key_filepath = certInfo.sslPrivateKeyFilePath.c_str(); // 服务器私钥
    info.ssl_ca_filepath = certInfo.sslCaFilePath.c_str(); // 服务器设备证书
}

bool WebSocketServer::Start()
{
    CHECK_TRUE_RETURN(m_isRunning->load(), true);

    if (m_info.threadCnt > 0) {
        CHECK_FALSE_LOG_ERROR_RETURN(m_pool.Start(), false, "%s thread m_pool start error", m_info.name.c_str());
        m_isThreadPoolRun = true;
    }

    RecordStatus(m_info.name, "WebSocketServer Start");

    // 初始化 libevent 多线程支持
    evthread_use_pthreads();

    // libwebsockets服务端日志默认 打印到snm日志文件里 /opt/csp/cspsnmadapt/sbin/snm_app_run.log
    lws_set_log_level(LLL_USER | LLL_ERR | LLL_WARN | LLL_NOTICE | LLL_INFO | LLL_DEBUG, Log4Websocket);

    // libwebsockets 协议列表
    const lws_protocols protocols[] = {
        {"websocket", WsServerCallback, 0, BUFFER_SIZE_1MB, 0, this},
        {nullptr, nullptr, 0, 0, 0, nullptr} // 结束标记
    };

    // 初始化 libwebsockets 上下文
    if (m_context == nullptr) {
        const auto baseIp = Utils::GetBaseIp();
        lws_context_creation_info info{};
        info.iface = baseIp.c_str();
        info.port = m_info.port;
        info.options = LWS_SERVER_OPTION_LIBEVENT |
                       LWS_SERVER_OPTION_HTTP_HEADERS_SECURITY_BEST_PRACTICES_ENFORCE; // 使用 libevent 作为事件库
        info.protocols = protocols;

        info.timeout_secs = 60;                 // 60秒无响应断开
        info.pt_serv_buf_size = BUFFER_SIZE_512KB;      // 每个线程缓冲区
        info.max_http_header_pool = 256;    // 头部内存池
        info.gid = -1;
        info.uid = -1;

        CertInfo certInfo{};
        if (m_info.enableSsl) {
            SetSslInfo(info, certInfo);
        }

        m_context = lws_create_context(&info);
        CHECK_NULL_LOG_ERROR_RETURN(m_context, false, "%s libwebsockets init failed", m_info.name.c_str());
    }

    m_isRunning->store(true);
    thread worker([this] { MainWorker(); });
    worker.detach();

    LOG_ERR("%s initial at port:%u ok", m_info.name.c_str(), m_info.port);
    return true;
}

void WebSocketServer::Stop()
{
    CHECK_FALSE_RETURN_VOID(m_isRunning->load());

    RecordStatus(m_info.name, "WebSocketServer Stop");
    m_isRunning->store(false);
    m_pool.Stop();

    std::this_thread::sleep_for(std::chrono::milliseconds(MILLI_IN_SECOND)); // 等待一秒，等待线程都退出
}

void WebSocketServer::MessageCallback(const WebSocketCbMsg& msg)
{
    CHECK_NULL_RETURN_VOID(msg.wsi);
    auto itr = m_info.callbacks.find(msg.reason);
    if (itr != m_info.callbacks.end()) {
        itr->second(msg); // 需要处理的消息，调用回调
    }
}

void WebSocketServer::ReadCustomHeader(const char* name, int nlen, void* opaque)
{
    CHECK_NULL_RETURN_VOID(name);
    CHECK_NULL_RETURN_VOID(opaque);

    auto msg = (WebSocketCbMsg*) opaque;
    SafeString buffer{__FUNCTION__, __LINE__, BUFFER_SIZE_1KB};
    int valueLen = lws_hdr_custom_copy((lws*) msg->wsi, buffer.Get(), (int) buffer.Size(), name, nlen);
    CHECK_TRUE_RETURN_VOID(valueLen <= 0);

    msg->headers[StringUtils::TrimRightString(string(name, nlen), ":")] = string(buffer.Get(), valueLen);
}

void WebSocketServer::CheckSendQueueStatus()
{
    vector<lws*> wsis;
    m_needToSendMsgToClients.first.lock();
    m_needToSendMsgToClients.second.swap(wsis);
    m_needToSendMsgToClients.first.unlock();

    for (auto wsi : wsis) {
        if (IsLwsValid(wsi)) {
            lws_callback_on_writable(wsi);
        }
    }
}

bool WebSocketServer::ProcInWebsocketThread(lws* wsi, lws_callback_reasons reason, void* user, void* in, size_t len)
{
    switch (reason) {
        case LWS_CALLBACK_ESTABLISHED:
            ProcClientConnected(wsi);
            break;

        case LWS_CALLBACK_CLOSED:
            ProcClientClosed(wsi);
            break;

        case LWS_CALLBACK_TIMER: // 定时触发时
            CheckSendQueueStatus();
            lws_set_timer_usecs(wsi, 10 * LWS_US_PER_MS);  // 重置定时器
            return true; // 定时器处理，直接返回，后续不处理

        case LWS_CALLBACK_SERVER_WRITEABLE:
            ProcSendMsgToClient(wsi);
            return true;  // 给客户端发送消息，后续不处理

        default:
            break;
    }

    return false;
}

int WebSocketServer::WsServerCallback(lws* wsi, lws_callback_reasons reason, void* user, void* in, size_t len)
{
    auto protocol = lws_get_protocol(wsi);
    CHECK_NULL_RETURN(protocol, 0);

    auto* self = (WebSocketServer*) protocol->user;
    CHECK_FALSE_RETURN(self->m_isRunning->load(), 1); // 服务器已经被关闭了

    CHECK_TRUE_RETURN(self->ProcInWebsocketThread(wsi, reason, user, in, len), 0); // 如果返回true，说明不需要后续线程池继续处理

    auto msg = WebSocket::NewWebSocketCbMsg(wsi, reason, user, in, len);
    if (reason == LWS_CALLBACK_ESTABLISHED) {
        auto ret = lws_hdr_custom_name_foreach(wsi, ReadCustomHeader, msg.get());
        CHECK_FALSE_LOG_ERROR(ret == 0, "Read http header error:%d", ret);
    }

    if (self->m_isThreadPoolRun) {
        self->m_pool.Put(msg); // 加入处理队列
    } else {
        self->MessageCallback(*msg);
    }

    return 0;
}

void WebSocketServer::ProcClientClosed(lws* wsi)
{
    lws* timerWsi = nullptr;
    m_toClientMsgQueues.first.lock();
    auto& queue = m_toClientMsgQueues.second;
    queue.erase(wsi);
    if (!queue.empty()) {
        timerWsi = (lws*) queue.begin()->first;
    }
    m_toClientMsgQueues.first.unlock();

    if (wsi == m_timerUsingWsi) {
        // 有定时的链接退出了
        m_timerUsingWsi = timerWsi;
        if (m_timerUsingWsi != nullptr) {
            lws_set_timer_usecs(m_timerUsingWsi, 10 * LWS_US_PER_MS); // 再次启动定时器
        }
    }

    auto itr = m_clientRecords.find(wsi);
    if (itr != m_clientRecords.end()) {
        PRINT_AND_LOG_INFO("client closed, server listen @ %hu, client:%s", m_info.port,
                      itr->second.ToString().c_str());
        m_clientRecords.erase(itr);
    }
}

void WebSocketServer::ProcClientConnected(lws* wsi)
{
    auto fd = lws_get_socket_fd(wsi);
    sockaddr_in clientAddr{};
    socklen_t sockLen = sizeof(clientAddr);
    getpeername(fd, (struct sockaddr*) &clientAddr, &sockLen);

    PRINT_AND_LOG_INFO("New connection, server listen @ %d, client ip:%s, port:%d", (int)m_info.port,
                  inet_ntoa(clientAddr.sin_addr), (int)__bswap_16(clientAddr.sin_port));

    if (m_timerUsingWsi == nullptr) {
        lws_set_timer_usecs(wsi, 10 * LWS_US_PER_MS); // 第一次启动定时器
        m_timerUsingWsi = wsi;
    }

    m_clientRecords.emplace(wsi, ClientRecord{inet_ntoa(clientAddr.sin_addr), ntohs(clientAddr.sin_port)});
}

void WebSocketServer::AddSendMsgToClient(const lws* wsi, const SafeLwsStringPtr& msg)
{
    CHECK_NULL_LOG_ERROR_RETURN_VOID(msg, "msg is nullptr");
    CHECK_FALSE_LOG_ERROR_RETURN_VOID(IsLwsValid((lws*) wsi), "wsi is invalid");

    m_toClientMsgQueues.first.lock();
    auto& sendQue = m_toClientMsgQueues.second[(lws*) wsi];
    sendQue.emplace(msg);
    m_toClientMsgQueues.first.unlock();

    m_needToSendMsgToClients.first.lock();
    m_needToSendMsgToClients.second.push_back((lws*) wsi);
    m_needToSendMsgToClients.first.unlock();
}

void WebSocketServer::ProcSendMsgToClient(lws* wsi)
{
    const static int ONE_BATCH_SEND_MSG_CNT = 16;
    CHECK_NULL_RETURN_VOID(wsi);

    vector<SafeLwsStringPtr> msgs{};
    bool isEmpty = false;

    m_toClientMsgQueues.first.lock();
    auto& sendQue = m_toClientMsgQueues.second[wsi];
    for (int i = 0; i < ONE_BATCH_SEND_MSG_CNT && !sendQue.empty(); ++i) {
        msgs.emplace_back(sendQue.front());
        sendQue.pop();
    }
    isEmpty = sendQue.empty();
    m_toClientMsgQueues.first.unlock();

    for (auto& msg : msgs) { // 批量发送
        if (msg != nullptr) {
            lws_write(wsi, msg->Data(), msg->Length(), LWS_WRITE_TEXT);
        }
    }

    if (!isEmpty) {
        lws_callback_on_writable(wsi);
    }
}

void WebSocketServer::MakeAllReport()
{
    CHECK_FALSE_RETURN_VOID(CMFrm::Utils::CheckFileExist("/tmp/writeWebSocketServer"));
    CHECK_FALSE_RETURN_VOID(remove("/tmp/WebSocketServer") == 0);

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
    FileUtils::WriteTextFile("/tmp/WebSocketServer.txt", content.c_str(), content.size());
}

void WebSocketServer::Report(stringstream& ss)
{
    /*
 const Info m_info;

    pair<mutex, vector<lws*>> m_needToSendMsgToClients{};
    pair<mutex, unordered_map<lws*, std::queue<SafeLwsStringPtr>>> m_toClientMsgQueues{};
    ThreadPool<WebSocketCbMsg> m_pool{{m_info.name, m_info.queueCap, m_info.threadCnt, ThreadPoolCallback, this, true}};
    shared_ptr<atomic_bool> m_isRunning{make_shared<atomic_bool>(false)};

    unordered_map<lws*, ClientRecord> m_clientRecords; // 仅主线程操作
    lws_context* m_context{nullptr}; // 仅主线程操作
    lws* m_timerUsingWsi{nullptr}; // 仅主线程操作
    bool m_isThreadPoolRun{false}; // 仅主线程操作，提升判断是否开启线程池的效率
     * */

}
}