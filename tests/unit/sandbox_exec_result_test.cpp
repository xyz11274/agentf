// C++ 单元测试——SandboxHelpers::parse_exec_result_json（B24）
// 覆盖 TC-SBOX-052（ExecResult JSON 解析正确）/ TC-SBOX-053（exit_code 不始终为 0）
// 业务规则: B24 C++ 侧 exec() 必须从 HTTP 响应中正确提取全部字段
// 错误模型: ExecResult JSON 解析遗漏——exit_code 始终为 0、stderr 丢失、truncated 失效
// 失败意味着: Docker/E2B 模式 tool 执行失败被误判为成功，截断标记丢失
//
// 本测试是纯单元测试（不依赖 sandbox_http_server.py），直接测试
// parse_exec_result_json 的 JSON 解析逻辑，同时覆盖三种 mode 的 exec 路径。
#include <gtest/gtest.h>

#include <string>

#include "sandbox/sandbox_helpers.h"

namespace agentflow {
namespace sandbox {

// TC-SBOX-052: ExecResult JSON 解析正确——全部字段正确提取。
// 输入: 完整 ExecResult JSON（含 exit_code=42, stdout, stderr, truncated, duration_ms）
// 预期: 所有字段正确解析，与输入值一致
// 不变量: B24
TEST(SandboxExecResultTest, ParsesAllFieldsCorrectly) {
  std::string resp = R"({"exit_code":42,"stdout":"hello","stderr":"warn","truncated":true,"duration_ms":1234})";
  auto result = SandboxHelpers::parse_exec_result_json(resp, "http://sandbox:8080");

  EXPECT_EQ(result.exit_code, 42);
  EXPECT_EQ(result.stdout, "hello");
  EXPECT_EQ(result.stderr, "warn");
  EXPECT_TRUE(result.truncated);
  EXPECT_EQ(result.duration_ms, 1234);
}

// TC-SBOX-053: exit_code 不始终为 0——验证非零 exit_code 正确透传。
// 这是实际 bug 的回归测试：DockerSandbox/E2BSandbox 之前 exit_code 始终为 0。
// 输入: exit_code=1, stdout="", stderr="command failed"
// 预期: exit_code=1, stderr="command failed"
TEST(SandboxExecResultTest, ExitCodeNotAlwaysZero) {
  std::string resp = R"({"exit_code":1,"stdout":"","stderr":"command failed","truncated":false,"duration_ms":50})";
  auto result = SandboxHelpers::parse_exec_result_json(resp, "http://sandbox:8080");

  EXPECT_EQ(result.exit_code, 1);   // 回归点：以前这里返回 0
  EXPECT_EQ(result.stderr, "command failed");
  EXPECT_FALSE(result.truncated);
}

// exit_code=255 边界值。
TEST(SandboxExecResultTest, ExitCode255Boundary) {
  std::string resp = R"({"exit_code":255,"stdout":"","stderr":"","truncated":false,"duration_ms":0})";
  auto result = SandboxHelpers::parse_exec_result_json(resp, "http://sandbox:8080");

  EXPECT_EQ(result.exit_code, 255);
}

// stdout 含特殊字符（引号、反斜杠、换行转义）。
// rapidjson 正确解析 JSON 转义序列：\n → 换行, \t → 制表符, \" → 引号
TEST(SandboxExecResultTest, StdoutWithEscapedChars) {
  std::string resp = R"({"exit_code":0,"stdout":"line1\nline2\t\"quoted\"","stderr":"","truncated":false,"duration_ms":0})";
  auto result = SandboxHelpers::parse_exec_result_json(resp, "http://sandbox:8080");

  EXPECT_EQ(result.exit_code, 0);
  // rapidjson 正确解析 JSON 转义：\n → 换行符, \t → 制表符, \" → "
  EXPECT_EQ(result.stdout, "line1\nline2\t\"quoted\"");
}

// stderr 非空场景。
TEST(SandboxExecResultTest, StderrNotEmpty) {
  std::string resp = R"({"exit_code":2,"stdout":"","stderr":"Error: file not found","truncated":false,"duration_ms":10})";
  auto result = SandboxHelpers::parse_exec_result_json(resp, "http://sandbox:8080");

  EXPECT_EQ(result.exit_code, 2);
  EXPECT_EQ(result.stderr, "Error: file not found");
}

// truncated=false 场景。
TEST(SandboxExecResultTest, TruncatedFalse) {
  std::string resp = R"({"exit_code":0,"stdout":"ok","stderr":"","truncated":false,"duration_ms":5})";
  auto result = SandboxHelpers::parse_exec_result_json(resp, "http://sandbox:8080");

  EXPECT_FALSE(result.truncated);
}

// duration_ms=0 边界值。
TEST(SandboxExecResultTest, DurationMsZero) {
  std::string resp = R"({"exit_code":0,"stdout":"","stderr":"","truncated":false,"duration_ms":0})";
  auto result = SandboxHelpers::parse_exec_result_json(resp, "http://sandbox:8080");

  EXPECT_EQ(result.duration_ms, 0);
}

// 空响应——endpoint 不可达。
TEST(SandboxExecResultTest, EmptyResponseReturnsError) {
  auto result = SandboxHelpers::parse_exec_result_json("", "http://sandbox:8080");

  EXPECT_EQ(result.exit_code, 1);
  EXPECT_EQ(result.stderr, "Sandbox endpoint unreachable: http://sandbox:8080");
}

// 无法解析的响应——exit_code 字段缺失。
TEST(SandboxExecResultTest, MissingExitCodeReturnsRawResponse) {
  std::string resp = R"({"stdout":"hello"})";
  auto result = SandboxHelpers::parse_exec_result_json(resp, "http://sandbox:8080");

  EXPECT_EQ(result.exit_code, 1);
  EXPECT_EQ(result.stderr, resp);  // 无法解析，返回原始响应
}

// 短 timeout 字段顺序不影响解析。
TEST(SandboxExecResultTest, FieldOrderIndependent) {
  std::string resp = R"({"duration_ms":99,"truncated":true,"stderr":"err","stdout":"out","exit_code":7})";
  auto result = SandboxHelpers::parse_exec_result_json(resp, "http://sandbox:8080");

  EXPECT_EQ(result.exit_code, 7);
  EXPECT_EQ(result.stdout, "out");
  EXPECT_EQ(result.stderr, "err");
  EXPECT_TRUE(result.truncated);
  EXPECT_EQ(result.duration_ms, 99);
}

// 大 duration_ms 值。
TEST(SandboxExecResultTest, LargeDurationMs) {
  std::string resp = R"({"exit_code":0,"stdout":"","stderr":"","truncated":false,"duration_ms":300000})";
  auto result = SandboxHelpers::parse_exec_result_json(resp, "http://sandbox:8080");

  EXPECT_EQ(result.duration_ms, 300000);
}

}  // namespace sandbox
}  // namespace agentflow