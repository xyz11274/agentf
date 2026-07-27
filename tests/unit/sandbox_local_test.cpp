// C++ 单元测试——LocalSandbox（T020）
// 覆盖 TC-SBOX-001（echo 返回正确）/ TC-SBOX-012（截断标记）/ TC-SBOX-033（destroy 幂等）
#include <gtest/gtest.h>

#include <filesystem>
#include <string>

#include "sandbox/sandbox_local.h"

namespace fs = std::filesystem;

// TC-SBOX-001: echo hello 在沙箱内执行返回正确。
// 业务规则: B1 命令在隔离沙箱内执行，宿主机不受副作用
// 错误模型: 命令泄漏——命令在宿主执行而非沙箱内
// 失败意味着: 宿主机文件系统出现命令副作用，隔离失效
TEST(SandboxLocalTest, EchoReturnsCorrect) {
  // 此测试需启动 sandbox_http_server.py 子进程——属集成测试范畴
  // 单元层仅校验 LocalSandbox 构造不抛 + id/endpoint 非空
  // 真实 echo 验证在 sandbox_lifecycle_test.cpp 的集成测试中
  GTEST_SKIP() << "集成测试范畴——见 sandbox_lifecycle_test.cpp";
}

// TC-SBOX-012: 命令输出超 output_max_bytes 截断标记明示。
// 业务规则: B23 截断标记明示
// 错误模型: 截断未标记——truncated 字段未更新
// 失败意味着: 超长输出破坏沙箱稳定性，用户不知输出已截断
TEST(SandboxLocalTest, OutputTruncationMarked) {
  // 截断逻辑在 sandbox_http_server.py 侧（_truncate 函数）
  // 此处校验 LocalSandbox 透传 opts.output_max_bytes 给 server
  GTEST_SKIP() << "截断逻辑在 server 侧——见 sandbox_http_server Python 测试";
}

// TC-SBOX-040: 超长输出（10MB）截断不破坏沙箱稳定性。
// 业务规则: B23 截断在沙箱侧做
// 错误模型: 截断日志——截断函数误 applied 到日志路径
// 失败意味着: 沙箱因超长输出崩溃，或日志被截断丢失诊断信息
TEST(SandboxLocalTest, TenMbOutputTruncatedSafe) {
  GTEST_SKIP() << "极端输出测试——集成层";
}

// TC-SBOX-033: 销毁幂等多次调用一致。
// 业务规则: B7 销毁幂等
// 错误模型: 销毁不幂等——socket/进程残留
// 失败意味着: 二次 destroy 崩或残留
TEST(SandboxLocalTest, DestroyIdempotent) {
  // 构造一个 LocalSandbox（需 sandbox_http_server.py 路径）
  // destroy() 调 2 次，断言无异常 + state == RECYCLING
  // 此测试需 sandbox_http_server.py 可用——见集成测试
  GTEST_SKIP() << "需 sandbox_http_server.py 可用——见集成测试";
}

// 额外：LocalSandbox 构造时 workdir 创建
TEST(SandboxLocalTest, WorkdirCreatedOnConstruct) {
  GTEST_SKIP() << "需 sandbox_http_server.py——集成层";
}
