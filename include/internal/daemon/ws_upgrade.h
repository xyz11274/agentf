// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License
//
// WebSocket upgrade handler for agentflowd RPC server
// Detects HTTP upgrade requests on TCP connections and performs WebSocket handshake

#ifndef AGENTFLOW_DAEMON_WS_UPGRADE_H
#define AGENTFLOW_DAEMON_WS_UPGRADE_H

#include <cstdint>
#include <string>

namespace agentflow {
namespace daemon {

// Result of checking a connection for WebSocket upgrade
enum class WsUpgradeResult {
  NOT_WS,        // Not a WebSocket upgrade request — treat as normal JSON-RPC
  UPGRADE_OK,    // WebSocket handshake completed successfully
  UPGRADE_FAIL,  // WebSocket handshake failed
};

// Check if the incoming data is an HTTP WebSocket upgrade request.
// If it is, perform the handshake and return UPGRADE_OK.
// Returns NOT_WS if this is normal JSON-RPC data (no HTTP headers detected).
WsUpgradeResult try_ws_upgrade(int fd, const std::string& initial_data);

// Encode a WebSocket text frame for sending
std::string ws_encode_text_frame(const std::string& payload);

// Decode a WebSocket frame. Returns true if a complete frame was decoded.
// Puts the decoded payload in `out_payload`. Consumed bytes are removed from `buffer`.
bool ws_decode_frame(std::string& buffer, std::string& out_payload, uint8_t& out_opcode);

// WebSocket opcodes
constexpr uint8_t WS_OPCODE_TEXT   = 0x01;
constexpr uint8_t WS_OPCODE_CLOSE = 0x08;
constexpr uint8_t WS_OPCODE_PING  = 0x09;
constexpr uint8_t WS_OPCODE_PONG  = 0x0A;

}  // namespace daemon
}  // namespace agentflow

#endif  // AGENTFLOW_DAEMON_WS_UPGRADE_H
