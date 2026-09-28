// 进化流水线：把分析与评测证据整理为候选经验，再进行去重、关系判定和入库。
// 阅读顺序：输入验证 → 候选生成 → 元反思 → 记忆更新；来源证据与处理结果分开保存。
// 失败或未知结论不自动变成可靠经验；已有经验的状态变化必须能追溯到证据。
// 此处是对外声明，具体实现位于 src；修改接口时需考虑现有调用方与持久化版本。

#pragma once

#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "agentflow/trajectory/hybrid_evaluator.h"

namespace agentflow::trajectory {

// 在新增输出边界复用既有持久化数据凭据检查。
void ensure_safe_evolution_data(const google::protobuf::Message &message);

struct EvolutionOptions {
  std::size_t max_input_files = 100000;
  std::size_t max_candidates = 10000;
  std::size_t min_supporting_dialogues = 2;
  std::size_t min_supporting_episodes = 2;
  std::size_t max_memory_entries = 512;
  std::size_t max_applied_batch_ids = 4096;
  std::size_t max_meta_prompt_bytes = 2U * 1024U * 1024U;
  std::size_t max_memory_bytes = 256U * 1024U * 1024U;
  std::size_t max_memory_match_candidates = 8;
  std::size_t max_memory_match_prompt_bytes = 256U * 1024U;
  std::size_t max_memory_update_retries = 3;
  std::size_t full_curation_new_entry_threshold = 20;
  // 仅并发后台关系判断；内存快照和最终提交仍由整理线程串行管理。
  std::size_t memory_review_parallelism = 4;
  std::size_t memory_review_max_attempts = 2;
  std::size_t memory_review_retry_backoff_ms = 15000;
  double min_memory_duplicate_confidence = 0.9;
  double min_memory_relation_confidence = 0.75;
  bool enable_meta_reflection = false;
  bool enable_memory_semantic_matching = true;
  bool require_chinese_reason = true;
  std::shared_ptr<const PromptTemplates> prompts;
};

struct DialogueEvolutionResult {
  std::filesystem::path input_path;
  bool evaluation_available = false;
  EvaluationOutput evaluation;
  std::string error_code;
  std::string error_message;
};

struct EvolutionBatchOutput {
  evaluation_proto::HybridEvaluationBatchReport report;
  std::vector<DialogueEvolutionResult> dialogues;
  std::vector<ModelRequest> model_requests;
};

struct MemoryUpdateOutput {
  evaluation_proto::MemoryUpdateResult result;
  std::vector<ModelRequest> model_requests;
};

class TrajectoryEvolutionPipeline {
public:
  // 初始化 TrajectoryEvolutionPipeline 的状态和资源；输入参数的校验与所有权在构造阶段确定。
  // 对象创建后复用既有配置，实际执行与收尾分别由后续接口负责。
  explicit TrajectoryEvolutionPipeline(EvaluatorOptions evaluator_options = EvaluatorOptions(),
                                       EvolutionOptions evolution_options = EvolutionOptions(),
                                       std::shared_ptr<ModelAdapter> model_adapter = nullptr);

  // 遍历指定目录内有效输入，逐份评测并保留独立失败状态。
  EvolutionBatchOutput evaluate_directory(const std::filesystem::path &root) const;

  // 评测调用方明确选择的文件，避免隐式扫描其他历史数据。
  EvolutionBatchOutput evaluate_files(const std::vector<std::filesystem::path> &paths) const;

  // 将分析报告归并为反思候选与批次结果；来源证据和作用域随候选保留。
  EvolutionBatchOutput
  synthesize_reports(const std::vector<evaluation_proto::HybridEvaluationReport> &reports) const;

  // 将分析报告归并为反思候选与批次结果；来源证据和作用域随候选保留。
  EvolutionBatchOutput
  synthesize_reports(const std::vector<evaluation_proto::HybridEvaluationReport> &reports,
                     const evaluation_proto::EvolutionMemoryStore &memory) const;

  // 读取记忆库并验证版本与结构，损坏内容不得作为可注入经验。
  static evaluation_proto::EvolutionMemoryStore
  load_memory(const std::filesystem::path &path, std::size_t max_file_bytes = 256U * 1024U * 1024U);

  // 与轨迹入库使用同一文件锁；回调只做本地计算，不能持锁调用模型。
  // 读取最新库后应用增量，成功时同时更新 Protobuf 和 JSON 阅读视图。
  static bool transform_memory_file(
      const std::filesystem::path &protobuf_path, const std::filesystem::path &json_path,
      const std::function<void(evaluation_proto::EvolutionMemoryStore &)> &update,
      std::size_t max_file_bytes = 256U * 1024U * 1024U);

  // 把本批候选合入既有库，统一执行重复、关联、冲突和容量管理。
  static evaluation_proto::MemoryUpdateResult
  merge_memory(const evaluation_proto::HybridEvaluationBatchReport &batch,
               evaluation_proto::EvolutionMemoryStore *memory);

  static evaluation_proto::MemoryUpdateResult
  update_memory_file(const evaluation_proto::HybridEvaluationBatchReport &batch,
                     const std::filesystem::path &protobuf_path,
                     const std::filesystem::path &json_path = {},
                     std::size_t max_file_bytes = 256U * 1024U * 1024U);

  MemoryUpdateOutput curate_memory_file(const evaluation_proto::HybridEvaluationBatchReport &batch,
                                        const std::filesystem::path &protobuf_path,
                                        const std::filesystem::path &json_path = {},
                                        std::size_t max_file_bytes = 256U * 1024U * 1024U,
                                        std::size_t prior_model_calls = 0) const;

  // 将当前结果转换为公开 JSON 表示，内部临时状态不随意暴露。
  static std::string to_json(const evaluation_proto::HybridEvaluationBatchReport &report,
                             bool pretty = true);

  // 将当前结果转换为公开 JSON 表示，内部临时状态不随意暴露。
  static std::string to_json(const evaluation_proto::EvolutionMemoryStore &memory,
                             bool pretty = true);

  // 按协议格式写出结果，供跨语言读取和持久化回放。
  static void write_protobuf(const evaluation_proto::HybridEvaluationBatchReport &report,
                             const std::filesystem::path &path);

  // 按协议格式写出结果，供跨语言读取和持久化回放。
  static void write_protobuf(const evaluation_proto::EvolutionMemoryStore &memory,
                             const std::filesystem::path &path);

  // 返回外部配置载入的元反思提示词，调用处不硬编码业务策略。
  static std::string meta_reflection_system_prompt();

  // 返回记忆关系审查提示词，供重复与冲突判断复用。
  static std::string memory_curator_system_prompt();

private:
  EvaluatorOptions evaluator_options_;
  EvolutionOptions evolution_options_;
  std::shared_ptr<ModelAdapter> model_adapter_;
};

} // 命名空间结束：agentflow::trajectory
