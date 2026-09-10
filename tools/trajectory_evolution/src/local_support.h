#pragma once

#include "integration_json.h"

namespace trajectory::evolution::integration {
// Shared privacy gate; no filesystem, network, or model evaluation.
void validate_local_data(const Json& value);
}
