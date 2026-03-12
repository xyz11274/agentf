/**
 * LLM Query Node - C++ SO Library implementation (V2 signature with DataBus)
 *
 * This node calls the llm-query library's simple_query interface
 * to send user prompts to LLM and return responses.
 *
 * V2 Signature: Uses INodeDataBus to access shared configuration
 * from the framework's DataBus.
 *
 * Inputs (from databus or inputs map):
 *   - api_key: API key for authentication
 *   - model: Model name (optional, uses default if empty)
 *   - base_url: API base URL (optional, uses default if empty)
 *   - prompt: User prompt text
 *   - input: Alternative input field (fallback if prompt is empty)
 *
 * Outputs:
 *   - status: "success" or "error"
 *   - response: LLM response text (on success)
 *   - error: Error message (on error)
 */

#include <string>
#include <iostream>
#include <llm-query/llm-query.hpp>

// Include framework types and SO node interface
#include "agent/common.h"
#include "agentflow/so_node.h"

using DataVariant = agentflow::DataVariant;
using DataMap = agentflow::DataMap;
using INodeDataBus = agentflow::INodeDataBus;
using SoDataVariant = agentflow::SoDataVariant;

// Helper to get string from DataMap
static std::string get_string_from_map(const DataMap& map, const std::string& key, const std::string& default_val = "") {
    auto it = map.find(key);
    if (it != map.end()) {
        if (auto* s = std::get_if<std::string>(&it->second)) {
            return *s;
        }
    }
    return default_val;
}

// Helper to get string from DataBus
static std::string get_string_from_databus(INodeDataBus* databus, const std::string& key, const std::string& default_val = "") {
    if (databus) {
        SoDataVariant value;
        if (databus->get(key, value)) {
            if (auto* s = std::get_if<std::string>(&value)) {
                return *s;
            }
        }
    }
    return default_val;
}

// V2 signature - with DataBus access
extern "C" void llm_query_node_v2(const DataMap& inputs, DataMap& outputs, INodeDataBus* databus) {
    // Try to get configuration from DataBus first (injected by framework),
    // then fall back to inputs map
    std::string api_key = get_string_from_databus(databus, "api_key");
    if (api_key.empty()) {
        api_key = get_string_from_map(inputs, "api_key");
    }

    std::string model = get_string_from_databus(databus, "model");
    if (model.empty()) {
        model = get_string_from_map(inputs, "model", "gpt-4.1");
    }

    std::string base_url = get_string_from_databus(databus, "base_url");
    if (base_url.empty()) {
        base_url = get_string_from_map(inputs, "base_url");
    }

    // Get prompt - try 'prompt' first, then 'input'
    std::string prompt = get_string_from_map(inputs, "prompt");
    if (prompt.empty()) {
        prompt = get_string_from_map(inputs, "input");
    }

    // Debug output
    std::cerr << "[LLM_NODE_V2] Calling LLM with model: " << model << std::endl;
    std::cerr << "[LLM_NODE_V2] Prompt: " << prompt << std::endl;
    std::cerr << "[LLM_NODE_V2] API Key present: " << (!api_key.empty() ? "yes" : "no") << std::endl;

    // Validate required inputs
    if (api_key.empty()) {
        outputs["status"] = std::string("error");
        outputs["error"] = std::string("API key not provided");
        return;
    }
    if (prompt.empty()) {
        outputs["status"] = std::string("error");
        outputs["error"] = std::string("No prompt provided");
        return;
    }

    // Call LLM using llm-query library
    try {
        std::string response = llm_query::simple_query(api_key, prompt, model, base_url);
        outputs["status"] = std::string("success");
        outputs["response"] = response;

        // Debug output
        std::cerr << "[LLM_NODE_V2] Response received successfully" << std::endl;

    } catch (const llm_query::ApiError& e) {
        outputs["status"] = std::string("error");

        // Parse error details
        std::string error_msg = e.what();
        if (error_msg.find("401") != std::string::npos) {
            outputs["error"] = std::string("Authentication failed: Invalid API key.");
            std::cerr << "[LLM_NODE_V2] Authentication error: Invalid API key" << std::endl;
        } else if (error_msg.find("429") != std::string::npos) {
            outputs["error"] = std::string("Rate limit exceeded. Please try again later.");
            std::cerr << "[LLM_NODE_V2] Rate limit error" << std::endl;
        } else {
            outputs["error"] = std::string("API Error: ") + error_msg;
            std::cerr << "[LLM_NODE_V2] API Error: " << error_msg << std::endl;
        }

    } catch (const std::exception& e) {
        outputs["status"] = std::string("error");
        outputs["error"] = std::string("Error: ") + std::string(e.what());

        std::cerr << "[LLM_NODE_V2] Error: " << e.what() << std::endl;
    }
}

// V1 signature - backward compatibility
extern "C" void llm_query_node(const DataMap& inputs, DataMap& outputs) {
    llm_query_node_v2(inputs, outputs, nullptr);
}