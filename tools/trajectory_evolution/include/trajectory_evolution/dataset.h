// 数据集入口：导入、去重、划分并冻结任务与验收要求。
// 同一会话或相关任务组保持在同一 split，避免训练与留出相互污染。
// 版本指纹覆盖输入和标准；修改验收要求后，旧评分不能直接作为新基线。
// 此处是对外声明，具体实现位于 src；修改接口时需考虑现有调用方与持久化版本。

#pragma once

#include <filesystem>
#include <memory>
#include <string>

#include "agentflow/trajectory/hybrid_evaluator.h"
#include "evaluation_dataset.pb.h"

namespace trajectory::evolution {

struct DatasetBuildOptions {
  // 复用评测器模型配置；留空时使用数据集配置。
  std::filesystem::path model_config;
  std::shared_ptr<agentflow::trajectory::ModelAdapter> model_adapter;
};

// 仅离线处理，不修改在线记忆、策略或 Agent 执行。
dataset::v1::EvaluationDataset build_evaluation_dataset(const std::filesystem::path &config_path,
                                                        const DatasetBuildOptions &options = {});

// 只写入新目录，manifest.json 是完成标记。
// 未完成写入仍可检查，但不能当作有效的冻结数据集。
void write_evaluation_dataset(const dataset::v1::EvaluationDataset &dataset,
                              const std::filesystem::path &directory);
// 读取数据集清单与任务文件，校验完整性后再交给执行器。
dataset::v1::EvaluationDataset read_evaluation_dataset(const std::filesystem::path &directory);
// 汇总数据划分、数量和来源，供运行前核对，不执行 Agent。
std::string evaluation_dataset_summary(const dataset::v1::EvaluationDataset &dataset);

} // 命名空间结束：trajectory::evolution
