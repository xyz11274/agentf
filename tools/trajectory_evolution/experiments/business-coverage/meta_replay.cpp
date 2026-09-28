// 本轮修复复测：仅复用已完成的分析报告调用包内元反思，不执行 Agent，也不修改记忆库。
#include "agentflow/trajectory/runtime_config.h"
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <set>

namespace tr = agentflow::trajectory;
namespace ep = tr::evaluation_proto;
namespace fs = std::filesystem;

int main(int argc, char **argv) {
  try {
    if (argc != 5) throw std::runtime_error("expected config, old batch, evaluation root, output");
    auto config = tr::load_runtime_configuration(argv[1]);
    ep::HybridEvaluationBatchReport original;
    std::ifstream batch_file(argv[2], std::ios::binary);
    if (!original.ParseFromIstream(&batch_file)) throw std::runtime_error("invalid saved batch");
    std::vector<ep::HybridEvaluationReport> reports;
    for (const auto &run : original.runs()) {
      const auto &source = run.source();
      if (source.user_id() != "business-training") throw std::runtime_error("unexpected source");
      std::ifstream input(fs::path(argv[3]) / source.conversation_id() /
                          (source.dialogue_id() + ".evaluation.pb"), std::ios::binary);
      ep::HybridEvaluationReport report;
      if (!report.ParseFromIstream(&input) || report.source_fingerprint() != run.source_fingerprint())
        throw std::runtime_error("cached analysis mismatch");
      tr::ensure_safe_evolution_data(report);
      reports.push_back(std::move(report));
    }
    // 在调用模型之前确认 Seed 与历史失败批次完全相同，不引入后来出现的新记忆。
    auto seed_options = config.evolution;
    seed_options.enable_meta_reflection = false;
    const auto seeds = tr::TrajectoryEvolutionPipeline(config.evaluator, seed_options)
                           .synthesize_reports(reports);
    std::set<std::string> before, after;
    for (const auto &candidate : original.candidates()) before.insert(candidate.candidate_id());
    for (const auto &candidate : seeds.report.candidates()) after.insert(candidate.candidate_id());
    if (before != after) throw std::runtime_error("seed identities differ; no request sent");
    const auto *key = std::getenv(config.api_key_env.c_str());
    if (!key || !*key) throw std::runtime_error("missing configured API key");
    config.model.api_key = key;
    config.evaluator.redaction_secrets.push_back(key);
    config.evolution.enable_meta_reflection = true;
    auto adapter = tr::make_openai_compatible_model_adapter(config.model);
    const auto result = tr::TrajectoryEvolutionPipeline(config.evaluator, config.evolution, adapter)
                            .synthesize_reports(reports);
    fs::create_directories(fs::path(argv[4]).parent_path());
    std::ofstream output(argv[4]);
    output << tr::TrajectoryEvolutionPipeline::to_json(result.report);
    if (!output.flush()) throw std::runtime_error("cannot save replay result");
    const auto &meta = result.report.meta_reflection();
    std::cout << "reports=" << reports.size() << " seeds=" << before.size()
              << " status=" << ep::ModelCallStatus_Name(meta.status())
              << " calls=" << result.model_requests.size() << " attempts=" << meta.attempts()
              << " error=" << meta.error_code() << '\n';
    return meta.status() == ep::MODEL_CALL_STATUS_SUCCEEDED ? 0 : 2;
  } catch (const std::exception &error) {
    std::cerr << "meta replay failed: " << error.what() << '\n';
    return 1;
  }
}
