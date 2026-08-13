#include <gtest/gtest.h>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>
#include "sidecar_config.h"

using namespace sidecar;

namespace {

// RAII：设置/恢复环境变量，避免测试间污染
struct EnvGuard {
    const char* name_;
    std::string saved_;
    bool had_ = false;

    explicit EnvGuard(const char* name) : name_(name) {
        const char* v = std::getenv(name);
        if (v) {
            had_ = true;
            saved_ = v;
        }
    }
    ~EnvGuard() {
        if (had_) {
            setenv(name_, saved_.c_str(), 1);
        } else {
            unsetenv(name_);
        }
    }
};

std::string write_temp_json(const std::string& content) {
    char tmpl[] = "/tmp/sidecar_cfg_test_XXXXXX";
    int fd = mkstemp(tmpl);
    if (fd < 0) {
        return "";
    }
    std::string path = tmpl;
    std::ofstream out(path);
    out << content;
    out.close();
    close(fd);
    return path;
}

}  // namespace

// ═══════════════════════════════════════════════════════════
// 默认值（无配置文件）
// ═══════════════════════════════════════════════════════════

TEST(SidecarConfig, TC_CFG_001_DefaultsWhenNoFile) {
    EnvGuard g1("AF_BR_COREMIND_AGENT_ID");
    EnvGuard g2("AGENT_ID");
    EnvGuard g3("BRIDGE_URL");
    EnvGuard g4("AF_SIDECAR_CAPABILITIES");
    unsetenv("AF_BR_COREMIND_AGENT_ID");
    unsetenv("AGENT_ID");
    unsetenv("BRIDGE_URL");
    unsetenv("AF_SIDECAR_CAPABILITIES");

    auto cfg = load_sidecar_config("");  // 空路径 = 无配置文件
    EXPECT_EQ(cfg.agent.name, "outfit-assistant");
    EXPECT_EQ(cfg.agent.version, "1.0.0");
    ASSERT_EQ(cfg.agent.capabilities.size(), 5u);
    EXPECT_EQ(cfg.agent.capabilities[0], "outfit_consult");
    EXPECT_EQ(cfg.bridge_url, "http://127.0.0.1:8199/health");
}

TEST(SidecarConfig, TC_CFG_002_MissingFileFallsBackToDefaults) {
    EnvGuard g1("AF_BR_COREMIND_AGENT_ID");
    EnvGuard g2("AGENT_ID");
    EnvGuard g3("BRIDGE_URL");
    EnvGuard g4("AF_SIDECAR_CAPABILITIES");
    unsetenv("AF_BR_COREMIND_AGENT_ID");
    unsetenv("AGENT_ID");
    unsetenv("BRIDGE_URL");
    unsetenv("AF_SIDECAR_CAPABILITIES");

    auto cfg = load_sidecar_config("/nonexistent/path/sidecar.json");
    EXPECT_EQ(cfg.agent.name, "outfit-assistant");
    EXPECT_EQ(cfg.bridge_url, "http://127.0.0.1:8199/health");
}

// ═══════════════════════════════════════════════════════════
// JSON 配置加载
// ═══════════════════════════════════════════════════════════

TEST(SidecarConfig, TC_CFG_010_LoadsJsonFields) {
    EnvGuard g1("AF_BR_COREMIND_AGENT_ID");
    EnvGuard g2("AGENT_ID");
    EnvGuard g3("BRIDGE_URL");
    EnvGuard g4("AF_SIDECAR_CAPABILITIES");
    unsetenv("AF_BR_COREMIND_AGENT_ID");
    unsetenv("AGENT_ID");
    unsetenv("BRIDGE_URL");
    unsetenv("AF_SIDECAR_CAPABILITIES");

    std::string path = write_temp_json(R"({
        "agent": {
            "name": "weather-agent",
            "version": "2.3.0",
            "capabilities": ["weather_query", "alarm_notify"]
        },
        "bridge_url": "http://127.0.0.1:8199/health"
    })");

    auto cfg = load_sidecar_config(path);
    EXPECT_EQ(cfg.agent.name, "weather-agent");
    EXPECT_EQ(cfg.agent.version, "2.3.0");
    ASSERT_EQ(cfg.agent.capabilities.size(), 2u);
    EXPECT_EQ(cfg.agent.capabilities[0], "weather_query");
    EXPECT_EQ(cfg.agent.capabilities[1], "alarm_notify");
    EXPECT_EQ(cfg.bridge_url, "http://127.0.0.1:8199/health");
    std::remove(path.c_str());
}

TEST(SidecarConfig, TC_CFG_011_InvalidJsonFallsBackToDefaults) {
    EnvGuard g1("AF_BR_COREMIND_AGENT_ID");
    EnvGuard g2("AGENT_ID");
    EnvGuard g3("BRIDGE_URL");
    EnvGuard g4("AF_SIDECAR_CAPABILITIES");
    unsetenv("AF_BR_COREMIND_AGENT_ID");
    unsetenv("AGENT_ID");
    unsetenv("BRIDGE_URL");
    unsetenv("AF_SIDECAR_CAPABILITIES");

    std::string path = write_temp_json("{ not valid json !!");
    auto cfg = load_sidecar_config(path);
    EXPECT_EQ(cfg.agent.name, "outfit-assistant");
    std::remove(path.c_str());
}

TEST(SidecarConfig, TC_CFG_012_PartialJsonKeepsMissingDefaults) {
    EnvGuard g1("AF_BR_COREMIND_AGENT_ID");
    EnvGuard g2("AGENT_ID");
    EnvGuard g3("BRIDGE_URL");
    EnvGuard g4("AF_SIDECAR_CAPABILITIES");
    unsetenv("AF_BR_COREMIND_AGENT_ID");
    unsetenv("AGENT_ID");
    unsetenv("BRIDGE_URL");
    unsetenv("AF_SIDECAR_CAPABILITIES");

    std::string path = write_temp_json(R"({
        "agent": {"name": "doc-agent"}
    })");
    auto cfg = load_sidecar_config(path);
    EXPECT_EQ(cfg.agent.name, "doc-agent");
    EXPECT_EQ(cfg.agent.version, "1.0.0");  // 缺省保持默认
    ASSERT_EQ(cfg.agent.capabilities.size(), 5u);
    EXPECT_EQ(cfg.bridge_url, "http://127.0.0.1:8199/health");
    std::remove(path.c_str());
}

// ═══════════════════════════════════════════════════════════
// env 覆盖优先级（env > 配置文件 > 代码默认值）
// ═══════════════════════════════════════════════════════════

TEST(SidecarConfig, TC_CFG_020_EnvOverridesJson) {
    EnvGuard g1("AF_BR_COREMIND_AGENT_ID");
    EnvGuard g2("AGENT_ID");
    EnvGuard g3("BRIDGE_URL");
    EnvGuard g4("AF_SIDECAR_CAPABILITIES");
    unsetenv("AGENT_ID");
    unsetenv("BRIDGE_URL");
    unsetenv("AF_SIDECAR_CAPABILITIES");
    setenv("AF_BR_COREMIND_AGENT_ID", "env-agent", 1);

    std::string path = write_temp_json(R"({
        "agent": {"name": "file-agent"}
    })");
    auto cfg = load_sidecar_config(path);
    EXPECT_EQ(cfg.agent.name, "env-agent");  // env 优先于配置文件
    std::remove(path.c_str());
}

TEST(SidecarConfig, TC_CFG_021_NewEnvPreferredOverLegacyEnv) {
    EnvGuard g1("AF_BR_COREMIND_AGENT_ID");
    EnvGuard g2("AGENT_ID");
    setenv("AF_BR_COREMIND_AGENT_ID", "new-env-agent", 1);
    setenv("AGENT_ID", "legacy-agent", 1);

    auto cfg = load_sidecar_config("");
    EXPECT_EQ(cfg.agent.name, "new-env-agent");  // 新变量优先，AGENT_ID 向后兼容
}

TEST(SidecarConfig, TC_CFG_022_CapabilitiesEnvCsv) {
    EnvGuard g1("AF_BR_COREMIND_AGENT_ID");
    EnvGuard g2("AGENT_ID");
    EnvGuard g3("AF_SIDECAR_CAPABILITIES");
    unsetenv("AF_BR_COREMIND_AGENT_ID");
    unsetenv("AGENT_ID");
    setenv("AF_SIDECAR_CAPABILITIES", "cap_a, cap_b,cap_c", 1);

    auto cfg = load_sidecar_config("");
    ASSERT_EQ(cfg.agent.capabilities.size(), 3u);
    EXPECT_EQ(cfg.agent.capabilities[0], "cap_a");
    EXPECT_EQ(cfg.agent.capabilities[1], "cap_b");  // 去除首尾空白
    EXPECT_EQ(cfg.agent.capabilities[2], "cap_c");
}

TEST(SidecarConfig, TC_CFG_023_BridgeUrlEnvOverridesJson) {
    EnvGuard g1("AF_BR_COREMIND_AGENT_ID");
    EnvGuard g2("AGENT_ID");
    EnvGuard g3("BRIDGE_URL");
    unsetenv("AF_BR_COREMIND_AGENT_ID");
    unsetenv("AGENT_ID");
    setenv("BRIDGE_URL", "http://bridge.example:9999/health", 1);

    std::string path = write_temp_json(R"({
        "bridge_url": "http://file.example:1234/health"
    })");
    auto cfg = load_sidecar_config(path);
    EXPECT_EQ(cfg.bridge_url, "http://bridge.example:9999/health");
    std::remove(path.c_str());
}

// ═══════════════════════════════════════════════════════════
// 配置文件路径解析（--config CLI > AF_SIDECAR_CONFIG env > 默认）
// ═══════════════════════════════════════════════════════════

TEST(SidecarConfig, TC_CFG_030_CliConfigPathPreferred) {
    EnvGuard g("AF_SIDECAR_CONFIG");
    unsetenv("AF_SIDECAR_CONFIG");
    char* argv[] = {const_cast<char*>("coremind_bridge"),
                    const_cast<char*>("--config"),
                    const_cast<char*>("/tmp/my-sidecar.json"),
                    const_cast<char*>("--listen"),
                    const_cast<char*>("0.0.0.0:23767")};
    EXPECT_EQ(find_sidecar_config_path(5, argv), "/tmp/my-sidecar.json");
}

TEST(SidecarConfig, TC_CFG_031_EnvConfigPathFallback) {
    EnvGuard g("AF_SIDECAR_CONFIG");
    setenv("AF_SIDECAR_CONFIG", "/etc/agentflow/sidecar.json", 1);
    char* argv[] = {const_cast<char*>("coremind_bridge"),
                    const_cast<char*>("--listen"),
                    const_cast<char*>("0.0.0.0:23767")};
    EXPECT_EQ(find_sidecar_config_path(3, argv), "/etc/agentflow/sidecar.json");
}

TEST(SidecarConfig, TC_CFG_032_DefaultEnvDirWhenNoOverride) {
    EnvGuard g("AF_SIDECAR_CONFIG");
    unsetenv("AF_SIDECAR_CONFIG");
    char* argv[] = {const_cast<char*>("coremind_bridge")};
    EXPECT_EQ(find_sidecar_config_path(1, argv), "configs/dev/sidecar.json");
}
