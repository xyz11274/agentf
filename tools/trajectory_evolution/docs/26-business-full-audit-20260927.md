# 业务自进化旧链路全量审计（2026-09-27）

这份审计在清理旧 `training` 和 `gepa` 之前生成。扫描覆盖当前训练目录的 35 个报告、416 条训练 trial、400 个唯一任务，以及已校验归档中的 167 个 GEPA 报告、7,736 条 trial。归档逐 blob 校验过 SHA-256，报告逐条解析；未用抽样结果替代全量扫描。

## 记录事实

| 范围 | 全量结果 |
|---|---:|
| 训练报告 | 34 个完成报告，另有 1 个 `017-attempt-01` 不完整报告 |
| 训练任务 | 416 条 trial / 400 个唯一 task；16 条是恢复 attempt 的重复记录 |
| 训练异常 | 1 条 `verdict=failed`；训练报告的模型 judge 未启用，因此 415 条为 `uncertain` |
| 训练记忆 | 301 条候选：110 proposed、191 insufficient support；revision 120 |
| 记忆证据 | 1,168 条 support evidence 全部 `SEMANTIC_VERDICT_UNASSESSED` |
| GEPA 报告 | 167 个，166 完整、1 不完整；7,736 条 trial，7,725 完成 |
| GEPA 异常 | 45 条 `evaluation_judge_failed`，11 条 `runner_cancelled`；11 条没有 trace |
| GEPA 指标观测 | token 7,718 条，模型调用 7,725 条，loop 7,725 条；均值约 99,275 token、15.45 调用、15.45 loop |
| 意图观测 | measured 5,043、dispatch_unobserved 3,567、unlabeled 5,071、conditional_review 996 |

旧真实 holdout 的 L0 / Node27 结果为质量 `0.519522 / 0.519494`、严格通过 `85/200 / 89/200`、无效 loop `2.56 / 2.40`、时延 `77.893s / 90.229s`、token `99,851.7 / 101,065.4`、意图 F1 `0.8800 / 0.9123`。Node27 的 L1/L2/L3 实际应用为 `1560/15/9`。因此意图识别上升没有转化成目标完成质量，loop 也只下降 6.25%，同时 token 上升 1.22%、时延上升 15.84%。Node15/18 同样未通过真实 200+200 holdout。

## 已确认的算法与实现问题

1. **记忆准入过早。** 301 条候选中 191 条没有达到支持门槛，全部 1,168 条证据仍是 `UNASSESSED`。候选的 `confidence` 可以达到 0.9 左右，但没有语义裁判结果证明建议有效。110 条 proposed 经验被 L1 注入后形成大量上下文噪声，解释了 L1 1560 次以及 token/时延上升。
2. **训练阶段没有质量裁判。** 训练 415 条为 `uncertain`，意图字段大量是 `unlabeled` 或 `dispatch_unobserved`。训练产生的反思可以进入候选库，却没有先经过最小的目标完成/无效 loop 反事实检查。
3. **训练错误扩大到批次。** `coldstart.inspect()` 原来遇到任意单题 trace/error 就暂停整批；429、超时、非法 JSON 会阻断后续题和记忆入库，违反任务级容错要求。
4. **旧产物重复。** 每个完成评测同时落 `report.json`、`report.pb`、`results.jsonl`、Markdown 和 `trials/*.json`。GEPA 旧树 44.8GB，7,736 条 trial 中大量重复来自这些表示；`training/training/*` 还多了一层目录。
5. **资源目标没有进入旧搜索排序。** 旧 `latency_weight` 为 0，候选排序忽略时延；L1 注入又增加了 prompt/context，造成质量没有提升而资源变差。现修正为 loop/token/latency 权重总和 0.45，给质量保留 0.55。
6. **Holdout 配对结构曾被误读。** 旧 cached pair 的严格配对在顶层、目标完成配对为空，导致 Node15/18 被误判 `insufficient_paired_trials`；代码已把严格通过和质量配对分开重算。`tree.json.final_holdout` 的 retrospective validation 摘要也已标明，不能冒充真实 holdout。
7. **恢复被不必要的版本漂移阻断。** 上次新启动确实建立了 job，约 830 次模型调用后，后续 supervisor 因并行代码编辑造成 `offline_gepa.py` runtime hash 变化而报 `gepa_runtime_changed`。这不是模型质量结果；新的入口应在恢复时允许继续使用现有 checkpoint，不因代码/配置指纹变化反复终止。
8. **全链路没有统一生命周期锁。** 清理 training、重建 dataset、运行 GEPA 分属不同锁，存在清理与子进程并发的风险；完整入口需要从清理前置检查到发布持有一个锁。

## 修复顺序

1. 保留本审计摘要和旧 GEPA 的已校验压缩归档，停止所有并行编辑，再清空 `trace_output/business-simulator` 中旧 training/gepa/发布记录。
2. 训练阶段只保留可恢复的 canonical report、最小 memory 和错误摘要；完成批次删除逐题副本、results JSONL、Markdown、原始 traces/evaluation/batches。
3. 记忆只把有目标完成或 loop 证据的候选送入 GEPA；不足支持的候选保留计数摘要，不注入 Agent。
4. 训练/评测单题模型失败继续当前批次，只有无法落盘或整批没有可用 trial 才暂停；恢复只依赖 task identity/checkpoint，不依赖易漂移的版本指纹。
5. GEPA 限制候选报告和 checkpoint 的数量与字段，保持整个新实验目录低于 5GB；最终只保留 L0/父节点/当前节点与真实 holdout 的指标及必要证据。

这份文件是旧记录的审计结论，不代表新实验已经达标。只有新的同题 200+200 holdout 同时满足质量、loop、token、时延、调用和意图门槛，才允许发布。
