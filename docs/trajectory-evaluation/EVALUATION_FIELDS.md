# AgentFlow Trajectory Evaluation 字段解读

本文解释 `trajectory_hybrid_evaluator` 直接评测 SessionLog 后生成的
`*.evaluation.json` 与 `*.evaluation.pb`。字段契约以
`tools/trajectory_hybrid_evaluator/proto/hybrid_evaluation.proto` 为准。

最终 Evaluation 不保存原始 Prompt、Messages、Tool 参数或 Tool 结果，而是保存：

1. 可回指 SessionLog Event 的结构化证据；
2. 确定性规则判断；
3. 经过严格校验的模型判断；
4. 规则与模型的融合结论；
5. Dialogue 级任务结论与进化反思。

## 1. 整体层次

```text
HybridEvaluationReport
├── source / source_fingerprint       输入身份与数据血缘
├── metrics                           确定性统计
├── assessments[]                     每个 Loop Episode 的评测
│   ├── evidence                      结构化事实
│   ├── rule                          确定性规则判断
│   ├── model                         模型语义判断
│   └── final_*                       融合后的最终结论
├── dialogue_assessment               整个用户任务的模型结论
├── reflection                        面向进化的候选建议
├── issues                            报告级异常
└── model/content metadata            模型调用与内容处理元数据
```

应始终区分以下三层：

| 层级 | 主要字段 | 回答的问题 |
| --- | --- | --- |
| 确定性层 | `metrics`、`rule` | 轨迹中客观发生了什么？ |
| Episode 层 | `model`、`final_verdict` | 每一轮是否产生了必要状态变化？ |
| Dialogue 层 | `dialogue_assessment`、`reflection` | 整个用户任务是否完成，应该如何改进？ |

## 2. Protobuf JSON 表示约定

JSON 与二进制 Protobuf 表达同一份报告，但 Protobuf JSON 有两个容易误解的特征。

### 2.1 `uint64` 会写成字符串

例如：

```json
{
  "episode_count": "9",
  "model_requested_count": "1"
}
```

这是为了避免 JavaScript 丢失 64 位整数精度，不是字段类型错误。

### 2.2 默认值通常不写入 JSON

| 缺失字段类型 | 通常代表的默认值 |
| --- | --- |
| `uint32/uint64` | `0` |
| `double` | `0.0` |
| `bool` | `false` |
| `string` | 空字符串 |
| `repeated` | 空数组 |
| `enum` | 枚举中的 `UNSPECIFIED` |

因此没有出现 `model_failed_count` 通常表示它是 `0`；没有出现
`rule_model_conflict` 通常表示它是 `false`，不应直接判定为采集缺失。

## 3. 顶层身份与版本字段

| 字段 | 含义 |
| --- | --- |
| `schema_version` | Evaluation 数据结构版本，用于兼容未来字段变化。 |
| `evaluation_kind` | 评测类型。SessionLog 直连模式当前为 `content_aware_dialogue_evaluation`。 |
| `evaluator_version` | Hybrid Evaluator 程序版本。 |
| `deterministic_rule_set_version` | 规则判断与规则/模型融合版本。 |
| `source_analyzer_version` | SessionLog 到 Loop Episode 的分析器版本。 |
| `source_rule_set_version` | 上游无效 Loop 诊断规则版本。 |
| `source_fingerprint` | 完整输入 SessionLog 的 SHA-256。输入任意变化都会改变。 |
| `source` | `user_id`、`conversation_id`、`dialogue_id`。 |
| `source_trace_integrity_valid` | 输入轨迹是否通过结构完整性校验。 |

`source_fingerprint` 可用于：

- 判断 Evaluation 是否仍对应当前 SessionLog；
- 避免重复评测；
- 建立 `SessionLog -> Evaluation` 数据血缘；
- 在进化记忆中识别相同输入版本。

## 4. `metrics`：确定性统计

`metrics` 完全由程序计算，不依赖模型。

### 4.1 Loop 数量

| 字段 | 含义 |
| --- | --- |
| `episode_count` | Coordinator 与所有 Skill 的 Loop Episode 总数。 |
| `coordinator_episode_count` | Coordinator 决策 Episode 数。 |
| `skill_episode_count` | Skill 内部执行 Episode 数。 |
| `continued_episode_count` | 选择继续当前 Loop 的 Episode 数。 |
| `terminal_episode_count` | 结束当前 Coordinator 或 Skill Loop 的 Episode 数。 |
| `outcome_failed_episode_count` | 客观结果被标记为失败的 Episode 数。 |

### 4.2 规则分类数量

| 字段 | 含义 |
| --- | --- |
| `invalid_episode_count` | 确定性规则已经证明无效的 Episode 数。 |
| `review_episode_count` | 规则发现失败或可疑行为，需要语义复核的数量。 |
| `unassessed_episode_count` | 规则没有足够证据作语义判断的数量。 |

这里有一个关键边界：

> `invalid_episode_count` 是确定性无效 Loop 的下界，不是模型融合后的最终无效
> Episode 总数。

模型后续新增的 `INVALID` 不会反向修改 `metrics`。需要统计融合结果时，应遍历：

```text
assessments[].final_verdict
```

同理，`UNASSESSED` 只表示规则不知道，不表示该步骤已经被证明为 `USEFUL`。

### 4.3 Tool 与 Dispatch

| 字段 | 含义 |
| --- | --- |
| `tool_call_count` | Tool 调用总数。 |
| `failed_tool_call_count` | 状态为 error、failed、timeout、cancelled 等的调用数。 |
| `non_retryable_failed_tool_call_count` | 明确标记为不可重试的失败调用数。 |
| `dispatch_batch_count` | Coordinator 派发的 Skill 批次数。 |
| `partial_dispatch_batch_count` | 部分 Skill 成功、部分失败的批次数。 |
| `failed_dispatch_batch_count` | 整批失败的数量。 |

### 4.4 原始轨迹中的 Evaluation 事件

| 字段 | 含义 |
| --- | --- |
| `evaluation_count` | SessionLog 中已记录的 Evaluation Event 数量。 |
| `passed_evaluation_count` | 明确 `passed=true` 的 Evaluation 数。 |
| `failed_evaluation_count` | 明确 `passed=false` 的 Evaluation 数。 |

这里的 `evaluation_count` 不是输出了多少份 `evaluation.json`，也不是模型调用数。

### 4.5 比率

```text
invalid_loop_rate
  = invalid_episode_count / episode_count

failed_outcome_rate
  = outcome_failed_episode_count / episode_count

failed_tool_call_rate
  = failed_tool_call_count / tool_call_count
```

分母为零时结果为 `0.0`。

### 4.6 Workflow 与 Business 状态

| 字段 | 判断对象 |
| --- | --- |
| `workflow_status` | Harness/Workflow 是否正常执行和收敛。 |
| `business_status` | 用户要求的业务产物是否成功。 |

`workflow_status` 主要根据 Dispatch 的 `workflow_status` 聚合；
`business_status` 根据 Tool、Skill 终态、Dispatch 结果和 Evaluation Event 聚合。

因此下面的组合完全合法：

```json
{
  "workflow_status": "AGGREGATE_STATUS_SUCCEEDED",
  "business_status": "AGGREGATE_STATUS_PARTIAL"
}
```

它表示 Harness 正常完成，但用户任务只有部分产物成功。业务失败不应自动污染工作流
生命周期状态。

聚合状态包括：

- `AGGREGATE_STATUS_UNKNOWN`
- `AGGREGATE_STATUS_SUCCEEDED`
- `AGGREGATE_STATUS_FAILED`
- `AGGREGATE_STATUS_PARTIAL`

## 5. `assessments[]`：每个 Loop Episode 的评测

数组中的每一项都是一个 `EpisodeAssessment`：

```text
EpisodeAssessment
├── evidence
├── rule
├── model
├── final_verdict
├── decision_source
├── final_confidence
├── rule_model_conflict
└── fusion_reason
```

### 5.1 `evidence`：Episode 证据包

#### 身份与顺序

| 字段 | 含义 |
| --- | --- |
| `packet_id` | 当前完整证据包的 SHA-256 派生 ID。 |
| `episode_id` | Loop Episode 的稳定 ID。 |
| `scope` | `coordinator` 或 `skill`。 |
| `owner_id` | 当前 Loop 所属的 Coordinator 或 Dispatch 实例。 |
| `skill_name` | Skill Episode 对应的技能名。 |
| `activation_index` | Coordinator 第几次被激活。 |
| `round` | 当前 Loop 轮次。 |
| `attempt_index` | Analyzer 归一化出的同轮尝试序号。 |
| `step_index` | 同一 owner 下的顺序编号。 |

#### 控制动作

| 字段 | 含义 |
| --- | --- |
| `action` | `continue` 或 `stop`。 |
| `reason` | 动作原因，如 `tool_calls_requested`、`final_response`。 |
| `continued` | 是否继续当前 Loop。 |
| `terminal` | 是否结束当前 Loop。 |
| `terminal_status` | `completed`、`tool_error`、`llm_error`、`max_rounds_reached` 等。 |

#### 结果状态

| 字段 | 含义 |
| --- | --- |
| `outcome_failed` | 当前 Episode 是否产生失败业务结果。 |
| `all_tool_results_failed` | 本轮 Tool 结果是否全部失败。 |
| `has_non_retryable_failure` | 是否出现明确不可重试失败。 |

#### 指纹

| 字段 | 含义 |
| --- | --- |
| `input_fingerprint` | 本轮归一化输入哈希。 |
| `action_fingerprint` | 本轮动作哈希。 |
| `outcome_fingerprint` | 本轮具体结果哈希。 |
| `outcome_class_fingerprint` | 归一化结果类别哈希，用于识别文本不同但类型相同的重复结果。 |

例如连续多轮得到不同 Request ID 的 `UnsupportedModel` 时，具体
`outcome_fingerprint` 可以不同，但归一化后的 `outcome_class_fingerprint` 可保持
一致，从而用于重复失败诊断。

### 5.2 `dispatch`

通常只在包含 Skill 派发行为的 Coordinator Episode 中有内容。

| 字段 | 含义 |
| --- | --- |
| `batch_id` | 本次 Skill 派发批次 ID。 |
| `skill_names` | 请求派发的 Skill 名称。 |
| `force` | 各 Skill 是否显式强制重启，与 `skill_names` 按位置对应。 |
| `batch_status` | `completed`、`partial` 或 `failed`。 |
| `workflow_status` | 底层 Workflow 执行状态。 |
| `successful_count` | 成功 Skill 数。 |
| `total_count` | 总 Skill 数。 |

### 5.3 `tools[]`

| 字段 | 含义 |
| --- | --- |
| `tool_call_id` | LLM 产生的 Tool Call ID。 |
| `tool_name` | 工具名称。 |
| `status` | completed、error、timeout 等。 |
| `error_class` | 归一化错误类型，如 `unsupported_model`。 |
| `retryability` | `retryable`、`non_retryable` 或 `not_applicable`。 |
| `arguments_fingerprint` | Tool 参数哈希，不保存原始敏感参数。 |
| `result_fingerprint` | Tool 结果哈希。 |
| `call_event_id` | 对应 SessionLog 的 `tool.call` Event ID。 |
| `result_event_id` | 对应 SessionLog 的 `tool.result` Event ID。 |

### 5.4 `diagnostics[]`

| 字段 | 含义 |
| --- | --- |
| `diagnostic_id` | 稳定诊断 ID。 |
| `category` | 异常类别。 |
| `counts_as_invalid` | 是否能直接计入确定性无效 Loop。 |
| `confidence` | 规则置信度。 |
| `severity` | 严重程度。 |
| `summary` | 诊断摘要。 |
| `observable_cause` | 从轨迹观察到的诱因。 |
| `tool_name` | 相关 Tool。 |
| `error_class` | 相关错误类别。 |
| `evidence_event_ids` | 支撑诊断的原始 Event。 |

### 5.5 `evaluations[]`

这是原始 SessionLog 中挂载到当前 Episode 的外部评测证据：

| 字段 | 含义 |
| --- | --- |
| `event_id` | Evaluation Event ID。 |
| `evaluator` / `evaluator_version` | 评测器身份和版本。 |
| `metric` | 指标名。 |
| `score` / `has_score` | 是否有数值分数及其值。 |
| `passed` / `has_passed` | 是否有明确通过判断及其值。 |

`has_score` 和 `has_passed` 用于区分“值为 0/false”和“原始记录没有该字段”。

### 5.6 前后相邻步骤

`previous_same_owner` 与 `next_same_owner` 描述同一 Coordinator 或同一 Skill 的
相邻 Episode，包含：

- `episode_id`
- `step_index`
- `action`
- `terminal_status`
- `outcome_failed`
- `outcome_class_fingerprint`

它们是重建 Loop 链、检测重复状态和学习停止条件的重要关系。

### 5.7 `source_event_ids`

列出组成当前 Episode 的原始 SessionLog Event ID。它是从 Evaluation 回放到原始
轨迹的主要 Join Key。

## 6. `rule`：确定性规则判断

| 字段 | 含义 |
| --- | --- |
| `verdict` | 规则判断。 |
| `confidence` | 规则置信度。 |
| `diagnostic_ids` | 命中的诊断 ID。 |
| `evidence_event_ids` | 支撑规则结论的 Event。 |
| `explanation` | 规则说明。 |

当前规则层主要产生：

- `SEMANTIC_VERDICT_INVALID`：确定性证据已经证明是无效 Loop；
- `SEMANTIC_VERDICT_UNCERTAIN`：存在失败或可疑行为，需要语义复核；
- `SEMANTIC_VERDICT_UNASSESSED`：规则没有足够证据。

规则层当前不会因为“没有异常”就直接证明某轮 `USEFUL`。

## 7. `model`：Episode 级模型判断

| 字段 | 含义 |
| --- | --- |
| `status` | 模型调用及校验状态。 |
| `adapter` | Adapter 名称，如 `openai-compatible-http`。 |
| `model_id` | 实际评测模型。 |
| `request_fingerprint` | 脱敏后模型请求的 SHA-256。 |
| `response_fingerprint` | 原始模型响应的 SHA-256。 |
| `verdict` | 模型对当前 Episode 的语义判断。 |
| `confidence` | 模型置信度。 |
| `expected_state_change` | 当前轮理论上应产生的状态变化。 |
| `observed_state_change` | 轨迹中实际观察到的状态变化。 |
| `concise_reason` | 模型判断理由。 |
| `evidence_event_ids` | 模型引用的真实 Event ID。 |
| `error_code` | 失败时的稳定错误码。 |
| `attempts` | HTTP 或回放 Adapter 的实际尝试次数。 |

### 7.1 Model Call Status

| 状态 | 含义 |
| --- | --- |
| `NOT_REQUESTED` | 当前运行没有请求模型。 |
| `SKIPPED` | 选择了模型评测，但因无效 Trace、请求过大、无 Adapter 等原因跳过。 |
| `SUCCEEDED` | 模型响应通过了完整 Schema、证据引用和安全校验。 |
| `TRANSPORT_ERROR` | 网络、超时或 HTTP 传输失败。 |
| `INVALID_RESPONSE` | 收到响应，但格式、数量或证据引用不合法。 |

直连模式对一个 Dialogue 只调用一次模型，一次响应同时返回任务结论、多个 Episode
判断和反思。因此同一 Dialogue 中不同 Episode 的 `request_fingerprint` 和
`response_fingerprint` 相同是正常现象，不代表重复调用。

## 8. 最终融合字段

| 字段 | 含义 |
| --- | --- |
| `final_verdict` | 规则与模型融合后的最终 Episode 结论。 |
| `decision_source` | 最终结论来自规则、模型或二者。 |
| `final_confidence` | 最终置信度。 |
| `rule_model_conflict` | 模型是否与不可覆盖的确定性规则冲突。 |
| `fusion_reason` | 为什么采用当前结论。 |

### 8.1 Final Verdict

| Verdict | 含义 |
| --- | --- |
| `USEFUL` | 当前轮产生了必要的状态变化。 |
| `INVALID` | 当前轮冗余、重复或确定无效。 |
| `NECESSARY_RETRY` | 前一步失败，但本次重试在语义上合理。 |
| `PREMATURE_STOP` | 在任务尚未合理收敛时过早停止。 |
| `UNCERTAIN` | 证据仍不足。 |
| `UNASSESSED` | 规则和模型均未形成判断。 |

### 8.2 Decision Source

| 来源 | 含义 |
| --- | --- |
| `NONE` | 没有可用的规则或模型语义结论。 |
| `RULE` | 只采用规则结论。 |
| `MODEL` | 规则未判断，采用模型结论。 |
| `HYBRID` | 规则筛选/硬约束与模型语义共同形成结论。 |

### 8.3 融合优先级

1. 规则已经判定 `INVALID` 时，模型不能推翻；
2. 规则 `INVALID` 且模型同意，来源记为 `HYBRID`；
3. 规则 `INVALID` 但模型给出相反的非 `UNCERTAIN` 判断，仍保持 `INVALID`，并设置
   `rule_model_conflict=true`；
4. 规则 `UNASSESSED`、模型成功时，采用模型，来源为 `MODEL`；
5. 规则 `UNCERTAIN`、模型成功时，采用模型，来源为 `HYBRID`；
6. 模型不可用时，保留规则结论。

## 9. `dialogue_assessment`：整个用户任务的结论

| 字段 | 含义 |
| --- | --- |
| `status` | 对话级模型评测状态。 |
| `adapter` / `model_id` | 评测 Adapter 与模型。 |
| `request_fingerprint` / `response_fingerprint` | 请求和响应哈希。 |
| `task_outcome` | 整体任务 `SUCCEEDED/FAILED/PARTIAL/UNKNOWN`。 |
| `confidence` | 任务结论置信度。 |
| `reason` | 模型结合用户目标和实际结果给出的原因。 |
| `evidence_event_ids` | 支撑任务结论的 SessionLog Event。 |
| `error_code` | 失败错误码。 |
| `attempts` | 实际调用次数。 |

`dialogue_assessment.task_outcome` 与 `metrics.business_status` 应同时保留：

- `metrics.business_status` 来自 Tool、Skill、Dispatch 和 Evaluation Event 的结构事实；
- `task_outcome` 来自模型结合用户原始目标做出的语义判断。

模型任务结论不会覆盖确定性指标。

## 10. `reflection`：面向进化的候选建议

| 字段 | 含义 |
| --- | --- |
| `summary` | 对整个 Dialogue 的反思摘要。 |
| `root_causes` | 根因候选。 |
| `skill_orchestration_changes` | Skill 编排改进建议。 |
| `prompt_changes` | Prompt 改进建议。 |
| `stop_condition_changes` | Loop 结束条件改进建议。 |
| `wasted_episode_ids` | 模型认为浪费的 Episode。 |
| `evidence_event_ids` | 支撑反思的原始 Event。 |

这些字段是候选进化材料，不代表建议已经自动写入 Skill、Prompt、Coordinator 或
停止策略。后续晋升前仍需要跨 Dialogue 聚合、置信度统计、灰度验证和回滚策略。

## 11. 报告级问题与模型调用元数据

### 11.1 `issues[]`

| 字段 | 含义 |
| --- | --- |
| `severity` | warning 或 error。 |
| `code` | 稳定问题码。 |
| `message` | 可读说明。 |
| `episode_id` | 可选的关联 Episode。 |

典型问题包括缺失 Diagnostic 引用、模型传输失败、模型响应非法等。

### 11.2 模型计数

| 字段 | 含义 |
| --- | --- |
| `model_requested_count` | 实际发起的对话级模型调用数。直连模式通常最多为 1。 |
| `model_succeeded_count` | 响应通过全部校验的调用数。 |
| `model_failed_count` | Transport 或响应校验失败数。 |
| `model_failures_degraded` | 模型失败后是否降级保留规则结果。 |

### 11.3 内容处理元数据

| 字段 | 含义 |
| --- | --- |
| `content_aware_model` | 当前报告来自支持读取脱敏具体内容的直连路径，不代表本次一定调用了模型。 |
| `model_scope` | 当前为 `dialogue`，即一个模型调用覆盖整个 Dialogue。 |
| `model_prompt_version` | 内容感知 Prompt 版本。 |
| `content_event_count` | 实际进入模型 Payload 的去重 Event 数。 |
| `redaction_count` | 构造模型输入时执行的脱敏替换次数。 |

当 `--model-selection none` 时，不构造模型内容请求，因此上述计数可能作为 Proto3
默认零值而不出现在 JSON 中。

## 12. 当前输出的实际状态解释

当前 `trace_output/evaluation/by-user` 中：

- 4 份报告完成了 `doubao-seed-evolving` 严格内容感知评测；
- 6 份报告是同一 C++ Evaluator 生成的规则层报告，
  `dialogue_assessment.status=MODEL_CALL_STATUS_NOT_REQUESTED`。

后 6 份文件描述的是最后一次成功落盘的规则评测运行。此前严格模型调用的
`transport_28` 超时因为严格模式直接退出，没有写入这些文件。因此 Evaluation 当前
表示“本次成功评测结果”，不是 Evaluator 所有历史尝试的审计日志。

## 13. 下游消费建议

建树或进化模块建议按以下优先级消费：

1. 用 `source` 和 `source_fingerprint` 建立数据血缘；
2. 用 `source_event_ids`、`evidence_event_ids` 回放原始证据；
3. 用 `metrics` 获取确定性统计；
4. 用 `final_verdict` 而不是 `rule.verdict` 统计最终 Episode 语义；
5. 只有在 `dialogue_assessment.status=SUCCEEDED` 时消费任务结论和 `reflection`；
6. 将 `wasted_episode_ids` 作为候选标签，而不是未经验证的真值；
7. 同时记录 Evaluator、规则、Analyzer、Prompt 和模型版本，避免跨版本指标混用。
