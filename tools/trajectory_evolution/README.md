# AgentFlow C++ 轨迹进化

`trajectory_evolution` 是一个独立的离线 C++ 工具，把建立进化记忆库之前的步骤收敛在同一个目录、同一个进程和同一个命令中：

```text
SessionLog JSONL
  → Loop Episode 与确定性指标
  → 规则/可选模型的 Hybrid Evaluation
  → 跨 Dialogue 候选归并与反证检查
  → 可选的一次元反思
  → 质量门
  → Protobuf 进化记忆库
```

它不修改 Harness、Coordinator、Skill Executor 或 Runtime 的在线执行路径，也不会自动激活候选策略。记忆条目目前始终是待验证的 `CANDIDATE`，为后续实验、回放和策略增强提供输入。

## 目录职责

本目录包含完整的离线进化前半闭环：

- `loop_episode_analyzer`：把 SessionLog 还原为 Coordinator/Skill Episode，计算重复、无进展、状态回摆、不可重试失败等确定性诊断；
- `hybrid_evaluator`：融合规则事实与可选模型判断，输出任务结果、逐 Episode 语义判断和 Dialogue 反思；
- `evolution_pipeline`：跨 Dialogue 生成候选、聚合支持/反证、执行质量门和可选元反思；
- `EvolutionMemoryStore`：以 Protobuf 为权威格式，幂等地保存候选、证据、批次和修订历史；
- `trajectory_evolution`：统一命令行入口。

Protobuf 契约位于 [`proto/hybrid_evaluation.proto`](proto/hybrid_evaluation.proto)。

## 构建与测试

在 AgentFlow 开发环境中运行：

```bash
conda run -n agentflow-dev cmake \
  -S tools/trajectory_evolution \
  -B build/trajectory-evolution \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=ON

conda run -n agentflow-dev cmake \
  --build build/trajectory-evolution -j2

conda run -n agentflow-dev ctest \
  --test-dir build/trajectory-evolution \
  --output-on-failure
```

## 从 SessionLog 建立记忆库

默认只运行确定性规则，不访问网络：

```bash
build/trajectory-evolution/trajectory_evolution evaluate \
  trace_output/sessionlogs/by-user \
  --output-root trace_output/evolution \
  --model-selection none
```

一次目录运行会生成：

```text
trace_output/evolution/
├── by-user/<user>/<conversation>/<dialogue>.evaluation.json
├── by-user/<user>/<conversation>/<dialogue>.evaluation.pb
├── runs/<batch-id>/batch.evaluation.json
├── runs/<batch-id>/batch.evaluation.pb
└── memory/
    ├── evolution-memory.pb
    ├── evolution-memory.json
    └── evolution-memory.pb.lock
```

- `evaluation.pb/json` 是每个 Dialogue 的完整 Hybrid Evaluation；
- `batch.evaluation.pb/json` 保存本批输入、失败隔离结果、候选、元反思状态和记忆更新结果；
- `evolution-memory.pb` 是权威记忆库；
- `evolution-memory.json` 是每次运行同步生成的可读视图；
- `.lock` 是跨进程写锁文件，可以长期保留。

同一个 `batch_id` 重放不会增加记忆修订号。批次身份覆盖成功评测结果（其中包含 SessionLog 内容指纹）、最终候选、元反思结果，以及失败输入身份和错误信息；这些内容变化时会形成新批次。记忆写入使用进程间文件锁和同目录临时文件原子替换。

目录中的单个坏 JSONL 会记录为失败的 `EvaluationRunSummary`，不会阻止其他 Dialogue 完成。只有本批一个成功评测都没有时，命令才返回失败退出码。

## 质量门和记忆语义

候选来源有两类：

1. 确定性诊断，例如 `non_retryable_repeat`、`exact_repeat`、`no_state_progress`、`state_oscillation`、`result_not_consumed`；
2. 已通过模型响应严格校验的 Dialogue 反思，包括 Skill 编排、Prompt 和停止条件建议。

可选元反思只允许合并 `target_type` 与 `target_scope` 相同的既有候选，并且只能引用本批真实 Seed ID 和 Event ID。任何字段非法时，整次元反思原子拒绝，原始规则候选继续有效。

候选状态：

- `PROPOSED`：支持的 Dialogue 和 Episode 数达到门槛，且没有反证；
- `INSUFFICIENT_SUPPORT`：证据真实，但尚未达到门槛；
- `CONFLICTED`：相同作用域和错误条件下存在 `USEFUL` 或 `NECESSARY_RETRY` 反证；
- `REJECTED_BY_GATE`：没有可回链的支持证据，不写入记忆库。

默认门槛是至少 2 个 Dialogue、2 个 Episode，可通过以下选项调整：

```bash
--min-supporting-dialogues 2
--min-supporting-episodes 2
```

`PROPOSED`、`INSUFFICIENT_SUPPORT` 和 `CONFLICTED` 都会以候选状态进入记忆，便于后续积累支持、观察冲突和保留负证据；它们不会自动成为线上策略。每条记忆保存首次/最近批次、观察批次集合、去重证据和发生实质变化时的候选快照修订。

## 可选模型评测与元反思

直连 OpenAI-compatible Chat Completions Endpoint：

```bash
export EVOLUTION_API_KEY='...'

build/trajectory-evolution/trajectory_evolution evaluate \
  trace_output/sessionlogs/by-user \
  --output-root trace_output/evolution-model \
  --model-selection suspicious \
  --meta-reflection \
  --model-endpoint 'https://provider.example/v1/chat/completions' \
  --model-id 'MODEL_ID' \
  --api-key-env EVOLUTION_API_KEY \
  --timeout-ms 240000 \
  --max-output-tokens 4096 \
  --max-model-calls 512
```

`--model-selection suspicious` 对每个 Dialogue 只请求一次模型，评测任务结果、可疑 Episode 和反思；`all` 会把所有 Episode 纳入同一次 Dialogue 请求。`--meta-reflection` 最多再进行一次跨 Dialogue 调用，并与 Dialogue 调用共用 `--max-model-calls` 批次预算。

Endpoint 必须是完整路径。生产地址必须使用 HTTPS；HTTP 仅允许 Loopback；默认禁止重定向和系统代理。API Key 只能通过 `--api-key-env` 指定的环境变量读取，不接受命令行明文 Key。

模型不可用、传输失败或输出非法时，规则层继续完成并记录稳定错误码。模型原始响应不写入评测或记忆，只保存指纹和已校验的结构化结论。

如需离线复核请求，可显式增加：

```bash
--requests-out trace_output/evolution-model/model-requests.jsonl
```

该文件包含脱敏、限长后的具体内容，属于敏感派生数据，不应提交到 Git。也可以用 `--model-responses RESPONSES.jsonl` 回放预录模型响应。

## 单 Dialogue 兼容模式

统一二进制仍支持只评测一个 SessionLog：

```bash
build/trajectory-evolution/trajectory_evolution evaluate \
  DIALOGUE.jsonl \
  --model-selection none \
  --output DIALOGUE.evaluation.json \
  --protobuf-out DIALOGUE.evaluation.pb
```

旧 `analysis.json` 也可作为单文件兼容输入，但目录模式只发现 `*.jsonl`，且不再要求先落盘 `analysis.json`。

## 隐私与完整性边界

- 原始 Prompt、Messages、Tool 参数/结果不会复制进最终 Evaluation、Batch 或 Memory Protobuf；
- 具体内容只在启用模型时构造为瞬时、脱敏、限长请求；
- 常见供应商 Key、Bearer/JWT、私钥、敏感字段和显式配置的 Secret 会被检查；发现凭据形态时拒绝持久化；
- Trace 完整性失败默认拒绝；`--allow-invalid-trace` 只允许隔离保存评测，不允许它生成候选或发送给模型；
- 输出根目录和模型回放文件必须位于 SessionLog 输入根目录之外，避免下一批把派生产物重新当作原始轨迹；
- 每条候选证据保留 User/Conversation/Dialogue、Episode、Diagnostic 和 Event ID，可回到 SessionLog 验证。

## 退出码

- `0`：至少一个 Dialogue 完成；模型降级和个别文件失败会写入报告；
- `2`：命令行、模型配置或静态路径冲突；
- `3`：本批没有成功评测，或评测/持久化阶段失败；
- `4`：未预期异常。

当前实现止于“候选进化记忆库”。下一阶段应从记忆库选择候选，在隔离环境中生成策略变体、回放/对照评测，并只把通过验证的修订提升为 `VALIDATED`；不应直接让离线反思修改线上 Prompt、Skill 编排或 Loop 条件。
