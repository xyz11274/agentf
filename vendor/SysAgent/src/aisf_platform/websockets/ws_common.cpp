#include "framework_common/utils/env_util.h"
#include "framework_common/utils/file_util.h"

#include "aisf_platform/websockets/common.h"
#include "aisf_platform/websockets/safe_lws_string.h"

namespace AisfPlat::WebSocket {

void GetCertPath(CertInfo& info)
{
    info.pwd.SetToSafe();
    const auto pEnv = getenv("INNER_TLS_PRIVATE_KEY_PWD");
    CHECK_NULL_LOG_ERROR_RETURN_VOID(pEnv, "INNER_TLS_PRIVATE_KEY_PWD not set");
    info.pwd.Reset(strlen(pEnv));
    info.pwd.Write(pEnv, strlen(pEnv));

    std::string sslPath = CMFrm::Utils::GetEnv("SSLPATH", "/opt/sharevolume/ssl/cert");

    // 设置CA证书
    info.sslCaFilePath = StringUtils::Format(BUFFER_SIZE_256B, "%s%s", sslPath.c_str(), "ca.crt");
    // 设置秘钥
    info.sslPrivateKeyFilePath = StringUtils::Format(BUFFER_SIZE_256B, "%s%s", sslPath.c_str(), "tls.key.pwd");
    // 设置设备证书
    info.sslCertFilePath = StringUtils::Format(BUFFER_SIZE_256B, "%s%s", sslPath.c_str(), "tls.crt");
}

void Log4Websocket(int level, const char* line)
{
    CHECK_NULL_RETURN_VOID(line);

    char* printLog = (char*) line;
    printLog[strlen(line) - 1] = '\0';
    switch (level) {
        case LLL_ERR:
            LOG_ERR("%s", printLog);
            break;
        case LLL_WARN:
            LOG_WARN("%s", printLog);
            break;
        case LLL_NOTICE:
            LOG_DEBUG("%s", printLog);
            break;
        case LLL_INFO:
            LOG_DEBUG("%s", printLog);
            break;
        default:
            LOG_DEBUG("%s", printLog);
            break;
    }
}

bool IsLwsValid(lws* wsi)
{
    CHECK_NULL_RETURN(wsi, false);
    auto context = lws_get_context(wsi);    // 获取连接的上下文
    CHECK_NULL_LOG_ERROR_RETURN(context, false, "context is nullptr"); // 检查上下文是否有效（例如未被销毁）

    auto fd = lws_get_socket_fd(wsi);
    CHECK_TRUE_LOG_ERROR_RETURN(fd == LWS_SOCK_INVALID, false, "wsi socket is invalid");

    int sockErr = 0;
    socklen_t len = sizeof(sockErr);
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, (char*) &sockErr, &len) < 0 || sockErr != 0) {
        LOG_ERR("socket error:%s", strerror(sockErr));
        return false;
    }

    return true;
}

bool SafeLwsWrite(lws* wsi, const void* in, size_t len, lws_write_protocol protocol)
{
    CHECK_TRUE_RETURN(in == nullptr || len == 0, false);
    CHECK_FALSE_LOG_ERROR_RETURN(IsLwsValid(wsi), false, "wsi is invalid");

    if (SafeLwsString::IsSafeLwsString(in)) { // 已经是安全是字符串了，直接写
        auto sendLen = lws_write(wsi, (unsigned char*) in, len, protocol);
        CHECK_FALSE_LOG_ERROR_RETURN(sendLen > 0, false, "wsi write error:%d", sendLen);
    } else { // 非安全的字符串，不能直接写，需要特殊处理一下
        SafeLwsString buffer(in, len);
        CHECK_TRUE_LOG_ERROR_RETURN(buffer.Empty(), false, "buffer error");
        auto sendLen = lws_write(wsi, buffer.Data(), buffer.Length(), protocol);
        CHECK_FALSE_LOG_ERROR_RETURN(sendLen > 0, false, "wsi write error:%d", sendLen);
    }

    return true;
}

void RecordStatus(const string& name, const string& action)
{
    CHECK_FALSE_RETURN_VOID(CMFrm::Utils::CheckFileExist("/tmp/websockets.enable"));

    static const auto HEADER = StringUtils::Format(BUFFER_SIZE_512KB, "%23s \t%16s \t%s\n", "Time", "Name", "Action");
    static mutex lock;
    static deque<string> actions;

    auto timeStr = Utils::MillisecondToString(Utils::GetTimeMs());
    auto item = StringUtils::Format(BUFFER_SIZE_512KB, "%23s \t%16s \t%s\n", timeStr.c_str(), name.c_str(),
                                    action.c_str());
    stringstream out;
    out << HEADER;

    {
        std::lock_guard<std::mutex> guard(lock);
        while (actions.size() >= NUMBER_HUNDRED) {
            actions.pop_front();
        }
        actions.emplace_back(item);
        for (const auto &one: actions) {
            out << one;
        }
    }

    const auto content = out.str();
    FileUtils::WriteTextFile("/tmp/websockets.status", content.c_str(), content.size());
}

shared_ptr<WebSocketCbMsg> NewWebSocketCbMsg(lws* wsi, lws_callback_reasons reason, void* user, void* in, size_t len)
{
    auto msg = make_shared<WebSocketCbMsg>();
    msg->wsi = wsi;
    msg->reason = reason;
    msg->user = user;
    msg->len = len;

    CHECK_TRUE_RETURN(in == nullptr || len == 0, msg);

    msg->data->Write(in, len);
    msg->in = msg->data->Data();

    return msg;
}
}