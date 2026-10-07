// 策略命令：处理版本编译、导入导出和可复用策略包。
// 策略版本与本次运行身份分开；导出不能携带凭据或依赖临时工作目录。

#include "policy_cli.h"
#include "trajectory_evolution/policy_runtime.h"
#include "trajectory_evolution/trace.h"
#include "control_generation.h"
#include "local_support.h"

#include <array>
#include <iostream>
#include <google/protobuf/util/json_util.h>

namespace agentflow::trajectory {
// 处理策略编译、导出或查询命令，不把命令行解析嵌入运行算法。
int run_policy_runtime(int argc, char **argv) {
  namespace te = ::trajectory::evolution;
  if (argc != 3 || (std::string(argv[2]) != "compile" && std::string(argv[2]) != "recompile" &&
                    std::string(argv[2]) != "query" && std::string(argv[2]) != "query-batch" &&
                    std::string(argv[2]) != "verify" &&
                    std::string(argv[2]) != "control-input" &&
                    std::string(argv[2]) != "control-compile")) {
    std::cerr
        << "Usage: trajectory_evolution runtime compile|recompile|query|query-batch|verify|control-input|control-compile < request.json\n";
    return 2;
  }
  const std::string operation = argv[2];
  try {
    std::string input;
    std::array<char, 4096> buffer{};
    while (std::cin) {
      std::cin.read(buffer.data(), buffer.size());
      input.append(buffer.data(), static_cast<std::size_t>(std::cin.gcount()));
      if (input.size() > 2U * 1024U * 1024U)
        throw std::runtime_error("runtime_request_too_large");
    }
    if (operation == "query-batch") {
      // One host run uses one runtime. Evaluate its real queries in order,
      // preserving native competition and once-per-run injection state.
      const auto batch = te::integration::Json::parse(input);
      te::integration::keys(batch, {"policy", "queries"});
      te::integration::require(batch["queries"].array(), "runtime_query_invalid");
      te::PolicyRuntime runtime(te::integration::proto<te::v1::RuntimePolicy>(batch["policy"]));
      auto decisions = te::integration::Json(te::integration::Json::Array{});
      for (const auto &query : batch["queries"].items())
        decisions.push(te::integration::Json::parse(te::to_json(
            runtime.decide(te::integration::proto<te::v1::RuntimeQuery>(query)), false)));
      std::cout << decisions.dump() << '\n';
      return 0;
    }
    if (operation == "control-input" || operation == "control-compile") {
      // Reuse the native control compiler and its existing approved-source,
      // scope, host capability and budget checks. No model is called here.
      const auto controls = te::integration::Json::parse(input);
      if (operation == "control-input")
        te::integration::keys(controls, {"policy", "control_learning"});
      else
        te::integration::keys(controls, {"policy", "control_learning", "draft"});
      te::integration::validate_local_data(controls);
      const auto policy = te::integration::proto<te::v1::RuntimePolicy>(controls["policy"]);
      const te::PolicyRuntime validated(policy);
      const auto &configuration = controls["control_learning"];
      te::integration::validate_control_learning(configuration);
      if (operation == "control-input") {
        std::cout << te::integration::control_generation_input(
                         policy.snapshot(), policy.options(), configuration).dump() << '\n';
      } else {
        const auto options = te::integration::apply_control_draft(
            policy.snapshot(), policy.options(), configuration, controls["draft"]);
        std::cout << te::to_json(te::compile_runtime_policy(policy.snapshot(), options), false)
                  << '\n';
      }
      return 0;
    }
    te::v1::RuntimeCommand request;
    if (!google::protobuf::util::JsonStringToMessage(input, &request).ok())
      throw std::runtime_error("runtime_request_invalid");
    if (operation == "compile") {
      if (request.has_policy() || request.has_query() || request.has_observation())
        throw std::runtime_error("runtime_compile_invalid");
      const auto snapshot = te::compile_shadow_policy_file(
          request.memory_path(), request.identity(), request.shadow_options());
      std::cout << te::to_json(te::compile_runtime_policy(snapshot, request.options()), false)
                << '\n';
    } else if (operation == "recompile") {
      if (!request.has_policy() || !request.has_options() || !request.memory_path().empty() ||
          request.has_identity() || request.has_shadow_options() || request.has_query() ||
          request.has_observation())
        throw std::runtime_error("runtime_recompile_invalid");
      const te::PolicyRuntime validated(request.policy());
      std::cout << te::to_json(
                       te::compile_runtime_policy(request.policy().snapshot(), request.options()),
                       false)
                << '\n';
    } else {
      if (!request.memory_path().empty() || request.has_identity() ||
          request.has_shadow_options() || request.has_options() ||
          (operation == "query" && request.has_observation()))
        throw std::runtime_error("runtime_query_invalid");
      te::PolicyRuntime runtime(request.policy());
      if (operation == "query")
        std::cout << te::to_json(runtime.decide(request.query()), false) << '\n';
      else
        std::cout << te::to_json(runtime.verify(request.query(), request.observation()), false)
                  << '\n';
    }
    return 0;
  } catch (const te::integration::Error &error) {
    // The control compiler throws fixed contract codes. Keep bounded code-only
    // feedback so a model can repair its draft without exposing raw input.
    // Other runtime operations retain their existing generic diagnostics.
    const std::string code = error.what();
    const bool safe_code = !code.empty() && code.size() <= 100 &&
        code.front() >= 'a' && code.front() <= 'z' &&
        std::all_of(code.begin(), code.end(), [](unsigned char c) {
          return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
        });
    const bool control = operation == "control-input" || operation == "control-compile";
    std::cout << te::integration::Json{{"status", "error"},
        {"reason", control && safe_code ? code : "runtime_request_failed"}}.dump() << '\n';
    return 2;
  } catch (...) {
    // 只输出固定诊断，输入、模型和解析错误都可能包含敏感信息。
    std::cout << R"({"status":"error","reason":"runtime_request_failed"})" << '\n';
    return 2;
  }
}
} // 命名空间结束：agentflow::trajectory
