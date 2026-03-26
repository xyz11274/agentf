/**
 * LLM Query Node - C++ SO Library implementation (V3 signature with session-isolated DataBus)
 *
 * This node calls the llm-query library's simple_query interface
 * to send user prompts to LLM and return responses.
 *
 * V3 Signature: Uses INodeDataBusClient for session-isolated DataBus access.
 *   - sys namespace: Framework configuration (api_key, model, base_url) and response output
 *   - user namespace: Custom node data (optional)
 *
 * V2 Signature: Uses INodeDataBus for backward compatibility (raw DataBus access)
 * V1 Signature: Basic inputs/outputs only
 *
 * Async V3 Signature: Uses IAsyncCallSDK for non-blocking streaming LLM calls.
 *   - Streams chunks via SendToSelf
 *   - Sends [DONE] marker when complete
 *
 * Inputs (from databus or inputs map):
 *   - api_key: API key for authentication (preferably from sys namespace)
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
#include <memory>
#include <llm-query/llm-query.hpp>

// Include framework types and SO node interface
#include "agent/common.h"
#include "agentflow/so_node.h"

using DataVariant = agentflow::DataVariant;
using DataMap = agentflow::DataMap;
using INodeDataBus = agentflow::INodeDataBus;
using INodeDataBusClient = agentflow::INodeDataBusClient;
using INamespaceClient = agentflow::INamespaceClient;
using IAsyncCallSDK = agentflow::IAsyncCallSDK;
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

// Helper to get string from namespace client
static std::string get_string_from_namespace(INamespaceClient* ns, const std::string& key, const std::string& default_val = "") {
    if (ns) {
        SoDataVariant value;
        if (ns->get(key, value)) {
            if (auto* s = std::get_if<std::string>(&value)) {
                return *s;
            }
        }
    }
    return default_val;
}

// Helper to get string from DataBus (V2)
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

// V3 signature - with session-isolated DataBus client (recommended)
extern "C" void llm_query_node_v3(const DataMap& inputs, DataMap& outputs, INodeDataBusClient* client) {
    // Debug: show session info
    std::cerr << "[LLM_NODE_V3] Session ID: " << (client ? client->session_id() : "N/A") << std::endl;

    // Get configuration from sys namespace (injected by framework or test code)
    // Key path: /session/{session-id}/sys/api_key
    std::string api_key;
    std::string model;
    std::string base_url;

    if (client && client->sys()) {
        SoDataVariant value;

        // Get api_key from sys namespace
        if (client->sys()->get("api_key", value)) {
            if (auto* s = std::get_if<std::string>(&value)) {
                api_key = *s;
            }
        }
        std::cerr << "[LLM_NODE_V3] sys->get('api_key'): " << (api_key.empty() ? "NOT FOUND" : "FOUND") << std::endl;

        // Get model from sys namespace
        if (client->sys()->get("model", value)) {
            if (auto* s = std::get_if<std::string>(&value)) {
                model = *s;
            }
        }

        // Get base_url from sys namespace
        if (client->sys()->get("base_url", value)) {
            if (auto* s = std::get_if<std::string>(&value)) {
                base_url = *s;
            }
        }
    }

    // Fallback to inputs map
    if (model.empty()) {
        model = get_string_from_map(inputs, "model", "gpt-4.1");
    }
    if (base_url.empty()) {
        base_url = get_string_from_map(inputs, "base_url");
    }

    // Get prompt - try 'prompt' first, then 'input'
    std::string prompt = get_string_from_map(inputs, "prompt");
    if (prompt.empty()) {
        prompt = get_string_from_map(inputs, "input");
    }

    // Debug output
    std::cerr << "[LLM_NODE_V3] Calling LLM with model: " << model << std::endl;
    std::cerr << "[LLM_NODE_V3] Prompt: " << prompt << std::endl;
    std::cerr << "[LLM_NODE_V3] API Key present: " << (!api_key.empty() ? "yes" : "no") << std::endl;

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

        // Store response in sys namespace with dialogue isolation for framework access
        // Node assembles: {dialogue_id}/response
        // Client adds prefix: /session/{session_id}/sys/{dialogue_id}/response
        if (client && client->sys()) {
            std::string dialogue_id = get_string_from_map(inputs, "dialogue_id");
            std::string response_key = dialogue_id.empty() ? "response" : dialogue_id + "/response";
            client->sys()->put(response_key, SoDataVariant(response));
            std::cerr << "[LLM_NODE_V3] Stored response at key: " << response_key << std::endl;
        }

        std::cerr << "[LLM_NODE_V3] Response received successfully" << std::endl;

    } catch (const llm_query::ApiError& e) {
        outputs["status"] = std::string("error");

        // Parse error details
        std::string error_msg = e.what();
        if (error_msg.find("401") != std::string::npos) {
            outputs["error"] = std::string("Authentication failed: Invalid API key.");
            std::cerr << "[LLM_NODE_V3] Authentication error: Invalid API key" << std::endl;
        } else if (error_msg.find("429") != std::string::npos) {
            outputs["error"] = std::string("Rate limit exceeded. Please try again later.");
            std::cerr << "[LLM_NODE_V3] Rate limit error" << std::endl;
        } else {
            outputs["error"] = std::string("API Error: ") + error_msg;
            std::cerr << "[LLM_NODE_V3] API Error: " << error_msg << std::endl;
        }

        // Also store error in sys namespace
        if (client && client->sys()) {
            std::string* err_str = std::get_if<std::string>(&outputs["error"]);
            if (err_str) {
                client->sys()->put("error", SoDataVariant(*err_str));
            }
        }

    } catch (const std::exception& e) {
        outputs["status"] = std::string("error");
        outputs["error"] = std::string("Error: ") + std::string(e.what());

        std::cerr << "[LLM_NODE_V3] Error: " << e.what() << std::endl;

        // Also store error in sys namespace
        if (client && client->sys()) {
            client->sys()->put("error", SoDataVariant(std::string(e.what())));
        }
    }
}

// ============================================================================
// V3 Async Signature - Non-blocking streaming LLM calls
// ============================================================================

// Thread-local storage for accumulating response across callbacks
static thread_local std::string t_accumulated_response;

/**
 * Async V3 signature - Non-blocking streaming LLM query
 *
 * This version uses the IAsyncCallSDK to perform streaming LLM calls
 * without blocking the worker thread. Each chunk is sent back via
 * SendToSelf, and the node is re-invoked to process chunks.
 */
extern "C" void llm_query_node_async_v3(const DataMap& inputs, DataMap& outputs, INodeDataBusClient* client) {
    // Get async SDK
    IAsyncCallSDK* async_sdk = client ? client->async() : nullptr;
    if (!async_sdk) {
        outputs["status"] = std::string("error");
        outputs["error"] = std::string("Async SDK not available");
        return;
    }

    // ===== 1. Check if this is a callback invocation =====
    bool is_callback = false;
    std::string callback_key;
    std::string callback_msg;

    auto cb_it = inputs.find("__async_callback__");
    if (cb_it != inputs.end()) {
        if (auto* b = std::get_if<bool>(&cb_it->second)) {
            is_callback = *b;
        }
    }

    if (is_callback) {
        // Extract callback data
        auto key_it = inputs.find("__async_key__");
        auto msg_it = inputs.find("__async_msg__");

        if (key_it != inputs.end()) {
            if (auto* s = std::get_if<std::string>(&key_it->second)) {
                callback_key = *s;
            }
        }
        if (msg_it != inputs.end()) {
            if (auto* s = std::get_if<std::string>(&msg_it->second)) {
                callback_msg = *s;
            }
        }

        std::cerr << "[LLM_NODE_ASYNC] Callback received: key=" << callback_key
                  << ", msg_len=" << callback_msg.size() << std::endl;

        // Check if this is the done marker
        if (callback_msg == "[DONE]") {
            // Stream complete - return the accumulated response
            outputs["status"] = std::string("success");
            outputs["response"] = t_accumulated_response;

            // Store in sys namespace with dialogue isolation for framework access
            if (client && client->sys()) {
                std::string dialogue_id = get_string_from_map(inputs, "dialogue_id");
                std::string response_key = dialogue_id.empty() ? "response" : dialogue_id + "/response";
                client->sys()->put(response_key, SoDataVariant(t_accumulated_response));
                std::cerr << "[LLM_NODE_ASYNC] Stored response at key: " << response_key << std::endl;
            }

            // Forward [DONE] to coordinator via SendMsg
            // Get session_id and dialogue_id from inputs
            std::string session_id = get_string_from_map(inputs, "session_id");
            std::string dialogue_id_from_input = get_string_from_map(inputs, "dialogue_id");

            if (client && client->async() && !session_id.empty()) {
                std::string coordinator_task_id = session_id + "_coordinator";
                client->async()->SendMsg(
                    session_id,
                    dialogue_id_from_input,
                    coordinator_task_id,
                    "coordinator",
                    "llm-stream",
                    "[DONE]"
                );
                std::cerr << "[LLM_NODE_ASYNC] Forwarded [DONE] to coordinator" << std::endl;
            }

            std::cerr << "[LLM_NODE_ASYNC] Stream complete, response length: "
                      << t_accumulated_response.size() << std::endl;

            // Clear the accumulated response for next call
            t_accumulated_response.clear();

            // Don't set __async_pending__ - workflow will continue
            return;
        }

        // Accumulate the chunk
        t_accumulated_response += callback_msg;

        // Forward chunk to coordinator via SendMsg (for streaming output)
        // Get session_id and dialogue_id from inputs
        std::string session_id = get_string_from_map(inputs, "session_id");
        std::string dialogue_id_from_input = get_string_from_map(inputs, "dialogue_id");

        if (client && client->async() && !session_id.empty()) {
            std::string coordinator_task_id = session_id + "_coordinator";
            client->async()->SendMsg(
                session_id,
                dialogue_id_from_input,
                coordinator_task_id,
                "coordinator",
                "llm-stream",
                callback_msg
            );
            std::cerr << "[LLM_NODE_ASYNC] Forwarded chunk to coordinator, len=" << callback_msg.size() << std::endl;
        }

        // Continue waiting for more chunks (use DataVariant directly)
        outputs["__async_pending__"] = true;
        outputs["call_key"] = callback_key;
        outputs["current_chunk"] = callback_msg;

        std::cerr << "[LLM_NODE_ASYNC] Accumulated chunk, total length: "
                  << t_accumulated_response.size() << std::endl;
        return;
    }

    // ===== 2. Initial invocation - start async LLM call =====
    std::cerr << "[LLM_NODE_ASYNC] Starting async LLM call" << std::endl;

    // Get configuration from sys namespace
    std::string api_key;
    std::string model;
    std::string base_url;

    if (client && client->sys()) {
        SoDataVariant value;

        if (client->sys()->get("api_key", value)) {
            if (auto* s = std::get_if<std::string>(&value)) {
                api_key = *s;
            }
        }

        if (client->sys()->get("model", value)) {
            if (auto* s = std::get_if<std::string>(&value)) {
                model = *s;
            }
        }

        if (client->sys()->get("base_url", value)) {
            if (auto* s = std::get_if<std::string>(&value)) {
                base_url = *s;
            }
        }
    }

    // Fallback to inputs map
    if (api_key.empty()) {
        api_key = get_string_from_map(inputs, "api_key");
    }
    if (model.empty()) {
        model = get_string_from_map(inputs, "model", "qwen3.5-plus");
    }
    if (base_url.empty()) {
        base_url = get_string_from_map(inputs, "base_url");
    }

    // Get prompt
    std::string prompt = get_string_from_map(inputs, "prompt");
    if (prompt.empty()) {
        prompt = get_string_from_map(inputs, "input");
    }

    // Validate
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

    // Clear accumulated response for new request
    t_accumulated_response.clear();

    std::cerr << "[LLM_NODE_ASYNC] Config: model=" << model << ", prompt=" << prompt.substr(0, 50)
              << "..., api_key_present=" << (!api_key.empty() ? "yes" : "no") << std::endl;

    // ===== 3. Start async LLM call =====
    const std::string call_key = "llm-stream";

    // Capture necessary data for the async thread
    std::string captured_api_key = api_key;
    std::string captured_model = model;
    std::string captured_base_url = base_url;
    std::string captured_prompt = prompt;

    async_sdk->AsyncCall(call_key, [captured_api_key, captured_model, captured_base_url, captured_prompt, async_sdk]() {
        try {
            std::cerr << "[LLM_NODE_ASYNC_THREAD] Creating LLM client..." << std::endl;

            // Create LLM client
            llm_query::OpenAIConfig config;
            config.api_key = captured_api_key;
            if (!captured_base_url.empty()) {
                config.base_url = captured_base_url;
            }
            config.default_model = captured_model;
            config.timeout_seconds = 120;

            std::cerr << "[LLM_NODE_ASYNC_THREAD] API key len=" << captured_api_key.size()
                      << ", base_url=" << captured_base_url
                      << ", model=" << captured_model << std::endl;

            llm_query::OpenAIClient client(config);

            // Create request config
            llm_query::RequestConfig request;
            request.model = captured_model;
            request.messages.push_back({llm_query::Role::User, captured_prompt});
            request.stream = true;

            std::cerr << "[LLM_NODE_ASYNC_THREAD] Starting streaming request..." << std::endl;

            // Stream with callback
            size_t chunk_count = 0;
            size_t total_content_len = 0;
            client.query_stream(request, [&async_sdk, &chunk_count, &total_content_len](const std::string& chunk, bool is_done) {
                if (is_done) {
                    // Send done marker
                    async_sdk->SendToSelf("llm-stream", "[DONE]");
                    std::cerr << "[LLM_NODE_ASYNC_THREAD] Stream done after " << chunk_count
                              << " chunks, total content: " << total_content_len << " bytes" << std::endl;
                } else {
                    // Only send non-empty chunks
                    if (!chunk.empty()) {
                        async_sdk->SendToSelf("llm-stream", chunk);
                        chunk_count++;
                        total_content_len += chunk.size();
                        std::cerr << "[LLM_NODE_ASYNC_THREAD] Chunk #" << chunk_count
                                  << " size=" << chunk.size() << std::endl;
                    }
                }
            });

            std::cerr << "[LLM_NODE_ASYNC_THREAD] query_stream returned, total chunks: " << chunk_count << std::endl;

        } catch (const std::exception& e) {
            // Send error as chunk
            std::string error_msg = "[ERROR]" + std::string(e.what());
            async_sdk->SendToSelf("llm-stream", error_msg);
            async_sdk->SendToSelf("llm-stream", "[DONE]");
            std::cerr << "[LLM_NODE_ASYNC_THREAD] Error: " << e.what() << std::endl;
        }
    });

    std::cerr << "[LLM_NODE_ASYNC] Async call started, returning pending state" << std::endl;

    // Return pending state (use DataVariant directly for outputs map)
    outputs["__async_pending__"] = true;
    outputs["call_key"] = call_key;
    outputs["status"] = std::string("streaming");
}

// V2 signature - with DataBus access (backward compatibility)
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