#include "shadow_cli.h"
#include "trajectory_evolution/shadow_runtime.h"
#include "trajectory_evolution/trace.h"

#include <array>
#include <iostream>
#include <string>
#include <google/protobuf/util/json_util.h>

namespace agentflow::trajectory {
namespace v1 = ::trajectory::evolution::v1;
using ::trajectory::evolution::compile_shadow_policy_file;
using ::trajectory::evolution::ShadowRuntime;
using ::trajectory::evolution::to_json;

int run_shadow(int argc, char** argv) {
  if(argc != 3 || (std::string(argv[2]) != "compile"
                  && std::string(argv[2]) != "query")) {
    std::cerr << "Usage: trajectory_evolution shadow compile|query < request.json\n";
    return 2;
  }
  try {
    // Hard transport ceiling; policy/query limits are separately configured.
    std::string input;
    std::array<char, 4096> buffer{};
    while(std::cin) {
      std::cin.read(buffer.data(), buffer.size());
      input.append(buffer.data(), static_cast<std::size_t>(std::cin.gcount()));
      if(input.size() > 2U * 1024U * 1024U) {
        throw std::runtime_error("shadow_request_too_large");
      }
    }
    v1::ShadowCommand request;
    if(!google::protobuf::util::JsonStringToMessage(input, &request).ok()) {
      throw std::runtime_error("invalid_shadow_request");
    }
    if(std::string(argv[2]) == "compile") {
      if(request.has_snapshot() || request.has_query()) {
        throw std::runtime_error("invalid_compile_request");
      }
      std::cout << to_json(compile_shadow_policy_file(
          request.memory_path(), request.identity(), request.options()), false) << '\n';
    } else {
      if(!request.memory_path().empty() || request.has_identity() || request.has_options()) {
        throw std::runtime_error("invalid_query_request");
      }
      std::cout << to_json(ShadowRuntime(request.snapshot()).query_advice(request.query()), false)
                << '\n';
    }
    return 0;
  } catch(...) {
    // Never echo parser/provider/path details or credentials from input.
    std::cout << R"({"status":"error","reason":"shadow_request_failed","mode":"shadow","application_status":"not_applied"})" << '\n';
    return 2;
  }
}

}  // namespace agentflow::trajectory
