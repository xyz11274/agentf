// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License
//
// SSL/TLS adapter interface for secure communication

#pragma once

#include <memory>
#include <string>
#include <vector>
#include <cstdint>

namespace agentflow {
namespace network {

/**
 * SSL/TLS error codes
 */
enum class SslError {
  SSL_ERR_OK = 0,
  SSL_ERR_INIT_FAILED = 1,
  SSL_ERR_CERT_NOT_FOUND = 2,
  SSL_ERR_KEY_NOT_FOUND = 3,
  SSL_ERR_LOAD_FAILED = 4,
  SSL_ERR_HANDSHAKE_FAILED = 5,
  SSL_ERR_CONNECTION_FAILED = 6,
  SSL_ERR_READ_FAILED = 7,
  SSL_ERR_WRITE_FAILED = 8,
  SSL_ERR_INVALID_STATE = 9,
};

/**
 * SSL/TLS protocol versions
 */
enum class SslVersion {
  SSL_TLS_1_0 = 1,
  SSL_TLS_1_1 = 2,
  SSL_TLS_1_2 = 3,
  SSL_TLS_1_3 = 4,
};

/**
 * SSL/TLS cipher suite info
 */
struct SslCipherInfo {
  std::string name;
  uint32_t bits;
  std::string protocol;
};

/**
 * SSL certificate info
 */
struct SslCertificateInfo {
  std::string subject;
  std::string issuer;
  std::string fingerprint;
  int64_t valid_from;  // Unix timestamp
  int64_t valid_until; // Unix timestamp
  std::vector<std::string> alt_names;
};

/**
 * Abstract SSL connection interface
 */
class ISslConnection {
 public:
  virtual ~ISslConnection() = default;

  /**
   * Perform SSL handshake
   */
  virtual bool handshake() = 0;

  /**
   * Read encrypted data
   */
  virtual int read(uint8_t* buffer, size_t size) = 0;

  /**
   * Write encrypted data
   */
  virtual int write(const uint8_t* buffer, size_t size) = 0;

  /**
   * Get peer certificate info
   */
  virtual std::shared_ptr<SslCertificateInfo> get_peer_certificate() = 0;

  /**
   * Get cipher info
   */
  virtual std::shared_ptr<SslCipherInfo> get_cipher_info() = 0;

  /**
   * Get protocol version
   */
  virtual SslVersion get_protocol_version() = 0;

  /**
   * Get last error
   */
  virtual SslError get_error() const = 0;

  /**
   * Get error message
   */
  virtual std::string get_error_message() const = 0;

  /**
   * Close connection
   */
  virtual void close() = 0;

  /**
   * Check if connection is established
   */
  virtual bool is_connected() const = 0;
};

/**
 * Abstract SSL context interface
 */
class ISslContext {
 public:
  virtual ~ISslContext() = default;

  /**
   * Load certificate from file
   */
  virtual bool load_certificate(const std::string& cert_file) = 0;

  /**
   * Load private key from file
   */
  virtual bool load_private_key(const std::string& key_file) = 0;

  /**
   * Load CA certificate for verification
   */
  virtual bool load_ca_certificate(const std::string& ca_file) = 0;

  /**
   * Set minimum TLS version
   */
  virtual bool set_min_protocol_version(SslVersion version) = 0;

  /**
   * Enable/disable certificate verification
   */
  virtual void set_verify_peer(bool verify) = 0;

  /**
   * Create client connection
   */
  virtual std::shared_ptr<ISslConnection> create_client_connection(
      int socket_fd) = 0;

  /**
   * Create server connection
   */
  virtual std::shared_ptr<ISslConnection> create_server_connection(
      int socket_fd) = 0;

  /**
   * Get context error
   */
  virtual SslError get_error() const = 0;

  /**
   * Get error message
   */
  virtual std::string get_error_message() const = 0;
};

/**
 * Abstract SSL adapter interface
 * Isolates OpenSSL or other SSL implementations from AgentFlow core
 */
class ISslAdapter {
 public:
  virtual ~ISslAdapter() = default;

  /**
   * Initialize SSL library
   */
  virtual bool initialize() = 0;

  /**
   * Cleanup SSL library
   */
  virtual void cleanup() = 0;

  /**
   * Create SSL context for client
   */
  virtual std::shared_ptr<ISslContext> create_client_context() = 0;

  /**
   * Create SSL context for server
   */
  virtual std::shared_ptr<ISslContext> create_server_context() = 0;

  /**
   * Get library version
   */
  virtual std::string get_version() = 0;

  /**
   * Check if SSL support is available
   */
  virtual bool is_available() const = 0;
};

}  // namespace network
}  // namespace agentflow
