#include "trace_cli.h"
#include "trajectory_evolution/trace.h"

#include <fstream>
#include <iostream>
#include <stdexcept>

namespace agentflow::trajectory {

bool is_standard_trace_input(const std::filesystem::path& path) {
  return ::trajectory::evolution::is_trace_path(path);
}

int run_doctor(int argc, char** argv) {
  using namespace ::trajectory::evolution;
  try {
    v1::CapabilityManifest manifest;
    manifest.set_schema_version(1);
    manifest.set_requested_level(v1::L0);
    manifest.set_fallback("downgrade");
    std::filesystem::path path, manifest_path;
    std::string requested_level, fallback;
    for(int i = 2; i < argc; ++i) {
      const std::string argument = argv[i];
      if(argument == "--help" || argument == "-h") {
        std::cout << "trajectory_evolution doctor <run.trace.json|run.trace.pb|sessionlog.jsonl>\n"
                     "  [--level L0..L4] [--manifest PATH] [--fallback strict|downgrade]\n"
                     "Prints JSON; exits 0 when enabled, 3 when invalid/strictly unsupported.\n";
        return 0;
      }
      if(argument == "--manifest" || argument == "--level" || argument == "--fallback") {
        if(++i >= argc) throw std::runtime_error("option requires a value");
        if(argument == "--manifest") {
          if(!manifest_path.empty()) throw std::runtime_error("duplicate manifest option");
          manifest_path = argv[i];
        } else if(argument == "--level") {
          if(!requested_level.empty()) throw std::runtime_error("duplicate level option");
          requested_level = argv[i];
        } else {
          if(!fallback.empty()) throw std::runtime_error("duplicate fallback option");
          fallback = argv[i];
        }
      } else if(!argument.empty() && argument.front() != '-' && path.empty()) {
        path = argument;
      } else {
        throw std::runtime_error("unexpected doctor argument");
      }
    }
    if(path.empty()) throw std::runtime_error("doctor requires an input path");
    if(!manifest_path.empty()) {
      std::ifstream input(manifest_path, std::ios::binary | std::ios::ate);
      if(!input || input.tellg() < 0 || input.tellg() > 65536) {
        throw std::runtime_error("manifest is unreadable or exceeds its limit");
      }
      const auto size = input.tellg();
      input.seekg(0);
      std::string bytes(static_cast<std::size_t>(size), '\0');
      if(size > 0 && !input.read(bytes.data(), size)) {
        throw std::runtime_error("cannot read manifest");
      }
      manifest = parse_manifest_json(bytes);
    }
    if(!requested_level.empty()) {
      v1::Level level;
      if(!v1::Level_Parse(requested_level, &level) || level < v1::L0 || level > v1::L4) {
        throw std::runtime_error("level must be L0..L4");
      }
      manifest.set_requested_level(level);
    }
    if(!fallback.empty()) manifest.set_fallback(fallback);
    const auto report = inspect_file(path, manifest);
    std::cout << to_json(report) << '\n';
    return report.trace_valid() && report.effective_level() != v1::LEVEL_NONE ? 0 : 3;
  } catch(const std::exception&) {
    // Imported evidence, parser diagnostics and file paths may contain secrets.
    std::cerr << "ERROR: doctor input or contract is invalid; check schema, path and options.\n";
    return 2;
  }
}

}  // namespace agentflow::trajectory
