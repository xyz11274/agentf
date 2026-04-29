// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License
//
// WebSocket upgrade handler implementation
// Minimal RFC 6455 WebSocket handshake and framing for JSON-RPC over WS

#include "daemon/ws_upgrade.h"

#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cstring>
#include <sstream>

#include "utils/logger.h"

namespace agentflow {
namespace daemon {

namespace {

// Compute SHA-1 using OpenSSL if available, otherwise a simple fallback
// For WebSocket, we only need SHA-1 for the Sec-WebSocket-Accept header
// which is a protocol requirement, not a security feature

#ifdef AGENTFLOW_WITH_OPENSSL
#include <openssl/sha.h>

std::string sha1_base64(const std::string& input) {
  unsigned char hash[SHA_DIGEST_LENGTH];
  SHA1(reinterpret_cast<const unsigned char*>(input.c_str()), input.size(), hash);

  static const char kBase64[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string result;
  result.reserve(28);

  for (int i = 0; i < 20; i += 3) {
    uint32_t n = (static_cast<uint32_t>(hash[i]) << 16);
    if (i + 1 < 20) n |= (static_cast<uint32_t>(hash[i + 1]) << 8);
    if (i + 2 < 20) n |= static_cast<uint32_t>(hash[i + 2]);

    result += kBase64[(n >> 18) & 0x3F];
    result += kBase64[(n >> 12) & 0x3F];
    result += (i + 1 < 20) ? kBase64[(n >> 6) & 0x3F] : '=';
    result += (i + 2 < 20) ? kBase64[n & 0x3F] : '=';
  }
  return result;
}

#else
// Fallback: simple SHA-1 implementation for WebSocket handshake
// This is only used for the Sec-WebSocket-Accept computation

struct SHA1_CTX {
  uint32_t state[5];
  uint64_t count;
  uint8_t buffer[64];
};

void sha1_init(SHA1_CTX* ctx) {
  ctx->state[0] = 0x67452301;
  ctx->state[1] = 0xEFCDAB89;
  ctx->state[2] = 0x98BADCFE;
  ctx->state[3] = 0x10325476;
  ctx->state[4] = 0xC3D2E1F0;
  ctx->count = 0;
}

void sha1_transform(uint32_t state[5], const uint8_t block[64]) {
  uint32_t w[80];
  for (int i = 0; i < 16; i++) {
    w[i] = (static_cast<uint32_t>(block[i * 4]) << 24)
         | (static_cast<uint32_t>(block[i * 4 + 1]) << 16)
         | (static_cast<uint32_t>(block[i * 4 + 2]) << 8)
         | static_cast<uint32_t>(block[i * 4 + 3]);
  }
  for (int i = 16; i < 80; i++) {
    uint32_t tmp = w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16];
    w[i] = (tmp << 1) | (tmp >> 31);
  }

  uint32_t a = state[0], b = state[1], c = state[2], d = state[3], e = state[4];
  for (int i = 0; i < 80; i++) {
    uint32_t f, k;
    if (i < 20)      { f = (b & c) | (~b & d); k = 0x5A827999; }
    else if (i < 40) { f = b ^ c ^ d;          k = 0x6ED9EBA1; }
    else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
    else              { f = b ^ c ^ d;          k = 0xCA62C1D6; }

    uint32_t temp = ((a << 5) | (a >> 27)) + f + e + k + w[i];
    e = d; d = c; c = (b << 30) | (b >> 2); b = a; a = temp;
  }
  state[0] += a; state[1] += b; state[2] += c; state[3] += d; state[4] += e;
}

void sha1_update(SHA1_CTX* ctx, const uint8_t* data, size_t len) {
  size_t idx = static_cast<size_t>(ctx->count % 64);
  ctx->count += len;
  for (size_t i = 0; i < len; i++) {
    ctx->buffer[idx++] = data[i];
    if (idx == 64) {
      sha1_transform(ctx->state, ctx->buffer);
      idx = 0;
    }
  }
}

void sha1_final(SHA1_CTX* ctx, uint8_t digest[20]) {
  uint64_t bits = ctx->count * 8;
  ctx->buffer[ctx->count % 64] = 0x80;
  size_t idx = (ctx->count % 64) + 1;
  if (idx > 56) {
    while (idx < 64) ctx->buffer[idx++] = 0;
    sha1_transform(ctx->state, ctx->buffer);
    idx = 0;
  }
  while (idx < 56) ctx->buffer[idx++] = 0;
  for (int i = 0; i < 8; i++) {
    ctx->buffer[63 - i] = static_cast<uint8_t>(bits & 0xFF);
    bits >>= 8;
  }
  sha1_transform(ctx->state, ctx->buffer);
  for (int i = 0; i < 5; i++) {
    digest[i * 4]     = static_cast<uint8_t>((ctx->state[i] >> 24) & 0xFF);
    digest[i * 4 + 1] = static_cast<uint8_t>((ctx->state[i] >> 16) & 0xFF);
    digest[i * 4 + 2] = static_cast<uint8_t>((ctx->state[i] >> 8) & 0xFF);
    digest[i * 4 + 3] = static_cast<uint8_t>(ctx->state[i] & 0xFF);
  }
}

std::string sha1_base64(const std::string& input) {
  SHA1_CTX ctx;
  sha1_init(&ctx);
  sha1_update(&ctx, reinterpret_cast<const uint8_t*>(input.c_str()), input.size());
  uint8_t hash[20];
  sha1_final(&ctx, hash);

  static const char kBase64[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string result;
  result.reserve(28);
  for (int i = 0; i < 20; i += 3) {
    uint32_t n = (static_cast<uint32_t>(hash[i]) << 16);
    if (i + 1 < 20) n |= (static_cast<uint32_t>(hash[i + 1]) << 8);
    if (i + 2 < 20) n |= static_cast<uint32_t>(hash[i + 2]);
    result += kBase64[(n >> 18) & 0x3F];
    result += kBase64[(n >> 12) & 0x3F];
    result += (i + 1 < 20) ? kBase64[(n >> 6) & 0x3F] : '=';
    result += (i + 2 < 20) ? kBase64[n & 0x3F] : '=';
  }
  return result;
}
#endif  // AGENTFLOW_WITH_OPENSSL

// Extract header value from HTTP request
std::string extract_header(const std::string& request, const std::string& header_name) {
  std::string search = header_name + ":";
  size_t pos = request.find(search);
  if (pos == std::string::npos) return "";
  pos += search.size();
  while (pos < request.size() && (request[pos] == ' ' || request[pos] == '\t')) pos++;
  size_t end = pos;
  while (end < request.size() && request[end] != '\r' && request[end] != '\n') end++;
  return request.substr(pos, end - pos);
}

constexpr const char* kWsGuid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

}  // namespace

WsUpgradeResult try_ws_upgrade(int fd, const std::string& initial_data) {
  // Quick check: WebSocket upgrades start with "GET "
  if (initial_data.size() < 4 || initial_data.substr(0, 4) != "GET ") {
    return WsUpgradeResult::NOT_WS;
  }

  // Check for Upgrade: websocket header
  std::string upgrade = extract_header(initial_data, "Upgrade");
  if (upgrade != "websocket") {
    return WsUpgradeResult::NOT_WS;
  }

  // Check for Sec-WebSocket-Key
  std::string ws_key = extract_header(initial_data, "Sec-WebSocket-Key");
  if (ws_key.empty()) {
    Logger::instance().warn("[WS_UPGRADE] Missing Sec-WebSocket-Key");
    return WsUpgradeResult::UPGRADE_FAIL;
  }

  // Compute Sec-WebSocket-Accept
  std::string combined = ws_key + kWsGuid;
  std::string accept = sha1_base64(combined);

  // Build HTTP 101 response
  std::string response =
      "HTTP/1.1 101 Switching Protocols\r\n"
      "Upgrade: websocket\r\n"
      "Connection: Upgrade\r\n"
      "Sec-WebSocket-Accept: " + accept + "\r\n"
      "\r\n";

  ssize_t sent = send(fd, response.c_str(), response.size(), MSG_NOSIGNAL);
  if (sent < 0) {
    Logger::instance().error_f("[WS_UPGRADE] Send failed: %s", strerror(errno));
    return WsUpgradeResult::UPGRADE_FAIL;
  }

  Logger::instance().info("[WS_UPGRADE] WebSocket handshake completed");
  return WsUpgradeResult::UPGRADE_OK;
}

std::string ws_encode_text_frame(const std::string& payload) {
  std::string frame;
  frame.reserve(payload.size() + 10);

  // FIN + opcode (text)
  frame += static_cast<char>(0x80 | WS_OPCODE_TEXT);

  // Payload length
  if (payload.size() <= 125) {
    frame += static_cast<char>(payload.size());
  } else if (payload.size() <= 65535) {
    frame += static_cast<char>(126);
    frame += static_cast<char>((payload.size() >> 8) & 0xFF);
    frame += static_cast<char>(payload.size() & 0xFF);
  } else {
    frame += static_cast<char>(127);
    for (int i = 7; i >= 0; i--) {
      frame += static_cast<char>((payload.size() >> (i * 8)) & 0xFF);
    }
  }

  // Mask is not set (server→client frames are unmasked)
  frame += payload;
  return frame;
}

bool ws_decode_frame(std::string& buffer, std::string& out_payload, uint8_t& out_opcode) {
  if (buffer.size() < 2) return false;

  uint8_t b0 = static_cast<uint8_t>(buffer[0]);
  uint8_t b1 = static_cast<uint8_t>(buffer[1]);

  out_opcode = b0 & 0x0F;
  bool masked = (b1 & 0x80) != 0;
  uint64_t payload_len = b1 & 0x7F;

  size_t header_size = 2;
  if (payload_len == 126) {
    if (buffer.size() < 4) return false;
    payload_len = (static_cast<uint64_t>(static_cast<uint8_t>(buffer[2])) << 8)
               | static_cast<uint64_t>(static_cast<uint8_t>(buffer[3]));
    header_size = 4;
  } else if (payload_len == 127) {
    if (buffer.size() < 10) return false;
    payload_len = 0;
    for (int i = 0; i < 8; i++) {
      payload_len = (payload_len << 8) | static_cast<uint8_t>(buffer[2 + i]);
    }
    header_size = 10;
  }

  if (masked) header_size += 4;  // Masking key

  if (buffer.size() < header_size + payload_len) return false;

  out_payload.resize(payload_len);
  if (masked) {
    const uint8_t* mask = reinterpret_cast<const uint8_t*>(buffer.data() + header_size - 4);
    const uint8_t* data = reinterpret_cast<const uint8_t*>(buffer.data() + header_size);
    for (uint64_t i = 0; i < payload_len; i++) {
      out_payload[i] = data[i] ^ mask[i % 4];
    }
  } else {
    std::memcpy(out_payload.data(), buffer.data() + header_size, payload_len);
  }

  buffer.erase(0, header_size + payload_len);
  return true;
}

}  // namespace daemon
}  // namespace agentflow
