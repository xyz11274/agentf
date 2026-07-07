#pragma once

#include "safe_lws_string.h"
#include "aisf_platform/thread/thread_pool.h"

namespace AisfPlat {
const static char* WSI_CIPER_LIST = "ECDHE-ECDSA-AES256-GCM-SHA384:"
                                    "ECDHE-ECDSA-AES128-GCM-SHA256:"
                                    "ECDHE-RSA-AES256-GCM-SHA384:"
                                    "ECDHE-RSA-AES128-GCM-SHA256";

const static inline unordered_map<int, string> LWS_CB_REASON_TO_STRING = {
    {0, "ESTABLISHED"}, {1, "CLIENT_CONNECTION_ERROR"}, {2, "CLIENT_FILTER_PRE_ESTABLISH"}, {3, "CLIENT_ESTABLISHED"},
    {4, "CLOSED"}, {5, "CLOSED_HTTP"}, {6, "RECEIVE"}, {7, "RECEIVE_PONG"}, {8, "CLIENT_RECEIVE"},
    {9, "CLIENT_RECEIVE_PONG"}, {10, "CLIENT_WRITEABLE"}, {11, "SERVER_WRITEABLE"}, {12, "HTTP"}, {13, "HTTP_BODY"},
    {14, "HTTP_BODY_COMPLETION"}, {15, "HTTP_FILE_COMPLETION"}, {16, "HTTP_WRITEABLE"},
    {17, "FILTER_NETWORK_CONNECTION"}, {18, "FILTER_HTTP_CONNECTION"}, {19, "SERVER_NEW_CLIENT_INSTANTIATED"},
    {20, "FILTER_PROTOCOL_CONNECTION"}, {21, "OPENSSL_LOAD_EXTRA_CLIENT_VERIFY_CERTS"},
    {22, "OPENSSL_LOAD_EXTRA_SERVER_VERIFY_CERTS"}, {23, "OPENSSL_PERFORM_CLIENT_CERT_VERIFICATION"},
    {24, "CLIENT_APPEND_HANDSHAKE_HEADER"}, {25, "CONFIRM_EXTENSION_OKAY"}, {26, "CLIENT_CONFIRM_EXTENSION_SUPPORTED"},
    {27, "PROTOCOL_INIT"}, {28, "PROTOCOL_DESTROY"}, {29, "WSI_CREATE"}, {30, "WSI_DESTROY"}, {31, "GET_THREAD_ID"},
    {32, "ADD_POLL_FD"}, {33, "DEL_POLL_FD"}, {34, "CHANGE_MODE_POLL_FD"}, {35, "LOCK_POLL"}, {36, "UNLOCK_POLL"},
    {38, "WS_PEER_INITIATED_CLOSE"}, {39, "WS_EXT_DEFAULTS"}, {40, "CGI"}, {41, "CGI_TERMINATED"},
    {42, "CGI_STDIN_DATA"}, {43, "CGI_STDIN_COMPLETED"}, {44, "ESTABLISHED_CLIENT_HTTP"}, {45, "CLOSED_CLIENT_HTTP"},
    {46, "RECEIVE_CLIENT_HTTP"}, {47, "COMPLETED_CLIENT_HTTP"}, {48, "RECEIVE_CLIENT_HTTP_READ"},
    {49, "HTTP_BIND_PROTOCOL"}, {50, "HTTP_DROP_PROTOCOL"}, {51, "CHECK_ACCESS_RIGHTS"}, {52, "PROCESS_HTML"},
    {53, "ADD_HEADERS"}, {54, "SESSION_INFO"}, {55, "GS_EVENT"}, {56, "HTTP_PMO"}, {57, "CLIENT_HTTP_WRITEABLE"},
    {58, "OPENSSL_PERFORM_SERVER_CERT_VERIFICATION"}, {59, "RAW_RX"}, {60, "RAW_CLOSE"}, {61, "RAW_WRITEABLE"},
    {62, "RAW_ADOPT"}, {63, "RAW_ADOPT_FILE"}, {64, "RAW_RX_FILE"}, {65, "RAW_WRITEABLE_FILE"}, {66, "RAW_CLOSE_FILE"},
    {67, "SSL_INFO"}, {69, "CHILD_CLOSING"}, {70, "CGI_PROCESS_ATTACH"}, {71, "EVENT_WAIT_CANCELLED"},
    {72, "VHOST_CERT_AGING"}, {73, "TIMER"}, {74, "VHOST_CERT_UPDATE"}, {75, "CLIENT_CLOSED"},
    {76, "CLIENT_HTTP_DROP_PROTOCOL"}, {77, "WS_SERVER_BIND_PROTOCOL"}, {78, "WS_SERVER_DROP_PROTOCOL"},
    {79, "WS_CLIENT_BIND_PROTOCOL"}, {80, "WS_CLIENT_DROP_PROTOCOL"}, {81, "RAW_SKT_BIND_PROTOCOL"},
    {82, "RAW_SKT_DROP_PROTOCOL"}, {83, "RAW_FILE_BIND_PROTOCOL"}, {84, "RAW_FILE_DROP_PROTOCOL"},
    {85, "CLIENT_HTTP_BIND_PROTOCOL"}, {86, "HTTP_CONFIRM_UPGRADE"}, {89, "RAW_PROXY_CLI_RX"}, {90, "RAW_PROXY_SRV_RX"},
    {91, "RAW_PROXY_CLI_CLOSE"}, {92, "RAW_PROXY_SRV_CLOSE"}, {93, "RAW_PROXY_CLI_WRITEABLE"},
    {94, "RAW_PROXY_SRV_WRITEABLE"}, {95, "RAW_PROXY_CLI_ADOPT"}, {96, "RAW_PROXY_SRV_ADOPT"},
    {97, "RAW_PROXY_CLI_BIND_PROTOCOL"}, {98, "RAW_PROXY_SRV_BIND_PROTOCOL"}, {99, "RAW_PROXY_CLI_DROP_PROTOCOL"},
    {100, "RAW_PROXY_SRV_DROP_PROTOCOL"}, {101, "RAW_CONNECTED"}, {102, "VERIFY_BASIC_AUTHORIZATION"},
    {103, "WSI_TX_CREDIT_GET"}, {104, "CLIENT_HTTP_REDIRECT"}, {105, "CONNECTING"},
    {200, "MQTT_NEW_CLIENT_INSTANTIATED"}, {201, "MQTT_IDLE"}, {202, "MQTT_CLIENT_ESTABLISHED"},
    {203, "MQTT_SUBSCRIBED"}, {204, "MQTT_CLIENT_WRITEABLE"}, {205, "MQTT_CLIENT_RX"}, {206, "MQTT_UNSUBSCRIBED"},
    {207, "MQTT_DROP_PROTOCOL"}, {208, "MQTT_CLIENT_CLOSED"}, {209, "MQTT_ACK"}, {210, "MQTT_RESEND"},
    {211, "MQTT_UNSUBSCRIBE_TIMEOUT"}, {212, "MQTT_SHADOW_TIMEOUT"}, {1000, "USER"}
};

struct WebSocketServerInfo {
    string name;
    string ip;
    uint16_t port{0};
    string path;

    [[nodiscard]] string ToString() const
    {
        return StringUtils::Format(BUFFER_SIZE_1KB,
                                   "[%s] %s%s", name.c_str(), Utils::JoinIpPort(ip, port).c_str(), path.c_str());
    }
};

class WebSocketCbMsg final : public ThreadSafeQueueMsgBase {
public:
    map<string, string> headers; // 建链消息才有

    const lws* wsi{nullptr}; // 不可做任何操作，否则会导致libwebsockets异常！！！！
    lws_callback_reasons reason{LWS_CALLBACK_USER};
    void* user{nullptr};

    SafeLwsStringPtr data{make_shared<SafeLwsString>()}; // 如果使用多线程，数据必须拷贝到此处缓存
    void* in{nullptr};
    size_t len{0};

    WebSocketCbMsg() = default;
    virtual ~WebSocketCbMsg() = default;

    [[nodiscard]] size_t Size() const override { return sizeof(*this) + SafeLwsString::PRE_LEN + data->Length(); }
};

using WebSocketCbMsgSafe = std::shared_ptr<WebSocketCbMsg>;

typedef void (* WebSocketCallbackFunc)(const WebSocketCbMsg& msg);
}