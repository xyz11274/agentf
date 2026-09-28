// test_gepa_config 回归验证：检查包内协议、边界条件与失败处理。
// 本地桩用于重现确定性场景，不代表真实 Agent 或远程模型的业务能力。
// 测试应同时验证正常路径与拒绝路径，避免错误数据被当成成功结果。

#include "../src/gepa/gepa_search.h"
#include "../src/gepa/control_generation.h"
#include <fstream>
#include <iostream>
#include <iterator>

// 回归用例：main。
// 构造本函数中的固定输入并核验状态与返回结果；不发起真实业务实验。
int main(int argc, char **argv) {
  try {
    if (argc != 2)
      return 2;
    const auto config = trajectory::evolution::integration::load_gepa_config(argv[1]);
    const auto fingerprint = trajectory::evolution::integration::gepa_config_fingerprint(config);
    if (fingerprint.size() != 64)
      return 3;
    // 默认说明必须覆盖编译器实际开放的原语和条件，防止代码升级后提示词仍禁用新字段。
    // 此处检查契约覆盖，不用本地桩声称远程模型一定会遵循说明。
    using namespace trajectory::evolution;
    v1::RuntimeOptions runtime;
    runtime.set_requested_level(v1::L3);
    runtime.add_host_capabilities("schedule.dispatch");
    auto *loop = runtime.add_loop_rules();
    loop->set_max_iterations(8);
    loop->set_max_no_progress(3);
    loop->set_max_replans(2);
    loop->set_no_progress_decision("replan");
    const auto contract = integration::control_generation_input(
        v1::ShadowPolicySnapshot{}, runtime, integration::Json{{"enabled", true}});
    const auto prompt_path = std::filesystem::path(argv[1]).parent_path() /
                             "prompts/control-candidates.txt";
    std::ifstream input(prompt_path);
    if (!input)
      throw std::runtime_error("control prompt unavailable");
    const std::string prompt((std::istreambuf_iterator<char>(input)), {});
    const auto mentioned = [&](const std::string &name) {
      if (prompt.find(name) == std::string::npos)
        throw std::runtime_error("control prompt missing capability: " + name);
    };
    for (const auto *kind : {"action_primitives", "loop_primitives"})
      for (const auto &primitive : contract[kind].items())
        mentioned(primitive.str("primitive_id"));
    for (const auto &condition : contract["loop_conditions"].fields())
      if (condition.first != "combination")
        mentioned(condition.first);
    for (const auto *field : {"before_argument", "after_argument", "terminal_error_codes",
                              "error_scope_arguments", "attempt_key_arguments"})
      mentioned(field);
    std::cout << "GEPA configuration validated\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
