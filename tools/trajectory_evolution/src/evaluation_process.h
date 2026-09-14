#pragma once

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace trajectory::evolution {

struct EvaluationProcessLimits {
  std::size_t address_space_bytes = 0;
  std::size_t cpu_seconds = 0;
  int nice = 0;
};

struct EvaluationProcessResult {
  std::string output;
  std::string error_code;
  int exit_code = -1;
  double duration_ms = 0;
};

// Internal CLI entry; ordinary applications call run_evaluation_process.
int run_evaluation_supervisor(int argc, char** argv);

// No shell expansion; only explicitly selected environment variables reach the
// host. stdout/stderr are bounded in memory and never logged verbatim.
EvaluationProcessResult run_evaluation_process(
    const std::vector<std::string>& command,
    const std::vector<std::string>& environment,
    const std::filesystem::path& directory, const std::string& input,
    std::size_t timeout_ms, std::size_t max_output_bytes,
    const std::function<bool()>& cancelled,
    const EvaluationProcessLimits& limits = {},
    const std::function<bool()>& capture_finished = {},
    const std::function<std::string()>& entry_status = {});

}  // namespace trajectory::evolution
