// 公开接口：定义宿主接入所需的类型、配置和调用约定。
// 此处是对外声明，具体实现位于 src；修改接口时需考虑现有调用方与持久化版本。

#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace agentflow::trajectory {

struct PromptTemplates {
  std::string episode_system;
  std::string episode_user;
  std::string dialogue_system;
  std::string dialogue_user;
  std::string meta_reflection_system;
  std::string meta_reflection_user;
  std::string memory_curator_system;
  std::string memory_curator_user;
  // 可选配置，现有配置文件与 AgentFlow 提示词继续兼容。
  std::string standard_dialogue_system;
};

// 替换模板中声明的占位符，拒绝缺失字段，避免向模型发送未展开的模板。
std::string
render_prompt_template(const std::string &template_name, const std::string &prompt_template,
                       const std::vector<std::pair<std::string, std::string>> &variables,
                       std::size_t max_output_bytes);

// 载入可热替换的默认提示词模板，保持配置与编译产物解耦。
std::shared_ptr<const PromptTemplates> load_default_prompt_templates();

} // 命名空间结束：agentflow::trajectory
