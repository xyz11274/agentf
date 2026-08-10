// C++ 单元测试——sandbox_helpers（T040）
// 覆盖 AC-US2-001/002/003 的 C++ 侧——SandboxHelpers 构造 tool_params 调 Sandbox::exec
// 用桩 Sandbox（StubSandbox）绕过 LocalSandbox 的 fork+exec，纯校验 tool_params JSON 构造正确性
#include <gtest/gtest.h>

#include <string>
#include <utility>

#include "sandbox/sandbox_helpers.h"
#include "sandbox/sandbox_types.h"

namespace agentflow {
namespace sandbox {
namespace {

// 桩 Sandbox：捕获 exec() 的 tool + tool_params_json，返回固定 ExecResult
// 用途：纯校验 SandboxHelpers 构造的 tool_params JSON 符合 contracts/sandbox-http-server.md
class StubSandbox : public Sandbox {
 public:
  std::string last_tool;
  std::string last_tool_params;
  ExecResult last_opts_result;  // opts.working_dir / opts.timeout_ms / opts.env_vars
  ExecResult preset_result;     // 预设返回值

  ExecResult exec(const std::string& tool,
                  const std::string& tool_params_json,
                  const ExecOptions& opts) override {
    last_tool = tool;
    last_tool_params = tool_params_json;
    ExecOptions* opts_ptr = const_cast<ExecOptions*>(&opts);
    (void)opts_ptr;  // 捕获 opts（简化：仅记录 working_dir）
    return preset_result;
  }
  std::string id() const override { return "stub-001"; }
  void destroy() override {}
  std::string endpoint() const override { return "http://127.0.0.1:8080"; }
  SandboxState state() const override { return SandboxState::RUNNING; }
  int64_t last_active_at() const override { return 0; }
  void mark_active() override {}
  void snapshot() override {}
  void pause() override {}   // Stub：无平台侧暂停语义
  void resume() override {}  // Stub：无平台侧恢复语义
  const SandboxKey& owner() const override { return owner_; }
  const std::string& workdir() const override { return workdir_; }

 private:
  SandboxKey owner_;
  std::string workdir_ = "/tmp/workdir";
};

// bash tool_params 含 command
TEST(SandboxHelpersTest, BashConstructsCorrectParams) {
  StubSandbox sb;
  sb.preset_result = ExecResult{};
  sb.preset_result.exit_code = 0;
  ExecOptions opts;
  SandboxHelpers::bash(sb, "echo hello", opts);
  EXPECT_EQ(sb.last_tool, "bash");
  EXPECT_NE(sb.last_tool_params.find("\"command\":\"echo hello\""), std::string::npos);
}

// bash 危険命令透传（SandboxHelpers 不做宿主侧检查——沙箱侧 server 保留第二道防线）
TEST(SandboxHelpersTest, BashPassesDangerousCommand) {
  StubSandbox sb;
  sb.preset_result = ExecResult{};
  sb.preset_result.exit_code = 1;  // 沙箱侧应拒
  ExecOptions opts;
  SandboxHelpers::bash(sb, "rm -rf /", opts);
  EXPECT_NE(sb.last_tool_params.find("\"command\":\"rm -rf /\""), std::string::npos)
      << "SandboxHelpers 透传命令给沙箱侧，不做宿主侧预检查";
}

// JSON 转义：含引号/反斜杠的字符串正确转义
TEST(SandboxHelpersTest, JsonEscapeSpecialChars) {
  StubSandbox sb;
  sb.preset_result = ExecResult{};
  sb.preset_result.exit_code = 0;
  ExecOptions opts;
  // 命令含双引号 + 反斜杠 + 换行
  SandboxHelpers::bash(sb, "echo \"a\\b\nc\"", opts);
  // 转义后应含 \" \\ \n（而非裸 " \ \n）
  EXPECT_EQ(sb.last_tool_params.find("\"a\\b"), std::string::npos)
      << "反斜杠应被转义为 \\\\";
  EXPECT_NE(sb.last_tool_params.find("\\n"), std::string::npos)
      << "换行应被转义为 \\n";
 }

 // ── validate_upload_path（会话级上传路径校验） ─────────────────────

 TEST(SandboxHelpersTest, ValidateUploadPath_AcceptsSessionPath) {
   auto result = SandboxHelpers::validate_upload_path("workspace/user1_client-a/upload/image.png");
   EXPECT_TRUE(result.empty()) << "会话级路径应被接受: " << result;
 }

 TEST(SandboxHelpersTest, ValidateUploadPath_RejectsLegacyPath) {
   auto result = SandboxHelpers::validate_upload_path("workspace/upload/image.png");
   EXPECT_FALSE(result.empty()) << "旧格式 workspace/upload/ 应被拒绝";
   EXPECT_NE(result.find("PATH_NOT_IN_UPLOAD_DIR"), std::string::npos);
 }

 TEST(SandboxHelpersTest, ValidateUploadPath_RejectsEmptySession) {
   auto result = SandboxHelpers::validate_upload_path("workspace//upload/image.png");
   EXPECT_FALSE(result.empty()) << "空会话目录应被拒绝";
 }

 TEST(SandboxHelpersTest, ValidateUploadPath_RejectsNonWorkspacePrefix) {
   auto result = SandboxHelpers::validate_upload_path("tmp/upload/image.png");
   EXPECT_FALSE(result.empty()) << "非 workspace/ 前缀应被拒绝";
 }

 TEST(SandboxHelpersTest, ValidateUploadPath_RejectsAbsolutePath) {
   auto result = SandboxHelpers::validate_upload_path("/tmp/agentflow/workspace/x/upload/image.png");
   EXPECT_FALSE(result.empty()) << "宿主机绝对路径应被拒绝";
 }

 TEST(SandboxHelpersTest, ValidateUploadPath_RejectsPathOutsideUpload) {
   auto result = SandboxHelpers::validate_upload_path("workspace/user1_client-a/other/file.png");
   EXPECT_FALSE(result.empty()) << "upload 目录外的路径应被拒绝";
 }

 }  // namespace
 }  // namespace sandbox
 }  // namespace agentflow
