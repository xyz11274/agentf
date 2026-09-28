// 任务评测入口：隔离运行宿主、收集多轮真实轨迹、执行硬检查和模型验收。
// 执行状态、严格通过率、目标完成度、评测覆盖率分别保存；不要合并为一个成功标志。
// 多轮拆分只作用于裁判请求，不改变 Agent 输入顺序或原始采集结果。
// 复评只读取留存证据；普通恢复必须匹配数据集、策略与评测配置版本。
// 此处是对外声明，具体实现位于 src；修改接口时需考虑现有调用方与持久化版本。

#pragma once

#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <set>

#include "trajectory_evolution/dataset.h"

namespace trajectory::evolution {

// 报告文件的统一安全硬上限；具体批次仍遵守 max_report_bytes，默认容量不变。
// 只限制离线落盘/读取，不扩大模型请求、单题轨迹或 HTTP 消息的上限。
inline constexpr std::size_t kMaxEvaluationReportBytes = 1073741824;

struct TaskEvaluationOptions {
  std::filesystem::path model_config;
  std::shared_ptr<agentflow::trajectory::ModelAdapter> model_adapter;
  bool allow_unreviewed = false;
  // 在试验之间及监督宿主进程期间调用。
  std::function<bool()> cancelled;
  // 字段追加在末尾，保持现有 C++ 聚合初始化调用方兼容。
  // 后台调用方提供的冻结配置；命令行路径仍保持有效。
  std::string config_json;
  // 可选检查点，必须对应同一数据集、版本和评分设置。
  std::filesystem::path resume_report;
  // 显式仅重评模型结论，保留执行证据和确定性检查。
  // 与 resume_report 互斥，绝不启动 Agent 子进程。
  std::filesystem::path regrade_report;
  // 与 regrade_report 一起使用时，也在保留证据上重跑配置的确定性检查。
  // 其他原检查仍然有效，包括产物和隐私失败。
  bool recheck = false;
  // 普通恢复时必须重新执行的旧 trial。其余完整执行仍可复用，裁判错误仍只重评。
  // 该字段仅供同一冻结评测内的恢复协调使用，不改变公开命令行协议。
  std::set<std::string> resume_rerun_trial_ids;
};

// 规范行为摘要不受 run 身份和记忆计数影响。
std::string agent_version_id(const std::string &artifact_json);

// 读取当前模型裁判的内容指纹；不调用模型、不执行 Agent，不输出凭据。
// 用于核验预先评好的报告，不能替代任务、版本、硬规则和来源完整性检查。
std::string task_evaluation_judge_fingerprint(
    const std::filesystem::path &config_path, const std::string &config_json = {});

// 只做离线任务执行与评分，不学习、不修改记忆，也不发布策略。
// 宿主命令是可信程序，此处不是执行不可信程序的安全沙箱。
dataset::v1::TaskEvaluationReport run_task_evaluation(
    const std::filesystem::path &dataset_directory, const std::filesystem::path &config_path,
    const std::filesystem::path &output_directory, const TaskEvaluationOptions &options = {});

} // 命名空间结束：trajectory::evolution
