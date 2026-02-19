// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License
//
// WebSocket adapter interface for libwebsockets or other WS implementations

#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace agentflow {
namespace network {

enum class WebSocketError {
  WS_OK             = 0,
  WS_INIT_FAILED    = 1,
  WS_BIND_FAILED    = 2,
  WS_ACCEPT_FAILED  = 3,
  WS_SEND_FAILED    = 4,
  WS_RECEIVE_FAILED = 5,
  WS_INVALID_STATE  = 6,
};

struct WebSocketMessage {
  std::vector<uint8_t> data;
  bool                 is_text{ false };
};

class IWebSocket {
public:
  virtual ~IWebSocket() = default;

  virtual bool           send(const WebSocketMessage& msg)          = 0;
  virtual bool           close(int code, const std::string& reason) = 0;
  virtual bool           is_open() const                            = 0;
  virtual WebSocketError last_error() const                         = 0;
};

class IWebSocketServer {
public:
  virtual ~IWebSocketServer() = default;

  using OnMessageCB = std::function<void(std::shared_ptr<IWebSocket>, const WebSocketMessage&)>;
  using OnOpenCB    = std::function<void(std::shared_ptr<IWebSocket>)>;
  using OnCloseCB   = std::function<void(std::shared_ptr<IWebSocket>, int, const std::string&)>;

  virtual bool           start(const std::string& host, uint16_t port) = 0;
  virtual void           stop()                                        = 0;
  virtual void           set_on_message(OnMessageCB cb)                = 0;
  virtual void           set_on_open(OnOpenCB cb)                      = 0;
  virtual void           set_on_close(OnCloseCB cb)                    = 0;
  virtual WebSocketError last_error() const                            = 0;
};

class IWebSocketClientFactory {
public:
  virtual ~IWebSocketClientFactory()                                                     = default;
  virtual std::shared_ptr<IWebSocket> connect(const std::string&              url,
                                              const std::vector<std::string>& protocols) = 0;
  virtual WebSocketError              last_error() const                                 = 0;
};

class IWebSocketAdapter {
public:
  virtual ~IWebSocketAdapter()                                             = default;
  virtual bool                                     initialize()            = 0;
  virtual void                                     cleanup()               = 0;
  virtual std::shared_ptr<IWebSocketServer>        create_server()         = 0;
  virtual std::shared_ptr<IWebSocketClientFactory> create_client_factory() = 0;
  virtual std::string                              get_version()           = 0;
  virtual bool                                     is_available() const    = 0;
};

}  // namespace network
}  // namespace agentflow
