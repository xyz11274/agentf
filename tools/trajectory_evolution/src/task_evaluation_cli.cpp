#include "dataset_cli.h"
#include "trajectory_evolution/task_evaluation.h"
#include "integration_json.h"

#include <csignal>
#include <iostream>
#include <set>

namespace trajectory::evolution {
namespace {
volatile std::sig_atomic_t interrupted = 0;
void interrupt_evaluation(int) { interrupted = 1; }
struct Signals {
  struct sigaction previous_int{}, previous_term{};
  Signals() {
    interrupted = 0;
    struct sigaction action{};
    action.sa_handler = interrupt_evaluation; sigemptyset(&action.sa_mask);
    sigaction(SIGINT, &action, &previous_int); sigaction(SIGTERM, &action, &previous_term);
  }
  ~Signals() { sigaction(SIGINT, &previous_int, nullptr); sigaction(SIGTERM, &previous_term, nullptr); }
};
}

int run_task_evaluation_cli(int argc, char** argv) {
  using namespace integration;
  try {
    if(argc == 4 && std::string(argv[3]) == "--help") {
      std::cout << "trajectory_evolution dataset run DATASET --config evaluation.json --output-root NEW_DIR\n"
                   "  [--model-config runtime.json] [--allow-unreviewed]\n"
                   "  [--regrade-report PREVIOUS_REPORT] (model judgment only; no Agent execution)\n"
                   "  [--recheck] (with --regrade-report: also recheck configured rules on saved evidence)\n"
                   "Offline only; configured host commands execute with your permissions.\n"
                   "Exit 0: all passed; 1: task failures; 3: incomplete grading; 2: invalid configuration.\n";
      return 0;
    }
    std::filesystem::path dataset, config, output;
    TaskEvaluationOptions options;
    std::set<std::string> seen;
    for(int i = 3; i < argc; ++i) {
      const std::string arg = argv[i];
      if(!arg.empty() && arg[0] != '-' && dataset.empty()) { dataset = arg; continue; }
      require(seen.insert(arg).second, "evaluation_duplicate_argument");
      if(arg == "--allow-unreviewed") options.allow_unreviewed = true;
      else if(arg == "--recheck") options.recheck = true;
      else if(arg == "--config" || arg == "--output-root" || arg == "--model-config" || arg == "--regrade-report") {
        require(++i < argc, "evaluation_missing_argument");
        if(arg == "--config") config = argv[i];
        else if(arg == "--output-root") output = argv[i];
        else if(arg == "--regrade-report") options.regrade_report = argv[i];
        else options.model_config = argv[i];
      } else throw Error("evaluation_argument_invalid");
    }
    require(!dataset.empty() && !config.empty() && !output.empty(), "evaluation_arguments_required");
    Signals signals;
    options.cancelled = [] { return interrupted != 0; };
    const auto report = run_task_evaluation(dataset, config, output, options);
    const auto summary = json(report.summary());
    std::cout << summary.dump() << '\n';
    if(interrupted || summary.num("errors", 0) || summary.num("uncertain", 0)) return 3;
    return summary.num("failed", 0) ? 1 : 0;
  } catch(const Error& error) {
    std::cerr << "ERROR: " << error.what() << '\n'; return 2;
  } catch(...) {
    std::cerr << "ERROR: evaluation_configuration_or_storage_failed\n"; return 2;
  }
}
}  // namespace trajectory::evolution
