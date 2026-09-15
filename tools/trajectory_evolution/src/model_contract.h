#pragma once

#include "integration_json.h"

namespace trajectory::evolution::integration {

// Read wire-format metadata, never descriptions, model prose or application code.
Json infer_model_binding(const Json& input);
Json infer_model_usage(const Json& output);
Json model_tool_contracts(const Json& input);
Json check_tool_contract(const Json& contracts, const std::string& operation, const Json& arguments);

}  // namespace trajectory::evolution::integration
