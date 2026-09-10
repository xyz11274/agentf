# 11 · 规则、模型与 Hybrid Evaluator

[文档首页](README.md)

本章说明**已有轨迹的分析、评估和反思输入**。验证某个策略版本是否提升 Agent 的实际执行效果，
使用[第 19 章的任务评测模块](19-datasets-and-benchmarks.md)：在独立实例中重新执行固定任务并比较结果。
该模块由 L0–L3 策略准入和后续 L4 优化共同复用，结果还会更新[记忆实测状态](13-private-memory.md)。

## 本章目标

理解模型真正看见的内容、规则指标、输出校验和不同判定的融合。

## 设计思路

确定性检查提供结构与指标，模型补充语义判断。评测失败、业务失败与轨迹不完整分别记录，规则确认的无效结论在融合时保留。

![Hybrid Evaluator：规则与模型职责](figures/11-hybrid.svg)

读图：评测模型状态、业务结果和候选支持分别记录。 [放大 SVG](figures/11-hybrid.svg)。

## 怎样识别无效执行与进行模型评测

### 当前通用分析器的确定性诊断范围

目前标准轨迹规则重点实现 `non_retryable_repeat`，即明确不可重试后仍进行相同失败调用。
SessionLog 分析器与标准轨迹分析器按各自输入结构生成诊断，具体类别以对应报告为准。

通用规则只有同时满足下面条件才命中：

1. 两次动作的输入指纹、实际执行者、执行流、loop_id 和操作名相同。
2. 当前轮次大于前一次轮次。
3. 同生产者序号或显式因果链确认当前动作发生在前一次之后。
4. 两次都失败，且都明确标记为 `non_retryable`。
5. 两次错误码非空且相同。

命中后生成置信度 1.0 的确定性诊断，并引用相关调用/结果事件。
实现按匹配键查找前一次动作。
重复重试判定同时检查执行流、操作、输入和先后关系。

`invalid_loop_lower_bound` 统计已有确定性证据支持的无效逻辑轮次。
模型评出的无效 Episode 数、规则下界、总循环数是不同指标。

标准分析器 standard-trace-analyzer-v2 按动作的 attributes.semantic 区分模型、业务工具和自定义活动。
tool_call_count 及其失败计数只统计业务工具；明确 executed=false 的拒绝或跳过不计入执行次数。
模型活动仍保留完整事件证据、失败状态与重复调用诊断。同名模型和工具分别判断重复，避免相互混淆。
旧轨迹没有 semantic 标记时按原 action 约定解释为工具，以保持历史数据可读。

### 执行价值的规则判定

`src/hybrid_evaluator.cpp` 中的规则判定顺序是：

| 证据 | 规则结果 |
| --- | --- |
| 有 `counts_as_invalid` 的诊断 | INVALID，取对应诊断的置信度 |
| 显式评测失败或可疑诊断 | UNCERTAIN，保留相应证据 |
| 只有失败结果，没有足够因果证据 | UNCERTAIN |
| 没有上述证据 | UNASSESSED，置信度 0 |

评测结合业务状态与执行上下文，区分必要探索、可恢复错误和重复操作。

### 模型看见什么

通用轨迹与 SessionLog 的内容感知路径都在 `src/sessionlog_evaluator.cpp`。
该配置同时供标准轨迹和 SessionLog 评测入口使用。

`make_dialogue_request` 将以下内容交给评测模型：

- 本次运行/对话的来源标识。
- 所有 Episode 的结构证据、规则结论，以及本次需要评测的 `selected_episode_ids`。
- `content_events`：经过脱敏和限长的实际用户输入、模型文本、工具参数与结果、错误、状态等事件内容。
- `allowed_experience_scopes`：模型能够选择的经验作用域。
- 脱敏和截断计数、证据内容不可信的声明。

指纹用于关联和去重，具体文本用于语义分析。
直接评测旧的纯 analysis 报告时，模型只能看到该报告本身拥有的信息，无法恢复未提供的原文。

模型输入限于本次请求包含的字段。
元反思和记忆整理阶段看到的又是候选摘要，不会自动获得全部原始日志，详见[候选与元反思](12-reflection-and-candidates.md)和[私有记忆](13-private-memory.md)。

### 模型选择、限长和响应校验

`model_selection` 的含义：

- `none`：只使用规则。
- `suspicious`：选择规则已关注、失败或具有可疑信号的 Episode。
- `all`：选择所有 Episode。
- `auto`：由入口结合模型配置选择；通用后台有 endpoint 时采用 suspicious，无模型配置时不调用模型。

内容感知路径通常是**一次 Run 构造一次模型请求**，同时要求返回选中 Episode 的评测和整次任务的判断/反思。
`all` 选择全部 Episode，并按输入预算组织请求；选中数为 0 时可评测整次任务内容，Episode 结果使用空数组。

重要的限制是字节与结构限制，不全是 token 限制：

| 位置 | 当前限制/处理 |
| --- | --- |
| 一段内容字符串 | 超过 32 KiB 时按 UTF-8 边界截断，保留截断标记 |
| 对象/数组 | 每容器最多保留 256 项 |
| 递归内容 | 深度超过 32 时截断 |
| 内容感知评测提示词 | system + user 合计最多 2 MiB，超出不无限扩大请求 |
| 内容感知模型响应 | 最多 256 KiB，还受模型适配器限制 |
| 多数模型结论文本 | 2048 字节以内；响应列表和引用也有独立上限 |
| 模型生成长度/超时 | 外部配置的 max_output_tokens、timeout_ms 等 |

输出记录截断计数与标记，评测器据此识别可用内容和缺失上下文。
可配置限额见配置表，其余结构上限在 C++ 实现中定义。

模型返回必须通过程序检查：JSON 结构、枚举、置信度范围、选中 Episode 集合、中文说明要求，以及事件引用白名单。
每条经验还需要独立作用域、条件、建议和自身证据。部分经验项不合格可单独丢弃并记警告；关键评测结构不合格会使整次模型语义结果失败。
`strict_model=false` 时记录失败并保留规则结果；开启 strict 时可以直接使该评测阶段失败。
超时、预算不足、输出截断和引用错误记录为模型评测错误。

### 规则与模型怎样融合

融合按以下优先级执行：

1. 规则明确判定 INVALID 时保留该结论。
2. 模型也认定 INVALID，可形成混合来源并保留更高置信度。
3. 模型与确定性无效结论冲突时记录冲突，但保留规则硬结论；UNCERTAIN 本身不当作明确反对。
4. 没有确定性无效结论时，可使用通过校验的模型语义判定。
5. 模型未调用或失败时，保留规则结论；没有证据就仍然 UNASSESSED/UNCERTAIN。

模型还会产出整次任务的 `task_outcome` 和反思：根因、动作编排建议、经验注入建议、结束条件建议、无效 Episode 及事件证据。
`skill_orchestration_changes` 是现有输出字段的历史名称，在通用输入中解释为动作编排建议，不要求宿主存在 Skill。
宿主提供的执行状态、业务状态和模型评估应分别阅读，不把某个分支的失败强行上升为整个工作流失败。


![模型调用与预算分层](figures/11-budgets.svg)

读图：逻辑调用、HTTP 尝试和业务模型调用分别计数。 [放大 SVG](figures/11-budgets.svg)。

## 当前在哪些地方使用模型

| 调用点 | 模型输入 | 模型输出 | 程序保留的决定权 |
| --- | --- | --- | --- |
| 内容感知评测 | 结构证据 + 脱敏后的实际事件内容 | 任务结果、Episode 价值、根因与建议 | 不允许覆盖确定性 invalid，校验引用与结构 |
| 元反思 | 同类型同 scope 的候选内容、seeds 与证据白名单 | 合并组和更明确的经验 | 接受与否、证据继承、状态重算 |
| 增量记忆语义匹配 | incoming candidate + 短名单正文 | 重复、相关、冲突或独立 | 门槛、主键、合并、关系写入 |
| 全库语义整理 | 库内当前条目 + 短名单 | 同上 | 容量、关系一致性、失败保留与提交 |
| 公共提炼与复核 | 经验的 scope/条件/建议，不发送用户身份与原日志；复核还看同 scope 的公共候选 | 泛化建议；new/duplicate/conflict/reject/uncertain | 结构、隐私、独立证据、置信度、支持计票、是否公开 |
| 私人—公共关系 | 当前私人与公共经验的具体内容 | duplicate/related/conflict/independent | 双版本绑定、冲突全局暂停、未知关系不注入 |
| 在线查询与 L1–L3 | 当前边界请求 | 确定性策略决定 | 此阶段不额外调模型；业务模型调用仍属于宿主 |

提示词位于 [config/prompts](../config/prompts/)：

- [standard-dialogue-system.txt](../config/prompts/standard-dialogue-system.txt) 与 [dialogue-user.txt](../config/prompts/dialogue-user.txt)：评测职责、证据不可信声明、JSON 契约。
- [meta-reflection-system.txt](../config/prompts/meta-reflection-system.txt) 与 [meta-reflection-user.txt](../config/prompts/meta-reflection-user.txt)：同范围语义合并、禁止扩权/造证据。
- [memory-curator-system.txt](../config/prompts/memory-curator-system.txt) 与 [memory-curator-user.txt](../config/prompts/memory-curator-user.txt)：四种关系及返回格式。
- [runtime-experience.txt](../config/prompts/runtime-experience.txt)：获准经验怎样呈现给宿主模型，以及条件、权限与预算边界。
- shared-experience-distill.txt、shared-experience-review.txt、cross-memory-relation.txt：公共泛化、复核与跨库关系；下一次实际公共管理周期重读。

模型超时、输出上限、重试和 thinking 参数通过外部配置设置。
thinking 参数按配置传给供应商；省略时使用供应商默认行为。
max_model_calls 限制组件阶段内的逻辑调用，传输重试单独配置；总费用由部署预算控制。

## 报告层级和常用字段

| 层级 | 核心内容 | 使用方式 |
| --- | --- | --- |
| 原始 TraceRun/SessionLog | 真实输入输出、事件与终态 | 作为证据，不直接当作结论 |
| EpisodeEvidencePacket | 当前观察单元、指纹、诊断与来源事件 ID | 解释某个规则或模型 verdict 指向什么 |
| Episode assessment | 规则判断、模型状态、语义结论和置信度 | 区分“规则可疑”“模型确认”“证据不足” |
| dialogue assessment/reflection | 整轮解释、根因、浪费单元与经验建议 | 为 Seed 提供带证据的语义来源 |
| Batch runs | source/evaluation 指纹、成功/失败与数量 | 检查批次成员和阶段完整性 |
| synthesis / candidates | 合格报告数、候选与支持/反证 | 用于后续记忆入库 |
| memory_update | candidate → memory 的决策与修订 | 跟踪真正的持续入库 |

Protobuf 的具体字段定义在[hybrid_evaluation.proto](../proto/hybrid_evaluation.proto)。
JSON 默认或空字段可能按序列化规则省略；通过 model_status 和调用统计查看模型执行情况。

## 模型预算怎样消耗

统一后台的单轨迹评测、元反思和私库整理共享 evaluation.max_model_calls 的批次预算；
关系判定和修复请求均计入调用统计。
公共管理另有 max_model_calls_per_cycle，不在这个批次预算里；任务基线验收又有自己的预算。

模型端点超时、输出截断、非法 JSON、无效引用和语义不确定要保持各自错误或未判定状态。
strict_model 控制模型错误是否升级为评测错误；失败响应保留错误状态，候选生成使用成功评测的证据。
切换模型不修改历史报告的来源/内容指纹，不自动重评已经完成的日志。

## 检查限长是否影响结论

限长按字节、条数和输出 token 分别设置；max_context_bytes 控制运行时注入长度。
模型评测使用当前入口抽取、脱敏并按预算裁剪后的正文。
需要更多上下文时，调整对应预算并重新评测，输出中保留本次截断信息。

## 实现定位

- `src/trace_analyzer.cpp`
- `src/hybrid_evaluator.cpp`
- `src/sessionlog_evaluator.cpp`
- `include/agentflow/trajectory/hybrid_evaluator.h`
- `proto/hybrid_evaluation.proto`

---

[文档首页](README.md) · [上一章：轨迹采集、清洗、校验与 Episode](10-capture-and-validation.md) · [下一章：Seed、候选、反证与元反思](12-reflection-and-candidates.md)
