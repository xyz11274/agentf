#pragma once

#include "define.h"

namespace AisfPlat::WebSocket {

struct CertInfo {
    SafeString pwd{__FILE_NAME__, __LINE__};
    string sslCaFilePath;
    string sslPrivateKeyFilePath;
    string sslCertFilePath;
};

void GetCertPath(CertInfo& info);

void Log4Websocket(int level, const char* line);

[[nodiscard]] bool IsLwsValid(lws* wsi);

bool SafeLwsWrite(lws* wsi, const void* in, size_t len, lws_write_protocol protocol);

void RecordStatus(const string& name, const string& action);

[[nodiscard]] shared_ptr<WebSocketCbMsg> NewWebSocketCbMsg(lws* wsi, lws_callback_reasons reason,
                                                           void* user, void* in, size_t len);
}