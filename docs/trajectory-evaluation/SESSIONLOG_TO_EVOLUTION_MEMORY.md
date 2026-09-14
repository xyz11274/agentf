# 从原始 SessionLog 到进化记忆库

本文解释 `trajectory_evolution evaluate` 如何把 Harness 采集的原始 SessionLog
转化为可积累、可追溯的 `EvolutionMemoryStore`。重点不是字段逐项解释，而是每个
处理阶段的输入、动作、输出、失败语义以及数据血缘。

字段含义和内容限长细节分别参见：

- [EVALUATION_FIELDS.md](EVALUATION_FIELDS.md)
- [CONTENT_LIMITS.md](CONTENT_LIMITS.md)
- [BATCH_AND_MEMORY_FIELDS.md](BATCH_AND_MEMORY_FIELDS.md)


## 运行时配置也是可追踪输入

统一命令在读取 SessionLog 前，先从 `--config`、
`AGENTFLOW_TRAJECTORY_CONFIG` 或仓库默认位置加载 `runtime.json`，再读取其中引用的
8 份模型 Prompt。配置和 Prompt 在一次进程内形成共享只读快照；下一次命令会重新
读取文件，因此修改无需重新编译，又不会让一个长批次在中途混用两个版本。

进入 `evaluator_config_fingerprint` 的是影响 Evaluation/Evolution 语义的规范化参数、
8 份 Prompt 的内容指纹，以及实际参与相关评测阶段的模型适配器身份。原始 Prompt
正文不会复制到 Batch 或 Memory；需要审计具体请求时，必须显式使用
`--requests-out`，且该文件应作为敏感派生产物保留在 Git 之外。

配置只允许保存 API Key 的环境变量名称，不能保存 Key 值。模板占位符、相对路径、
文件大小、资源范围和未知字段会在任何模型请求之前统一校验。输出路径也不能覆盖
本次配置快照的任一源文件。完整使用规则见
[`tools/trajectory_evolution/README.md`](../../tools/trajectory_evolution/README.md)。
## 1. 先给出完整结论

目录模式的真实数据链路是：

```text
Harness + SessionLog Hook
        │
        │  每个 Dialogue 一份 JSONL 事件流
        ▼
SessionLog 文件发现、排序、去重
        │
        ▼
事件解析与轨迹完整性校验
        │
        ▼
Loop Episode 重建
        │
        ├── Coordinator Episode
        └── Skill Episode
        │
        ▼
确定性规则分析
        │
        ├── 结构证据与指标
        ├── 无效 Loop Diagnostic
        └── Workflow / Business 状态
        │
        ▼
可选的 Dialogue 级模型评测
        │
        ├── 任务是否完成
        ├── 可疑 Episode 的语义判断
        └── 编排 / 经验注入 / 停止条件反思
        │
        ▼
规则与模型融合
        │
        ▼
每个 Dialogue 一份 HybridEvaluationReport
        │
        ▼
跨 Dialogue 候选生成、去重、反证检查
        │
        ▼
可选的跨轨迹元反思
        │
        ▼
质量门：PROPOSED / INSUFFICIENT_SUPPORT / CONFLICTED / REJECTED
        │
        ▼
HybridEvaluationBatchReport
        │
        ▼
加锁、幂等合并、原子写入
        │
        ▼
EvolutionMemoryStore（Protobuf 权威文件 + JSON 可读视图）
```

当前统一管线不会要求先落盘 `analysis.log` 或 `analysis.json`。`AnalysisReport` 是
进程内中间对象，生成后直接交给 Hybrid Evaluator。真正长期保存的是：

```text
原始事实       SessionLog *.jsonl
逐对话派生结果 *.evaluation.pb / *.evaluation.json
批次派生结果   batch.evaluation.pb / batch.evaluation.json
累计进化记忆   evolution-memory.pb / evolution-memory.json
```

`session reader`、旧的 `analysis.json` 和分析器内部的模板候选都不是当前记忆库的
权威输入。

## 2. 五种核心数据对象

| 对象 | 粒度 | 作用 | 是否长期落盘 |
| --- | --- | --- | --- |
| SessionLog Event | 单事件 | 保存运行时实际发生的动作和状态 | 是，JSONL |
| Loop Episode | 单次 LLM 决策步骤 | 把分散事件还原成一次可评测步骤 | 否，作为中间结构 |
| HybridEvaluationReport | 单 Dialogue | 保存结构证据、规则判断、模型判断和反思 | 是，JSON/PB |
| HybridEvaluationBatchReport | 一批 Dialogue | 保存批次结果、候选、元反思和记忆更新结果 | 是，JSON/PB |
| EvolutionMemoryStore | 跨批次累计 | 保存候选策略、证据、状态与修订历史 | 是，PB 为权威，JSON 为视图 |

它们的数量关系通常是：

```text
N 个 SessionLog 文件
  → N 个 HybridEvaluationReport
  → 1 个 HybridEvaluationBatchReport
  → M 个 EvolutionCandidate
  → 合并进 1 个持续增长的 EvolutionMemoryStore
```

## 3. 第一步：发现并固定输入集合

入口是 `TrajectoryEvolutionPipeline::evaluate_directory()`。

程序会递归扫描输入目录，只接受扩展名为 `.jsonl` 的普通文件：

1. 跳过符号链接，避免输入树被绕出预期范围；
2. 最多接受 `max_input_files` 个文件，默认 100,000；
3. 按规范化路径排序，并删除重复路径；
4. 输入目录没有任何 JSONL 时直接失败；
5. 输出目录必须与输入树分离，防止下一批把 Evaluation 当作原始轨迹再次读取。

排序非常重要。文件系统遍历顺序不稳定，而固定排序能让相同输入更容易得到稳定的
批次身份和可复现输出。

典型输入结构是：

```text
trace_output/sessionlogs/by-user/
├── <user-id>/
│   ├── <conversation-id>/
│   │   ├── <dialogue-id>.jsonl
│   │   └── <dialogue-id>.jsonl
│   └── <conversation-id>/
│       └── <dialogue-id>.jsonl
└── <user-id>/
    └── <conversation-id>/
        └── <dialogue-id>.jsonl
```

目录只负责组织和人工查看。真正进入数据血缘的是日志头部/事件中的
`user_id + conversation_id + dialogue_id`，不能只依赖路径名称。

## 4. 第二步：解析 Event 并校验轨迹完整性

每个文件先由 `LoopEpisodeAnalyzer::analyze_file()` 读取。主要工作如下。

### 4.1 有界读取

- 单个 SessionLog 默认最大 256 MiB；
- 单行 Event 默认最大 32 MiB；
- 普通坏 JSON 会使该 Dialogue 评测失败；
- 只有文件末尾未写完的坏 JSON 行会被当作 `torn_tail_record` 警告并忽略。

最后一条规则用于容忍进程异常退出时留下的半行记录，但不会容忍文件中间的数据
损坏。

### 4.2 提取身份和基础字段

解析器从 `dialogue.header` 以及事件本身提取：

```text
user_id
conversation_id
dialogue_id
event_id
idempotency_key
producer_id
producer_sequence
timestamp_us
type / round / node_id / data
```

缺失 `event_id`、`producer_id`、正数序列或时间戳时，会生成临时回退值以便继续
诊断，同时记录完整性错误；这些回退值不代表轨迹已经可靠。

### 4.3 去重与顺序检查

`idempotency_key` 是事件去重键：

- 同键且内容完全相同：忽略后来的副本，记录 warning；
- 同键但内容不同：忽略冲突副本，记录 error；
- 没有幂等键：临时使用 `event_id`，记录 warning。

解析器还会按 `producer_id` 检查 `producer_sequence`：倒退或重复是 error，序列缺口
是 warning。不同并发 Producer 不要求全局序列连续，因此 Coordinator 和多个 Skill
分支可以并行产生事件。

### 4.4 起止与身份一致性

以下问题会使 `trace_integrity_valid=false`：

- 缺少 `dialogue.start` 或 `dialogue.end`；
- 同一文件出现多个 `dialogue_id`；
- Header 中的用户或会话身份相互冲突；
- Event、Tool 配对或 Producer 顺序存在结构性错误。

轨迹完整性失败时，报告可以被隔离保存，但默认不会送给模型，也不能生成进化候选。
`--allow-invalid-trace` 只改变是否允许保存隔离结果，不会把坏轨迹提升为训练燃料。

## 5. 第三步：把离散事件重建成 Loop Episode

原始 JSONL 是按时间追加的事件流，不天然等于“轨迹树节点”。分析器通过以下过程
恢复可评测的执行步骤。

### 5.1 先按 Producer 分支

所有事件先按 `producer_id` 分组，再在每个 Producer 内按以下顺序排序：

```text
producer_sequence → timestamp_us → event_id
```

这避免并行 Skill 的事件互相串线。每个 `llm.request` 是一个新 Episode 的起点，
直到同一 Producer 的下一个 `llm.request` 或该 Producer 事件结束。

### 5.2 确定 Episode 所有者

```text
Coordinator Episode
  owner_id = coordinator:<dialogue_id>

Skill Episode
  owner_id = exec_skill_key
  若缺失 exec_skill_key 才回退到 producer_id
```

因此两个同名 Skill 并发执行时，只要 `exec_skill_key` 不同，就会形成两条独立轨迹
分支，不会因为 `skill_name` 相同而合并。

### 5.3 拼装一次决策的完整证据

一个 Episode 会关联：

- `llm.request` 与对应的 `llm.response`；
- `loop.control` 的 continue/retry/stop/abort；
- `skill.dispatch` 与相同 `batch_id` 的 `batch.result`；
- `tool.call` 与按 `tool_call_id` 索引的 `tool.result`；
- `skill.end` 或 `dialogue.end` 终态；
- `evaluation.result`；
- 所有构成该 Episode 的 `source_event_ids`。

对 Tool Result，程序提取状态、错误类别和可重试性，但不把完整结果复制到最终
Evaluation。大内容被归一化后形成 `result_fingerprint`。

### 5.4 计算四类稳定指纹

| 指纹 | 代表什么 | 主要用途 |
| --- | --- | --- |
| `input_fingerprint` | 本轮 LLM messages | 判断输入状态是否变化 |
| `action_fingerprint` | Tool 名称/参数或 Skill 派发动作 | 判断是否重复执行相同动作 |
| `outcome_fingerprint` | 本轮具体结果 | 区分具体返回内容 |
| `outcome_class_fingerprint` | 归一化后的成功/失败类别 | 判断参数变化后是否仍停留在同类失败 |

同时保留原始 Event ID，所以“用指纹比较”和“回原日志查看具体内容”可以同时实现。

## 6. 第四步：确定性规则判断发生了什么

规则分析只使用可观察结构，不猜测用户意图。当前主要 Diagnostic 如下：

| Diagnostic | 触发含义 | 是否能直接计为无效 |
| --- | --- | --- |
| `invalid_action_repeat` | 已有终态 Batch 后，相同 Skill、Prompt、force=false 再次派发 | 是 |
| `result_not_consumed` | 上轮 Tool Result 的 `tool_call_id` 未进入下一次 LLM messages | 是 |
| `non_retryable_repeat` | 同一 Tool 的同类不可重试错误达到阈值仍继续 | 是 |
| `exact_repeat` | 动作、参数和失败类别完全重复 | 视可重试性和重复次数决定 |
| `no_state_progress` | 至少连续三次使用相同 Tool 集合并得到同类失败 | 是 |
| `state_oscillation` | 动作与结果呈 A-B-A 回摆且仍失败 | 是 |
| `max_rounds_exhausted` | Skill 耗尽轮次 | 只提示复核，不能单独证明此前轮次无效 |
| `review_required` | 有可疑行为，但确定性证据不足 | 否，留给模型判断 |

规则同时计算 Episode、Tool、Dispatch、失败率等指标，并分别聚合：

```text
workflow_status：底层 Workflow 是否正常启动、执行和闭合
business_status：用户要求的业务结果或产物是否成功
```

所以 `workflow_status=SUCCEEDED` 与 `business_status=FAILED/PARTIAL` 可以同时成立：
框架正常跑完不等于业务任务成功。

## 7. 第五步：形成单 Dialogue 的 Hybrid Evaluation

`HybridTrajectoryEvaluator::evaluate_sessionlog()` 先把进程内 `AnalysisReport` 转成
确定性 `HybridEvaluationReport`，再根据参数决定是否调用模型。

### 7.1 模型选择模式

| 模式 | 行为 |
| --- | --- |
| `none` | 不调用模型，只保存规则结果 |
| `suspicious` | 选择规则命中、结果失败、Dispatch partial/failed、tool_error 或 max_rounds 的 Episode |
| `all` | 选择全部 Episode |

无论 `suspicious` 还是 `all`，每个 Dialogue 最多发起一次模型请求。这一次请求同时
要求模型返回：

1. 整个用户任务的 `task_outcome`；
2. 所有被选 Episode 的语义判断；
3. Dialogue 级根因和进化反思。

### 7.2 模型能看到什么

模型输入由两部分组成：

```text
结构证据：Episode、Tool、Dispatch、Diagnostic、相邻步骤和指纹
具体内容：经过筛选、递归脱敏和限长的真实 SessionLog Event
```

因此模型并非只看到指纹。它可以理解用户要求、Skill Prompt、模型回复、Tool
参数/结果和最终回复；但最终 Evaluation 只保存验证后的结论与证据 ID，不复制这些
原始内容。

进入模型前会：

- 删除重复的 Provider 原始副本和 Artifact 大字段；
- 只保留上下文事件及与 Episode 有关的事件；
- 对字符串、数组、对象和递归深度限长；
- 脱敏显式 Secret、常见厂商 Key、Bearer/JWT、密码和本地路径；
- 把日志内所有自然语言声明为“不可信证据”，防止日志中的 Prompt Injection 被
  当成评测指令。

### 7.3 模型必须输出什么

模型响应必须是单个 JSON 对象，包含：

```text
task_outcome
episode_assessments[]
reflection
```

校验器要求 Episode 数量和 ID 精确匹配、置信度合法、中文理由存在，并且每个
`evidence_event_id` 都来自本次请求允许引用的真实 Event。凭据形态、未知 Episode、
伪造 Event、超长字段或多余/缺失判断都会使整份模型响应失效。

原始模型响应不会落入 Evaluation 或 Memory，只保存请求/响应指纹以及通过校验的
结构化字段。

### 7.4 规则与模型如何融合

融合优先级不是简单让模型覆盖规则：

```text
确定性 rule=INVALID
  → 最终仍为 INVALID
  → 模型只能表示同意或形成 rule_model_conflict，不能推翻

规则没有明确结论 + 模型有效
  → 使用模型 verdict

模型失败或输出非法
  → 丢弃全部模型语义字段
  → 保留规则结果并标记 degraded
```

最终每个 Episode 得到 `final_verdict`、`decision_source`、`final_confidence` 和
`fusion_reason`。整个 Dialogue 得到 `dialogue_assessment` 和 `reflection`。

## 8. 第六步：逐文件失败隔离并形成批次

目录模式按稳定顺序逐个评测文件。单个 Dialogue 的解析或评测异常会写成失败的
`EvaluationRunSummary`，不会终止其他 Dialogue。

每个成功 Dialogue 保存：

```text
source identity
source_fingerprint
evaluation_fingerprint
trace_integrity_valid
model_status / model_error_code
episode_count / invalid_episode_count
```

每个失败 Dialogue 保存输入身份指纹和经过脱敏、限长的错误信息。只有整批没有任何
成功 Evaluation 时，命令才以失败退出码结束。

## 9. 第七步：从 Evaluation 生成进化候选

`TrajectoryEvolutionPipeline::synthesize_reports()` 只接收成功生成的
`HybridEvaluationReport`。轨迹完整性失败的报告进入 quarantine，不参与候选生成。

### 9.1 候选的两个来源

确定性 Seed 来自 `counts_as_invalid=true` 的 Diagnostic：

| Diagnostic | Candidate Target |
| --- | --- |
| `invalid_action_repeat` | `SKILL_ORCHESTRATION` |
| `result_not_consumed` | `TOOL_RESULT_CONSUMPTION` |
| `non_retryable_repeat` | `RETRY_POLICY` |
| `exact_repeat` / `no_state_progress` / `state_oscillation` | `LOOP_STOP_CONDITION` |

模型 Seed 只来自已经完整校验成功的 Dialogue Reflection：

```text
skill_orchestration_changes → SKILL_ORCHESTRATION
experience_injections       → EXPERIENCE_INJECTION
stop_condition_changes      → LOOP_STOP_CONDITION
```

模型失败时不会产生 Reflection Seed；规则 Seed 不受影响。

### 9.2 候选去重键

候选不是按自然语言全文随意合并，而是通过稳定键归并：

```text
target_type
+ generator_version
+ normalized target_scope
+ normalized trigger_pattern
+ proposal_fingerprint
```

`candidate_id` 是该键的 SHA-256。相同候选在不同用户、会话和 Dialogue 中出现时，
证据会合并到同一个候选，但来源身份仍分别保留。

### 9.3 反证检查

对于带 `error_class` 的候选，程序会检查相同 scope 和相同错误类别下是否存在最终
结论为 `USEFUL` 或 `NECESSARY_RETRY` 的 Episode。存在时加入
`contradiction_evidence`。

这一步防止把“某次重试确实有必要”的场景错误推广为“遇到这种错误永远立刻停止”。

## 10. 第八步：跨轨迹元反思

启用 `--meta-reflection` 后，批次最多额外调用一次模型。它只能对已有 Seed 做语义
归并和文字提炼，不能创造新的证据或直接激活策略。

每个 Seed 带有：

```text
seed_id
target_type
target_scope
compatibility_group
问题、建议、收益
支持数量
真实 evidence_event_ids
```

程序还提供 `allowed_seed_groups` 白名单。模型返回的每个合并组必须是其中某一个组
的子集，即 `target_type + target_scope` 必须完全一致。校验还要求：

- Seed ID 必须存在且不能重复使用；
- Event ID 必须同时属于被选 Seed；
- 文本必须为有界中文；
- 置信度必须在 `[0, 1]`；
- 不得包含凭据。

元反思响应非法时会被原子拒绝：不采用任何半成品合并，原始候选继续进入质量门和
记忆库。元反思失败不会让规则评测和整批业务失败。

## 11. 第九步：质量门决定候选状态

每个候选重新统计独立 User、Conversation、Dialogue、Episode、无效 Episode 和
反证数量。默认门槛为：

```text
support_dialogue_count >= 2
support_units          >= 2
```

`support_units` 通常等于去重后的 Episode 数；如果模型 Reflection 没有给出
`wasted_episode_ids`，则回退为支持证据条数，避免“有跨 Dialogue 反思证据但没有明确
无效 Episode”的候选永远无法过门。

当前置信度大致由以下方式计算：

```text
confidence
  = 平均证据置信度 × 0.7
  + min(Dialogue 支持比例, Episode 支持比例) × 0.3

若存在反证，再乘 0.6
```

如果候选经过已校验的元反思合并，最终置信度取上述证据计算值与模型给出的元反思
置信度中的较大值；`meta_reflection_applied` 和 `source_seed_ids` 用于标明这一来源。

候选状态含义：

| 状态 | 条件 | 是否进入 Memory |
| --- | --- | --- |
| `PROPOSED` | 达到 Dialogue/support-units 门槛且无反证 | 是 |
| `INSUFFICIENT_SUPPORT` | 有真实证据，但支持量不足 | 是 |
| `CONFLICTED` | 存在有效步骤或必要重试的反证 | 是 |
| `REJECTED_BY_GATE` | 没有可回链支持证据 | 否 |

前三种都进入 Memory，是为了跨批次继续积累证据，而不是表示它们已能上线。

## 12. 第十步：生成稳定 Batch 身份

`batch_id` 不只是输入文件列表的哈希，它覆盖：

- 管线版本、候选生成器版本和 Evaluator 配置；
- 模型 Adapter 与模型 ID；
- 每个成功 Evaluation 的完整结果指纹；
- 每个失败输入的身份和脱敏错误；
- 最终候选内容；
- 元反思状态和结果指纹。

因此同一 SessionLog 使用不同模型、规则版本或得到不同反思时，会形成不同 Batch；
完全相同的结果重放则得到相同 `batch_id`。

## 13. 第十一步：合并并原子写入 EvolutionMemoryStore

CLI 在得到 Batch 后统一调用 `curate_memory_file()`：配置模型适配器时自动执行语义
整理，未配置时保守地完成确定性入库。原有确定性接口仍保留给库调用方兼容使用。

### 13.1 先加跨进程锁

CLI 整理先读取基线并在锁外完成需要的模型调用，提交时再在
`evolution-memory.pb.lock` 上取得排他锁、重新读取并比较基线；若其他进程已写入，则
基于新版本有限次重算。这既避免远程模型调用长期占锁，也避免两个进程互相覆盖
Memory。重算时完全相同的模型请求复用本次调用内的响应；缓存不落盘、不跨批次，真实
模型调用计数和预算不会被同一次并发重试重复消耗。

### 13.2 加载或初始化

已有 `evolution-memory.pb` 时，程序校验：

```text
schema_version
store_kind = trajectory_evolution_memory
store_version
文件大小与 Protobuf 可解析性
```

文件不存在时初始化空 Store。二进制 Protobuf 是权威状态，JSON 不是反向输入。

### 13.3 Batch 级幂等

如果 `batch_id` 已存在于 `applied_batch_ids`：

```text
applied = false
idempotent_replay = true
revision 不增加
```

这保证同一批结果重复执行不会重复增加证据或修订。

### 13.4 Candidate 级合并

`REJECTED_BY_GATE` 被跳过。其他候选先按稳定 `candidate_id` 和历史来源别名
查找；未命中时，只对相同 `target_type` 和规范化 `target_scope` 的历史项建立短名单。
字符 3-gram 相似度仅负责短名单排序，不会自动合并。

规范化后的类型、作用域、触发条件和修改建议完全相同时，确定性合并。配置模型适配器
后，其他模糊项自动由模型判断 `DUPLICATE`、`RELATED`、`CONTRADICTS` 或
`DISTINCT`；没有模型、请求失败、越权引用或低置信结果都按 `UNRESOLVED` 保守独立
入库。高置信 `DUPLICATE` 合并到既有 canonical Memory；`RELATED` 和
`CONTRADICTS` 建立双向关系但保留两个 Entry。已确认冲突不会被后续 `RELATED`
判断覆盖。

新 Entry 使用首次 Candidate 派生的稳定 ID：

```text
memory_id = memory:<canonical_candidate_id>
```

语义重复 Candidate 不改写该 ID，而是加入 `source_candidate_ids`。每个输入 Candidate
都在 `memory_update.decisions[]` 中保存最终 Memory 映射、动作、关系、匹配方法、
短名单、置信度、理由、模型指纹和错误码。Memory Entry 只保留当前 Candidate、来源
别名及紧凑关系；Revision 只保存 Candidate 快照。关系上的 `decision_id` 可回链到 Batch
中的详细判断。

### 13.5 自动周期全库整理

每次真正新建 Entry 都累加 `new_entries_since_full_curation`；后续容量淘汰不回减该值，
以免延迟复核。默认累计 20 次时，同一次 `evaluate` 自动开始全库复核；可用 `--memory-full-curation-threshold N` 调整：

1. 先按规范化策略键确定性合并重复项，并保留所有来源 Candidate ID 作为别名；
2. 对每条 `CANDIDATE` 或 `VALIDATED` 记忆复核最多 8 个同类型、同作用域候选；
3. `DUPLICATE` 合并，`RELATED` / `CONTRADICTS` 建立双向关系，`DISTINCT` 保持独立；
4. 全部完成后把待复核计数清零。

若全库只有互不兼容的独立项，没有语义比较请求，即使未配置模型也可以完整结束。若有
必要比较但模型、预算或响应不可用，本批已经完成的增量入库仍正常提交，
`full_curation_completed=false`，待复核计数保持不变，后续新 Batch 自动重试。全库决策
写入同一 `memory_update.decisions[]`，以 `full_store_curation=true` 区分。

### 13.6 有界工作集与容量淘汰

全库整理之后、提交之前，同一事务执行确定性容量管理：

- `--memory-max-entries N` 控制工作集 Entry 上限，默认 512；
- `--memory-replay-window N` 控制最近 Batch 的精确幂等窗口，默认 4096；
- 总 Protobuf 继续受 256 MiB 字节硬上限保护；超限时不再直接放弃本批，而是先淘汰
  低优先级 Entry。

淘汰顺序依次比较 Memory 生命周期、Candidate 状态、支持用户/Dialogue/Episode 数、
置信度和最近修订；`VALIDATED` 最后淘汰。由 `CONTRADICTS` 连接的记忆作为一个整体
保留或淘汰，删除后重建双向关系。每个删除动作以
`MemoryUpdateDecision.action=EVICTED` 写入本批，原始证据仍可从 Batch Evaluation 和
SessionLog 重建。

Batch ID 退出精确窗口后，不再承诺 Batch 级永久幂等；若 Candidate/别名仍在工作集，
稳定映射仍会阻止重复。若二者都已淘汰，旧批重放可能重新进入工作集。

### 13.7 原子持久化

更新顺序是：

1. 对待保存 Protobuf 做凭据扫描；
2. 写入同目录临时文件并 flush；
3. 使用 rename 原子替换 `evolution-memory.pb`；
4. 同步生成 `evolution-memory.json` 可读视图；
5. 释放文件锁。

JSON 视图写入失败不会否定已经成功写入的 Protobuf，但会在 `MemoryUpdateResult` 中
记录 `memory_json_view_write_failed`。

Memory 中的 Entry 当前始终以 `EVOLUTION_MEMORY_STATUS_CANDIDATE` 创建。管线不会
自动把它提升为 `VALIDATED`，更不会直接把经验注入在线上下文或修改 Skill 编排与
Loop 条件。

## 14. 最终落盘目录及写入顺序

默认目录结构：

```text
<output-root>/
├── by-user/
│   └── <user>/<conversation>/<dialogue>.evaluation.json
│   └── <user>/<conversation>/<dialogue>.evaluation.pb
├── runs/
│   └── <batch-id>/
│       ├── batch.evaluation.json
│       └── batch.evaluation.pb
├── memory/
│   ├── evolution-memory.pb
│   ├── evolution-memory.json
│   └── evolution-memory.pb.lock
└── model-requests.redacted.jsonl       # 仅显式 --requests-out 时生成
```

CLI 实际写入顺序是：

```text
逐 Dialogue Evaluation
  → 更新 Memory
  → 把 MemoryUpdateResult 写入 Batch
  → Batch Evaluation
  → 可选的脱敏模型请求日志
```

`model-requests.redacted.jsonl` 仍包含经过脱敏的真实任务内容，是敏感派生数据，只用于
离线复核，不应提交到 Git。

## 15. 一条证据如何从 Event 一直回到 Memory

以 `video-gen` 连续遇到 `unsupported_model` 为例：

```text
tool.call / tool.result Event
        │ tool_call_id + event_id
        ▼
Skill Loop Episode
        │ error_class=unsupported_model
        │ retryability=non_retryable
        ▼
non_retryable_repeat Diagnostic
        │ counts_as_invalid=true
        ▼
EpisodeAssessment.rule=INVALID
        │ 硬规则不可被模型推翻
        ▼
RETRY_POLICY EvolutionCandidate
        │ support_evidence 保存 user/conversation/dialogue/
        │ episode/diagnostic/event ID
        ▼
质量门与反证检查
        │
        ▼
EvolutionMemoryEntry
```

排查一条 Memory 建议时，应反向沿以下链路核验：

```text
memory_id
  → candidate_id
  → support_evidence[]
  → SourceIdentity + episode_ids + diagnostic_ids + evidence_event_ids
  → 对应 *.evaluation.json
  → 原始 SessionLog 中的 Event
```

这条反向链路是后续回放、建图、候选晋升和自动策略实验的基础。

## 16. 各类失败如何影响最终记忆

| 失败位置 | 当前行为 | 是否影响其他 Dialogue | 是否产生候选 |
| --- | --- | --- | --- |
| 文件无法读取/普通 JSON 损坏 | 当前 Dialogue 记为 evaluation failure | 否 | 否 |
| 末尾半行 JSON | 忽略半行并记录 warning | 否 | 视其余轨迹而定 |
| Trace 完整性失败 | 隔离，默认跳过模型 | 否 | 否 |
| Dialogue 模型网络失败 | 保留规则结果，标记 degraded | 否 | 仅规则候选 |
| Dialogue 模型响应非法 | 原子丢弃该响应 | 否 | 仅规则候选 |
| 元反思失败 | 保留元反思前候选 | 否 | 是，使用原候选 |
| 记忆语义判断失败/低置信 | 该候选保守独立入库并记录错误码 | 否 | 是，不猜测性合并 |
| 周期全库复核未完成 | 保留本批增量入库和 pending 计数，后续重试 | 否 | 已产生候选不丢失 |
| 某候选证据不足 | 标为 INSUFFICIENT_SUPPORT | 否 | 进入 Memory 继续积累 |
| 某候选存在反证 | 标为 CONFLICTED | 否 | 进入 Memory 保存冲突 |
| 相同 Batch 重放 | 幂等返回，不增加 revision | 否 | 不重复写入 |
| Memory Protobuf 写入失败 | 整体持久化失败 | 不适用 | 不产生半写文件 |
| Memory JSON 视图失败 | PB 保持权威，记录错误码 | 不适用 | 已写入 PB |

## 17. 一条命令完成整个过程

纯规则模式：

```bash
build/trajectory-evolution/trajectory_evolution evaluate \
  trace_output/sessionlogs/by-user \
  --output-root trace_output/evolution \
  --model-selection none
```

规则、Dialogue 模型评测、元反思和自动记忆语义整理全部启用：

```bash
export EVOLUTION_API_KEY='从安全环境注入，不要写入命令历史或仓库'

build/trajectory-evolution/trajectory_evolution evaluate \
  trace_output/sessionlogs/by-user \
  --output-root trace_output/evolution-model \
  --model-selection suspicious \
  --meta-reflection \
  --memory-full-curation-threshold 20 \
  --model-endpoint 'https://provider.example/v1/chat/completions' \
  --model-id 'MODEL_ID' \
  --api-key-env EVOLUTION_API_KEY \
  --thinking-mode disabled \
  --timeout-ms 1800000 \
  --max-model-calls 512 \
  --max-output-tokens 16384
```

模型模式并不取代规则模式。它是在确定性事实之上补充任务语义和改进建议；模型不可用
时，规则链路和 Memory 仍可继续工作。

## 18. 当前边界与下一步闭环

当前管线已经完成：

```text
日志 → 清洗/结构化 → 规则与模型评测 → 反思 → 候选记忆库
```

它还没有完成：

```text
候选选择
  → 生成 Skill 编排 / 经验注入 / Loop 停止策略变体
  → 隔离回放或 A/B 实验
  → 比较任务成功率、无效 Loop、Token、时延和安全指标
  → 将通过门槛的候选提升为 VALIDATED
  → 受控注入下一轮策略输入
```

因此 `EvolutionMemoryStore` 是“可验证的进化假设库”，不是已经生效的在线策略库。
后续自进化必须保留候选、实验变体、对照结果和晋升决策之间的数据血缘。
