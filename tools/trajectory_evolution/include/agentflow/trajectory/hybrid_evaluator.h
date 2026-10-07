// 混合评测：确定性规则提供可复核指标，模型补充业务语义判断。
// 阅读时区分原始执行事实、规则结论和模型结论，三者不能互相替代。
// 模型请求通过适配器发出；协议校验和隐私处理失败时，不生成伪造的成功评分。
// 此处是对外声明，具体实现位于 src；修改接口时需考虑现有调用方与持久化版本。

#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "agentflow/trajectory/prompt_templates.h"
#include "hybrid_evaluation.pb.h"

namespace agentflow::trajectory {

struct AnalysisReport;

namespace evaluation_proto = ::agentflow::trajectory::evaluation;

enum class ModelSelection {
  none,
  suspicious,
  all,
};

struct EvaluatorOptions {
  std::size_t max_file_bytes = 256U * 1024U * 1024U;
  std::size_t max_model_calls = 512;
  ModelSelection model_selection = ModelSelection::none;
  bool strict_model = false;
  bool allow_invalid_trace = false;
  bool require_chinese_reason = true;
  std::vector<std::string> redaction_secrets;
  std::shared_ptr<const PromptTemplates> prompts;
};

struct ModelRequest {
  std::string packet_id;
  std::string system_prompt;
  std::string user_prompt;
  std::vector<std::string> allowed_event_ids;
};

struct RawModelResponse {
  bool transport_ok = false;
  std::string adapter;
  std::string model_id;
  std::string body;
  std::string error_code;
  std::uint32_t attempts = 0;
};

class ModelAdapter {
public:
  virtual ~ModelAdapter() = default;
  // 处理一个已经组装好的请求；只返回传输结果与原始正文，不替上层做业务验收。
  virtual RawModelResponse evaluate(const ModelRequest &request) = 0;
  // 标识适配器实现（例如 HTTP 或本地回放），用于诊断调用来源。
  virtual std::string name() const = 0;
  // 返回当前模型标识供报告归因，不包含认证凭据。
  virtual std::string model_id() const = 0;
};

struct OpenAiCompatibleOptions {
  std::string endpoint;
  std::string model_id;
  std::string api_key;
  std::string thinking_type;
  // 一次 evaluate 调用的全部尝试共享同一网络预算。
  std::uint32_t timeout_ms = 30000;
  std::uint32_t connect_timeout_ms = 5000;
  std::uint32_t max_retries = 1;
  std::size_t max_response_bytes = 256U * 1024U;
  std::size_t max_request_bytes = 2U * 1024U * 1024U;
  std::uint32_t max_output_tokens = 800;
  bool disable_proxy = true;
  // 可选后台取消回调；未设置时保留原传输行为。
  std::function<bool()> cancelled;
  // 只有提供方支持 JSON 对象输出时才显式启用。
  bool json_object_output = false;
  // 可选 SSE 传输；空闲预算为零时保留原等待方式。
  bool stream = false;
  std::uint32_t idle_timeout_ms = 0;
};

// 创建离线响应适配器，便于可重复回归；它不会替代真实实验的模型能力验证。
std::shared_ptr<ModelAdapter> make_jsonl_model_adapter(const std::filesystem::path &path,
                                                       std::size_t max_file_bytes = 64U * 1024U *
                                                                                    1024U);

// 创建兼容聊天接口的模型适配器，集中处理超时、取消和凭据，业务层不直接拼接网络请求。
std::shared_ptr<ModelAdapter>
make_openai_compatible_model_adapter(const OpenAiCompatibleOptions &options);

struct EvaluationOutput {
  evaluation_proto::HybridEvaluationReport report;
  std::vector<ModelRequest> model_requests;
};

class EvaluationError : public std::runtime_error {
public:
  // 创建评测异常并保存受限诊断，不把原始输入附加到对外错误。
  explicit EvaluationError(const std::string &message) : std::runtime_error(message) {}
};

class HybridTrajectoryEvaluator {
public:
  // 初始化 HybridTrajectoryEvaluator 的状态和资源；输入参数的校验与所有权在构造阶段确定。
  // 对象创建后复用既有配置，实际执行与收尾分别由后续接口负责。
  explicit HybridTrajectoryEvaluator(EvaluatorOptions options = EvaluatorOptions(),
                                     std::shared_ptr<ModelAdapter> model_adapter = nullptr);

  // 读取并评测单个文件，把解析、分析与模型判定的错误分开处理。
  EvaluationOutput evaluate_file(const std::filesystem::path &path) const;

  // 评测已经读取的结构化输入，复用与文件入口相同的验证流程。
  EvaluationOutput evaluate_json(const std::string &analysis_json) const;

  // 通过兼容解析器处理原会话日志，复用统一评测和脱敏流程。
  EvaluationOutput evaluate_sessionlog(const std::filesystem::path &path) const;

  // 中性的 JSON 入口，避免把新协议命名空间暴露到旧头文件。
  EvaluationOutput evaluate_trace_json(const std::string &trace_json) const;

  // 将当前结果转换为公开 JSON 表示，内部临时状态不随意暴露。
  static std::string to_json(const evaluation_proto::HybridEvaluationReport &report,
                             bool pretty = true);

  // 按协议格式写出结果，供跨语言读取和持久化回放。
  static void write_protobuf(const evaluation_proto::HybridEvaluationReport &report,
                             const std::filesystem::path &path);

  // 按行写出允许保存的模型请求用于诊断，写入前遵守脱敏约束。
  static void write_model_requests_jsonl(const std::vector<ModelRequest> &requests,
                                         const std::filesystem::path &path);

  // 返回当前评测器使用的系统提示词，来源由运行配置控制。
  static std::string system_prompt();

  // 返回对话评测系统提示词，内容由外部模板提供。
  static std::string dialogue_system_prompt();

private:
  EvaluationOutput evaluate_content(const AnalysisReport &analysis, const std::string &content,
                                    bool standard_trace = false,
                                    const std::string &execution_status = {},
                                    const std::string &business_status = {},
                                    const std::string &model_content = {}) const;

  EvaluatorOptions options_;
  std::shared_ptr<ModelAdapter> model_adapter_;
};

// 将内部枚举映射为稳定的协议名称，保证日志和序列化使用统一表示。
std::string to_string(ModelSelection selection);
// 解析何时启用模型的选择模式，拒绝未支持的名称。
ModelSelection parse_model_selection(const std::string &value);

} // 命名空间结束：agentflow::trajectory
