# BFCL v3 离线验收

这是开源 BFCL v3 的第二把验收尺，复用 business-simulator 的 AgentFlow
coordinator、skill executor 和自进化服务。固定数据划分为 train 400、GEPA
search 150、selection 50、最终 holdout 200；产物只写入
`trace_output/bfcl-v3`。

2026-09-29 已新增“已见场景”划分，数据位于
`datasets/seen-scenarios-v1/`：`source/` 保存四份逻辑 ID 清单与划分说明，
`training-reviewed/` 是原生可加载数据。200 个场景各取两个变体训练、另外
两个变体评测；search/selection/holdout 分别覆盖 75/25/100 个场景。
训练四种变体各100题，holdout各50题；题目 ID 互斥，评测场景均在训练出现。
这是同场景不同条件的评测，不是同输入原题复测。

新划分复用原 ZIP，原分片与成绩保留。原生 `train.jsonl` 是训练400题加
search150题的容器；实际训练必须只取 `source/train_ids.txt`，不能将550题
都送入初始训练。旧训练记忆已看过部分新评测题，新实验必须从新train400
重建记忆。当前运行入口和检查点仍对应旧实验，本次数据准备不启动实验。

在全新目标目录重建该划分的方式（不会请求模型）：

```bash
python3 tools/trajectory_evolution/experiments/bfcl-v3/bfcl_v3_dataset.py tools/trajectory_evolution/experiments/bfcl-v3/datasets/source/bfcl_v3_splits.zip tools/trajectory_evolution/experiments/bfcl-v3/datasets/seen-scenarios-v1/source --seen-scenarios --previous-source tools/trajectory_evolution/experiments/bfcl-v3/datasets/source
python3 tools/trajectory_evolution/experiments/bfcl-v3/bfcl_v3_adapter.py tools/trajectory_evolution/experiments/bfcl-v3/datasets/seen-scenarios-v1/training-reviewed --source tools/trajectory_evolution/experiments/bfcl-v3/datasets/seen-scenarios-v1/source
```

以上目录已生成时无需重复执行；以下运行命令用于现有旧实验配置，启动新
实验前需将训练/GEPA配置接到新划分并建立新的记忆与同题基线。

不懂代码时，在仓库根目录执行：

```bash
tools/trajectory_evolution/experiments/bfcl-v3/run_bfcl_v3.sh fresh
```

断点继续用 `resume`，查看状态用 `status`，启动前门禁用 `check`。流程会依次
完成训练、GEPA 搜索/选择、holdout 和发布；每一段都有检查点，重启不会复制
业务产物。运行状态在 tmux 会话 `bfcl-v3` 中，查看：

```bash
tmux attach -t bfcl-v3
```
