# 当前工作状态（2026-09-26）

恢复上下文时先读 `EXPERIMENT_GOALS.md`、本文件和 `25-business-holdout-effect-audit-20260926.md`。

## 旧实验终态

- 旧 GEPA job：`8d505e...`，`finished/rejected`，`published=false`，当前无运行进程。
- Validation 50：L0 `.470857/2.36/83.249s/20`；父 Node6 `.584952/2.36/94.509s/27`；Node27 `.598190/2.00/77.526s/28`。这些不能称为 Holdout。
- 真实 Holdout 共享 L0 200；Node27/15/18 各 candidate 200，三份配对报告均 400 行（baseline 200 + candidate 200）。
- L0：quality `.519522`，strict `85/200`，invalid loop `2.56`，latency `77.893s`，tokens `99851.7`，intent F1 `.880`。
- Node27：quality `.519494`，strict `89/200`，invalid loop `2.40`，latency `90.229s`，tokens `101065.4`，intent F1 `.9123`；拒因 `quality_regression/max_latency_ratio/loop_target_not_met`。
- Node15：quality `.517573`，strict `90/200`，invalid loop `2.49`，latency `88.951s`，tokens `99908.6`，intent F1 `.88496`；真实配对 wins/losses/ties `24/19/157`，仍拒绝。
- Node18：quality `.503284`，strict `83/200`，invalid loop `2.18`，latency `88.569s`，tokens `96387.1`，intent F1 `.88789`；配对 `19/21/160`，含 critical goal failure，仍拒绝。

## 已完成代码改动

- `agent_evaluation.cpp`：cached pair 缺少嵌套 objective pairing 时回退顶层配对字段；不改分数和阈值。
- `task_evaluation.cpp`：接受 `artifact_retention`；轻量模式完成报告后清除重复 `trials`，可关闭 `report.md/results.jsonl`；默认配置保持兼容。
- `offline_gepa.py`：新实验打开轻量产物配置、`max_token_ratio=1.0`，并把 GEPA 搜索的 invalid-loop/token/latency 权重与发布目标对齐。
- `offline_experiment.py`：非程序员单入口 `--fresh/--status/--check`。
- 定向证据：标准 report/manifest 测试 1/1 通过；轻量模式实测保留 `report.json/report.pb/manifest` 且清除重复物。

## 待完成且不可宣称达标

1. 合并 cached pair 的 objective paired 统计和 final_holdout 元数据修复，避免 validation retrospective 冒充 Holdout。
2. 完成无损归档工具和 SHA256 核对；随后再删除旧 GEPA 重复物/重置 GEPA 树，不删除训练记忆和输入数据。
3. 定向编译/测试后，用简化入口重新跑完整离线链条；每 60 秒表格播报。
4. 只有真实 Holdout 同时满足质量 +5%、invalid loop 至少 -15%、latency/token/calls 不增加、intent 不下降，才可以发布；否则保留 rejected 证据。
