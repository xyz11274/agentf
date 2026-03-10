/**
 * LLM Query Node - C++ SO Library implementation
 *
 * This node calls the llm-query library's simple_query interface
 * to send user prompts to LLM and return responses.
 *
 * Inputs:
 *   - api_key: API key for authentication
 *   - model: Model name (optional, uses default if empty)
 *   - base_url: API base URL (optional, uses default if empty)
 *   - prompt: User prompt text
 *
 * Outputs:
 *   - status: "success" or "error"
 *   - response: LLM response text (on success)
 *   - error: Error message (on error)
 */

#include <string>
#include <map>
#include <variant>
#include <iostream>
#include <llm-query/llm-query.hpp>

using DataVariant = std::variant<int64_t, double, std::string, bool>;
using DataMap = std::map<std::string, DataVariant>;

extern "C" void llm_query_node(const DataMap& inputs, DataMap& outputs) {
    // Extract input parameters
    std::string api_key, model, base_url, prompt;

    if (auto it = inputs.find("api_key"); it != inputs.end()) {
        if (auto* s = std::get_if<std::string>(&it->second)) {
            api_key = *s;
        }
    }
    if (auto it = inputs.find("model"); it != inputs.end()) {
        if (auto* s = std::get_if<std::string>(&it->second)) {
            model = *s;
        }
    }
    if (auto it = inputs.find("base_url"); it != inputs.end()) {
        if (auto* s = std::get_if<std::string>(&it->second)) {
            base_url = *s;
        }
    }
    if (auto it = inputs.find("prompt"); it != inputs.end()) {
        if (auto* s = std::get_if<std::string>(&it->second)) {
            prompt = *s;
        }
    }

    // Debug output
    std::cerr << "[LLM_NODE] Calling LLM with model: " << model << std::endl;
    std::cerr << "[LLM_NODE] Prompt: " << prompt << std::endl;

    // Validate required inputs
    if (api_key.empty()) {
        outputs["status"] = std::string("error");
        outputs["error"] = std::string("LLM_API_KEY environment variable not set");
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
        std::cerr << "[LLM_NODE] Response received successfully" << std::endl;

    } catch (const llm_query::ApiError& e) {
        outputs["status"] = std::string("error");

        // Parse error details
        std::string error_msg = e.what();
        if (error_msg.find("401") != std::string::npos) {
            outputs["error"] = std::string("Authentication failed: Invalid API key. Please check LLM_API_KEY environment variable.");
            std::cerr << "[LLM_NODE] Authentication error: Invalid API key" << std::endl;
        } else if (error_msg.find("429") != std::string::npos) {
            outputs["error"] = std::string("Rate limit exceeded. Please try again later.");
            std::cerr << "[LLM_NODE] Rate limit error" << std::endl;
        } else {
            outputs["error"] = std::string("API Error: ") + error_msg;
            std::cerr << "[LLM_NODE] API Error: " << error_msg << std::endl;
        }

    } catch (const std::exception& e) {
        outputs["status"] = std::string("error");
        outputs["error"] = std::string("Error: ") + std::string(e.what());

        std::cerr << "[LLM_NODE] Error: " << e.what() << std::endl;
    }
}