#include "policy_cli.h"
#include "trajectory_evolution/policy_runtime.h"
#include "trajectory_evolution/trace.h"

#include <array>
#include <iostream>
#include <google/protobuf/util/json_util.h>

namespace agentflow::trajectory {
int run_policy_runtime(int argc, char** argv) {
  namespace te = ::trajectory::evolution;
  if(argc != 3 || (std::string(argv[2]) != "compile" && std::string(argv[2]) != "query"
                  && std::string(argv[2]) != "verify")) {
    std::cerr << "Usage: trajectory_evolution runtime compile|query|verify < request.json\n";
    return 2;
  }
  try {
    std::string input;
    std::array<char, 4096> buffer{};
    while(std::cin) {
      std::cin.read(buffer.data(), buffer.size());
      input.append(buffer.data(), static_cast<std::size_t>(std::cin.gcount()));
      if(input.size() > 2U * 1024U * 1024U) throw std::runtime_error("runtime_request_too_large");
    }
    te::v1::RuntimeCommand request;
    if(!google::protobuf::util::JsonStringToMessage(input, &request).ok())
      throw std::runtime_error("runtime_request_invalid");
    const std::string operation = argv[2];
    if(operation == "compile") {
      if(request.has_policy() || request.has_query() || request.has_observation())
        throw std::runtime_error("runtime_compile_invalid");
      const auto snapshot = te::compile_shadow_policy_file(
          request.memory_path(), request.identity(), request.shadow_options());
      std::cout << te::to_json(te::compile_runtime_policy(snapshot, request.options()), false) << '\n';
    } else {
      if(!request.memory_path().empty() || request.has_identity() || request.has_shadow_options()
          || request.has_options() || (operation == "query" && request.has_observation()))
        throw std::runtime_error("runtime_query_invalid");
      te::PolicyRuntime runtime(request.policy());
      if(operation == "query") std::cout << te::to_json(runtime.decide(request.query()), false) << '\n';
      else std::cout << te::to_json(runtime.verify(request.query(), request.observation()), false) << '\n';
    }
    return 0;
  } catch(...) {
    // Fixed diagnostics only: input/model/parser errors may contain secrets.
    std::cout << R"({"status":"error","reason":"runtime_request_failed"})" << '\n';
    return 2;
  }
}
}  // namespace agentflow::trajectory
