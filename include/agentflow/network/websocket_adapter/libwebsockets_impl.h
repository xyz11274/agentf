// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License
//
// libwebsockets-based WebSocket adapter

#pragma once

#include "agentflow/network/websocket_adapter.h"

#ifdef AGENTFLOW_WITH_LIBWEBSOCKETS
#include <libwebsockets.h>
#endif

namespace agentflow {
namespace network {

#ifdef AGENTFLOW_WITH_LIBWEBSOCKETS

class WebSocketLws : public IWebSocket {
 public:
  explicit WebSocketLws(lws* wsi);
  ~WebSocketLws() override;
  bool send(const WebSocketMessage& msg) override;
  bool close(int code, const std::string& reason) override;
  bool is_open() const override;
  WebSocketError last_error() const override;

 private:
  lws* wsi_;
  WebSocketError err_;
};

class WebSocketServerLws : public IWebSocketServer {
 public:
  WebSocketServerLws();
  ~WebSocketServerLws() override;
  bool start(const std::string& host, uint16_t port) override;
  void stop() override;
  void set_on_message(OnMessageCB cb) override;
  void set_on_open(OnOpenCB cb) override;
  void set_on_close(OnCloseCB cb) override;
  WebSocketError last_error() const override;

 private:
  struct lws_context* context_;
  OnMessageCB on_message_;
  OnOpenCB on_open_;
  OnCloseCB on_close_;
  WebSocketError err_;
};

class WebSocketClientFactoryLws : public IWebSocketClientFactory {
 public:
  WebSocketClientFactoryLws();
  ~WebSocketClientFactoryLws() override;
  std::shared_ptr<IWebSocket> connect(const std::string& url, const std::vector<std::string>& protocols) override;
  WebSocketError last_error() const override;

 private:
  WebSocketError err_;
};

class WebSocketLwsAdapter : public IWebSocketAdapter {
 public:
  WebSocketLwsAdapter();
  ~WebSocketLwsAdapter() override;
  bool initialize() override;
  void cleanup() override;
  std::shared_ptr<IWebSocketServer> create_server() override;
  std::shared_ptr<IWebSocketClientFactory> create_client_factory() override;
  std::string get_version() override;
  bool is_available() const override { return available_; }

 private:
  bool available_;
};

#else

class WebSocketStub : public IWebSocket {
 public:
  bool send(const WebSocketMessage&) override { return false; }
  bool close(int, const std::string&) override { return false; }
  bool is_open() const override { return false; }
  WebSocketError last_error() const override { return WebSocketError::WS_INIT_FAILED; }
};

class WebSocketServerStub : public IWebSocketServer {
 public:
  bool start(const std::string&, uint16_t) override { return false; }
  void stop() override {}
  void set_on_message(OnMessageCB) override {}
  void set_on_open(OnOpenCB) override {}
  void set_on_close(OnCloseCB) override {}
  WebSocketError last_error() const override { return WebSocketError::WS_INIT_FAILED; }
};

class WebSocketClientFactoryStub : public IWebSocketClientFactory {
 public:
  std::shared_ptr<IWebSocket> connect(const std::string&, const std::vector<std::string>&) override { return nullptr; }
  WebSocketError last_error() const override { return WebSocketError::WS_INIT_FAILED; }
};

class WebSocketStubAdapter : public IWebSocketAdapter {
 public:
  bool initialize() override { return false; }
  void cleanup() override {}
  std::shared_ptr<IWebSocketServer> create_server() override { return std::make_shared<WebSocketServerStub>(); }
  std::shared_ptr<IWebSocketClientFactory> create_client_factory() override { return std::make_shared<WebSocketClientFactoryStub>(); }
  std::string get_version() override { return "stub"; }
  bool is_available() const override { return false; }
};

#endif  // AGENTFLOW_WITH_LIBWEBSOCKETS

}  // namespace network
}  // namespace agentflow
