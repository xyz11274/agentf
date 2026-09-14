# Batch Evaluation 与 Evolution Memory 字段解读

本文解释目录评测产生的两类跨 Dialogue 数据：

```text
HybridEvaluationBatchReport
  → 本次输入、评测结果、候选生成、元反思和记忆更新的批次报告

EvolutionMemoryStore
  → 跨多个 Batch 持续积累的候选进化记忆库
```

单个 Dialogue 的 `*.evaluation.json` 字段见
[EVALUATION_FIELDS.md](EVALUATION_FIELDS.md)，从 SessionLog 到 Memory 的完整处理过程见
[SESSIONLOG_TO_EVOLUTION_MEMORY.md](SESSIONLOG_TO_EVOLUTION_MEMORY.md)。

字段契约以
`tools/trajectory_evolution/proto/hybrid_evaluation.proto` 为准。

## 1. Batch 和 Memory 的职责不同

```text
10 个 Dialogue Evaluation
          │
          ▼
1 个 Batch Evaluation
  ├── 记录这 10 个 Dialogue 哪些成功、哪些失败
  ├── 保存本批生成的最终候选
  ├── 保存元反思结果
  └── 保存本批写入 Memory 的结果
          │
          ▼
1 次 Memory Update
          │
          ▼
Evolution Memory
  ├── 已应用的所有 Batch ID
  ├── 跨批次合并后的候选
  └── 每个候选的修订历史
```

可以把两者理解为：

| 对象 | 性质 | 回答的问题 |
| --- | --- | --- |
| Batch Evaluation | 单次运行报告 | 这一批输入发生了什么，产生了哪些候选？ |
| Evolution Memory | 累计状态 | 到目前为止，系统积累了哪些进化假设和证据？ |

Batch 不会因为后来又运行了新批次而改变；Memory 会随着新 Batch 持续更新。

## 2. Protobuf JSON 的阅读规则

Batch 和 Memory 都以 Protobuf 定义数据结构，并提供 JSON 可读视图。

### 2.1 `uint64` 在 JSON 中表现为字符串

例如：

```json
{
  "evaluation_succeeded_count": "10",
  "revision": "1"
}
```

它们在 Protobuf 中仍是无符号 64 位整数。写成 JSON 字符串是为了避免 JavaScript
读取大整数时丢失精度。

### 2.2 默认值经常不出现在 JSON 中

| Protobuf 类型 | 字段缺失时的常见默认值 |
| --- | --- |
| 数字 | `0` |
| 布尔值 | `false` |
| 字符串 | 空字符串 |
| repeated | 空数组 |
| enum | `UNSPECIFIED` |

因此没有出现 `evaluation_failed_count`，通常表示它是 0，不表示采集缺失。

下文表格为简洁起见会省略部分公共枚举前缀。实际 JSON 中通常写成完整名称，例如：

```text
SUCCEEDED     → MODEL_CALL_STATUS_SUCCEEDED
PROPOSED      → EVOLUTION_CANDIDATE_STATUS_PROPOSED
CANDIDATE     → EVOLUTION_MEMORY_STATUS_CANDIDATE
RETRY_POLICY  → CANDIDATE_TARGET_TYPE_RETRY_POLICY
```

### 2.3 哪个文件是权威数据

```text
batch.evaluation.pb   与 batch.evaluation.json 表达同一批次报告
evolution-memory.pb   是累计记忆库的权威文件
evolution-memory.json 是 PB 同步生成的可读视图
```

Memory 加载始终读取 `.pb`，不会读取 JSON 反向恢复状态。

## 3. `HybridEvaluationBatchReport` 顶层结构

```text
HybridEvaluationBatchReport
├── 版本、配置和 batch_id
├── 输入与成功/失败计数
├── runs[]
├── synthesis
├── candidates[]
├── meta_reflection
└── memory_update
```

### 3.1 版本与身份字段

| 字段 | 含义 |
| --- | --- |
| `schema_version` | Batch Protobuf 结构版本。当前为 1。 |
| `evaluation_kind` | 报告类型，当前为 `hybrid_evaluation_and_candidate_synthesis`。 |
| `pipeline_version` | 跨 Dialogue 管线版本，用于区分候选生成与合并逻辑。 |
| `candidate_generator_version` | EvolutionCandidate 生成器版本。 |
| `evaluator_config_fingerprint` | 本次评测关键配置的稳定指纹。 |
| `batch_id` | 本批最终身份，格式为 `batch:<sha256>`。 |

`evaluator_config_fingerprint` 当前覆盖：

- Pipeline 和 Candidate Generator 版本；
- 模型选择模式；
- `max_file_bytes` 和 `max_model_calls`；
- 是否允许无效轨迹；
- 是否要求中文理由；
- Dialogue/Episode 质量门阈值；
- 是否启用元反思；
- 实际使用的 Model Adapter 和模型 ID。

`batch_id` 进一步覆盖：

- 每个成功 Dialogue 的 `source_fingerprint + evaluation_fingerprint`；
- 每个失败输入的 `input_fingerprint + error_code + error_message` 指纹；
- 最终 Candidate 内容；
- `meta_reflection` 的状态和结果指纹。

`memory_update` 不参与 `batch_id`。否则 Batch ID 必须先写 Memory 才能得到，而
Memory 幂等判断又需要先知道 Batch ID，会形成循环依赖。

### 3.2 输入数量字段

| 字段 | 含义 | 不代表什么 |
| --- | --- | --- |
| `discovered_input_count` | 本次发现并尝试处理的 JSONL 文件数 | 不是 Event 数 |
| `evaluation_succeeded_count` | 成功生成 HybridEvaluationReport 的 Dialogue 数 | 不是业务成功数，也不是模型成功数 |
| `evaluation_failed_count` | 在读取、解析或评测阶段未生成报告的 Dialogue 数 | 不是 Tool/Skill 失败数 |

一个 Dialogue 可以满足：

```text
evaluation_succeeded_count += 1
但 business_status = FAILED
```

因为“评测成功”只表示程序成功分析了这条轨迹，不表示原业务任务成功。

## 4. `runs[]`：每个输入文件的批次摘要

每个输入文件对应一个 `EvaluationRunSummary`。

### 4.1 输入与 Evaluation 身份

| 字段 | 含义 |
| --- | --- |
| `input_fingerprint` | CLI 目录模式下为规范化输入路径的 SHA-256，用于标识哪个输入失败或冲突。 |
| `source` | 成功报告的 `user_id / conversation_id / dialogue_id`。 |
| `source_fingerprint` | 完整 SessionLog 内容的 SHA-256；日志内容变化时会变化。 |
| `evaluation_fingerprint` | 完整 HybridEvaluationReport Protobuf 的 SHA-256。 |
| `evaluation_available` | 是否成功生成了逐 Dialogue Evaluation。 |

必须区分三个指纹：

```text
input_fingerprint       → 输入文件位置身份
source_fingerprint      → 原始 SessionLog 内容身份
evaluation_fingerprint  → 评测结果身份
```

同一路径下日志内容变化时，`input_fingerprint` 可以不变，但 `source_fingerprint` 和
`evaluation_fingerprint` 会变化。

### 4.2 轨迹和模型摘要

| 字段 | 含义 |
| --- | --- |
| `trace_integrity_valid` | 原始轨迹是否通过结构完整性校验。 |
| `model_status` | Dialogue 级模型评测状态，来自 `dialogue_assessment.status`。 |
| `model_error_code` | 模型未成功时的稳定错误码。 |
| `episode_count` | 该 Dialogue 的 Episode 总数。 |
| `invalid_episode_count` | `assessments[].final_verdict=INVALID` 的数量。 |

`model_status` 不是业务状态，也不是 Workflow 状态。常见值为：

| 值 | 含义 |
| --- | --- |
| `NOT_REQUESTED` | 规则模式，没有要求模型评测。 |
| `SKIPPED` | 配置、预算、轨迹完整性或请求大小使模型分支跳过。 |
| `SUCCEEDED` | 模型响应通过全部结构、证据和安全校验。 |
| `TRANSPORT_ERROR` | 网络、超时或 Provider 调用失败。 |
| `INVALID_RESPONSE` | Provider 有响应，但 JSON、字段、证据或安全校验失败。 |

### 4.3 文件级失败字段

| 字段 | 含义 |
| --- | --- |
| `error_code` | 未能生成 Dialogue Evaluation 时的批处理错误类别。 |
| `error_message` | 经过凭据和本地路径脱敏、再限长的错误信息。 |

当 `evaluation_available=false` 时，通常没有 `source`、`source_fingerprint`、
`evaluation_fingerprint` 和模型摘要，只保留输入身份与失败原因。

## 5. `synthesis`：候选生成统计

`CandidateSynthesisMetrics` 描述从逐 Dialogue 报告到最终候选的收敛过程。

| 字段 | 含义 |
| --- | --- |
| `input_report_count` | 送入候选合成器的成功 Evaluation 数。 |
| `accepted_report_count` | 轨迹完整、允许贡献候选的报告数。 |
| `quarantined_report_count` | 因轨迹完整性失败而隔离的报告数。 |
| `deterministic_seed_count` | 确定性 Diagnostic 产生的原始 Seed 次数。 |
| `reflection_seed_count` | 已校验 Dialogue Reflection 产生的原始 Seed 次数。 |
| `candidate_count` | 去重、反证、元反思和质量门处理后的最终候选数。 |
| `proposed_count` | 最终状态为 PROPOSED 的候选数。 |
| `conflicted_count` | 最终状态为 CONFLICTED 的候选数。 |
| `insufficient_support_count` | 最终状态为 INSUFFICIENT_SUPPORT 的候选数。 |
| `rejected_count` | 最终状态为 REJECTED_BY_GATE 的候选数。 |

Seed 数量不等于 Candidate 数量：

```text
多个 Dialogue 产生语义和稳定键相同的 Seed
  → 合并为一个 Candidate

多个 Candidate 被元反思判定为同一问题
  → 继续合并
```

`deterministic_seed_count` 和 `reflection_seed_count` 是合并前的产生次数；
`candidate_count` 才是本批最终保存的候选数量。

## 6. `candidates[]`：本批最终进化候选

每一项都是 `EvolutionCandidate`，代表一个需要后续实验验证的策略修改假设。

### 6.1 候选身份与版本

| 字段 | 含义 |
| --- | --- |
| `schema_version` | Candidate 数据结构版本。 |
| `candidate_id` | 候选稳定 ID，格式为 `candidate:<sha256>`。 |
| `generator_version` | 生成该候选的生成器版本。 |
| `proposal_fingerprint` | 规范化 `proposed_change` 后的 SHA-256。 |

`candidate_id` 的稳定键包含：

```text
target_type
generator_version
normalized target_scope
normalized trigger_pattern
proposal_fingerprint
```

证据数量不参与 `candidate_id`，所以同一候选可以跨 Batch 不断增加证据而保持同一个
Memory Entry。目标、适用范围、触发条件或建议内容实质变化时，会形成新 ID。

新结构化经验使用 `generator_version=scoped-experience-generator-v1`，其作用域、
触发条件与行为分别来自 `reflection.experience_details`，证据只取该条引用。
这不增加 Memory 字段，也不是效果验证或上线批准。旧字符串经验仍可入库管理，
但不能与新契约经验经精确去重、元反思或语义整理合并后获得新注入资格；新 Shadow
会用 `experience_contract` 原因排除旧经验。不要手工改版本号代替重新评测。

### 6.2 要改什么

| 字段 | 含义 |
| --- | --- |
| `target_type` | 修改目标类型。 |
| `target_scope` | 修改作用域，例如 `coordinator` 或 `skill:<skill-name>`。 |
| `trigger_pattern` | 何种可观察条件下应用该建议。 |
| `observed_problem` | 从轨迹证据总结出的实际问题。 |
| `proposed_change` | 建议实施的策略修改。 |
| `expected_benefit` | 预期改善的指标或行为。 |
| `error_class` | 与建议相关的稳定错误类别；模型建议可能为空。 |
| `retryability` | 相关 Tool 结果的可重试性，如 `retryable`、`non_retryable`、`mixed` 或 `unknown`。 |

`target_type` 可取：

| 值 | 修改对象 |
| --- | --- |
| `SKILL_ORCHESTRATION` | Coordinator 的 Skill 选择、并发和重排逻辑 |
| `EXPERIENCE_INJECTION` | 可检索并注入后续决策上下文的复用经验 |
| `LOOP_STOP_CONDITION` | Loop 结束与无进展检测条件 |
| `RETRY_POLICY` | 可重试/不可重试错误处理策略 |
| `TOOL_RESULT_CONSUMPTION` | Tool Result 写回下一轮上下文的规则 |

这些字段只是候选描述，不会直接修改在线 Harness。

### 6.3 质量门状态

| 字段 | 含义 |
| --- | --- |
| `status` | 当前候选的证据质量状态。 |
| `gate_reason` | 为什么得到该状态。 |
| `confidence` | 由证据置信度、支持覆盖度和反证共同计算的值。 |
| `required_supporting_dialogues` | 需要的最少独立 Dialogue 数。 |
| `required_supporting_episodes` | 需要的最少 Episode/支持单元数。 |

候选 `status`：

| 状态 | 含义 |
| --- | --- |
| `PROPOSED` | 达到支持门槛且没有反证，可以进入后续隔离实验。 |
| `INSUFFICIENT_SUPPORT` | 有真实证据，但覆盖量还不足。 |
| `CONFLICTED` | 相同条件下存在 Useful 或 Necessary Retry 反证。 |
| `REJECTED_BY_GATE` | 没有可回链的支持证据，不写入 Memory。 |

`PROPOSED` 也不等于 `VALIDATED`，更不等于已经上线。

普通候选的 `confidence` 由证据均值和支持覆盖度计算，存在反证时会降低。候选经过
元反思合并后，最终值取“重新计算的证据置信度”和“已校验的元反思置信度”中的较大
者。因此看到较高置信度时，还应结合 `meta_reflection_applied` 判断其来源。

### 6.4 支持和反证统计

| 字段 | 含义 |
| --- | --- |
| `support_user_count` | 支持证据覆盖的独立 User 数。 |
| `support_conversation_count` | 支持证据覆盖的独立 `(user, conversation)` 数。 |
| `support_dialogue_count` | 支持证据覆盖的独立 `(user, conversation, dialogue)` 数。 |
| `support_episode_count` | 支持证据覆盖的独立 Episode 数。 |
| `invalid_episode_count` | 支持证据中 verdict=INVALID 的独立 Episode 数。 |
| `contradiction_count` | 反证条数。 |

Conversation 和 Dialogue 使用包含上级身份的复合键统计。两个用户即使碰巧使用相同
`conversation_id`，也不会被错误合并成同一个会话。

质量门内部使用 `support_units`：通常是独立 Episode 数；当 Dialogue Reflection
没有提供 `wasted_episode_ids` 时，回退为支持证据条数。因此
`support_episode_count=0` 不一定表示完全没有支持，只表示没有明确指向 Episode。

### 6.5 元反思血缘

| 字段 | 含义 |
| --- | --- |
| `meta_reflection_applied` | 该候选是否由跨轨迹元反思合并或改写。 |
| `source_seed_ids` | 元反思所合并的原始 Candidate/Seed ID。 |

未经过元反思的候选通常不写这两个字段的非默认值。

## 7. `support_evidence[]` 与 `contradiction_evidence[]`

两者都使用 `CandidateEvidence`，区别在于证据支持还是反驳候选。

| 字段 | 含义 |
| --- | --- |
| `evidence_id` | 证据稳定 ID，由来源身份和引用 ID 集合生成。 |
| `kind` | 证据来源类型。 |
| `source` | `user_id / conversation_id / dialogue_id`。 |
| `source_fingerprint` | 证据对应的原始 SessionLog 内容指纹。 |
| `episode_ids` | 关联 Episode。 |
| `diagnostic_ids` | 关联确定性 Diagnostic。 |
| `evidence_event_ids` | 可回原始 SessionLog 的 Event ID。 |
| `verdict` | 该证据对应的最终/规则语义判断。 |
| `confidence` | 该条证据的置信度。 |

`kind` 可取：

| 值 | 来源 |
| --- | --- |
| `DETERMINISTIC_RULE` | 确定性无效 Loop Diagnostic |
| `DIALOGUE_REFLECTION` | 已严格校验的单 Dialogue 模型反思 |
| `META_REFLECTION` | Protobuf 预留的元反思证据类型；当前实现不单独创建这种 Evidence |
| `CONTRADICTION` | 相同条件下的 Useful/Necessary Retry 反证 |

当前元反思会合并原 Seed 已有的支持/反证，并通过 `meta_reflection_applied=true` 和
`source_seed_ids[]` 保存元反思血缘，不会额外伪造一条 CandidateEvidence。

查看 Candidate 时不能只读 `proposed_change`。至少应同时检查：

```text
status
support_dialogue_count / support_episode_count
contradiction_count
support_evidence[].evidence_event_ids
contradiction_evidence[].evidence_event_ids
```

## 8. `meta_reflection`：本批跨轨迹合并结果

`MetaReflectionAssessment` 描述可选的批次级模型调用——在所有 Dialogue 评测之后，
对候选 Seed 再做一次跨轨迹归并。它最多执行一次，与 Dialogue 调用共享批次预算。

| 字段 | 含义 |
| --- | --- |
| `status` | 元反思调用和校验状态。 |
| `adapter` | 使用的模型适配器。 |
| `model_id` | 实际模型 ID。 |
| `request_fingerprint` | 元反思 system+user Prompt 的指纹。 |
| `response_fingerprint` | Provider 原始响应的指纹。 |
| `error_code` | 跳过、传输失败或响应非法时的稳定错误码。 |
| `attempts` | Model Adapter 实际记录的尝试次数。 |
| `input_seed_count` | 进入元反思前的候选 Seed 数。 |
| `output_candidate_count` | 模型成功输出并通过校验的合并组数量。 |

两个数量容易混淆。以下是一次真实 10-Dialogue 评测的数值：

```text
input_seed_count = 27
output_candidate_count = 9
最终 synthesis.candidate_count = 13
```

这表示 9 个元反思合并组消化了部分 Seed；没有被模型返回的 Seed 会原样保留，所以
最终候选数不一定等于 `output_candidate_count`。

当 `status=INVALID_RESPONSE` 时，整次元反思原子丢弃，`candidates[]` 保留元反思前
的候选。`error_code=incompatible_seed_group` 等错误说明模型违反了候选兼容边界，
不表示 Dialogue 评测失败。

## 9. `memory_update`：本批写入记忆库的结果

`MemoryUpdateResult` 是 Batch 生成后，CLI 实际更新 Memory 得到的结果。

| 字段 | 含义 |
| --- | --- |
| `requested` | 本次是否请求更新 Memory。 |
| `applied` | 本批是否作为新 Batch 应用到 Store。 |
| `idempotent_replay` | 是否检测到相同 `batch_id` 已应用。 |
| `revision_before` | 更新前 Store revision。 |
| `revision_after` | 更新后 Store revision。 |
| `inserted_entry_count` | 新建的 Memory Entry 数。 |
| `updated_entry_count` | Candidate、来源映射或关系发生实质变化的既有 Entry 数。 |
| `unchanged_entry_count` | 找到同一 Entry，但合并后没有实质变化的数量。 |
| `error_code` | 非致命持久化附属错误，例如 JSON 视图写入失败或幂等映射缺失。 |
| `decisions` | 每个输入 Candidate 的增量决策，以及本次触发的全库复核决策。 |
| `curator_version` | 产生本次决策的 Memory Curator 版本。 |
| `model_requested_count` | 记忆整理实际发出的外部模型请求数；并发重算复用相同响应时不重复计数。 |
| `model_succeeded_count` | 实际模型请求中，通过结构、安全和引用边界校验的数量。 |
| `model_failed_count` | 实际模型请求中，传输失败或响应未通过校验的数量。 |
| `concurrency_retry_count` | 提交前发现 Memory 已被其他进程更新而重新计算的次数。 |
| `full_curation_triggered` | 本次是否达到新增记忆阈值并启动全库复核。 |
| `full_curation_completed` | 确定性合并和所有必要语义复核是否完整完成。 |
| `pending_new_entry_count` | Store 中累计、尚未被一次完整全库复核清零的新 Entry 数。 |
| `full_curation_examined_count` | 本次全库复核实际形成决策的项目数。 |
| `full_curation_merged_count` | 本次全库复核合并掉的重复 Entry 数。 |
| `full_curation_error_code` | 全库复核未完成的原因；不表示增量入库失败。 |
| `evicted_entry_count` | 本次为满足 Entry 数或总字节上限而淘汰的 Entry 数。 |
| `pruned_applied_batch_id_count` | 本次从精确 Batch 幂等窗口移除的最旧 Batch ID 数。 |

典型首次写入：

```json
{
  "requested": true,
  "applied": true,
  "revision_after": "1",
  "inserted_entry_count": "13"
}
```

典型幂等重放：

```json
{
  "requested": true,
  "idempotent_replay": true,
  "revision_before": "1",
  "revision_after": "1"
}
```

由于 Protobuf JSON 省略默认值，第二个例子没有显示 `applied=false`。
幂等重放仍会执行纯容量维护，以便旧 Store 在更严格的新配置下收敛；此时
`revision` 不增加，但 `evicted_entry_count` 或
`pruned_applied_batch_id_count` 可以非零。


`decisions[]` 中最重要的字段是：

| 字段 | 含义 |
| --- | --- |
| `candidate_id` | 本批输入 Candidate。 |
| `memory_id` | 最终归属的稳定 Memory；插入和合并都必须有值。 |
| `matched_memory_id` | 匹配到的历史 Memory；新建独立项时为空。 |
| `action` | `INSERTED`、`MERGED`、`UNCHANGED`、`SKIPPED`、`LINKED` 或 `EVICTED`。 |
| `relation` | `EXACT`、`DUPLICATE`、`RELATED`、`CONTRADICTS`、`DISTINCT` 或 `UNRESOLVED`。 |
| `match_method` | 按 Candidate ID、已存别名、结构化规则或模型得出。 |
| `considered_matches` | 送入语义判断的短名单、字符相似度、内容指纹和 Memory 状态。 |
| `confidence / reason` | 决策置信度和中文理由。 |
| `adapter / model_id` | 使用模型时的适配器和模型身份。 |
| `request_fingerprint / response_fingerprint` | 可审计但不暴露原始响应的请求/响应指纹。 |
| `error_code / attempts` | 降级原因和模型传输尝试次数。 |
| `decision_id` | 可由 Batch 中详细判断稳定回链到 Memory 关系的审计 ID。 |
| `full_store_curation` | `true` 表示周期全库复核产生，不是本批 Candidate 的首次增量决策。 |

CLI 配置模型适配器后会自动进行语义整理；未配置模型时，存在兼容短名单但规则无法
证明关系的候选会独立入库，并记录 `relation=UNRESOLVED`、
`error_code=adapter_not_configured`。这不是全批失败，而是避免猜测性合并的保守结果。

使用 `--no-memory` 时不会更新 Store，`memory_update` 通常保持默认值并从 JSON 中
省略。
同一 Candidate 在一次更新中可以先有 `INSERTED` 决策，再有一条
`EVICTED` 决策：前者表示入库判断，后者表示最终容量管理结果。淘汰决策保存被移除
的 `candidate_id`、`memory_id`、确定性理由和稳定 `decision_id`。被淘汰 Entry
不复制到另一个无限增长的 Archive Store；完整证据仍在 Batch Evaluation、逐 Dialogue
Evaluation 和 SessionLog 中。


## 10. `EvolutionMemoryStore` 顶层字段

```text
EvolutionMemoryStore
├── schema_version / store_kind / store_version
├── revision / new_entries_since_full_curation
├── applied_batch_ids[]
└── entries[]
```

| 字段 | 含义 |
| --- | --- |
| `schema_version` | Memory Protobuf 结构版本。当前为 1。 |
| `store_kind` | 固定为 `trajectory_evolution_memory`，防止误读其他 PB。 |
| `store_version` | Memory 文件与基础持久化语义版本。 |
| `new_entries_since_full_curation` | 自上次完整全库复核后的新建次数；容量淘汰不回减，以免推迟全库复核。 |
| `revision` | 已成功应用的新 Batch 次数形成的 Store 全局修订号。 |
| `applied_batch_ids` | 最近已应用 Batch 的精确去重窗口，默认最多 4096 项。 |
| `entries` | 当前有界工作集中的 `EvolutionMemoryEntry`，默认最多 512 项。 |

`revision` 不是 Candidate 数量，也不是 Entry 更新次数。每个此前未应用的 Batch 会使
它增加 1；相同 Batch 重放不增加。

即使一个新 Batch 最终没有可写入的 Candidate，只要它通过 Memory Update 并被记入
`applied_batch_ids`，Store revision 仍会增加。这表示 Store 已消费该批次，而不是
表示一定新增了策略。

### 10.1 容量与淘汰语义
Memory 不是无限历史仓库，而是可由上游产物重建的有界工作集。每批整理结束时：

1. 先把 `applied_batch_ids` 截到最近的配置窗口；
2. 若 Entry 数或序列化字节超限，按生命周期、候选状态、支持用户/Dialogue/Episode、
   置信度和最近修订选择低优先级项；
3. `VALIDATED` 比 `CANDIDATE` 更晚淘汰，`REJECTED` / `ARCHIVED` 更早淘汰；
4. `CONTRADICTS` 连通组整体保留或整体淘汰；
5. 删除后重建双向关系，最终 Protobuf 不得超过配置字节上限。

精确 Batch 幂等只保证最近窗口。旧 Batch ID 退出窗口后，只要 Candidate/来源别名仍在
Memory，稳定映射仍会阻止重复；若两者都已淘汰，旧批重放可能重新入库。
## 11. `entries[]`：一条累计记忆


每项是 `EvolutionMemoryEntry`。

| 字段 | 含义 |
| --- | --- |
| `memory_id` | 格式为 `memory:<candidate_id>`。 |
| `status` | Memory Entry 的生命周期状态。 |
| `candidate` | 当前最新、跨批次聚合后的完整 EvolutionCandidate。 |
| `first_batch_id` | 该记忆首次出现的 Batch。 |
| `last_batch_id` | 最近观察并合并该记忆的 Batch。 |
| `observed_batch_ids` | 所有观察到该记忆的 Batch ID，去重保存。 |
| `revisions` | Candidate 内容发生实质变化时保存的 Candidate 快照。 |
| `source_candidate_ids` | 所有被映射到该 Memory 的 Candidate ID；语义重复项作为别名保留。 |
| `relations` | 与其他 Memory 的 `RELATED` 或 `CONTRADICTS` 双向关系。 |

`memory_id` 由首次成为该 Entry canonical Candidate 的稳定 `candidate_id` 派生，后续
语义重复 Candidate 不会改写它，而是加入 `source_candidate_ids`。这给出稳定的
`candidate_id → memory_id` 映射；新的支持证据、反证或状态变化通常更新同一 Entry。

每条 `EvolutionMemoryRelation` 只保存 `target_memory_id`、关系类型、置信度和
`decision_id`。详细短名单、理由、模型身份与错误留在产生它的 Batch decision 中。
关系只表示候选策略之间的语义，不表示依赖执行顺序；`RELATED` 和 `CONTRADICTS` 都
不会合并两个 Entry。冲突关系具有安全优先级，不会被后续 `RELATED` 复核覆盖。

### 11.1 Memory 生命周期状态

`EvolutionMemoryStatus` 可取：

| 状态 | 含义 |
| --- | --- |
| `CANDIDATE` | 待实验验证的进化假设。 |
| `VALIDATED` | 未来经过回放/A-B 实验验证的策略。 |
| `REJECTED` | 未来经实验否决的策略。 |
| `ARCHIVED` | 不再参与当前选择；容量不足时会早于活跃候选淘汰。 |

当前管线创建新 Entry 时只会设置为 `CANDIDATE`，还没有实现自动晋升、拒绝或归档。

## 12. `revisions[]`：Entry 的实质变化历史

每一项是 `EvolutionMemoryRevision`：

| 字段 | 含义 |
| --- | --- |
| `store_revision` | 产生该快照时的 Store 全局 revision。 |
| `batch_id` | 导致该变化的 Batch。 |
| `candidate_snapshot` | 当时完整的 Candidate 状态。 |

工作集中的新 Entry 会立即写入第一个 Candidate 快照。已有 Entry 在 Candidate 聚合内容或来源
别名发生实质变化时新增快照，例如：

- 新增支持 Dialogue 或 Episode；
- 新增反证；
- 置信度发生变化；
- 从 INSUFFICIENT_SUPPORT 变成 PROPOSED；
- 从 PROPOSED 变成 CONFLICTED。

Revision 有意保持紧凑，只回放 Candidate 内容；当前来源别名和关系保存在 Entry，关系
判断的完整依据保存在 Batch `memory_update.decisions[]`。仅关系变化不会伪造 Candidate
快照。

同一新 Batch 观察到候选，但合并后内容完全相同时：

```text
last_batch_id / observed_batch_ids 更新
unchanged_entry_count += 1
不新增 revisions[] 快照
```

## 13. 两套 `status` 必须分开理解

这是 Batch 和 Memory 中最容易读错的地方。

| 位置 | 类型 | 表示什么 |
| --- | --- | --- |
| `candidate.status` | `EvolutionCandidateStatus` | 当前证据是否达到质量门、是否冲突 |
| `entry.status` | `EvolutionMemoryStatus` | 这条记忆在实验与上线流程中的生命周期 |

合法组合包括：

```text
entry.status = CANDIDATE
candidate.status = PROPOSED
```

它表示“证据已达到进入实验的门槛，但还没有通过实验验证”。

也可能是：

```text
entry.status = CANDIDATE
candidate.status = CONFLICTED
```

它表示“记忆库保留这条假设及其正反证，但当前不应直接用于策略增强”。

## 14. Batch 如何合并进 Memory

完整算法可以简化为：

```text
读取并校验 evolution-memory.pb；不存在则初始化
  │
  ▼
batch_id 是否已在 applied_batch_ids？
  ├── 是：恢复 Candidate→Memory 映射，revision 不变，不调用模型
  └── 否：逐 Candidate 自动整理
            ├── REJECTED_BY_GATE：跳过
            ├── 稳定 ID/历史别名命中：合并到 canonical Memory
            ├── 规范化策略键完全相同：确定性合并
            ├── 无结构兼容项：作为 DISTINCT 插入
            └── 有兼容短名单但规则无法判断
                  ├── 已配置模型：判断 DUPLICATE / RELATED /
                  │                 CONTRADICTS / DISTINCT
                  └── 无模型或判断失败：作为 UNRESOLVED 独立插入
  │
  ▼
新建 Entry 累加 pending 计数（后续容量淘汰不回减）
  ├── 未达到阈值：结束本次增量管理
  └── 达到阈值：同一次调用自动全库复核
        ├── 先确定性合并规范化重复项
        ├── 每条可用记忆复核最多 8 个同类型、同作用域候选
        ├── 全部完成：pending 清零
        └── 模型、预算或响应失败：保留已入库结果和 pending，后续自动重试
  │
  ▼
模型调用与容量规划都在锁外完成
  │
  ▼
容量规划
  ├── applied_batch_ids 保留最近配置窗口
  ├── Entry 数/总字节超限：按确定性优先级淘汰
  └── CONTRADICTS 连通组不得拆分
  │
  ▼
取得排他锁后重新读取 Memory
  ├── 基线已变化：基于新版本重新整理
  │                 完全相同的模型请求复用本次响应
  └── 基线未变化：revision + 1，记录 batch_id 和每项 decision
  │
  ▼
凭据扫描 → 临时文件写入 + flush + rename 原子替换 PB
  │
  ▼
同步生成 JSON 可读视图并释放锁
```

字符相似度只用于对同类型、同作用域 Memory 排序，不能自动合并。相同稳定 ID、
历史别名、规范化策略键完全一致，或高置信模型 `DUPLICATE` 才会更新同一 Entry。
模型失败、低置信度和无法判断都会独立入库，因此单个整理失败不会阻止本批其他候选。
`RELATED` 和 `CONTRADICTS` 都保留双方；冲突关系优先且不可被后续相关关系降级。未来
策略选择器必须对冲突组执行“至多激活一条”，当前记忆管理层不提前伪造激活状态。

## 15. 如何从 Memory 反查原始轨迹

建议按以下顺序调查一条记忆：

```text
EvolutionMemoryStore.entries[].memory_id
  → entry.candidate
  → support_evidence[] / contradiction_evidence[]
  → source.user_id / conversation_id / dialogue_id
  → episode_ids[] / diagnostic_ids[] / evidence_event_ids[]
  → 对应 by-user/.../*.evaluation.json
  → 对应 SessionLog JSONL 中的 event_id
```

各层承担不同职责：

| 层 | 最适合查看什么 |
| --- | --- |
| Memory | 当前有界工作集中的跨批次结论、状态和修订 |
| Batch | 某次运行如何产生/更新这些候选 |
| Dialogue Evaluation | 规则、模型、融合和 Episode 结构证据 |
| SessionLog | Prompt、Tool 参数/结果和实际事件原文 |

Memory 不复制原始 Prompt、Messages 或 Tool Payload，因此想理解具体业务上下文时，
必须沿 Event ID 回到 SessionLog。

## 16. 常见误读

### 16.1 `evaluation_succeeded_count=10` 表示十个任务都成功

错误。它只表示十个 Dialogue 都成功生成了 Evaluation。任务结果要看逐 Dialogue 的
`dialogue_assessment.task_outcome` 和 `metrics.business_status`。

### 16.2 `evaluation_failed_count=0` 表示没有 Tool 失败

错误。Tool 失败属于 Evaluation 内容，不属于文件级评测失败。

### 16.3 `model_status=SUCCEEDED` 表示模型同意规则

错误。它只表示模型响应通过校验。是否与规则冲突要看逐 Episode 的
`rule_model_conflict` 和 `fusion_reason`。

### 16.4 `meta_reflection.output_candidate_count` 是最终候选总数

错误。它只计算模型返回并通过校验的合并组。未参与合并的 Seed 会保留，最终总数看
`synthesis.candidate_count` 或 `candidates` 数组长度。

### 16.5 `candidate.status=PROPOSED` 表示策略已经生效

错误。它只表示达到了离线证据门槛。当前 `entry.status` 仍为 `CANDIDATE`，线上
Harness 不会自动读取并应用。

### 16.6 `revision=13` 表示有 13 条记忆

错误。`revision` 表示 Store 应用了多少次新的批次修订；Entry 数量看
`entries.length`。

### 16.7 删除 JSON 就删除了 Memory

错误。`.pb` 才是权威状态。JSON 是可重新生成的阅读视图，但不要在评测正在写入时
手工删除或修改任何 Memory 文件。

## 17. 最小阅读顺序

如果只想快速判断一次运行是否健康，建议依次查看：

1. Batch 顶层的 `evaluation_succeeded_count / evaluation_failed_count`；
2. `runs[].trace_integrity_valid / model_status / model_error_code`；
3. `synthesis` 的 Seed、Candidate 和各状态数量；
4. `meta_reflection.status / error_code`；
5. `memory_update.applied / idempotent_replay / revision_after`；
6. Memory 的 `entries[].candidate.status / contradiction_count`；
7. 对重要候选沿 `evidence_event_ids` 回查 Evaluation 和 SessionLog。

这能把“管线有没有成功”“模型有没有成功”“业务有没有成功”“候选能不能进入实验”
和“Memory 有没有真实更新”五个问题分开判断。
