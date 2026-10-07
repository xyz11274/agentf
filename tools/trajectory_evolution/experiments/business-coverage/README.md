# 业务实验最小复现包

这个目录只保存业务数据、训练记忆和真实实验入口。评测使用
`examples/agent/business-simulator/.env` 中的真实 API；脚本会自动清除
HTTP/HTTPS/ALL proxy 并把模型失败按题记录，不能用离线缓存冒充成绩。

`datasets/pack/manifest.json` 保存每个 JSONL 的行数和 SHA256；comparison-500 从 seed004 冻结文件原样复制，不按成绩挑题。`trace_output/business-simulator/` 只保留 `training/`、`gepa/`、`published/` 三个阶段目录；权威 memory 位于 training/evolution，发布策略位于 published。

## 一键真实复现

先在当前 shell 配好真实 API（或写入 `.env`，不要把 key 提交到 git）：

```bash
cd /root/lai-code/Agent/AgentFlow
# .env 至少包含 AF_LLM_BASE_URL、AF_LLM_API_KEY、AF_LLM_MODEL
# 真实调用 API：清理可重建产物，执行 train → 生成 seed → GEPA → val/holdout 对比 → 选优发布。
bash tools/trajectory_evolution/experiments/business-coverage/reproduce_business.sh
```

这条命令按同一生命周期执行：

1. 校验原始数据、业务入口、原生 evaluator 和训练输入；
2. 清理可重建的旧 `training/`、`gepa/`、`published/` 产物；
3. 用真实 API 运行完整 train，写入唯一权威 memory；
4. 从该 memory 准备并运行 GEPA（默认 30 轮预算），在固定 val 搜索与选择；
5. 对候选做最终 holdout，按门禁选出最佳策略并写入 `published/`。

适合长时间运行时使用 tmux：

```bash
# 进入持久会话，防止终端断开影响长任务。
tmux new -s business-reproduce
cd /root/lai-code/Agent/AgentFlow
# 真实 API 全链路运行并把终端播报写入临时日志（不写入凭据）。
bash tools/trajectory_evolution/experiments/business-coverage/reproduce_business.sh 2>&1 | tee /tmp/business-reproduce.log
# Ctrl-b d 脱离；以下命令重新进入并查看运行状态。
tmux attach -t business-reproduce
```

需要分阶段运行时：

```bash
# 从头真实训练并生成候选记忆；会调用 API。
python3 tools/trajectory_evolution/experiments/business-coverage/reproduce.py --fresh
# 训练或 GEPA 中断后，从已有 checkpoint 继续；会调用 API。
python3 tools/trajectory_evolution/experiments/business-coverage/reproduce.py --resume
# 复用 training memory 重新执行 GEPA、验证和候选选择；会调用 API。
python3 tools/trajectory_evolution/experiments/business-coverage/reproduce.py --gepa
# 只把已通过门禁的候选写入 published；不重新调用模型。
python3 tools/trajectory_evolution/experiments/business-coverage/reproduce.py --publish
# 只读查看阶段状态和文件完整性；不调用模型。
python3 tools/trajectory_evolution/experiments/business-coverage/reproduce.py --status
```

## 关于历史发布候选

历史发布物 `trace_output/business-simulator/published/seed004/` 的 manifest 记录了
`candidate_id=seed_relearn_scoped_004`、`method=known_business300_plus_frozen_training_replay200`。
它把固定业务 300 题与训练 replay 200 题合并评测，且 `independent_holdout=false`；因此不能声称
一次全新随机 API 运行必然得到相同分数。上面的命令会真实重建同一 train→GEPA→publish 机制，
但 seed 内容、模型响应和分数必须以本次完整报告为准。若要复核历史候选，只读取其
`strategy.json`、`comparison.json`、`manifest.json` 和 `run.sh`，不要把旧分数注入新 GEPA。

## 指标口径

所有成绩由原生 trajectory-evolution evaluator 产生并写入报告。`quality` 是任务目标质量均值，
`strict_passed` 是严格门禁通过题数，`invalid_loop` 由原生 loop analyzer 从轨迹证据统计，
`latency/tokens/calls` 来自实际请求；缺失或不同数据集口径的字段不能用于比较。发布前必须在同题同裁判口径下满足质量和严格通过相对 L0 至少 5%、无效 loop 至少下降 15%，且 token、调用数、时延不增加；无法核验则阻断发布。

也可以分阶段操作：

```bash
python3 tools/trajectory_evolution/experiments/business-coverage/reproduce.py --fresh   # 全链路
python3 tools/trajectory_evolution/experiments/business-coverage/reproduce.py --resume  # 恢复
python3 tools/trajectory_evolution/experiments/business-coverage/reproduce.py --gepa    # 复用 training memory 重跑 GEPA
python3 tools/trajectory_evolution/experiments/business-coverage/reproduce.py --publish # 仅发布已通过候选
```
