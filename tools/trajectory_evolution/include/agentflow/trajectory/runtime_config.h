// 运行配置加载：解析模型参数、提示词路径、预算和功能开关。
// 部署差异放配置文件，不硬编码进算法；路径按配置所属目录解析。
// 读取配置时检查类型与取值范围，凭据通过环境引用而非配置明文传递。
// 此处是对外声明，具体实现位于 src；修改接口时需考虑现有调用方与持久化版本。

#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "agentflow/trajectory/evolution_pipeline.h"

namespace agentflow::trajectory {

struct RuntimeConfiguration {
  std::filesystem::path config_path;
  std::vector<std::filesystem::path> source_files;
  EvaluatorOptions evaluator;
  EvolutionOptions evolution;
  OpenAiCompatibleOptions model;
  std::string api_key_env;
  std::uint32_t sessionlog_max_output_tokens = 4096;
  bool no_api_key = false;
  bool model_selection_explicit = false;
  bool max_output_tokens_explicit = false;
};

// 按显式路径和安装布局寻找运行配置，不依赖开发机的固定工作目录。
std::filesystem::path discover_runtime_configuration();

// 按外部配置加载模型、提示词和预算，修改文本或配置不需要重新编译。
RuntimeConfiguration load_runtime_configuration(const std::filesystem::path &path);

} // 命名空间结束：agentflow::trajectory
