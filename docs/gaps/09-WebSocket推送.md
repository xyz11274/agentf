# Gap 09: WebSocket 推送未完整实现

## 状态
⚠️ **部分实现**

## 问题描述

TCP 连接支持 WebSocket 升级，但升级后服务端推送通知时**没有封装 WebSocket frame**。

## 代码分析

**WebSocket 升级** — 已实现（`ws_upgrade.cpp:174-213`）：
```
客户端发送 HTTP Upgrade 请求
    → try_ws_upgrade() 返回 UPGRADE_OK
    → ws_mode = true
    → 后续消息走 ws_decode_frame() / ws_encode_text_frame()
```

**服务端推送** — 未封装 WebSocket frame（`rpc_server.cpp:320-327`）：
```cpp
void RpcServer::send_notification(int client_fd, const std::string& method, 
                                   const std::string& params_json) {
    std::string notification = build_notification(method, params_json);
    notification += "\n";                    // ← 加的是换行符
    ssize_t sent = send(client_fd, notification.c_str(), ...);  // ← 直接 send，没有 WS frame
}
```

当客户端通过 WebSocket 连接时，调用 `send_notification()` 会发送裸 JSON 字符串（不是 WS frame），浏览器端的 WebSocket API 无法识别。

## 影响

- WebSocket 连接的客户端只能发请求收响应（走 `handle_request` 返回时有 frame 封装）
- 但收不到服务端主动推送的通知
- `event.subscribe` 订阅事件后无法通过 WebSocket 实时接收

## 建议方向

修正 `send_notification()` 使其在 ws_mode 下使用 `ws_encode_text_frame()`：

```cpp
void RpcServer::send_notification(int client_fd, const std::string& method,
                                   const std::string& params_json) {
    std::string notification = build_notification(method, params_json);
    
    auto it = active_clients_.find(client_fd);
    if (it != active_clients_.end() && it->second == ChannelType::TCP) {
        // TODO: 需要知道该连接是否处于 ws_mode
        // 如果是 ws_mode: 使用 ws_encode_text_frame()
        // 否则: notification += "\n";
    }
    
    send(client_fd, notification.c_str(), notification.size(), MSG_NOSIGNAL);
}
```

需要额外存储每个 client_fd 是否处于 ws_mode。

## 参考

- `src/daemon/ws_upgrade.cpp:215-238` — `ws_encode_text_frame()` 已实现
- `src/daemon/rpc_server.cpp:320-327` — `send_notification()` 推送未封装 WS frame
- `src/daemon/rpc_server.cpp:401` — `client_handler()` 中维护了 ws_mode，但未暴露给 send_notification
