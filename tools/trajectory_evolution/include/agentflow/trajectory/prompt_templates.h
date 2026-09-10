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
  // Optional; existing configurations and AgentFlow prompts remain compatible.
  std::string standard_dialogue_system;
};

std::string render_prompt_template(
    const std::string& template_name,
    const std::string& prompt_template,
    const std::vector<std::pair<std::string, std::string>>& variables,
    std::size_t max_output_bytes);

std::shared_ptr<const PromptTemplates> load_default_prompt_templates();

}  // namespace agentflow::trajectory
