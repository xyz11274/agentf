#ifndef AGENTFLOW_INTERNAL_AGENT_CONTEXT_H
#define AGENTFLOW_INTERNAL_AGENT_CONTEXT_H

#include <cstdint>
#include <memory>
#include <string>
#include <map>
#include <any>
#include <mutex>

namespace agentflow {
namespace framework {

/**
 * AgentContext maintains the execution context for an agent
 * Provides access to configuration, state, and runtime information
 */
class AgentContext {
public:
  struct Config {
    std::string context_id;
    std::string agent_id;
    int         max_context_variables = 1024;
  };

  explicit AgentContext(const Config& config);
  ~AgentContext();

  // Getters
  const std::string& get_context_id() const { return config_.context_id; }
  const std::string& get_agent_id() const { return config_.agent_id; }
  const Config&      get_config() const { return config_; }

  // Context variables - for storing dynamic data
  void       set_variable(const std::string& key, const std::any& value);
  bool       get_variable(const std::string& key, std::any& value) const;
  bool       has_variable(const std::string& key) const;
  void       remove_variable(const std::string& key);
  void       clear_all_variables();

  // Environment variables
  void       set_env_variable(const std::string& key, const std::string& value);
  bool       get_env_variable(const std::string& key, std::string& value) const;
  void       clear_env_variables();

  // Context properties
  void       set_property(const std::string& key, const std::string& value);
  bool       get_property(const std::string& key, std::string& value) const;
  int        get_variable_count() const;
  bool       is_variables_full() const;

  // Context state
  bool       is_valid() const { return is_valid_; }
  void       invalidate();

private:
  Config                                      config_;
  std::map<std::string, std::any>             context_variables_;
  std::map<std::string, std::string>          env_variables_;
  std::map<std::string, std::string>          properties_;
  bool                                        is_valid_ = true;
  mutable std::mutex                          mutex_;
};

}  // namespace framework
}  // namespace agentflow

#endif  // AGENTFLOW_INTERNAL_AGENT_CONTEXT_H
