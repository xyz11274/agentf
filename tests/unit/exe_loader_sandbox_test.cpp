// C++ 单元测试——exe_loader sandbox_opts 注入（T029）
// 覆盖 TC-SBOX-024（符号链接逃逸）。
// build_safe_env 已删（白名单机制整体删除，见 sandbox-env-injection.md §6.2 决策追加），
// 仅保留 resolve_safe_path 相关单测。
#include <gtest/gtest.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

#include "workflow/node/exe_loader.h"  // CodeLoader::resolve_safe_path

namespace fs = std::filesystem;

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
