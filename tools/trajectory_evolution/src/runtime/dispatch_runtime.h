#pragma once

#include "instrumentation.h"

namespace trajectory::evolution::integration {

// A dispatch callback only submits work. Completion arrives through a later
// point, possibly in another process; all wave state belongs to the runtime.
Json dispatch_batch_point(Json& run, const Json& request, const Json& config,
                          const PolicyRuntime* cached_policy);

}  // namespace trajectory::evolution::integration
