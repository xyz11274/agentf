// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License
//
// OpenSSL implementation for SSL/TLS support

#pragma once

#include "runtime/network/ssl_adapter.h"

#ifdef AGENTFLOW_WITH_OPENSSL
#include <openssl/err.h>
#include <openssl/ssl.h>
#endif

namespace agentflow {
namespace network {

#ifdef AGENTFLOW_WITH_OPENSSL

/**
 * OpenSSL-based SSL connection implementation
 */
class SslOpensslConnection : public ISslConnection {
public:
  explicit SslOpensslConnection(SSL* ssl, int socket_fd);
  ~SslOpensslConnection() override;

  bool                                handshake() override;
  int                                 read(uint8_t* buffer, size_t size) override;
  int                                 write(const uint8_t* buffer, size_t size) override;
  std::shared_ptr<SslCertificateInfo> get_peer_certificate() override;
  std::shared_ptr<SslCipherInfo>      get_cipher_info() override;
  SslVersion                          get_protocol_version() override;
  SslError                            get_error() const override { return error_; }
  std::string                         get_error_message() const override;
  void                                close() override;
  bool                                is_connected() const override { return connected_; }

private:
  SSL*             ssl_;
  int              socket_fd_;
  bool             connected_;
  mutable SslError error_;

  SslError convert_openssl_error(int ret);
};

/**
 * OpenSSL-based SSL context implementation
 */
class SslOpensslContext : public ISslContext {
public:
  explicit SslOpensslContext(SSL_CTX* ctx);
  ~SslOpensslContext() override;

  bool                            load_certificate(const std::string& cert_file) override;
  bool                            load_private_key(const std::string& key_file) override;
  bool                            load_ca_certificate(const std::string& ca_file) override;
  bool                            set_min_protocol_version(SslVersion version) override;
  void                            set_verify_peer(bool verify) override;
  std::shared_ptr<ISslConnection> create_client_connection(int socket_fd) override;
  std::shared_ptr<ISslConnection> create_server_connection(int socket_fd) override;
  SslError                        get_error() const override { return error_; }
  std::string                     get_error_message() const override;

private:
  SSL_CTX*         ctx_;
  mutable SslError error_;

  int openssl_version_from_ssl_version(SslVersion version);
};

/**
 * OpenSSL adapter implementation
 */
class SslOpensslAdapter : public ISslAdapter {
public:
  SslOpensslAdapter();
  ~SslOpensslAdapter() override;

  bool                         initialize() override;
  void                         cleanup() override;
  std::shared_ptr<ISslContext> create_client_context() override;
  std::shared_ptr<ISslContext> create_server_context() override;
  std::string                  get_version() override;
  bool                         is_available() const override { return available_; }

private:
  bool available_;
};

#else

/**
 * Stub SSL connection for when OpenSSL is not available
 */
class SslStubConnection : public ISslConnection {
public:
  explicit SslStubConnection(int socket_fd) : socket_fd_(socket_fd) {}
  ~SslStubConnection() override = default;

  bool                                handshake() override { return false; }
  int                                 read(uint8_t*, size_t) override { return -1; }
  int                                 write(const uint8_t*, size_t) override { return -1; }
  std::shared_ptr<SslCertificateInfo> get_peer_certificate() override { return nullptr; }
  std::shared_ptr<SslCipherInfo>      get_cipher_info() override { return nullptr; }
  SslVersion  get_protocol_version() override { return SslVersion::SSL_TLS_1_2; }
  SslError    get_error() const override { return SslError::SSL_ERR_INIT_FAILED; }
  std::string get_error_message() const override { return "OpenSSL not available"; }
  void        close() override {}
  bool        is_connected() const override { return false; }

private:
  int socket_fd_;
};

/**
 * Stub SSL context for when OpenSSL is not available
 */
class SslStubContext : public ISslContext {
public:
  bool                            load_certificate(const std::string&) override { return false; }
  bool                            load_private_key(const std::string&) override { return false; }
  bool                            load_ca_certificate(const std::string&) override { return false; }
  bool                            set_min_protocol_version(SslVersion) override { return false; }
  void                            set_verify_peer(bool) override {}
  std::shared_ptr<ISslConnection> create_client_connection(int) override { return nullptr; }
  std::shared_ptr<ISslConnection> create_server_connection(int) override { return nullptr; }
  SslError    get_error() const override { return SslError::SSL_ERR_INIT_FAILED; }
  std::string get_error_message() const override { return "OpenSSL not available"; }
};

/**
 * Stub SSL adapter for when OpenSSL is not available
 */
class SslStubAdapter : public ISslAdapter {
public:
  bool                         initialize() override { return false; }
  void                         cleanup() override {}
  std::shared_ptr<ISslContext> create_client_context() override {
    return std::make_shared<SslStubContext>();
  }
  std::shared_ptr<ISslContext> create_server_context() override {
    return std::make_shared<SslStubContext>();
  }
  std::string get_version() override { return "stub"; }
  bool        is_available() const override { return false; }
};

#endif  // AGENTFLOW_WITH_OPENSSL

}  // namespace network
}  // namespace agentflow
