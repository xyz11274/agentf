// 宿主进程监督：受控传递环境、限制资源、等待退出并处理超时与取消。
// 本组件负责进程生命周期，不把用户可执行程序当成安全沙箱。
// 清理只针对本次启动的进程树，避免影响并行任务或主 Agent。

#pragma once

// 宿主执行只继承白名单环境变量；进程失败也要尽力保留可用采集证据。

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace trajectory::evolution {

struct EvaluationProcessLimits {
  std::size_t address_space_bytes = 0;
  std::size_t cpu_seconds = 0;
  int nice = 0;
};

struct EvaluationProcessResult {
  std::string output;
  std::string error_code;
  int exit_code = -1;
  double duration_ms = 0;
};

// 仅内部命令行使用，普通应用调用 run_evaluation_process。
int run_evaluation_supervisor(int argc, char **argv);

// 不进行 shell 展开，只有明确选择的环境变量传给宿主。
// 标准输出和错误流在内存中限长，绝不原样写入日志。
EvaluationProcessResult run_evaluation_process(
    const std::vector<std::string> &command, const std::vector<std::string> &environment,
    const std::filesystem::path &directory, const std::string &input, std::size_t timeout_ms,
    std::size_t max_output_bytes, const std::function<bool()> &cancelled,
    const EvaluationProcessLimits &limits = {}, const std::function<bool()> &capture_finished = {},
    const std::function<std::string()> &entry_status = {});

} // 命名空间结束：trajectory::evolution
