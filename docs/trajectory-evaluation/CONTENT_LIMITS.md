# AgentFlow Trajectory Evaluation 内容限长机制

本文解释 `trajectory_hybrid_evaluator` 在 SessionLog 直连模式中如何限制输入、模型
Prompt、HTTP 请求和模型响应。限长的目标是控制内存、网络、模型上下文和恶意输入
风险，同时避免把不完整数据误当成完整评测。

## 1. 总体处理顺序

```text
SessionLog JSONL
  ↓ 文件和单行硬限制
Loop Episode 结构分析
  ↓ 选择相关 Event、去重、删除重复大字段
递归脱敏和字段级限长
  ↓
Dialogue 级模型 Payload
  ↓ Prompt 总大小硬限制
OpenAI-compatible HTTP Body
  ↓ HTTP 请求体硬限制
模型生成
  ↓ Token 和响应字节限制
响应字段级严格校验
```

限长有两种不同语义：

- **截断**：保留部分内容并记录截断；
- **硬拒绝**：不再继续模型评测，绝不把超限数据悄悄当作完整输入。

## 2. 限制总表

| 阶段 | 对象 | 当前上限 | 超限行为 |
| --- | --- | ---: | --- |
| 输入 | 单个 SessionLog | 256 MiB | 整次分析失败 |
| 输入 | 单行 JSONL Event | 32 MiB | 整次分析失败 |
| 结构分析 | Episode 数 | 100,000 | 整次评测失败 |
| 结构分析 | Diagnostic 数 | 100,000 | 整次评测失败 |
| 结构分析 | 单 Episode 数组项 | 10,000 | 整次评测失败 |
| 结构分析 | 单 Episode 证据 ID | 10,000 | 整次评测失败 |
| 结构分析 | 单个结构化字符串 | 1 MiB | 整次评测失败 |
| 错误归一化 | Tool 结果摘要 | 8,192 bytes | 截断后用于分类/指纹 |
| 模型内容 | 单个字符串 | 32 KiB | UTF-8 安全截断并追加原文哈希 |
| 模型内容 | 嵌套对象字段数 | 256 | 只保留前 256 项 |
| 模型内容 | 嵌套数组元素数 | 256 | 只保留前 256 项 |
| 模型内容 | JSON 递归深度 | 32 层 | 替换为深度截断标记 |
| Prompt | system + user Prompt | 2 MiB | 模型分支跳过或严格失败 |
| HTTP | 完整请求 Body | 2 MiB | `request_too_large` |
| 生成 | SessionLog 直连默认输出 | 4,096 tokens | Provider 停止继续生成 |
| 生成 | 可配置最大输出 | 32,768 tokens | 配置拒绝 |
| HTTP | Provider 完整响应 | 256 KiB | `response_too_large` |
| 响应校验 | 单个文本字段 | 2,048 bytes | 整份模型响应拒绝 |
| 响应校验 | 单个反思列表 | 64 项 | 整份模型响应拒绝 |
| 响应校验 | Evidence ID 列表 | 256 项 | 整份模型响应拒绝 |

MiB/KiB 均按二进制单位计算：

```text
1 KiB = 1,024 bytes
1 MiB = 1,048,576 bytes
```

## 3. 原始 SessionLog 限制

默认上限：

```text
max_file_bytes = 256 MiB = 268,435,456 bytes
max_line_bytes = 32 MiB  = 33,554,432 bytes
```

程序先检查完整文件大小，再逐行检查 JSONL Event。超过任一上限都会抛出错误，不会：

- 只读取文件前半部分；
- 忽略超长 Event；
- 生成看似正常但证据不完整的 Evaluation。

`EvaluatorOptions.max_file_bytes` 可通过 C++ Library API 修改，但当前 CLI 没有对应
参数。单行 32 MiB 使用 Loop Analyzer 默认值。

## 4. Event 选择与重复内容消除

限长前会先减少没有评测价值的重复数据。

### 4.1 可进入模型的 Event 类型

```text
dialogue.start
coordinator.activation
llm.request
llm.response
loop.control
skill.dispatch
batch.result
skill.start
tool.call
tool.result
artifact.persist
skill.end
evaluation.result
dialogue.end
```

### 4.2 默认不进入模型的重复/传输 Event

```text
llm.transport.request
messages.snapshot
client.delivery
```

### 4.3 从保留 Event 中删除的重复大字段

| Event | 删除字段 | 原因 |
| --- | --- | --- |
| `llm.response` | `request`、`provider_response`、`meta` | 已有有效请求/响应表示，避免 Provider 原始副本重复 |
| `artifact.persist` | `entries` | Artifact 大内容不进入模型 |
| `skill.end` | `artifacts` | 避免与 Artifact Event 重复 |

### 4.4 Contextual Event 与结构引用

以下 Contextual Event 即使没有直接出现在某个 Episode 的结构引用里，也会保留：

```text
dialogue.start / dialogue.end
coordinator.activation
skill.dispatch / batch.result
skill.start / skill.end
artifact.persist
evaluation.result
```

其他可用 Event 只有被结构证据引用时才进入模型。相同 `event_id` 最多进入一次。

## 5. 字符串限长

每个模型内容字符串最大：

```text
kMaxContentStringBytes = 32 KiB = 32,768 bytes
```

这是 UTF-8 字节数，不是字符数。中文通常占 3 个 UTF-8 字节，因此能保留的中文字符
数量会少于 32,768。

超限处理：

1. 从第 32,768 字节附近向前寻找合法 UTF-8 边界；
2. 保留边界前内容；
3. 追加完整原文的 SHA-256。

```text
<前 32 KiB 左右内容><TRUNCATED sha256=<完整原文哈希>>
```

最终字符串会因为追加标记而略大于 32 KiB。哈希用于判断两份被截断原文是否一致，
不会把完整原文写入 Evaluation。

程序先执行字符串截断，再执行 Secret、凭据模式和本地路径脱敏。

## 6. JSON 容器和递归深度

对递归进入的 JSON 对象与数组：

```text
kMaxContainerItems = 256
max depth = 32
```

### 6.1 嵌套对象

只保留遍历顺序中的前 256 个字段，后续字段直接省略，并将
`truncation_count` 增加 1。

### 6.2 嵌套数组

只保留前 256 个元素，后续元素省略，并将 `truncation_count` 增加 1。

### 6.3 深度

进入超过 32 层的位置后，使用：

```text
<TRUNCATED:DEPTH>
```

替换更深的值。

### 6.4 当前实现的边界

256 项限制作用于递归进入的嵌套对象和数组，但 `sanitized_event_data()` 直接遍历的
Event 顶层 `data` 对象本身没有单独套用 256 字段上限；整个 `content_events` 数组
也没有独立的 Event 数量上限。

这两处最终由以下限制兜底：

- SessionLog 文件最大 256 MiB；
- 完整 Prompt 最大 2 MiB。

因此，大量小 Event 或顶层字段过多时，当前行为不是智能抽样，而可能在最终 Prompt
检查阶段跳过整个模型分支。

## 7. 完整 Prompt 的 2 MiB 硬限制

所有 Episode 结构证据和内容 Event 组装完成后，程序检查：

```text
system_prompt.size() + user_prompt.size() <= 2,097,152 bytes
```

超过上限：

```text
error_code = model_request_too_large
model.status = MODEL_CALL_STATUS_SKIPPED
```

非严格模式保留确定性规则结果并生成降级报告；严格模式抛出错误，CLI 退出码为 3。

这里不会自动：

- 丢弃最老 Event；
- 只保留失败 Episode；
- 对 Dialogue 分块；
- 生成多次模型调用后合并。

也就是说，2 MiB 是硬门槛，不是全局智能截断目标。

## 8. HTTP Body 的第二层 2 MiB 限制

Prompt 还会封装成 OpenAI-compatible JSON：

```json
{
  "model": "...",
  "temperature": 0,
  "max_tokens": 4096,
  "messages": [
    {"role": "system", "content": "..."},
    {"role": "user", "content": "..."}
  ]
}
```

Adapter 再检查完整 HTTP Body：

```text
max_request_bytes = 2 MiB
```

由于 JSON 外层结构和转义会增加字节数，可能出现：

```text
原始 system + user Prompt 未超过 2 MiB
但序列化后的 HTTP Body 超过 2 MiB
```

此时 Adapter 返回 `request_too_large`。

Library API 可以修改 `max_request_bytes`，但构造器要求：

```text
0 < max_request_bytes <= 8 MiB
```

当前 CLI 没有暴露该参数。

## 9. 模型输出 Token 限制

OpenAI-compatible Adapter 的结构默认值是 800 Token，但 CLI 对直接输入 `.jsonl` 的
SessionLog 模式会在用户没有显式配置时改为：

```text
max_output_tokens = 4096
```

可通过：

```bash
--max-output-tokens N
```

修改，合法范围：

```text
1 <= N <= 32768
```

Token 限制由 Provider 执行，与本地的响应字节限制相互独立。

## 10. HTTP 响应字节限制

Adapter 对 Provider 返回的完整 HTTP Response 设置：

```text
max_response_bytes = 256 KiB = 262,144 bytes
```

接收过程中一旦发现将超过该值，会停止写入。最终错误码为：

```text
response_too_large
```

从 Provider JSON 中提取出的 `choices[0].message.content` 还会再次检查不得超过
256 KiB。

Library API 可以修改该限制，但必须：

```text
0 < max_response_bytes <= 8 MiB
```

当前 CLI 没有暴露该参数。

## 11. 模型响应字段级限制

即使完整响应低于 256 KiB，也必须通过严格字段校验。

### 11.1 文本字段

以下必填文本均必须非空且不超过 2,048 bytes：

- `task_outcome.reason`
- `expected_state_change`
- `observed_state_change`
- `concise_reason`
- `reflection.summary`
- 各反思数组中的单个字符串

当前配置还要求任务理由、Episode 简要理由和反思摘要包含中文。

### 11.2 列表

| 列表 | 上限 |
| --- | ---: |
| `root_causes` | 64 |
| `skill_orchestration_changes` | 64 |
| `experience_injections` | 64 |
| `stop_condition_changes` | 64 |
| `wasted_episode_ids` | 64 |
| 各处 `evidence_event_ids` | 256 |

Evidence ID 列表必须非空，且每个 ID 都必须存在于本次模型请求允许引用的 Event 集合。
`wasted_episode_ids` 只能引用当前 Dialogue 的 Episode。

### 11.3 Episode 数量

模型返回的 `episode_assessments` 必须且只能覆盖 `selected_episode_ids`：

- 不能缺少；
- 不能重复；
- 不能多出未知 Episode。

任意一个字段超限或非法，整份对话级模型响应原子拒绝，不会只采用其中合法部分。

## 12. 结构分析层的额外上限

在构建 Episode Evidence Packet 时还有防御性限制：

```text
Episode 总数                 100,000
Diagnostic 总数              100,000
单 Episode 内数组项           10,000
单 Episode 的 Evidence ID     10,000
单个结构化字符串               1 MiB
```

这些限制超出时会拒绝整个结构输入，而不是部分截断。

Loop Analyzer 为了进行错误分类、重复结果检测和指纹计算，会把归一化 Tool 结果文本
限制为 8,192 bytes。该限制只影响错误归类材料；模型内容中的原始 Tool 结果字符串仍
使用 32 KiB 上限。

## 13. `none`、`suspicious` 与 `all` 对长度的影响

### `none`

- 不构造具体内容 Prompt；
- 不调用模型；
- 只输出确定性规则结果。

### `suspicious`

- 只要求模型为规则筛出的可疑 Episode 返回逐轮判断；
- 仍向模型提供全部 Episode 的结构证据；
- 仍提供整个 Dialogue 中相关的内容 Event；
- 主要减少输出长度，不一定显著减少输入长度。

### `all`

- 要求模型判断全部 Episode；
- 输入和输出都可能最大；
- Episode 较多时更容易触及输出 Token 或响应字节上限。

## 14. 计数与持久化情况

模型请求 Payload 内包含：

```json
{
  "redaction_count": 9,
  "truncation_count": 0
}
```

最终 Evaluation Protobuf 当前只保存：

```text
content_event_count
redaction_count
```

尚未保存 `truncation_count`。因此仅看最终 `evaluation.json`：

- 可以知道有多少 Event 进入模型；
- 可以知道执行了多少次脱敏；
- 不能判断发生了多少次字符串、容器或深度截断。

使用 `--requests-out` 可以查看请求 Payload 中的 `truncation_count`，但该文件包含
脱敏后的具体对话内容，应作为敏感派生调试数据管理，不应提交到 Git。

## 15. 对评测可靠性的实际影响

当前限长策略能够保证：

- 不读取无限大的输入；
- 不向模型发送无限大的请求；
- 不接收无限大的响应；
- 不因单个超长字段造成内存无界增长；
- 超过总 Prompt 上限时不会伪装成完整模型评测。

当前还没有解决：

1. Prompt 超过 2 MiB 时的分块评测；
2. 基于 Event 重要性的预算分配；
3. 对超长 Tool 结果先做结构化摘要再评测；
4. 在最终 Protobuf 中持久化 `truncation_count`；
5. 精确记录哪些 Event/字段被截断；
6. 对大量 Episode 分批评测后进行一致性合并。

在进入大规模自进化训练前，至少应把 `truncation_count` 持久化。否则下游无法仅凭
Evaluation 判断模型结论是基于完整内容，还是基于发生过截断的内容。
