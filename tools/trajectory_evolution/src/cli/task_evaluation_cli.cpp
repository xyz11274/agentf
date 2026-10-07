// 任务评测命令：解析执行、恢复和只重评分选项。
// 只重评分必须复用留存证据，不能偷偷重跑 Agent；输出与已有结果冲突时拒绝覆盖。

#include "dataset_cli.h"
#include "trajectory_evolution/task_evaluation.h"
#include "integration_json.h"

#include <csignal>
#include <iostream>
#include <set>

namespace trajectory::evolution {
namespace {
volatile std::sig_atomic_t interrupted = 0;

// 设置评测取消标记，执行器在安全边界停止并保存已有结果。
void interrupt_evaluation(int) {
  interrupted = 1;
}

struct Signals {
  struct sigaction previous_int {
  }, previous_term{};

  // 初始化 Signals 的状态和资源；输入参数的校验与所有权在构造阶段确定。
  // 对象创建后复用既有配置，实际执行与收尾分别由后续接口负责。
  Signals() {
    interrupted = 0;

    struct sigaction action {};

    action.sa_handler = interrupt_evaluation;
    sigemptyset(&action.sa_mask);
    sigaction(SIGINT, &action, &previous_int);
    sigaction(SIGTERM, &action, &previous_term);
  }

  // 销毁对象时释放本类持有的资源；收尾不能抛出业务异常，也不能继续接收新请求。
  ~Signals() {
    sigaction(SIGINT, &previous_int, nullptr);
    sigaction(SIGTERM, &previous_term, nullptr);
  }
};
}

// 解析评测命令并安装取消处理，随后委托统一执行器。
int run_task_evaluation_cli(int argc, char **argv) {
  using namespace integration;
  try {
    if (argc == 4 && std::string(argv[3]) == "--help") {
      std::cout
          << "trajectory_evolution dataset run DATASET --config evaluation.json --output-root "
             "NEW_DIR\n"
             "  [--model-config runtime.json] [--allow-unreviewed]\n"
             "  [--resume-report PREVIOUS_REPORT] (same configuration; reuse completed execution)\n"
             "  [--resume-rerun-trial TRIAL_ID ...] (recapture selected infrastructure failures)\n"
             "  [--regrade-report PREVIOUS_REPORT] (model judgment only; no Agent execution)\n"
             "  [--recheck] (with --regrade-report: also recheck configured rules on saved "
             "evidence)\n"
             "Offline only; configured host commands execute with your permissions.\n"
             "Exit 0: all passed; 1: task failures; 3: incomplete grading; 2: invalid "
             "configuration.\n";
      return 0;
    }
    std::filesystem::path dataset, config, output;
    TaskEvaluationOptions options;
    std::set<std::string> seen;
    for (int i = 3; i < argc; ++i) {
      const std::string arg = argv[i];
      if (!arg.empty() && arg[0] != '-' && dataset.empty()) {
        dataset = arg;
        continue;
      }
      if (arg != "--resume-rerun-trial")
        require(seen.insert(arg).second, "evaluation_duplicate_argument");
      if (arg == "--allow-unreviewed")
        options.allow_unreviewed = true;
      else if (arg == "--recheck")
        options.recheck = true;
      else if (arg == "--resume-rerun-trial") {
        require(++i < argc, "evaluation_missing_argument");
        const std::string trial_id = argv[i];
        require(!trial_id.empty() && trial_id.size() <= 256 && trial_id.front() != '-',
                "evaluation_trial_identity_invalid");
        options.resume_rerun_trial_ids.insert(trial_id);
      } else if (arg == "--config" || arg == "--output-root" || arg == "--model-config" ||
               arg == "--regrade-report" || arg == "--resume-report") {
        require(++i < argc, "evaluation_missing_argument");
        if (arg == "--config")
          config = argv[i];
        else if (arg == "--output-root")
          output = argv[i];
        else if (arg == "--regrade-report")
          options.regrade_report = argv[i];
        else if (arg == "--resume-report")
          options.resume_report = argv[i];
        else
          options.model_config = argv[i];
      } else
        throw Error("evaluation_argument_invalid");
    }
    require(!dataset.empty() && !config.empty() && !output.empty(),
            "evaluation_arguments_required");
    require(options.resume_rerun_trial_ids.empty() ||
                (!options.resume_report.empty() && options.regrade_report.empty()),
            "evaluation_rerun_requires_resume");
    Signals signals;
    options.cancelled = [] { return interrupted != 0; };
    const auto report = run_task_evaluation(dataset, config, output, options);
    const auto summary = json(report.summary());
    std::cout << summary.dump() << '\n';
    if (interrupted || summary.num("errors", 0) || summary.num("uncertain", 0))
      return 3;
    return summary.num("failed", 0) ? 1 : 0;
  } catch (const Error &error) {
    std::cerr << "ERROR: " << error.what() << '\n';
    return 2;
  } catch (...) {
    std::cerr << "ERROR: evaluation_configuration_or_storage_failed\n";
    return 2;
  }
}
} // 命名空间结束：trajectory::evolution
