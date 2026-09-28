# Business Simulator Holdout 效果审计（2026-09-26）

本审计在清理旧实验产物前生成，依据 `trace_output/business-simulator/gepa/workspace/agent-evaluation/8d505e3581ea19ad45ef2c4545fd3307c10c55ef114021835d5ca08c30e1b4c0/search/decision.json`、三份配对报告和源码静态核对。它只记录证据，不改写成绩。

## 权威 Holdout 结果

| 指标 | L0 baseline（200） | Node27 candidate（200） | 变化 | 门槛结论 |
|---|---:|---:|---:|---|
| 质量/目标完成均值 | 0.5195218254 | 0.5194940476 | -0.0000277778（-0.0053%） | 未提升 5%，质量回归 |
| 严格通过 | 85/200（42.5%） | 89/200（44.5%） | +4 | 不能抵消质量回归 |
| 平均无效 loop | 2.56 | 2.40 | -6.25% | 未达到至少 15% |
| 平均时延 | 77,892.88 ms | 90,228.82 ms | +15.84% | 违反不得增加 |
| 平均 token | 99,851.73 | 101,065.35 | +1.22% | 违反不得增加 |
| 意图识别 multi-intent F1 | 0.8800 | 0.9123 | +0.0323 | 提升，但未转化为质量 |
| L1/L2/L3 实际应用 | 0/0/0 | 1560/15/9 | 新增 | L1 使用量高，L3 覆盖低 |

Node15 和 Node18 的报告也都完整包含 baseline 200 + candidate 200，共 400 条；三份候选报告的 L0 均来自同一 baseline 报告。`tree.json` 中的 `.5982/.5026/.5364` 属于 validation/frozen retrospective 摘要，不能作为 Holdout 实测成绩。

最终 attempt 的候选细项：Node15 质量 `0.5175734127`、严格通过 `90/200`、无效 loop `2.49`、时延 `88,950.65 ms`、token `99,908.57`、意图 F1 `0.88496`；Node18 质量 `0.5032837302`、严格通过 `83/200`、无效 loop `2.18`、时延 `88,568.72 ms`、token `96,387.09`、意图 F1 `0.88789`。Node15 配对 pass delta `+0.025`（wins 24/losses 19/ties 157）；Node18 配对 pass delta `-0.01`（wins 19/losses 21/ties 160）。

## 已确认缺陷

1. `src/evaluation/agent_evaluation.cpp` 的 `gate_result()` 在目标模式只读取 `comparisons.candidate.goal_completion.paired_trials`。`gepa_cached_pair.v1` 只写顶层 `comparisons.candidate.paired_trials`，导致 Node15/18 的真实 200 对被误判为 0 对并标记 `insufficient_paired_trials`。已加入兼容回退；不会改变阈值或旧分数。
2. GEPA 的 `final_holdout` 标记为 `protocol=derived-top-k-v2`、`retrospective=true`，会复制 validation 冻结排名；发布判断必须使用 `decision.json` 的真实 200×200 配对门禁。
3. L1 注入在 `src/runtime/policy_runtime.cpp` 的模型请求边界执行；本次 L1 应用 1560 次且 token/时延上升，说明经验上下文预算/命中策略需要压缩和去重。L3 只应用 9 次，无法支撑 15%～30% loop 目标。
4. `src/evaluation/task_evaluation.cpp` 每个试次写 `trials/<id>.json`，结束时又把同一试次写入 `report.json`、`report.pb`、`results.jsonl` 和 `report.md`。当前生产 reader 实际使用 `report.json`，Holdout cache 还使用 `report.pb`；`trials` 与 `results.jsonl` 是重复副本。
5. `task_evaluation.cpp` 的 `trial()` 已将模型超时、429 后耗尽重试、空/非法响应和 judge 失败记录为当前 trial 的错误；`model_adapter.cpp` 对 408/429/5xx 和可重试传输错误有指数退避。但 worker 对写入/内部异常仍可能把整批置为失败，应将模型/任务异常继续保持 task-scoped，并为恢复保留最小 checkpoint。
6. 业务排程器按四个连续执行/轨迹错误停止批次，这是保护机制；重跑时必须保留已完成 checkpoint，不能把单题失败扩大为全树失败。

## 重构验收条件

- 仍保留 `report.json` 作为 canonical 评测证据、必要的 `report.pb` 兼容入口和 `manifest/state/decision` 最小索引；完成运行默认清除 `trials`、`results.jsonl`、`report.md` 等重复物，调试模式才保留。
- 单题模型失败、judge 失败或无效 JSON 不终止其它题；失败题计入分母并可恢复，不能计为通过。
- L1 注入必须有字节预算和稳定去重；L2/L3 只在实际边界命中时计数。任何新增控制不能增加模型调用、token 或总时延。
- Holdout 仍是共享 L0 200 + 每候选 200，且门禁兼容普通报告和 cached pair 两种配对结构。
- 达成至少质量 +5%、无效 loop -15%（目标 -30%），且时延/token/调用不增加、意图识别不下降后才允许发布；否则保持 rejected 并继续保留失败证据。
