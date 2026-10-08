// 学习命令：提交轨迹或训练评测证据，触发统一的后台学习流程。
// 导入时核验数据来源与集合用途，不把留出评测轨迹当作训练经验。

#include "learn_cli.h"
#include "trajectory_evolution/integration.h"
#include "trajectory_evolution/trace.h"
#include "integration_json.h"
#include <algorithm>
#include <atomic>
#include <csignal>
#include <iostream>
#include <pthread.h>
#include <set>
#include <thread>

namespace trajectory::evolution {
namespace {
// 仅离线评测命令接管终止信号。在线服务及训练导入保持各自原有生命周期。
// 先屏蔽再创建服务线程，使信号由下方 sigwait 线程统一接收；退出时恢复原掩码。
struct EvaluationSignalMask {
  sigset_t accepted{}, previous{};
  bool enabled;
  explicit EvaluationSignalMask(bool active) : enabled(active) {
    if (!enabled) return;
    sigemptyset(&accepted);
    sigaddset(&accepted, SIGINT);
    sigaddset(&accepted, SIGTERM);
    integration::require(pthread_sigmask(SIG_BLOCK, &accepted, &previous) == 0,
                         "evaluation_signal_setup_failed");
  }
  ~EvaluationSignalMask() {
    if (enabled) pthread_sigmask(SIG_SETMASK, &previous, nullptr);
  }
};

// 在普通线程中唤醒后台等待，避免在异步信号处理器中加锁或写文件。
// 无信号时阻塞等待，不新增空闲轮询；对象先于 service 析构，不访问已销毁的服务。
struct EvaluationSignalWake {
  std::atomic<int> received{0};
  std::atomic<bool> closing{false};
  std::thread listener;
  EvaluationSignalWake(const EvaluationSignalMask &mask, IntegrationService &service)
      : listener([&, this] {
          while (!closing.load()) {
            int signal = 0;
            const auto error = sigwait(&mask.accepted, &signal);
            if (closing.load()) break;
            received.store(error ? SIGTERM : signal);
            service.wake_background();
            if (error) break;
          }
        }) {}
  ~EvaluationSignalWake() {
    closing.store(true);
    // 只唤醒自己的等待线程；不会向 Agent 进程或同机其他作业发送信号。
    pthread_kill(listener.native_handle(), SIGTERM);
    listener.join();
  }
};
} // 结束当前匿名命名空间。

// 处理训练轨迹导入命令，遵守来源、划分和幂等条件。
int run_learning_import(int argc, char **argv) {
  using namespace integration;
  namespace fs = std::filesystem;
  try {
    fs::path input, config, training_report, dataset;
    bool enqueue_only = false, evaluation_only = false, refresh_evidence = false;
    std::size_t max_cycles = 1000;
    for (int i = 2; i < argc; ++i) {
      const std::string arg = argv[i];
      if (arg == "--help" || arg == "-h") {
        std::cout
            << "trajectory_evolution learn [LOG_OR_DIRECTORY] --config SERVICE_JSON "
               "[--enqueue-only] [--evaluation-only] [--refresh-evidence] [--max-cycles N]\n"
               "Imports complete traces into the same workspace/queue as serve and LocalRuntime.\n"
               "Omit input to resume pending work. Duplicate imports never re-evaluate. No HTTP "
               "server is started.\n";
        std::cout << "Cold start: --training-evaluation REPORT_DIRECTORY --dataset FROZEN_DATASET "
                     "(L0 train only).\n";
        return 0;
      }
      if (arg == "--enqueue-only")
        enqueue_only = true;
      else if (arg == "--evaluation-only")
        evaluation_only = true;
      else if (arg == "--refresh-evidence")
        refresh_evidence = true;
      else if (arg == "--config" || arg == "--max-cycles" || arg == "--training-evaluation" ||
               arg == "--dataset") {
        require(++i < argc, "learning_option_value_missing");
        if (arg == "--config") {
          require(config.empty(), "learning_duplicate_config");
          config = argv[i];
        } else if (arg == "--training-evaluation") {
          require(training_report.empty(), "learning_duplicate_report");
          training_report = argv[i];
        } else if (arg == "--dataset") {
          require(dataset.empty(), "learning_duplicate_dataset");
          dataset = argv[i];
        } else {
          max_cycles = Json::parse(argv[i]).integer();
          require(max_cycles > 0 && max_cycles <= 100000, "learning_cycle_limit_invalid");
        }
      } else {
        require(input.empty() && !arg.empty() && arg[0] != '-', "learning_argument_invalid");
        input = arg;
      }
    }
    require(!config.empty(), "learning_service_config_required");
    require(training_report.empty() == dataset.empty() &&
                (training_report.empty() || input.empty()),
            "learning_training_arguments_invalid");
    require(!refresh_evidence || (input.empty() && training_report.empty() && dataset.empty() &&
                                  !enqueue_only && !evaluation_only),
            "evidence_refresh_arguments_invalid");
    EvaluationSignalMask signals(evaluation_only);
    IntegrationService service(config);
    if (refresh_evidence) {
      const auto response = service.refresh_evidence();
      std::cout << response.body << '\n';
      return response.status == 200 ? 0 : 3;
    }
    if (evaluation_only) {
      require(input.empty() && training_report.empty() && dataset.empty() && !enqueue_only,
              "evaluation_only_arguments_invalid");
      EvaluationSignalWake interruption(signals, service);
      Json state;
      for (std::size_t cycle = 0; cycle < max_cycles && !interruption.received.load(); ++cycle) {
        const auto observed = service.work_version();
        service.process_agent_evaluations();
        state = Json::parse(service.learning_status());
        if (!state.num("agent_evaluation_pending_jobs", 0) ||
            !state.str("agent_evaluation_error").empty() || interruption.received.load())
          break;
        // 离线 GEPA 只等待候选评测；旧轨迹、私库整理和共享审查不属于本阶段。
        service.wait_for_work(observed, 60000);
      }
      state = Json::parse(service.learning_status());
      const auto stopped_by = interruption.received.load();
      if (stopped_by) state["interrupted"] = true;
      std::cout << state.dump() << '\n';
      // 返回前 service 正常析构：取消在途搜索并保存 finished_ms，恢复时可扣除停机时间。
      if (stopped_by) return 128 + stopped_by;
      return state.num("agent_evaluation_pending_jobs", 0) ||
                     !state.str("agent_evaluation_error").empty()
                 ? 3
                 : 0;
    }
    std::vector<fs::path> files;
    if (!input.empty()) {
      if (fs::is_directory(input)) {
        for (const auto &item : fs::recursive_directory_iterator(input)) {
          if (item.is_symlink())
            continue;
          if (item.is_regular_file() &&
              (is_trace_path(item.path()) || item.path().extension() == ".jsonl"))
            files.push_back(item.path());
          require(files.size() <= 100000, "learning_import_limit");
        }
      } else
        files.push_back(input);
      require(!files.empty(), "learning_no_logs_found");
    }
    std::sort(files.begin(), files.end());
    std::size_t imported = 0, reused = 0, rejected = 0;
    if (!training_report.empty()) {
      const auto response = service.import_training_evaluation(training_report, dataset);
      const auto value = Json::parse(response.body);
      require(response.status == 200, value.str("error", "training_import_rejected").c_str());
      imported = value["result"]["imported"].integer();
      reused = value["result"]["reused"].integer();
      rejected = value["result"]["rejected"].integer();
    }
    for (const auto &path : files) {
      const auto response = service.import_file(path);
      const auto value = Json::parse(response.body);
      if (response.status == 200) {
        if (value["result"].flag("reused", false))
          ++reused;
        else
          ++imported;
      } else {
        ++rejected;
        std::cerr << "import_error=" << value.str("error")
                  << " file_index=" << (imported + reused + rejected) << '\n';
      }
    }
    Json state;
    for (std::size_t cycle = 0; !enqueue_only && cycle < max_cycles; ++cycle) {
      const auto observed = service.work_version();
      service.process_evaluations(true);
      state = Json::parse(service.learning_status());
      if (!state.num("pending_runs", 0) && !state.num("unpublished_runs", 0) &&
          !state.num("training_pending_users", 0) && !state.num("memory_pending_reviews", 0) &&
          !state.num("agent_evaluation_pending_jobs", 0) &&
          (!state.num("shared_pending_users", 0) ||
           state.str("shared_status") == "waiting_for_model"))
        break;
      if (!state.num("pending_runs", 0) && !state.num("training_pending_users", 0) &&
          !state.num("memory_pending_reviews", 0) &&
          !state.num("agent_evaluation_pending_jobs", 0) &&
          state.str("shared_status") == "failed" && !state.num("shared_next_due_ms", 0))
        break;
      // pending 也可能只是在等待退避；交给统一调度期限唤醒，
      // 避免空转耗尽 max_cycles，导致离线命令提前退出。
      service.wait_for_work(observed, 60000);
    }
    state = Json::parse(service.learning_status());
    state["imported"] = imported;
    state["reused"] = reused;
    state["rejected"] = rejected;
    std::cout << state.dump() << '\n';
    const bool pending =
        state.num("pending_runs", 0) || state.num("unpublished_runs", 0) ||
        state.num("training_pending_users", 0) || state.num("memory_pending_reviews", 0) || state.num("agent_evaluation_pending_jobs", 0) ||
        (state.num("shared_pending_users", 0) && state.str("shared_status") != "waiting_for_model");
    return rejected || (!enqueue_only && (pending || !state.str("agent_evaluation_error").empty() ||
                                          state.num("training_failed_users", 0) ||
                                          state.num("memory_management_failed_users", 0) ||
                                          state["run_outcomes"].num("failed", 0) ||
                                          state.num("shared_quarantined_users", 0)))
               ? 3
               : 0;
  } catch (const Error &error) {
    std::cerr << "ERROR: " << error.what() << '\n';
    return 2;
  } catch (...) {
    std::cerr << "ERROR: learning_import_failed\n";
    return 2;
  }
}
}
