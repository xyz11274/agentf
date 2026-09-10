# 19 · 评测集、原始基线与任务验收

[文档首页](README.md)

## 本章目标

构建有标准答案或可检查判分条件的任务集，测基线和候选，不把轨迹诊断当能力评分。

## 设计思路

轨迹评测分析已有执行记录；任务评测调用受信宿主运行固定任务，再按预设标准判分。

![评测集：任务、标准与冻结版本](figures/19-dataset.svg)

读图：同一个任务评测器服务离线对照、后台策略准入与 GEPA 多轮候选比较。 [放大 SVG](figures/19-dataset.svg)。

## 在后台评测真实 Agent，并控制策略发布

### 已使用组件打点的 Agent：组件自动装配评测

已使用原生 C ABI 或组件 HTTP 打点的 Agent，可在 variant 中配置：

```json
{
  "id": "baseline",
  "version": "my-agent-revision-1",
  "command": ["python3", "{config_dir}/agent_main.py"],
  "instrumentation_config": "boundary-service.json",
  "pass_env": ["MY_MODEL_API_KEY"]
}
```

command 是自己的测试实例入口，接收 stdin JSON 的 task.task_input、followup_inputs、initial_state 等任务数据。
task 的字段为 task_id、task_input、followup_inputs、initial_state、constraints 和 budget；
group_id、split、标准答案和判分规则不传给宿主。需要临时会话标识时可使用顶层 trial_id，不依赖 group_id。
入口执行原有业务代码与 point，无需写版本文件、汇总计数、读取轨迹文件或打印评测响应 JSON。
成功输出取任务 after 的 output；可检查状态放在 output.state，产物声明可放在 output.artifacts。
每个 trial 对应一个完整逻辑 run，多轮与并发活动都归入它。

组件负责：

1. 创建该 trial 专属目录、临时服务凭据和冻结版本文件，复制配置并关闭实例内的学习/公共晋升。
2. 将试验配置与凭据放入子进程环境。包内初始化自动使用它们，正常进程不受影响。
3. 执行业务入口，收集完整逻辑 run 的终态；不依赖 stdout 的格式，也不读取线上数据目录。派发进程退出后仍等待异步活动收尾。
4. 校验版本、用户、run 完整性，自动恢复标准轨迹，从真实 action.end / loop.decision 汇总指标。
5. 使用同一规则/模型评分与准入门槛。结束后清理该 trial 的临时工作目录，保存报告与验收通过的产物。

无 artifact 时此模式生成 L0 基线；后台 AgentEvaluationService 会自动提供 L0 或 L1–L3 的冻结 artifact。
instrumentation_config 也进入后台评测配置指纹，修改后旧准入结果不继续沿用。
没有终态快照、版本不符或有采集缺口的试验记为评测错误，不记作能力通过。
异常退出时仍尝试保存已采集的结构化证据：完整、版本一致的轨迹保留在 evidence.trace，
并从实际事件汇总 metrics；不完整轨迹放入 evidence.failure_capture，包含 complete=false 和 validation，
不将部分计数填作完整试验指标。没有可用记录、身份不符或隐私检查未通过时，只记录
failure_evidence_unavailable 警告。原始 stdout/stderr 不作为故障证据保存；试跑原有错误码、超时判定和发布门禁保持不变。
`instrumentation_transport` 默认为 `native`；使用配置式 HTTP 打点时设置为 `http`，评测器会启动独立的本地临时采集服务，
通过 `TRAJECTORY_EVOLUTION_TRIAL_URL/CONFIG/TOKEN` 指向试验实例，包内配置式客户端自动读取，避免流入生产服务。
完全自定义的远程测试宿主仍可使用下文的任务宿主协议。

`AgentEvaluationService` 复用本章的任务执行器和判分标准。评测对象是“固定版本下 Agent 的完整执行”，
包括经验注入、动作选择与循环控制的实际效果。轨迹分析负责从过去的执行中提出改进；这里重新运行固定任务，检验改进是否有效。

| 使用位置  | 交给评测器的内容                 | 评测结果的用途                                       |
| --------- | -------------------------------- | ---------------------------------------------------- |
| L0        | 原始 Agent，`policy=null`      | 建立质量、调用次数、时延与无效循环基线               |
| L1        | 冻结的经验快照和精确授权         | 验证增强输入后的任务效果                             |
| L2        | 同时包含动作规则的冻结策略       | 验证放行、拒绝及任务完成情况                         |
| L3        | 同时包含循环规则的冻结策略       | 验证停止、重规划与任务完成情况                       |
| L4 优化器 | 提交一个新的 L1/L2/L3 可执行候选 | 复用同一数据集、执行器、评分器和准入标准进行多轮比较 |

L4 是实验编排的调用方；候选的 `level` 仍是其实际使用的 L0–L3 运行能力。
既有 `dataset run` 保持纯评测模式。自动提交、发布登记、记忆反馈由管理层完成。

### 第一步：准备独立的测试入口

评测宿主是你提供的一个程序，Python、C++、Rust、TypeScript 等均可：从 stdin 读一条 JSON，
运行同一套 Agent 实现，从 stdout 返回一条结果 JSON。调试信息使用 stderr。
测试入口需要准备独立工作目录、测试账户、工具数据和模型调用额度，不连接线上任务队列或业务写入目标。

每次试验都会启动独立子进程。宿主收到的额外字段是：

```json
{
  "version": "agent-version-v1:<64位SHA256>",
  "artifact": {
    "schema_version": 1,
    "agent": {"revision": "my-agent-2026-09-v1"},
    "user_id": "alice",
    "level": "L1",
    "policy": {"说明": "完整 RuntimePolicy，由组件提供；不要手写此示意对象"}
  },
  "evaluation_mode": true
}
```

原有 `task`、`trial_id`、`variant_id`、`repetition` 一并发送。
`task.followup_inputs` 要在同一测试会话内逐轮执行；异步或多 Agent 场景必须等待全部所属分支终态和最终聚合结果，再返回 stdout。
Coordinator 的派发进程退出只意味着派发结束，不能代替整个测试任务结束。

若 Agent 已接入本组件的原生或 HTTP 边界，测试宿主按下面方式加载候选：

1. 在本次工作目录创建 `version.json`，内容为 `{"version": request.version, "artifact": request.artifact, "trial_id": request.trial_id}`。
2. 复制你的服务配置用于测试，将 `evaluation_version_file` 指向该文件，`data_dir` 设为这个目录内的子目录，例如 `state/`。
3. 使用当前试验的用户身份启动测试实例，`run.begin.level` 使用 `artifact.level`；模型、动作、循环插入点继续复用业务实现。
4. 原生模式等待采集队列落盘；HTTP 模式等待整个 run 结束。返回该 run 的完整 trace 和 `run.begin` 返回的 `agent_version_id`。

测试实例会关闭自身的自动学习、公共库管理和候选评测调度，避免评测递归以及测试数据进入生产记忆库。
`evaluation_version_file` 是受信部署配置，生产 API 请求不能指定任意文件路径。
服务数据目录必须位于版本文件所在目录下，并且与该目录不同；不要把生产 `data_dir` 传给测试实例。

返回结果仍使用本章的验收协议，并增加：

```json
{
  "schema_version": 1,
  "trial_id": "收到的trial_id",
  "loaded_version": "run.begin返回的agent_version_id",
  "execution_status": "completed",
  "output": "实际最终答案",
  "state": {"实际验收状态": "由测试设施测得"},
  "metrics": {"llm_calls": 8, "tool_calls": 5, "tokens": 12000, "steps": 10},
  "trace": {"说明": "实际采集的完整trajectory.v1对象"}
}
```

上面的说明对象需替换为真实对象。宿主不接收 rubric、预期答案或判分规则。
`loaded_version` 必须匹配请求；轨迹 `run.start.attributes` 还必须同时包含
`agent_version_id` 和 `evaluation_trial_id`。轨迹保留 Agent 自己的 run ID，无需改成试验 ID。
后台准入强制检查完整轨迹、身份、版本绑定、执行结果一致性和敏感输出。
被测 Agent 的部署修订、模型及工具版本通过 `agent` 描述并由宿主实际加载；更换它们时更新 `agent.revision`。

源码仓库中的接入测试宿主位于 `tests/agent_evaluation_host.py`。它使用真实原生采集与策略接口，
业务模型是确定性测试替身，用于验证版本绑定和管理机制；正式评测应替换为你的 Agent 入口和真实业务模型。

### 第二步：配置固定任务、评分标准与资源预算

先按下文构建并检查已审核的任务集。复制 [evaluation.json](../config/evaluation.json)，配置：

- `variants`：只保留一个受信宿主入口，`command` 为 argv 数组，不经过 shell；管理层自动扩展成基线与候选。
- `checks`：每个任务的确定性验收标准；`judge`：可选模型评分器及预算，复用原有规则优先、模型补充的评分逻辑。
- `splits`、`repetitions`、`max_trials`：固定测试范围和样本规模。候选比较优先使用 val，holdout 留给独立验收。
- `timeout_ms`：可配置的单次宿主超时，同时受任务自身 budget.timeout_ms 约束，使用两者较小值。
- `parallelism`：后台最多 4，建议从 1 开始。`pass_env` 只列测试入口需要的环境变量名，不在 JSON 中保存密钥。
- `resources`：可配置 `nice`、`address_space_bytes`、`cpu_seconds`。未提供时后台使用 nice=10、1 GiB 地址空间、300 CPU 秒；网络等待由 timeout_ms 限制。

例如单机小规模测试可配置：

```json
"resources": {"nice": 10, "address_space_bytes": 2147483648, "cpu_seconds": 600}
```

资源限制在发送任务前应用到测试进程，子进程继承；整棵进程树和外部 GPU/API 资源使用容器或 cgroup、
独立测试服务及额度池管理。评测不在主 Agent 的请求链上执行，但共享机器或共享 API 配额仍会有资源竞争，部署时需要分别分配。

### 第三步：启用自动准入

复制 [agent-evaluation.json](../config/agent-evaluation.json)，修改 dataset、evaluation_config、agent.revision。
相对路径均以所在配置文件的目录为基准。然后在自己的 service 配置中加入：

```json
"policy_evaluation_file": "agent-evaluation.json"
```

如需自动将新合格经验提交为候选，同时设置 `auto_approve_experiences=true`；也可继续使用精确经验授权列表。
这项配置只决定候选包含哪些经验，启用准入后仍须通过任务评测才能发布。
未配置 policy_evaluation_file 的已有部署保持原授权行为，便于兼容升级。

启动同一个 `serve --config ...` 服务，或照常构造原生 LocalEvolution 即可。后台自动完成：

1. 读取各用户的私有加公共合格视图，冻结 L0 至 max_level 的候选。
2. 按“用户＋行为版本＋父版本＋评测配置/数据版本”去重入队。
3. 在独立测试宿主中执行。L0 只运行原始 Agent；其他级别与已发布的同级策略比较，首次与原始 L0 比较。
4. 核验覆盖率、质量、配对样本及可选成本/无效循环目标，记录通过、拒绝或证据不足。
5. 当前最新候选通过且父版本仍一致，才登记为已发布版本；后台刷新供新 run 读取的内存视图。
6. 回写记忆的版本级反馈和满足条件的单条经验评测状态，规则见[私有记忆](13-private-memory.md)。

启用 GEPA 时，第 3 步扩展为训练/验证的多轮搜索和最终留出比较；候选试验不回写生产记忆状态。
其任务去重绑定 seed 与实验上下文，不因发布改变 parent 而反复启动，详见[GEPA 专章](24-evolution-and-gepa.md)。

前台不等待评测结束：请求 L1–L3 时使用仍然兼容的已发布同级版本；尚无合格版本则返回
`level=L0, policy_gate=awaiting_evaluated_policy`，Agent 按原始逻辑继续。
已有 run 固定自己的策略；通过评测的候选只影响后续 run。
已授权经验被删除、改写、撤权或产生不兼容关系时，旧版本不会继续作为新 run 的可用策略。

### 准入指标怎么配置

| gate 字段                  | 默认值 | 处理规则                                                             |
| -------------------------- | ------ | -------------------------------------------------------------------- |
| min_trials                 | 2      | 至少达到此数量的确定评分试验；比较时还要求配对样本充足               |
| memory_min_pairs           | 2      | 单条经验自动验证/归档所需的配对改善或退化次数                        |
| min_coverage               | 1      | 基线与候选的确定评分覆盖率门槛                                       |
| min_pass_rate              | 0.8    | 候选确定评分试验的最低通过率                                         |
| max_pass_rate_drop         | 0      | 相对基线可容忍的通过率下降幅度                                       |
| max_latency_ratio          | 未启用 | 候选平均 Agent 时延 / 基线时延上限                                   |
| max_token_ratio            | 未启用 | 候选平均 token / 基线 token 上限；缺计数时拒绝此项                   |
| min_invalid_loop_reduction | 未启用 | 规则确认无效 loop 下界的下降比例，例如 0.3；缺有效轨迹指标时拒绝此项 |

质量先满足门槛，成本改善不能抵消质量失败。基线无效 loop 为 0 时，候选也需为 0；报告保留原始数值。
L0 用于基线测量，不要求相对自身取得成本或循环改善。
错误、取消和模型评分不确定会降低覆盖率；证据不足为 inconclusive，不计作经验反证。
宿主超时是一次未按预算完成的失败，基础设施不可用应返回 environment_error。

报告同时提供总试验数、确定评分数、通过/失败/错误/不确定数量、覆盖率、各变体与分类汇总、
配对胜负、平均/P95 Agent 时延、LLM/工具调用数及 tokens。未知计数不补成 0。
`summary.model_calls` 是评分器自身的模型调用数，`host_reported_metrics.llm_calls` 是 Agent 的模型调用数，两者分别记录。
对于 `evolution.boundary.v1` 轨迹，组件从实际完成的打点重新计算模型、工具、步骤和循环次数；
若宿主另报了不同的计数，使用轨迹计数并记录 `host_metric_mismatch` 警告。
计数覆盖已接入的边界，不推测未打点的内部调用；只有 run 起止记录的 L0 轨迹不能用于衡量内部调用效率。
模型 `after.metrics` 提供实际 usage 后才汇总 tokens，缺失 usage 不用宿主估计值补齐。

### 查看状态、重试与复用接口

管理请求发送到 HTTP `/v1/invoke`，使用当前用户服务令牌；不放在模型、工具或循环的同步链路中：

```json
{"operation":"agent_evaluation.status","request_id":"status-1"}
{"operation":"agent_evaluation.feedback","request_id":"feedback-1"}
{"operation":"agent_evaluation.retry","request_id":"retry-1","job_id":"待重试的64位ID"}
{"operation":"agent_evaluation.cancel","request_id":"cancel-1","job_id":"待取消的64位ID"}
```

`agent_evaluation.submit` 的额外字段是完整 `artifact`，可供候选生成器或 L4 优化器提交。
配置 `gepa_config` 后，这个提交入口自动启动 GEPA，树通过 `agent_evaluation.tree` 查询。
训练小批次使用可选 `evaluation.json.task_ids` 选择指定任务，必须与 `splits` 一致，未知/重复 ID 拒绝。
完整的搜索、预算、验收与恢复说明见[GEPA 专章](24-evolution-and-gepa.md)。
请求不能设置执行命令、文件路径或凭据，也不能跨用户访问任务。`status.jobs` 返回当前管理索引，
`status.publications` 按等级返回已发布 job/version；`feedback` 按 memory_id 和 content_revision 关联试验。
原生热路径 `invoke` 不执行有 I/O 的管理操作。原生部署在管理线程使用 `evo.manage("agent_evaluation.status")`，
重试例如 `evo.manage("agent_evaluation.retry", {"job_id": job_id})`。C++ 使用 `LocalRuntime::manage(JSON)`，
其他语言通过 C ABI `te_local_manage(handle, utf8_json, length)` 调用同一个已有实例，返回字符串用 `te_local_free` 释放。
manage 不执行 Agent 任务本身，但可能等待管理锁及落盘；不要放入 Agent 的同步执行链路。
独立实验编排代码还可以直接使用 `AgentEvaluationService`（头文件 `trajectory_evolution/agent_evaluation.h`），
每个数据目录只由一个管理实例持有，不能在原生运行时之外重复打开相同目录。

默认每次启动评测至少间隔 60 秒、每小时最多 12 个版本评测，后台一次处理一个版本；空队列等待通知。
同用户同级待运行候选被新候选替代时记为 superseded；正在执行的旧候选保留报告，完成后重新核验父版本与最新候选再决定发布。
重启将 running 标为 interrupted，需要显式 retry；error/inconclusive 也可以 retry，成功和业务拒绝不自动重试。
取消会终止正在运行的测试进程组；已经进入模型评分 HTTP 请求时，收尾受评分器 HTTP 超时约束。

`max_jobs` 默认 256，约束内存管理索引；已发布、各级最新和活跃任务保留在索引内，较早终态任务转存 job.json，
完整试验报告继续保留，并维持已完成任务去重。全为受保护任务时返回容量错误。
历史报告属于实验审计资料，磁盘保存周期由部署的归档策略管理，容量告警和处理方法见[运维](21-operations.md)。

### 结果保存在哪里

```text
data_dir/
  agent-evaluation/
    state.json                 # 任务状态、父版本、准入结果、发布登记、反馈索引
    <job_id>/
      candidate.json           # 冻结候选
      baseline.json            # 冻结对照
      job.json                 # 较早终态任务移出活跃索引后的记录
      attempt-1/
        report.json / report.pb / report.md
        results.jsonl
        trials/<trial_id>.json
        artifacts/<trial_id>/
  by-user/<user>/memory/
    evolution-memory.pb        # 记忆状态的权威持久化文件
    evolution-memory.json      # 可读镜像，含 evaluation_feedback
    evaluation-state.json      # 当前用户的完整评测反馈镜像
```

Agent、数据集、评分配置或评分模板变更后重启评测服务，使用新的评测上下文。旧结果保留用于审计，
不会作为新上下文下的准入结果；正在运行的评测前后均检查配置指纹，变更期间完成的结果不发布。

## 构建评测任务集

`dataset` 是同一个程序的离线子命令，借鉴 Hermes 的人工用例、模型出题、历史会话提取三种来源。
它**只构建并冻结任务集，不调用你的 Agent，不修改记忆，不启动 GEPA**。任意语言可以启动此命令，读取 UTF-8 JSONL；
C++ 可使用 `include/trajectory_evolution/dataset.h`，其他语言也可根据 [Protobuf](../proto/evaluation_dataset.proto) 生成读写代码。

### 先生成随包附带的 24 条中文起步任务

在项目根目录运行，不需要 API Key：

```sh
cmake --build build/trajectory-evolution --target trajectory_evolution -j 4
build/trajectory-evolution/trajectory_evolution dataset build \
  --config tools/trajectory_evolution/config/dataset.json \
  --output-root trace_output/datasets/agent-evaluation-starter-v1

build/trajectory-evolution/trajectory_evolution dataset check \
  trace_output/datasets/agent-evaluation-starter-v1
```

如果目标目录已存在，直接执行 `check`，或为新的版本选择新目录；程序**不覆盖**已有目录，且在发出模型请求前就检查。
安装包用户把命令替换为安装后的 `trajectory_evolution`，配置位于 `<prefix>/share/trajectory_evolution/config/dataset.json`。
配置内相对路径以配置文件目录为基准；`--model-config` 和 `--output-root` 的相对路径以命令工作目录为基准。

输出只有以下五个文件，不写入 SessionLog 或在线学习队列：

```text
agent-evaluation-starter-v1/
  manifest.json       # 版本、统计、错误/警告、文件校验值；最后写入的完成标记
  dataset.pb          # 完整任务集和来源信息
  train.jsonl         # 训练任务，每行一条
  val.jsonl           # 搜索验证任务，每行一条
  holdout.jsonl       # 最终留出任务，每行一条
```

随包用例覆盖并发失败隔离、逆序合流、依赖顺序、必要重试、权限失败、循环停止、多轮修改、会话隔离、
图片/视频产物核验、结构化输出、代码测试、幂等提交和隐私边界。默认得到 **14 train / 5 val / 5 holdout**，共 21 个场景组。
起步用例的状态为 `pending`。将声明式初态绑定到测试环境、复核验收标准后，再执行任务评测。

### 想用自己的数据，修改 sources 即可

复制或编辑 [dataset.json](../config/dataset.json)，保留其他配置，只替换 `sources`。三种来源可以混合：

```json
"sources": [
  {"kind": "golden", "path": "my-tasks.jsonl"},
  {"kind": "synthetic", "path": "agent-capabilities.txt", "count": 20, "batch_size": 5},
  {"kind": "traces", "path": "my-traces", "max_files": 100}
]
```

| 来源      | 你提供什么                                                                  | 是否调用模型 | 结果如何处理                                                                             |
| --------- | --------------------------------------------------------------------------- | ------------ | ---------------------------------------------------------------------------------------- |
| golden    | 人工编写或已经导出的任务 JSONL                                              | 否           | 校验任务，保留原有来源与暴露标记，按场景组划分                                           |
| synthetic | Agent 能力说明、技能说明或任务范围文本                                      | 是           | 分批出题；程序设为 pending、训练用途，审核与集合划分由程序管理                           |
| traces    | 标准`.trace.json` / `.trace.pb`，或兼容 SessionLog `.jsonl` 文件/目录 | 是           | 先复用现有规则检查与内容脱敏，再从具体证据提取任务；同用户同会话同组，保守限制为训练用途 |

`traces` 默认每个有效 Run/Dialogue 提取一条任务；同一会话不会拆入不同集合，但本版不自动恢复完整多轮环境。
若要完整多轮用例，应审核 `followup_inputs` 并补齐历史状态。目录超过 `max_files` 会明确报错，不默默抽取部分文件；
不跟随目录里的符号链接。某个文件或任务失败会记录在 `manifest.metadata.issues`，其他有效来源仍保留。

模型出题模板位于 [dataset-system.txt](../config/prompts/dataset-system.txt)，每次构建重新读取，不用编译。
模型连接复用现有 `runtime_config`；也可通过 `--model-config your-runtime.json` 指定已有配置。
其中 endpoint/model_id 非空时优先使用；为空时才读取 `OPENAI_BASE_URL` / `OPENAI_MODEL`，Key 从 `api_key_env` 指定变量读取
（未指定时读取 `OPENAI_API_KEY`）。启动前加载 `.env` 或秘密管理器，凭据通过环境变量提供。
生成输出上限使用 dataset 配置的 `generation_max_output_tokens`，默认 8192；连接超时、总请求超时、thinking 和重试沿用模型配置。
`budget.timeout_ms` 控制测试任务的执行时限；出题模型的 HTTP 超时通过模型配置设置。

`max_model_calls` 限制构建器发起的逻辑模型调用，默认 8；每次调用的传输重试另由 `model.max_retries` 控制。
明确重跑 `build` 会重新调用模型；想复用已经生成的任务，应把对应 JSONL 作为 golden 输入，程序会保留 synthetic/traces 来源。
模型失败、JSON 无效、输出截断或生成数量不足时，构建记录对应错误；manifest 保存错误码，原始模型响应不落盘。

### 自己写一条任务

下面是单条记录的展开形式；保存为 JSONL 时，一行写一个完整对象：

```json
{
  "task_id": "parallel-partial-demo",
  "group_id": "parallel-partial-family",
  "task_input": "并行读取 A 和 B 的销量，汇总可取得的数据并说明缺失项。",
  "followup_inputs": [],
  "expected_behavior": "返回 A 的销量 12，明确 B 权限不足，不把 B 当作零销量，不重复请求 B。",
  "category": "parallel_failure",
  "difficulty": "medium",
  "initial_state": {"A": {"sales": 12}, "B": {"error": "permission_denied"}},
  "constraints": ["只操作隔离的测试数据，不影响其他分支。"],
  "review_status": "pending"
}
```

字段含义：

| 字段                         | 使用方法                                                                                             |
| ---------------------------- | ---------------------------------------------------------------------------------------------------- |
| task_id                      | 可选稳定标识；未填写时程序按任务输入、后续输入、初态生成。每个 ID 对应一个确定任务                   |
| group_id                     | 必填。把同一会话、同一原题的变体、有关联的多 Agent 任务设成相同值，防止分组泄漏                      |
| task_input / followup_inputs | 首轮用户输入与后续用户输入顺序；与验收标准分开保存                                                   |
| expected_behavior            | 用于独立验收的任务成功标准                                                                           |
| category / difficulty        | 自定义能力类别，以及 easy / medium / hard                                                            |
| initial_state                | 声明式初始数据、工具能力、预设错误和缺失条件；宿主负责把它绑定到测试环境，程序不会执行里面的命令     |
| constraints / budget         | 约束与步骤、工具调用、任务时间预算；缺少 budget 时使用配置默认值                                     |
| provenance                   | 来源类型、来源内容哈希、是否已经参与学习，以及可选的生成模型与模板指纹；由构建器补充，重新导入时保留 |
| review_status                | pending 或 reviewed；只有导入方完成检查后才应在源任务中改为 reviewed，模型没有此权限                 |
| split                        | 可选指定 train / val / holdout；同组冲突或已用于学习却指定 val/holdout 会拒绝                        |
| task_version                 | 程序生成的完整任务内容版本；修改输入、标准、预算、来源或划分后会改变                                 |
| attributes                   | 额外业务字段，例如 fixture_status，按统一隐私规则检查                                                |

精确协议见 [JSON Schema](../api/evaluation-task-v1.schema.json)。Protobuf JSON 会省略空数组、false 等默认值，
learning_exposed 省略时使用默认值 false。中文内容按 UTF-8 处理；单个主输入或评分标准最多 32768 字节，
任务序列化后最多 128 KiB，最多 32 条后续输入。超限拒绝，不悄悄截掉问题；轨迹证据则沿用现有脱敏/限长机制，
可能包含截断标记，因此提取的任务仍需复核，尤其是缺失附件、前轮状态和动态工具结果。

真正执行时，只向被测 Agent 发送 task_input 和对应轮次的 followup_inputs；initial_state 交给宿主构建测试环境，
expected_behavior 仅交给评测器；被测 Agent 接收任务输入，测试设施按 initial_state 准备环境。

### 划分、审核与版本的边界

- 使用 `split_seed + group_id` 的 SHA-256 桶按 60/20/20 的目标比例分配；小规模集合可能存在比例偏差，空集合会报告警告。
  新增无关组保持已有组的划分不变；显式 split 优先，并经过同组隔离和学习暴露检查。
- 精确重复输入、后续输入及初态会合并，并合并其场景组；不同评分标准或显式划分冲突会拒绝整个构建。
  近义改写通过共同 group_id 归组。任务用于记忆或经验生成后，将 provenance.learning_exposed 设为 true，
  或在配置中加入 `"learning_groups": ["已见过的组"]`，将整组限制为 train。外部学习使用情况由数据提供方标记。
- 当前生成器保守地将 synthetic/traces 作为已暴露搜索材料；不会因你把它重新导入 golden 而洗掉标记。
  留出集使用独立任务，并保留其来源信息。
- 修改源 JSONL 的审核状态后重建到新目录。用 `dataset check DIR --require-reviewed` 检查是否全量复核完成；
  review_complete 标记任务审核状态；执行评测时另行配置宿主入口。l4_ready 为预留字段，取值 false。
- `dataset_version` 是规范化内容哈希，包含全部任务、划分与构建记录；`manifest.files` 是各文件校验值。
  check 校验 PB、JSONL、元信息和版本的一致性；发布与存储权限由部署环境保护。
- 输出目录仅新建，目录权限 0700、文件 0600；manifest 最后写入。中断可能留下不完整目录，换新目录重试，
  check 会报告不完整目录；构建使用新输出目录，错误输出按凭据保护规则处理。
- 退出码：0 为构建/校验成功，3 为部分任务被拒绝、空任务集或 require-reviewed 未通过，2 为配置/契约/冻结文件错误。
  任务构建完成后使用 dataset run 执行。已知凭据模式、环境密钥和敏感字段在输入检查中拦截；
  上传模型前由数据所有者确认材料使用权限。

构建完成后，可以使用下一节的离线任务评测执行真实任务。它不要求启用进化，也不会写入 Memory。

![一次 Trial 的四类结果](figures/19-scoring.svg)

读图：complete 表示全部计划试验均有终态，成功率按验收结果统计。 [放大 SVG](figures/19-scoring.svg)。

## 执行评测任务，不启用进化

`dataset run` 会加载固定任务集、运行你提供的 Agent 测试入口、检查结果，并生成报告。
支持重复试验、有限并发、规则验收、模型验收和多个显式版本的配对比较；不会改写 Agent、生成记忆或自动发布策略。
这与 `evaluate SESSIONLOG` 不同：后者分析已经存在的日志，本节命令会真正执行任务。

### 先运行随包的真实模型示例

示例是独立工具型 Agent，不依赖 AgentFlow、SessionLog 或已有 Hook。它实际调用文本模型、并行工具和图片生成接口。
示例任务覆盖三个数据源的失败隔离汇总、三轮预算修改，以及图片下载与解码，均带有已审核的验收标准。
核心执行器是 C++；仅这个示例需要 Python 3 和 Pillow（`python3 -m pip install Pillow`）。

在当前终端通过环境变量配置供应商：

```sh
export OPENAI_BASE_URL="https://ark.cn-beijing.volces.com/api/plan/v3"
export OPENAI_MODEL="deepseek-v4-flash"
export ARK_IMAGE_MODEL="doubao-seedream-5.0-lite"
# OPENAI_API_KEY 由你已有的密钥环境提供；程序不自动读取 .env。
```

在项目根目录执行：

```sh
cmake --build build/trajectory-evolution --target trajectory_evolution -j 4

build/trajectory-evolution/trajectory_evolution dataset build \
  --config tools/trajectory_evolution/examples/evaluation-dataset/demo-dataset.json \
  --output-root trace_output/datasets/evaluation-execution-demo-v1

build/trajectory-evolution/trajectory_evolution dataset run \
  trace_output/datasets/evaluation-execution-demo-v1 \
  --config tools/trajectory_evolution/examples/evaluation-dataset/agentplan-evaluation.json \
  --output-root trace_output/evaluation/my-first-evaluation
```

已有同名数据集时跳过 build。每次运行使用新的结果目录，不覆盖旧结果、不自动重跑失败任务。
安装包用户使用安装后的 `trajectory_evolution`，示例位于 `<prefix>/share/trajectory_evolution/examples/evaluation-dataset/`。
示例会消耗订阅额度：每次完整执行至多请求一张图片，文本请求数量随 Agent 的工具轮次变化；评测模型每题一次，
默认最多再进行一次格式修复，因此三题最多六次判分调用，仍受 judge.max_model_calls 的总预算限制。
普通单元测试不调用这些真实服务。

图片接口使用 `POST <OPENAI_BASE_URL>/images/generations`，保留 `/api/plan/v3`；不会回退到 `/api/v3`。
参数按 [火山 Agent Plan 接入视觉模型](https://www.volcengine.com/docs/82379/2375486?lang=zh) 和
[图片生成 API](https://www.volcengine.com/docs/82379/1541523?lang=zh) 的接口形式组织。
示例优先获取 Base64；若返回临时图片 URL，只向允许的 HTTPS 图片域下载，不携带 API Key，也不保存临时签名 URL。
Pillow 完整解码后统一保存 PNG；任务验收只声明图片交付和解码成功，不把尺寸检查当作画面语义评测。

### 换成自己的 Agent：只实现一个测试入口

不需要修改在线主流程或接入本项目的 Hook。提供一个可执行程序：从 stdin 读取一个 JSON 请求，完成整个测试任务后，
只向 stdout 输出一个 JSON 响应。调试信息写 stderr。这个程序可以直接调用 Agent，也可以启动异步任务并等待关联结果。

复制 [evaluation.json](../config/evaluation.json)，替换 `variants[0].command`，并填写自己的版本标识：

| 宿主语言                | command 示例                                       |
| ----------------------- | -------------------------------------------------- |
| Python                  | `["python3", "{config_dir}/my_eval_host.py"]`    |
| TypeScript / JavaScript | `["node", "{config_dir}/dist/eval-host.js"]`     |
| Rust / C++ / Go         | `["{config_dir}/bin/eval-host"]`                 |
| Java                    | `["java", "-jar", "{config_dir}/eval-host.jar"]` |
| C#                      | `["dotnet", "{config_dir}/EvalHost.dll"]`        |

其他语言只要能读写 UTF-8 JSON 并作为进程运行，也使用同一协议，无需语言 SDK。当前 C++ 进程监督实现运行于 Linux。
command 使用参数数组调用，`{config_dir}` 解析为配置文件目录；每次试验使用独立工作目录。
需要的凭据或模型变量通过 `pass_env` **填写变量名**白名单；默认只继承 PATH、LANG、LC_ALL、TZ，不继承整份服务环境。

收到的请求示意如下，字段来自程序，不需要自己构造：

```json
{
  "schema_version": 1,
  "trial_id": "0123456789abcdef0123456789abcdef",
  "variant_id": "baseline",
  "repetition": 1,
  "task": {
    "task_id": "sales",
    "task_input": "汇总销量并生成 report.json。",
    "followup_inputs": ["请标明缺失数据。"],
    "initial_state": {"east": 12, "west": 8},
    "constraints": [],
    "budget": {"max_steps": 16, "max_tool_calls": 32, "timeout_ms": 1800000}
  }
}
```

你的入口需要做四件事：

1. 按 initial_state 准备文件、工具数据或模拟故障。外部数据库等状态需要自己隔离并清理；独立 cwd 不会自动隔离外部服务。
2. 为每个 trial 创建独立会话，依次发送 task_input 和 followup_inputs；重复试验使用各自的会话与固定初态。
3. 等待任务及必要并发分支完成，使用 trial/run 标识关联最终结果。
4. 收集真实结果、环境状态、计数、可选标准轨迹和文件，返回下面的 JSON。

```json
{
  "schema_version": 1,
  "trial_id": "0123456789abcdef0123456789abcdef",
  "execution_status": "completed",
  "output": "已汇总可用数据，总销量20，缺失项已标明。",
  "state": {"verified_total": 20},
  "metrics": {"steps": 3, "tool_calls": 4, "loop_count": 3, "tokens": 1200},
  "artifacts": [{"path": "report.json", "media_type": "application/json"}]
}
```

output 支持任意 JSON 值；state 由宿主测试设施检查后填写。
execution_status 使用 completed / failed / cancelled / environment_error，根据整次执行填写。
执行状态和业务验收独立保存；评测器按标准检查可用结果，并记录执行警告。
environment_error/cancelled 单独记为未完成评分，保留样本与执行原因。

metrics 可省略，缺失不会填零；步骤和工具数有值时执行预算验收，运行中仍由宿主实施这些限制。
可选 trace 使用本包 `trajectory.v1` 标准对象，run_id 必须等于 trial_id，结束状态与最终输出必须与响应一致。
require_trace=true 时缺失或无效轨迹按验收错误处理；false 时可评测黑盒任务的输出。
程序不会自动从 stdout 文本猜测工具、并发关系或循环轮次。

预期答案、train/val/holdout 划分、审核信息和判分规则不会出现在宿主请求中。宿主必须把 initial_state 留给环境准备，
被测模型仅接收任务输入；评测配置、预期标准和结果目录由测试设施管理。
配置的宿主程序应经过部署方审核，工作目录与时间隔离之外的权限约束由部署环境提供。

### 怎样配置规则和模型验收

规则位于配置 `checks`，键是 task_id，`*` 表示对所有选中任务执行；两者合并。

```json
"checks": {
  "sales": [
    {"id": "total", "path": "/artifacts/report.json/json/total", "op": "equals", "value": 20},
    {"id": "missing", "path": "/artifacts/report.json/json/missing", "op": "contains", "value": "north"}
  ]
}
```

path 是相对于 evidence 的 JSON Pointer，支持对象、数组，以及 `~0` / `~1` 转义。
op 支持 exists、equals、contains、at_most、at_least；exists 省略 value，缺失目标按规则失败处理。
每个选中任务需配置显式检查规则或模型验收，并在任务审核时核对成功标准的覆盖范围。

`judge.enabled=false` 为纯规则；true 为规则加模型（无规则时为纯模型）。模型使用本包现有 OpenAI-compatible 适配器，
从 `judge.runtime_config` 或 `--model-config` 读取配置；外置模板是 [task-judge-system.txt](../config/prompts/task-judge-system.txt)。
模型接收任务、后续输入、expected_behavior 和经检查的具体证据；超过 judge.max_input_bytes 时返回错误。
模型输出包含中文理由、passed/failed/uncertain，以及 allowed_evidence_paths 中的证据路径。
模型故障、截断、引用错误或预算耗尽记为 error，证据不足记为 uncertain；规则明确失败时直接保留失败结论。
完整 JSON 围栏在移除标记后解析，其余响应按结构要求校验。
格式或引用校验失败时，judge.max_repair_attempts 控制额外修复次数（默认1，可设0关闭，最多3）；每次计入总模型预算，
修复请求使用固定校验错误与原始证据，保持任务执行、产物和验收标准不变。
checks 中的 attempts 记录实际发出的判分请求次数。传输重试仍独立由 runtime.model.max_retries 控制。

三个时间/数量预算分开配置：

- `timeout_ms` 与任务 budget.timeout_ms 取较小值，限制宿主进程整体墙钟时间；到期终止其进程组，记任务超时失败。
- `budget.max_steps/max_tool_calls` 由宿主在执行中限制，评测器对返回计数再验收；缺少计数明确标记不可观测。
- 模型 HTTP timeout、thinking 和传输重试在 runtime.model 中配置；judge.max_model_calls 控制逻辑评测调用数。

Agent Plan 示例的模型 HTTP timeout 为 30 分钟、thinking 为 disabled、传输重试为 0；均可编辑配置，无需重新编译。
示例 Agent 的 API timeout 由 AF_EVAL_API_TIMEOUT_SECONDS 控制，默认1800秒。

`repetitions` 控制每题重复次数，`parallelism` 控制并发试验数；`variants` 可增加手工准备的第二版 Agent。
所有版本运行同一批选中任务和重复编号，按标识配对，不按完成先后配对。第一版是比较基线。
这只是固定版本对照，不会生成候选、调用 GEPA 或自动发布。模型、工具、初始记忆等外部条件应由宿主固定并体现在 version 标识中。

### 报告在哪里、如何理解

打开结果目录的 **report.md**，它会列出每条任务的执行状态、验收状态和详情链接。

```text
my-first-evaluation/
  started.json        # 启动记录；不是完成标记
  manifest.json       # 所有计划试验都有终态后最后写入，含文件哈希
  report.md           # 人可读报告
  report.json         # 完整报告与逐次证据
  report.pb           # 同一报告的 Protobuf
  results.jsonl       # 每行一次试验，按计划顺序输出
  trials/<trial_id>.json
  artifacts/<trial_id>/<文件哈希>.json 或 .txt 或 .png
```

| 字段                                                        | 含义                                                                    |
| ----------------------------------------------------------- | ----------------------------------------------------------------------- |
| complete                                                    | 所有计划试验都有终态记录，包含成功、失败和取消                          |
| trial_id / task_id / task_version / variant_id / repetition | 精确关联哪道题、哪版题、哪个 Agent 版本、哪次重复                       |
| execution_status / verdict                                  | 分别记录宿主执行状态与独立验收结果                                      |
| checks                                                      | 每个规则或模型判定的状态、理由与证据路径                                |
| evidence                                                    | 经隐私校验的实际输出、宿主状态、可选轨迹/分析、独立读盘得到的产物事实   |
| evidence.runner_error / exit_code                            | 异常退出但取得安全采集证据时保留的运行器错误与进程退出码 |
| evidence.failure_capture                                    | 失败试跑的部分轨迹与结构校验结果；complete=false，不作为完整评测证据 |
| metrics                                                     | 通用边界轨迹按已观测执行重算的步骤、工具、loop 和 token；其他轨迹保留宿主报告值，缺失值保持未观测状态 |
| summary.coverage                                            | 明确 passed/failed 的数量 / 全部计划试验数                              |
| summary.pass_rate                                           | passed / 明确评分数；全部未评分时为 null                                |
| summary.pass_rate_all_trials                                | passed / 全部计划试验数；配合 errors/uncertain 阅读，避免排除故障后虚高 |
| by_variant / by_category / by_split                         | 分 Agent 版本、能力类别、数据集合的相同统计                             |
| invalid_loop_lower_bound                                    | 依据轨迹证据计算的无效 loop 规则下界与观测数                            |
| comparisons                                                 | 相同任务与重复编号的配对胜/负/平、未能比较的数量及通过率差              |

产物由 C++ 从实际工作目录读取，拒绝越界路径、符号链接、缺失文件和超限内容；支持 JSON、文本和 PNG。
JSON/文本经过隐私门槛后保留，PNG 内置校验只覆盖文件头和尺寸；视觉语义、视频、数据库副作用等由受信宿主提供独立检查结果，
可由外部验收器完成图片解码或视觉检查，将结果写入 state 后再执行对应规则。
原始 stdout/stderr 不落盘，敏感响应不发给模型；工作目录在试验退出后清理，仅保留通过检查的产物。
媒体与编码内容按业务场景增加专项隐私检查；敏感数据在获得所有者授权后于隔离环境中处理。

run 退出码：0 全部通过；1 有明确任务失败；3 有 error/uncertain/取消，需查看报告；2 配置、冻结数据集或持久化错误。
部分任务失败不会取消其他独立试验。Ctrl+C 会终止活动宿主并记录取消；已发出的模型请求仍受其 HTTP timeout 约束。
崩溃或写盘失败时保留 started.json 和已完成 trials；manifest 标记完整提交，需要重试时使用新的输出目录。
原来的 24 条 starter 任务仍为 pending；需要自行绑定宿主并审核后使用，调试可显式加 `--allow-unreviewed`，报告会保留该事实。

完整请求/响应契约见 [评测接口 Schema](../api/task-evaluation-v1.schema.json)，C++ 入口见
`include/trajectory_evolution/task_evaluation.h`，持久化消息为 evaluation_dataset.proto 的 TaskEvaluationReport。

## 评测任务集构建算法

`dataset.cpp` / `dataset_cli.cpp` 提供离线 build/check，与在线 Runtime 和自动学习调度分离。
借鉴 [Hermes Dataset Builder](https://github.com/NousResearch/hermes-agent-self-evolution/blob/main/evolution/core/dataset_builder.py)
的三来源设计，按场景组管理划分、来源、审核与版本：

1. golden 读取人工 JSONL；synthetic 读取能力说明并分批请求模型出题；traces 用现有 Evaluator 的无 adapter 路径
   构造经结构检查、递归脱敏和限长的具体证据，再调用生成模型。准备证据不额外调用一次评价模型。
2. 模型只允许填写问题、后续输入、标准、类别、难度、声明式初态和约束；来源、审核和集合由程序控制。
   所有模型任务 pending，默认训练用途。单文件/单样本失败隔离，错误以稳定代码记录，不把旧回复当正确标签。
3. 对输入、后续输入做空白归一化，再与初态一起计算精确重复指纹；重复案例合并来源与场景组，冲突标准拒绝。
   按任务/会话组而不是事件拆分；SHA-256 桶给出稳定分配，已用于学习的组全部限制为 train。
4. 计算任务内容版本和数据集版本，冻结成三份 JSONL 与一个 PB，最后写 manifest。
   check 复核文件哈希、PB 与 JSONL 内容、同组集合、学习暴露限制和版本；不自动替换旧版本。
5. 审核任务的近重复关系、测试环境和学习暴露状态，确认后记录审核结论。
   随包 24 条中文案例提供测试定义，可按宿主环境绑定后使用。

此处 dataset_version/task_version 是**评测材料版本**，与 memory_version、Memory revision、policy_id 均不同。
build/check 构建和校验任务集，run 执行任务并比较固定版本。
各入口在独立评测目录保存结果。
构建步骤与字段说明见本章前文。

## 任务执行与验收算法

`task_evaluation.cpp` 通过 C++ 库入口和 `dataset run` 提供完整离线评测：

1. 先验证冻结数据集、选中集合、审核状态、版本/重复编号、每题判分覆盖、所有限制和模型配置，再创建输出目录。
   现有目录拒绝覆盖。任务来源中的代码不会被执行，只有操作者配置的受信 command 可以启动。
2. 固定任务×重复×显式版本构成计划。有限工作线程各取一个试验，以不可混淆的 trial_id 关联请求、轨迹、产物和成绩。
   每次新建工作目录，环境按白名单传递；标准输入是 JSON，标准输出是单个终态 JSON，不假定宿主语言和内部编排架构。
3. `evaluation_process.cpp` 使用 posix_spawn 参数数组而非 shell。非阻塞管道同时发送输入、读取输出、排空 stderr，
   约束输出与墙钟时间，超时或取消时清理本地进程组；外部服务的隔离和取消由宿主处理。
4. 校验 trial_id、执行状态、隐私和可选标准轨迹；轨迹必须闭合且与最终响应身份、状态、输出一致。
   用标准分析器计算确定性诊断。无轨迹或缺失内部边界时不推断循环；返回计数标明宿主来源，缺失指标不填零。
5. 校验产物路径、大小与媒体类型；JSON 和文本做内容检查，PNG 提取文件头与尺寸，标记 header_only。
   完整解码、视觉检查与数据库验收等事实，由宿主测试设施产生并写入证据。
6. 规则以 JSON Pointer 读取事实，执行 exists/equals/contains/at_most/at_least；明确规则失败锁定失败。
   启用模型时，只有尚未明确失败的任务才送预设 rubric 和具体证据；复用模型适配器、外置模板与超时配置，不传凭据。
   校验枚举、中文理由和证据路径；模型故障或预算不足记 error，证据不足记 uncertain。
   格式或引用错误在 max_repair_attempts 范围内重新判分，计入总预算并记录 attempts。
   保持原始执行和判分标准，按输出结构严格解析修复结果。
7. 执行状态与业务结果独立保存。预期分支失败可整体通过；缺失/非法产物是验收失败；环境故障是未评分错误。
   一条失败不取消其他试验。每个终态先保存独立 JSON，最后按计划顺序输出 JSONL、JSON/PB、Markdown 和完成 manifest。
8. 分版本/类别/集合统计明确评分覆盖率、通过率、全量试验通过比例、宿主计数和轨迹无效循环下界。
   两版本按 task_id+repetition 配对，报告胜、负、平和因环境等原因无法比较的试验。
   报告用于版本对照与部署决策，策略管理流程见[完整闭环](24-evolution-and-gepa.md)。

评测任务明确提供验收条件。复现实验时同时固定任务集、判分配置、
Agent 版本、模型、工具数据和初始记忆。具体命令与配置见本章前文。

## 实现定位

- `src/dataset.cpp`
- `src/task_evaluation.cpp`
- `src/agent_evaluation.cpp`
- `src/agent_evaluation_support.h`
- `src/evaluation_process.cpp`
- `proto/evaluation_dataset.proto`
- `config/dataset.json`
- `config/evaluation.json`
- `config/agent-evaluation.json`
- `tests/test_task_evaluation.py`
- `tests/test_agent_evaluation.py`

---

[文档首页](README.md) · [上一章：目录、持久化、幂等与崩溃恢复](18-storage-and-recovery.md) · [下一章：安全、隐私、权限与资源边界](20-security-and-limits.md)
