#include <trajectory_evolution/evolution.h>

#include <iostream>

int main(int argc, char** argv) {
  namespace te = trajectory::evolution;
  if(argc < 3) {
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
    for(int i = 2; i < argc; ++i) inputs.emplace_back(argv[i]);
    // Only the installed library is used; no AgentFlow daemon or runtime.
    const auto batch = te::Pipeline(evaluator, evolution).evaluate_files(inputs);
    te::MemoryStore memory;
    te::Pipeline::merge_memory(batch.report, &memory);
    const auto revision = memory.revision();
    te::Pipeline::merge_memory(batch.report, &memory);
    if(memory.revision() != revision) return 1;
    std::cout << "evaluated=" << batch.report.evaluation_succeeded_count()
              << " failed=" << batch.report.evaluation_failed_count()
              << " candidates=" << batch.report.candidates_size()
              << " memory_entries=" << memory.entries_size()
              << " replay_idempotent=true\n";
    return batch.report.evaluation_succeeded_count() > 0
        && batch.report.evaluation_failed_count() == 0 ? 0 : 1;
  } catch(const std::exception&) {
    std::cerr << "Evolution input or configuration failed validation.\n";
    return 1;
  }
}
