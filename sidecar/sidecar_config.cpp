#include "sidecar_config.h"

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>

#include <json/json.h>

namespace sidecar {

namespace {

// 逗号分隔字符串 → vector（capabilities env 覆盖用）
std::vector<std::string> split_csv(const std::string& value) {
    std::vector<std::string> out;
    std::stringstream ss(value);
    std::string item;
    while (std::getline(ss, item, ',')) {
        // 去首尾空白
        size_t first = item.find_first_not_of(" \t");
        size_t last = item.find_last_not_of(" \t");
        if (first == std::string::npos) continue;
        out.push_back(item.substr(first, last - first + 1));
    }
    return out;
}

}  // namespace

std::string find_sidecar_config_path(int argc, char* argv[]) {
    // 1. CLI `--config <path>` 优先
    for (int i = 1; i + 1 < argc; i++) {
        if (std::string(argv[i]) == "--config") {
            return argv[i + 1];
        }
    }
    // 2. AF_SIDECAR_CONFIG 环境变量
    if (const char* env_path = std::getenv("AF_SIDECAR_CONFIG")) {
        if (*env_path) return env_path;
    }
    // 3. configs/{AGENTFLOW_ENV}/sidecar.json（AGENTFLOW_ENV 默认 dev，对齐 bridge 惯例）
    const char* env_flow = std::getenv("AGENTFLOW_ENV");
    std::string flow = (env_flow && *env_flow) ? env_flow : "dev";
    return "configs/" + flow + "/sidecar.json";
}

SidecarConfig load_sidecar_config(const std::string& config_path) {
    SidecarConfig cfg;  // 默认值（outfit-assistant / 1.0.0 / 默认 capabilities / 默认 bridge_url）

    if (!config_path.empty()) {
        std::ifstream in(config_path);
        if (!in.good()) {
            std::cerr << "[sidecar_config] WARN config file not found: " << config_path
                      << " — using defaults" << std::endl;
        } else {
            Json::Value root;
            Json::CharReaderBuilder reader;
            std::string errs;
            // 整文件读入 string 再包 istringstream（C++17 不支持双迭代器构造 istringstream）
            std::string content((std::istreambuf_iterator<char>(in)),
                                std::istreambuf_iterator<char>());
            std::istringstream input(content);
            if (!Json::parseFromStream(reader, input, &root, &errs)) {
                std::cerr << "[sidecar_config] WARN invalid JSON in " << config_path
                          << " (" << errs << ") — using defaults" << std::endl;
            } else {
                // agent 段
                const Json::Value& agent = root.get("agent", Json::Value());
                if (agent.isObject()) {
                    if (agent["name"].isString()) {
                        cfg.agent.name = agent["name"].asString();
                    }
                    if (agent["version"].isString()) {
                        cfg.agent.version = agent["version"].asString();
                    }
                    const Json::Value& caps = agent["capabilities"];
                    if (caps.isArray() && !caps.empty()) {
                        std::vector<std::string> loaded;
                        for (const auto& c : caps) {
                            if (c.isString()) loaded.push_back(c.asString());
                        }
                        if (!loaded.empty()) cfg.agent.capabilities = loaded;
                    }
                }
                // bridge_url 顶层字段
                if (root["bridge_url"].isString()) {
                    cfg.bridge_url = root["bridge_url"].asString();
                }
            }
        }
    }

    // 环境变量覆盖（env > 配置文件 > 代码默认值）
    // 统一 agent 名：新变量 AF_BR_COREMIND_AGENT_ID 优先，AGENT_ID 向后兼容
    if (const char* v = std::getenv("AF_BR_COREMIND_AGENT_ID")) {
        if (*v) cfg.agent.name = v;
    } else if (const char* v = std::getenv("AGENT_ID")) {
        if (*v) cfg.agent.name = v;
    }
    if (const char* v = std::getenv("BRIDGE_URL")) {
        if (*v) cfg.bridge_url = v;
    }
    if (const char* v = std::getenv("AF_SIDECAR_CAPABILITIES")) {
        if (*v) {
            auto caps = split_csv(v);
            if (!caps.empty()) cfg.agent.capabilities = caps;
        }
    }

    // F7.1 配置加载摘要：完整打印加载结果，便于定位"路由到了哪个 agent"
    std::cerr << "[sidecar_config] loaded config_path=" << config_path
              << " agent_name=" << cfg.agent.name
              << " version=" << cfg.agent.version
              << " capabilities=";
    for (size_t i = 0; i < cfg.agent.capabilities.size(); i++) {
        if (i > 0) std::cerr << ",";
        std::cerr << cfg.agent.capabilities[i];
    }
    std::cerr << " bridge_url=" << cfg.bridge_url << std::endl;

    return cfg;
}

}  // namespace sidecar
