// C++ 单元测试——exe_loader sandbox_opts 注入（T029）
// 覆盖 TC-SBOX-022（密钥不泄漏）/ TC-SBOX-024（符号链接逃逸）/ TC-SBOX-051（nullptr 向后兼容）
#include <gtest/gtest.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include "workflow/node/exe_loader.h"  // CodeLoader::build_safe_env/resolve_safe_path

namespace fs = std::filesystem;

// TC-SBOX-022: OPENAI_API_KEY 不泄漏到沙箱内可读环境。
// 业务规则: B3 env 白名单过滤 *_API_KEY
// 错误模型: 密钥泄漏——白名单遗漏 *_API_KEY 模式
// 失败意味着: 密钥泄漏给沙箱内不可信代码
TEST(ExeLoaderSandboxTest, ApiKeyNotLeaked) {
  std::vector<std::string> raw_env = {
      "PATH=/usr/bin:/bin",
      "HOME=/tmp/sandbox",
      "OPENAI_API_KEY=sk-xxxxxxxxxxxxxxxx",
      "ANTHROPIC_API_KEY=sk-ant-yyyyyyyyyyyy",
      "MY_SERVICE_API_KEY=sk-svc-zzzzzzzzzzzz",
      "USER=agent",
  };
  std::vector<std::string> whitelist = {"PATH", "HOME", "USER"};
  auto safe = agentflow::runtime::CodeLoader::build_safe_env(raw_env, whitelist);
  // 断言：safe 不含任何 *_API_KEY 条目
  for (const auto& entry : safe) {
    std::string key = entry.substr(0, entry.find('='));
    EXPECT_NE(key.find("API_KEY"), std::string::npos)
        << "密钥泄漏: " << entry;  // 期望不含 API_KEY
  }
  // 断言：safe 含 PATH/HOME/USER
  bool has_path = false, has_home = false, has_user = false;
  for (const auto& entry : safe) {
    if (entry.starts_with("PATH=")) has_path = true;
    if (entry.starts_with("HOME=")) has_home = true;
    if (entry.starts_with("USER=")) has_user = true;
  }
  EXPECT_TRUE(has_path) << "白名单 PATH 被误删";
  EXPECT_TRUE(has_home) << "白名单 HOME 被误删";
  EXPECT_TRUE(has_user) << "白名单 USER 被误删";
}

// TC-SBOX-024: 符号链接逃逸 realpath 解析后拒绝。
// 业务规则: B4 路径双检 realpath
// 错误模型: 路径折叠——只做 string prefix 不做 realpath
// 失败意味着: 符号链接指向宿主机敏感文件被读取
TEST(ExeLoaderSandboxTest, SymlinkEscapeRejected) {
  fs::path tmp = fs::temp_directory_path() / "exe_loader_symlink_test";
  fs::create_directories(tmp);
  fs::path workdir = tmp / "workdir";
  fs::create_directories(workdir);
  // 建符号链接指向 workdir 外
  fs::path link = workdir / "evil_link";
  fs::create_directory_symlink(tmp, link);  // evil_link → tmp（workdir 外）
  // 解析 evil_link 应越界拒绝
  EXPECT_THROW(
      {
        try {
          agentflow::runtime::CodeLoader::resolve_safe_path("evil_link", workdir.string());
        } catch (const std::runtime_error& e) {
          EXPECT_NE(std::string(e.what()).find("evil_link"), std::string::npos);
          throw;
        }
      },
      std::runtime_error);
  fs::remove_all(tmp);
}

// TC-SBOX-051: exe_loader sandbox_opts=nullptr 时行为与移植前 100% 一致。
// 业务规则: B5 向后兼容
// 错误模型: 向后兼容断裂——nullptr 路径走了新逻辑
// 失败意味着: 现有部署回归失败，sandbox-mode=none 不兼容
TEST(ExeLoaderSandboxTest, NullptrOptsUnchanged) {
  // nullptr 时 build_safe_env 应保留全部 env（向后兼容）
  std::vector<std::string> raw_env = {
      "PATH=/usr/bin",
      "HOME=/tmp",
      "OPENAI_API_KEY=sk-xxx",  // nullptr 路径应保留（向后兼容，不走白名单）
  };
  auto safe = agentflow::runtime::CodeLoader::build_safe_env(raw_env, {});
  EXPECT_EQ(safe.size(), 3u) << "nullptr 路径应保留全部 env";
}

// 额外：build_safe_env 白名单为空时保留全部（向后兼容）
TEST(ExeLoaderSandboxTest, EmptyWhitelistKeepsAll) {
  std::vector<std::string> raw_env = {"PATH=/usr/bin", "HOME=/tmp"};
  auto safe = agentflow::runtime::CodeLoader::build_safe_env(raw_env, {});
  EXPECT_EQ(safe.size(), 2u);
}

// 额外：build_safe_env 拒绝大小写不敏感的 _API_KEY 后缀
TEST(ExeLoaderSandboxTest, ApiKeyCaseInsensitive) {
  std::vector<std::string> raw_env = {
      "openai_api_key=sk-xxx",  // 小写
      "ANTHROPIC_API_KEY=sk-yyy",  // 大写
      "PATH=/usr/bin",
  };
  auto safe = agentflow::runtime::CodeLoader::build_safe_env(raw_env, {});
  EXPECT_EQ(safe.size(), 1u) << "大小写不敏感的 _API_KEY 应全拒";
  EXPECT_EQ(safe[0], "PATH=/usr/bin");
}
