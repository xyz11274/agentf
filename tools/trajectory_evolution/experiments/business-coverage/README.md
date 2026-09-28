# Business Simulator 离线进化实验

本目录是自进化包的业务接入示例；组件核心仍在 `tools/trajectory_evolution/src`，不依赖 AgentFlow。业务 Agent 的实际启动与打点在 `examples/agent/business-simulator/`，本目录只负责冻结输入、调用组件 CLI 和管理三个离线阶段。

组件内核（`src/`、`include/`、`clients/`、`config/`）只定义语言无关的轨迹、记忆、评测与策略接口；本目录是一个**可选的 AgentFlow 业务实验适配层**。业务程序、Excel 原表与 AgentFlow 的启动代码仍归业务侧（`examples/agent/business-simulator/`、`test_cases/product/`）；包只保存冻结后的评测输入和阶段配置。GEPA 并不直接驱动 AgentFlow：通用评测器启动业务侧给出的入口，入口按原有 RPC/打点流程执行每一轮。

## 目录与职责

| 位置 | 保存内容 | 创建时机 |
| --- | --- | --- |
| `tools/trajectory_evolution/experiments/business-coverage/datasets/` | 输入数据集：原始 `source/`、已审核的训练快照 `training-reviewed/`、只含任务 ID 的 `gepa-view.json` | 构建或准备数据集时 |
| `tools/trajectory_evolution/experiments/business-coverage/coldstart-deployment/` | 训练配置、批次计划、提示词 | 训练准备时 |
| `tools/trajectory_evolution/experiments/business-coverage/gepa-deployment/` | GEPA 配置和只读输入指纹计划 | GEPA 准备时 |
| `tools/trajectory_evolution/experiments/business-coverage/archive/` | 旧场景覆盖与旧运行时生成配置；仅作历史记录，不参与当前三阶段 | 历史迁移 |
| `trace_output/business-simulator/training/` | 真实训练轨迹、分析、元反思、私有/公共经验及状态 | 训练运行时 |
| `trace_output/business-simulator/gepa/` | 搜索树、候选策略、val/holdout 评测、GEPA 反馈及状态 | GEPA 准备或运行时 |
| `trace_output/business-simulator/published/` | 通过验收的可复用策略包与版本登记 | 发布成功时 |

产物根只允许这三个阶段目录；未运行的阶段不预建目录。配置、执行计划和输入数据集不放进产物根，也不通过软链接或硬链接伪装成另一阶段的文件。现役配置持久化的项目路径使用仓库相对路径；历史归档配置保留原始路径作为出处，不可直接启动。旧适配测试结果已移至 `trace_output/business-simulator-legacy/`。

运行本例前先进入构建原生客户端所用的同一 Python 环境（本仓库开发环境为 `agentflow-dev`）。脚本运行时让评测子进程的 `python3` 与当前解释器一致；`--check` 会在调用模型前检查解释器、原生库、业务入口、daemon 和冻结输入。

## 第一阶段：训练轨迹到经验库

```bash
python3 tools/trajectory_evolution/experiments/business-coverage/coldstart.py --prepare
python3 tools/trajectory_evolution/experiments/business-coverage/coldstart.py
```

第一次准备会建立 `training/`、冻结训练输入并生成包内配置；已有计划和训练产物不会覆盖。运行器按已完成批次跳过旧轨迹，只继续未完成批次的采集、分析、反思和经验管理。当前这批 400 道业务训练题已完成；准备 GEPA 不需要重跑，也不会重新导入原 train。

## 第二阶段：训练记忆到 GEPA

不熟悉代码时可直接使用统一入口：

```bash
python3 tools/trajectory_evolution/experiments/business-coverage/offline_experiment.py --fresh
python3 tools/trajectory_evolution/experiments/business-coverage/offline_experiment.py --status
```

`--fresh` 会先检查训练记忆、数据集和运行文件，再只清理旧 GEPA 树，保留训练与输入数据；模型/单题失败会记录在当前任务并继续可恢复流程。实验指标口径和发布门槛见 [`tools/trajectory_evolution/docs/25-business-holdout-effect-audit-20260926.md`](../../docs/25-business-holdout-effect-audit-20260926.md) 与 [`EXPERIMENT_GOALS.md`](../../docs/EXPERIMENT_GOALS.md)。

```bash
python3 tools/trajectory_evolution/experiments/business-coverage/offline_gepa.py --prepare
python3 tools/trajectory_evolution/experiments/business-coverage/offline_gepa.py --status
python3 tools/trajectory_evolution/experiments/business-coverage/offline_gepa.py --check
python3 tools/trajectory_evolution/experiments/business-coverage/offline_gepa.py --run
```

`--prepare` 核对训练完成状态、训练数据和私有/公共经验的哈希；生成一个只引用原始数据集的轻量视图和阶段二配置，不启动模型评测。原 val 200 题按完整场景组固定划为 150 题搜索反馈和 50 题候选选择；视图中的内部 `train` 标签仅表示 GEPA 搜索题，不是原业务 train。原 holdout 200 题只做最终验收。

`--check` 是无模型、无作业的启动门禁；通过只表示运行环境与冻结输入可用，不承诺候选能通过业务验收或发布门槛。正常搜索由 `--run` 启动。本例准备时为兼容的火山 API 启用 JSON 响应格式，评分和反思仍关闭 thinking，不只在诊断命令里生效。

只更换裁判而已有同策略的完整执行时，先通过组件的 `dataset run --regrade-report` 复评原轨迹；准备新实验可使用 `--prepare --seed-report 相对路径/report.json` 引用复评后的完整种子选择验证报告。内核校验策略版本、数据集、选择题集合及裁判指纹，不复制轨迹，也不把旧分数直接用于新搜索。已有完整 L0 报告时，同样可附加 `--baseline-report 相对路径/report.json`，避免再次执行有效基线。两个参数都只用于准备新实验，不能在运行中的作业替换成绩。

已因裁判或执行异常停止的作业，修复后使用 `python3 tools/trajectory_evolution/experiments/business-coverage/offline_gepa.py --retry JOB_ID`。JOB_ID 来自 `--status`。运行器通过本机现有管理接口接续同一作业，持续等待搜索与反馈提交后正常关闭服务；不复制数据、不更换配置或重置预算。完整执行只补必要评分，已确定结果继续复用；整题失败但部分目标未知也纳入补评。

前台可用 Ctrl+C，后台可向本次运行器进程发送 SIGTERM。运行器把信号转发给自己创建的评测子进程，等待取消检查点保存后记为 `paused`；相同 `--run` 命令可恢复原作业和完成报告，不重置已用预算。不要用 SIGKILL 代替正常停止。

`--status` 的 `gepa.memory_feedback` 分别显示 `none`（无反馈）、`collected`（仅搜索检查点中有证据）、`pending`（等待原库提交）和 `committed`（已完成私库及公共台账同步）。`records` 是修订记录数，`record_states` 是各修订状态的计数；只有 committed 才显示提交回执中的 `memory_revision`。状态查询不会读取经验正文或启动模型。

运行器同时核对进程退出码、GEPA 作业结论及反馈回执。候选被业务门槛拒绝且反馈已处理时记为 `finished`，通过并已登记发布时记为 `candidate_ready`；执行错误、未决评分或反馈尚未交付时记为 `failed`，即使 CLI 退出码是 0 也不会误报完成。正常停止记为 `paused`。

GEPA 服务通过 `memory_source_dir` 直接复用 `training/evolution` 中的权威记忆，私有库和公共库都不复制、不链接。搜索冻结的是父子策略及其来源修订，而不是要求记忆库永远不变。配置和数据集仍严格核对哈希，初始记忆哈希保留为审计记录。

搜索完成后，包内管理层自动把有效搜索配对写回原私库的 `gepa_feedback`，并同步原公共台账和可再生视图。150 道搜索题的有效证据可以回流；50 道选择验证和 200 道留出题不回流。组合与改写记录绑定完整背景，不把收益拆给原经验。下一次编译可采用获得支持的修订作为新种子，仍需重新评测才能发布。`gepa/` 只保存搜索、评测和投递审计，不另建经验库。隔离试次只接收冻结候选策略，不获得原库写入路径。详情见包内[GEPA 闭环](../../docs/24-evolution-and-gepa.md#gepa-反馈回到唯一记忆库)。

## 第三阶段：版本发布

```bash
python3 tools/trajectory_evolution/experiments/business-coverage/offline_publish.py
```

仅在 L3 候选通过最终 holdout 门禁且组件产出批准的 bundle 后，命令才创建或更新 `published/`。版本文件保留在 `published/versions/`，`latest.json` 登记最新版本及来源哈希；没有通过门禁时不创建空的发布目录。
