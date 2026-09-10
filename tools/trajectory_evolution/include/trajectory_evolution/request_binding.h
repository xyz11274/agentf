#pragma once

#include "runtime_policy.pb.h"

namespace trajectory::evolution {
void validate_request_binding(const v1::RuntimeOptions& options);
google::protobuf::Struct inject_request_context(
    const google::protobuf::Struct& request, const std::string& context,
    const v1::RuntimeOptions& options);
bool bound_request_contains(const google::protobuf::Struct& request,
                            const std::string& context,
                            const v1::RuntimeOptions& options);
}  // namespace trajectory::evolution
