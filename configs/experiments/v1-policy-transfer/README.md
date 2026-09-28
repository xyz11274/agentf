# 真实 v1：单文件策略迁移对照

验证当前 AgentFlow v1 在全新任务上，仅加载已导出策略是否获得收益。
这不是旧实验回放：不读取旧训练集、历史对话、经验库或旧启动目录，不重新运行 GEPA。

## 实验内容

- L0：正常运行，不加载策略。
- L3：同一 Agent、模型、工具、任务预算和评测器，只增加 `published-version.json`。
- 15 个新任务，各有 2–3 轮输入；每组重复两次，共 60 个完整任务试次。
- 包括预算核算、文件修复、HTML、网络访问、缺失文件、图片生成、并发任务、视频服务失败处理和多轮交接。
- 同一试次的多轮输入保留对话状态，不同试次重新启动真实 daemon 和 Bridge、使用独立工作目录与数据库。
- 文本模型为 `deepseek-v4-flash`；图像为 `doubao-seedream-5.0-lite`。凭据从已有 `.env` 读取，不写入这些配置或策略包。

`published-version.json` 是本目录唯一来自之前进化的输入。
它已经包含来源经验快照、触发条件、模板、检索参数、规则和正文改写。
`service.json` 是当前版本的正常打点部署配置；`daemon.json` 和 `bridge.yaml` 是 Agent 的正常启动配置，
不是策略训练配置。`evaluation.json` 的两个组采用完全相同的启动命令与输入绑定；
L3 通过 `policy_version_file` 走真实离线导入校验。

`agent.md` 是当前示例的副本，只把模型字段改为 `deepseek-v4-flash`，原文件不改；
该字段在 AgentFlow 中优先于模型环境变量。隔离副本保留工作流要求的
`examples/agent/outfit-test-agent` 相对布局。
两个组的采集配置均把 `AF_EXEC_SKILL_KEY`、`AF_COMPLETED_BATCH_KEY` 声明为公开调度编号，
不把它们误作 API 密钥；真实密钥仍按原有隐私规则检查。这些是当前 Agent 的部署设置，与策略训练无关。
这两个声明位于标准客户端配置 `point-client.json`，不放入服务端配置。
离线评测时客户端自动使用评测器提供的隔离 URL、用户和临时令牌，文件不含生产地址或密钥。
本次评测通信预算设为 1000 ms；正常快速请求不等待该时限，不执行后台学习。

本次没有训练集和验证集，所有任务显式标记为 holdout，故数据集汇总的 `l4_ready=false` 是预期值。
它用于验证固定版本的泛化收益，不用于继续搜索或择优修改该策略。

## 启动一次全新实验

在仓库根目录、已安装 AgentFlow 开发依赖的环境执行。先保证 `build/agentflowd`、
`build/trajectory-evolution/trajectory_evolution` 和原生客户端库已构建，18821、18822 端口空闲。

每次指定一个新的输出目录，不覆盖旧报告：

```bash
export AF_TRANSFER_OUTPUT="trace_output/v1-policy-transfer-$(date -u +%Y%m%d-%H%M%S)"
export AF_TRANSFER_PYTHON="$(python -c 'import sys; print(sys.executable)')"
build/trajectory-evolution/trajectory_evolution dataset build --config configs/experiments/v1-policy-transfer/dataset.json --output-root "$AF_TRANSFER_OUTPUT/dataset"
"$AF_TRANSFER_PYTHON" -m dotenv -f examples/agent/outfit-test-agent/.env run --override -- env "PATH=$PATH" "AF_TRANSFER_PYTHON=$AF_TRANSFER_PYTHON" build/trajectory-evolution/trajectory_evolution dataset run "$AF_TRANSFER_OUTPUT/dataset" --config configs/experiments/v1-policy-transfer/evaluation.json --output-root "$AF_TRANSFER_OUTPUT/evaluation"
```

`python -m dotenv` 只把本地凭据加载到进程环境，不执行 `.env` 中的 shell 命令。
评测器只向 Agent 转发 `pass_env` 允许的变量。Agent 源码来自当前仓库，
没有另一份实验 Agent，也不依赖 `build/trajectory-evolution/v1-coldstart-input/`。
命令固定当前 Python 解释器，Bridge 和子进程使用其开发环境，避免启动 shell 或 `.env` 把 Python 切换到缺少依赖的环境。

## 当前持久化运行

2026-09-15 02:55 UTC 重新启动正式实例。之前发现的评分载荷重复和 RPC 括号解析问题已修复；
真实复测 4/4 试次均完成执行和评分，8 个 Run 全部闭合（业务通过 2/4，未修改失败成绩）。
旧故障试跑已移到 build/policy-transfer-entry-H9glJJ/pre-rpc-evaluation，不计入本轮 60 试次。
正式产物收集同时覆盖实际 workspace 与上传副本的文件路径，防止相对布局不匹配漏掉文件。
持久化会话：

03:22 UTC 因三轮评分的重复输出载荷修复而续跑：原报告归档到
`build/policy-transfer-entry-H9glJJ/pre-compact-evaluation/`，使用新增 CLI 参数
`--resume-report build/policy-transfer-entry-H9glJJ/pre-compact-evaluation/report.json`。
这是同一配置下的断点恢复，不是复用旧实验；保留本轮已评分结果，只补评评分错误、重跑被取消题目并继续剩余任务。
最终报告会记录 `resume_source`、`reused_trials` 和 `regraded_trials`。

```bash
tmux -L policy-transfer attach -t v1-fresh
```

按 `Ctrl-b`，再按 `d` 可离开会话而不停止实验。不需要附加 tmux 也可以看日志：

```bash
tail -f trace_output/v1-policy-transfer-20260915/experiment.log
```

结果目录：`trace_output/v1-policy-transfer-20260915/evaluation/`。
其中只保存程序输出；本 README 和所有实验输入保留在 `configs/experiments/v1-policy-transfer/`。

## 预先固定的判定方式

先检查每题执行、采集与评分完整性，并检查 L0 无策略、L3 的版本 ID 与发布包一致。
再比较任务通过率、每题无效 loop 均值、LLM 调用次数和耗时。
无效 loop 相对下降率为 `(L0均值 - L3均值) / L0均值`；L0 为 0 时不计算百分比。
期望任务通过率不降低、无效 loop 均值至少降低 15%；是否达到以本轮真实结果为准。

模型和外部服务失败保留在报告中，不改写为成功，不删除不利题目，不将旧成绩混入。
基础设施故障需要修复时应留下说明；若改变 Agent、策略、任务或评分条件，另启新的完整对照。
