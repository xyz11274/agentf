#include "dataset_cli.h"
#include "trajectory_evolution/dataset.h"
#include "integration_json.h"

#include <iostream>
#include <set>

namespace trajectory::evolution {

int run_dataset(int argc, char** argv) {
  using namespace integration;
  if(argc >= 3 && std::string(argv[2]) == "run") return run_task_evaluation_cli(argc, argv);
  try {
    if(argc < 3 || std::string(argv[2]) == "--help") {
      std::cout << "trajectory_evolution dataset build --config dataset.json --output-root DIR\n"
                   "  [--model-config runtime.json]\n"
                   "trajectory_evolution dataset check DIR [--require-reviewed]\n"
                   "trajectory_evolution dataset run DIR --config evaluation.json --output-root DIR\n"
                   "  [--model-config runtime.json] [--allow-unreviewed]\n"
                   "Build freezes tasks; run executes and grades; neither updates memory.\n"
                   "Exit 0: valid; 3: partial/empty or review required; 2: invalid input.\n";
      return argc < 3 ? 2 : 0;
    }
    const std::string action = argv[2];
    require(action == "build" || action == "check", "dataset_command_invalid");
    std::filesystem::path config, directory;
    DatasetBuildOptions options;
    bool require_reviewed = false;
    std::set<std::string> seen;
    for(int i = 3; i < argc; ++i) {
      const std::string arg = argv[i];
      require(seen.insert(arg).second, "dataset_duplicate_argument");
      if(action == "build" && (arg == "--config" || arg == "--output-root" || arg == "--model-config")) {
        require(++i < argc, "dataset_missing_argument");
        if(arg == "--config") config = argv[i];
        else if(arg == "--output-root") directory = argv[i];
        else options.model_config = argv[i];
      } else if(action == "check" && arg == "--require-reviewed") {
        require_reviewed = true;
      } else if(action == "check" && directory.empty() && !arg.empty() && arg[0] != '-') {
        directory = arg;
      } else {
        throw Error("dataset_argument_invalid");
      }
    }
    require(!directory.empty(), "dataset_directory_required");
    dataset::v1::EvaluationDataset result;
    if(action == "build") {
      require(!config.empty(), "dataset_config_required");
      // Refuse an existing destination before any potentially paid model call.
      require(!std::filesystem::exists(std::filesystem::symlink_status(directory)),
              "dataset_output_exists");
      result = build_evaluation_dataset(config, options);
      write_evaluation_dataset(result, directory);
    } else {
      result = read_evaluation_dataset(directory);
    }
    const auto summary = Json::parse(evaluation_dataset_summary(result));
    std::cout << summary.dump() << '\n';
    return summary.num("rejected_count", 0) || result.tasks().empty()
        || (require_reviewed && !summary.flag("review_complete", false)) ? 3 : 0;
  } catch(const Error& error) {
    // Error codes are constants; never echo imported content, paths or responses.
    std::cerr << "ERROR: " << error.what() << '\n';
    return 2;
  } catch(...) {
    std::cerr << "ERROR: dataset_input_or_configuration_invalid\n";
    return 2;
  }
}

}  // namespace trajectory::evolution
