#include "learn_cli.h"
#include "trajectory_evolution/integration.h"
#include "trajectory_evolution/trace.h"
#include "integration_json.h"
#include <algorithm>
#include <iostream>
#include <set>

namespace trajectory::evolution {
int run_learning_import(int argc, char** argv) {
  using namespace integration;
  namespace fs = std::filesystem;
  try {
    fs::path input, config; bool enqueue_only = false; std::size_t max_cycles = 1000;
    for(int i = 2; i < argc; ++i) {
      const std::string arg = argv[i];
      if(arg == "--help" || arg == "-h") {
        std::cout << "trajectory_evolution learn [LOG_OR_DIRECTORY] --config SERVICE_JSON [--enqueue-only] [--max-cycles N]\n"
          "Imports complete traces into the same workspace/queue as serve and LocalRuntime.\n"
          "Omit input to resume pending work. Duplicate imports never re-evaluate. No HTTP server is started.\n";
        return 0;
      }
      if(arg == "--enqueue-only") enqueue_only = true;
      else if(arg == "--config" || arg == "--max-cycles") {
        require(++i < argc, "learning_option_value_missing");
        if(arg == "--config") { require(config.empty(), "learning_duplicate_config"); config = argv[i]; }
        else { max_cycles = Json::parse(argv[i]).integer(); require(max_cycles > 0 && max_cycles <= 100000, "learning_cycle_limit_invalid"); }
      } else { require(input.empty() && !arg.empty() && arg[0] != '-', "learning_argument_invalid"); input = arg; }
    }
    require(!config.empty(), "learning_service_config_required");
    IntegrationService service(config);
    std::vector<fs::path> files;
    if(!input.empty()) {
      if(fs::is_directory(input)) {
        for(const auto& item : fs::recursive_directory_iterator(input)) {
          if(item.is_symlink()) continue;
          if(item.is_regular_file() && (is_trace_path(item.path()) || item.path().extension() == ".jsonl")) files.push_back(item.path());
          require(files.size() <= 100000, "learning_import_limit");
        }
      } else files.push_back(input);
      require(!files.empty(), "learning_no_logs_found");
    }
    std::sort(files.begin(), files.end());
    std::size_t imported = 0, reused = 0, rejected = 0;
    for(const auto& path : files) {
      const auto response = service.import_file(path);
      const auto value = Json::parse(response.body);
      if(response.status == 200) { if(value["result"].flag("reused", false)) ++reused; else ++imported; }
      else { ++rejected; std::cerr << "import_error=" << value.str("error") << " file_index=" << (imported + reused + rejected) << '\n'; }
    }
    Json state;
    for(std::size_t cycle = 0; !enqueue_only && cycle < max_cycles; ++cycle) {
      service.process_evaluations(true); state = Json::parse(service.learning_status());
      if(!state.num("pending_runs", 0) && !state.num("unpublished_runs", 0)
          && (!state.num("shared_pending_users", 0) || state.str("shared_status") == "waiting_for_model")) break;
      if(!state.num("pending_runs", 0) && state.str("shared_status") == "failed" && !state.num("shared_next_due_ms", 0)) break;
      if(!state.num("pending_runs", 0)) service.wait_for_work(service.work_version(), 60000);
    }
    state = Json::parse(service.learning_status()); state["imported"] = imported; state["reused"] = reused; state["rejected"] = rejected;
    std::cout << state.dump() << '\n';
    const bool pending = state.num("pending_runs", 0) || state.num("unpublished_runs", 0)
        || (state.num("shared_pending_users", 0) && state.str("shared_status") != "waiting_for_model");
    return rejected || (!enqueue_only && (pending || state["run_outcomes"].num("failed", 0)
        || state.num("shared_quarantined_users", 0))) ? 3 : 0;
  } catch(const Error& error) { std::cerr << "ERROR: " << error.what() << '\n'; return 2; }
  catch(...) { std::cerr << "ERROR: learning_import_failed\n"; return 2; }
}
}
