// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License
//
// Chain Test Coordinator Node - Verifies multi-language chain execution results
// Uses V3 signature with session-isolated DataBus client

#include <string>

#include "agent/common.h"
#include "agentflow/so_node.h"

namespace {

using agentflow::DataMap;
using agentflow::DataVariant;
using agentflow::INamespaceClient;
using agentflow::INodeDataBusClient;
using agentflow::SoDataVariant;

std::string get_string(const DataMap& inputs, const std::string& key,
                       const std::string& fallback = "") {
  auto it = inputs.find(key);
  if (it == inputs.end()) {
    return fallback;
  }
  if (const auto* value = std::get_if<std::string>(&it->second)) {
    return *value;
  }
  return fallback;
}

std::string get_optional_string(INamespaceClient* ns, const std::string& key) {
  if (!ns) {
    return "";
  }
  SoDataVariant value;
  if (ns->get(key, value)) {
    if (const auto* text = std::get_if<std::string>(&value)) {
      return *text;
    }
  }
  return "";
}

}  // namespace

// V3 signature with session-isolated DataBus client
extern "C" void chain_coordinator_node_v3(const DataMap& inputs, DataMap& outputs,
                                          INodeDataBusClient* client) {
  std::string session_id = get_string(inputs, "session_id", "");
  std::string dialogue_id = get_string(inputs, "dialogue_id", "");

  // Build keys with dialogue_id prefix
  std::string chain_key = dialogue_id.empty() ? "chain_data" : dialogue_id + "/chain_data";
  std::string step_key = dialogue_id.empty() ? "chain_data_step" : dialogue_id + "/chain_data_step";
  std::string verification_key = dialogue_id.empty() ? "chain_verification" : dialogue_id + "/chain_verification";

  // Read results from the multi-language chain
  std::string final_data = get_optional_string(client ? client->user() : nullptr, chain_key);
  std::string current_step = get_optional_string(client ? client->user() : nullptr, step_key);
  std::string verification = get_optional_string(client ? client->user() : nullptr, verification_key);

  // Build response
  std::string response = "Multi-Language Chain Test Results:\n\n";
  response += "Session ID: " + session_id + "\n";
  response += "Dialogue ID: " + (dialogue_id.empty() ? "(none)" : dialogue_id) + "\n\n";

  if (!final_data.empty()) {
    response += "Final Data: " + final_data + "\n";
    response += "Last Step: " + current_step + "\n";
    response += "Verification: " + (verification.empty() ? "pending" : verification) + "\n\n";

    // Check if chain completed successfully
    if (final_data.find("GO_PROCESSED") != std::string::npos) {
      response += "✓ Chain completed successfully!\n";
      response += "  - C++ Producer: ✓\n";
      response += "  - Python Transformer: ✓\n";
      response += "  - Go Processor: ✓\n";
      response += "  - Shell Verifier: ✓\n";
    } else if (final_data.find("PYTHON_TRANSFORMED") != std::string::npos) {
      response += "⚠ Chain partially completed (Go step missing)\n";
      response += "  - C++ Producer: ✓\n";
      response += "  - Python Transformer: ✓\n";
      response += "  - Go Processor: ✗\n";
    } else if (final_data.find("CPP_DATA") != std::string::npos) {
      response += "⚠ Chain incomplete (only C++ step completed)\n";
      response += "  - C++ Producer: ✓\n";
      response += "  - Python Transformer: ✗\n";
    } else {
      response += "✗ Chain data format unexpected\n";
    }
  } else {
    response += "No chain data found\n";
    response += "Chain Status: FAILED\n";
  }

  // Write response to sys namespace for framework to return to user
  if (client && client->sys()) {
    std::string response_key = dialogue_id.empty() ? "response" : dialogue_id + "/response";
    client->sys()->put(response_key, SoDataVariant(response));
  }

  // Output results
  outputs["response"] = DataVariant(response);
  outputs["chain_status"] = DataVariant(verification.empty() ? "unknown" : verification);
}