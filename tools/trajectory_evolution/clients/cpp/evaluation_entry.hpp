#pragma once
#include <trajectory_evolution/evaluation_entry.h>
#include <stdexcept>
#include <string>
#include <type_traits>

namespace trajectory_evolution_http {
// Pass an existing JSON receiver; no framework-specific runner is required.
template<class Receiver> bool evaluate(Receiver&& receiver) {
  auto bound = [&receiver](const std::string& input) { receiver(input); };
  using Function = decltype(bound);
  const auto call = [](const char* input, void* context) -> int {
    try { (*static_cast<Function*>(context))(std::string(input)); return 0; }
    catch(...) { return -1; }
  };
  const auto result = te_evaluate(call, &bound);
  if(result < 0) throw std::runtime_error("evaluation_entry_failed");
  return result == 1;
}
}  // namespace trajectory_evolution_http
