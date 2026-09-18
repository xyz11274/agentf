#pragma once

#include <filesystem>

namespace agentflow::trajectory {
bool is_standard_trace_input(const std::filesystem::path& path);
int run_doctor(int argc, char** argv);
}  // namespace agentflow::trajectory
