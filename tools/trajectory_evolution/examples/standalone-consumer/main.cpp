// main 接入示例：演示宿主如何在已有执行边界调用公开接口。
// 示例用于说明接入方式，实际执行和业务工具仍由宿主自身负责。
// 需要保留真实请求、结果及因果关联，不能用示例常量代替生产运行证据。

#include <trajectory_evolution/evolution.h>

#include <iostream>

// 程序入口：读取参数后路由到对应实现，统一处理退出状态和资源收尾。
int main(int argc, char **argv) {
  namespace te = trajectory::evolution;
  if (argc < 3) {
    std::cerr << "Usage: evolution_consumer CONFIG TRACE [TRACE ...]\n";
    return 2;
  }
  try {
    const auto config = te::load_runtime_configuration(argv[1]);
    auto evaluator = config.evaluator;
    evaluator.model_selection = te::ModelSelection::none;
    auto evolution = config.evolution;
    evolution.enable_meta_reflection = false;
    std::vector<std::filesystem::path> inputs;
    for (int i = 2; i < argc; ++i)
      inputs.emplace_back(argv[i]);
    // 仅使用安装后的库，不需要 AgentFlow 守护进程或运行时。
    const auto batch = te::Pipeline(evaluator, evolution).evaluate_files(inputs);
    te::MemoryStore memory;
    te::Pipeline::merge_memory(batch.report, &memory);
    const auto revision = memory.revision();
    te::Pipeline::merge_memory(batch.report, &memory);
    if (memory.revision() != revision)
      return 1;
    std::cout << "evaluated=" << batch.report.evaluation_succeeded_count()
              << " failed=" << batch.report.evaluation_failed_count()
              << " candidates=" << batch.report.candidates_size()
              << " memory_entries=" << memory.entries_size() << " replay_idempotent=true\n";
    return batch.report.evaluation_succeeded_count() > 0 &&
                   batch.report.evaluation_failed_count() == 0
               ? 0
               : 1;
  } catch (const std::exception &) {
    std::cerr << "Evolution input or configuration failed validation.\n";
    return 1;
  }
}
