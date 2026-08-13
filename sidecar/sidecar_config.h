#ifndef COREMIND_BRIDGE_SIDECAR_CONFIG_H
#define COREMIND_BRIDGE_SIDECAR_CONFIG_H

#include <string>
#include <vector>

namespace sidecar {

/// Sidecar 单 agent 配置（转发路由目标）。
/// 来源优先级：环境变量 > 配置文件 > 代码默认值（对齐 configs/README.md 约定）。
struct AgentConfig {
    std::string name = "outfit-assistant";
    std::string version = "1.0.0";
    std::vector<std::string> capabilities = {
        "outfit_consult", "weather_query", "occasion_guide",
        "image_generation", "video_generation"};
};

/// Sidecar 完整配置。
struct SidecarConfig {
    AgentConfig agent;
    std::string bridge_url = "http://127.0.0.1:8199/health";
};

/// 查找配置文件路径（first-match-wins）：
///   1. `--config <path>` CLI 参数
///   2. `AF_SIDECAR_CONFIG` 环境变量
///   3. `configs/{AGENTFLOW_ENV}/sidecar.json`（AGENTFLOW_ENV 默认 dev）
/// 找不到时返回空串（调用方使用代码默认值）。
std::string find_sidecar_config_path(int argc, char* argv[]);

/// 从配置文件加载 SidecarConfig，并应用环境变量覆盖：
///   `AF_BR_COREMIND_AGENT_ID` / `AGENT_ID` → agent.name
///   `BRIDGE_URL`                            → bridge_url
///   `AF_SIDECAR_CAPABILITIES`               → agent.capabilities（逗号分隔）
/// 文件不存在/解析失败时回退默认值（不阻断启动，打印警告日志）。
SidecarConfig load_sidecar_config(const std::string& config_path);

}  // namespace sidecar

#endif // COREMIND_BRIDGE_SIDECAR_CONFIG_H
