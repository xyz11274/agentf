#pragma once

#include <cstddef>
#include <filesystem>
#include <string>

#include "trajectory_trace.pb.h"

namespace trajectory::evolution {

struct TraceLimits {
  std::size_t max_bytes = 256U * 1024U * 1024U;
  std::size_t max_events = 100000;
};

struct ValidatedTrace {
  v1::TraceRun trace;
  v1::CapabilityReport report;
};

// JSON uses Protobuf JSON names (snake_case is also accepted). Unknown fields
// are rejected; application-specific fields belong in attributes.
v1::TraceRun parse_trace_json(const std::string& json,
                            TraceLimits limits = {});
v1::TraceRun read_trace(const std::filesystem::path& path,
                       TraceLimits limits = {});
ValidatedTrace validate_trace(const v1::TraceRun& trace,
                              TraceLimits limits = {});
v1::CapabilityManifest parse_manifest_json(const std::string& json);
v1::CapabilityReport inspect_capabilities(
    const ValidatedTrace& trace, const v1::CapabilityManifest& manifest);
v1::CapabilityReport inspect_file(
    const std::filesystem::path& path,
    const v1::CapabilityManifest& manifest,
    TraceLimits limits = {});
std::string to_json(const google::protobuf::Message& message, bool pretty = true);
bool is_trace_path(const std::filesystem::path& path);

}  // namespace trajectory::evolution
