# Business Agent

用当前 AgentFlow 的 **coordinator → skill-runner** 承担原业务 **sysagent → 业务 Agent** 的派发和执行职责。沿用 `business-simulator` 目录名和 agent-id 以保持评测配置兼容；**联网、提醒、文件等直接执行；家居与预约连接独立、可运行的业务测试系统，验收检查实际状态**。

验收与能力说明：

- [本轮30例验收与5例修复复核](docs/acceptance-30.md)：业务状态、原规则分数及剩余问题。
- [18 个 skill 的真实执行能力](docs/real-world-capabilities.md)：逐项说明已验证效果与所需外部资源。
- [能力缺口及受影响用例](docs/capability-gaps.md)：对照三份原始 Excel 和 `benchmark/datasets/product.json`。
- [逐用例、逐轮明细](evals/capability-audit.json)：原表行号、原操作码、输入/预期、能力缺口和判定依据。

原始表当前转换结果共 **1,074 个用例、1,936 轮**。能力清单是静态影响分析，**不是通过率**；尚未执行全量模型验收。

## 启动

本机已配置文本和图片模型，配置在被忽略的 `.env` 中。仓库根目录执行：

```bash
bash examples/agent/business-simulator/run.sh
```

启动一个 AgentFlow daemon（`127.0.0.1:17893`）及本地业务服务/提醒调度器（`127.0.0.1:17894`）。`Ctrl+C` 关闭二者；关闭期间不主动生成到时事件，重启后补记未投递的事件并去重。数据库和产物会保留。

新环境使用 `setup-host.sh` 安装，再把 `.env.example` 复制为 `.env` 填写凭证。可用 `BUSINESS_SIM_PYTHON=/path/to/python` 指定已安装 runtime 的 Python。当前例子使用 `sandbox.mode=none`；skill 只暴露自己的业务工具，不向模型开放 shell/任意文件工具。

业务状态默认在 `outputs/business.sqlite3`，文件在同级 `artifacts/<scope>/`。`BUSINESS_SIM_DB` 可换路径，daemon 与评测器应指向同一个数据库；`BUSINESS_SIM_LISTEN` 和 `BUSINESS_BACKEND_PORT` 可以调整端口。

## 能真正执行哪些任务

| 能力 | 已实现行为 | 实际边界 |
|---|---|---|
| 18 个 skill 的网站操作 | 真实 Chromium 打开页面、观察 DOM、等待动态内容、点击、输入、选择、滚动、上传、下载、截图和媒体控制；按用户隔离登录态 | 每个站点仍受网络、登录、验证码、反自动化和账户权限影响；浏览器动作成功后必须回读业务结果 |
| 购物、餐饮、旅行、第三方 | 真实搜索和网站操作，保存比较/行程文件 | 没有预置账号、地址、旅客信息或支付方式；没有订单回执就不算完成 |
| 办公 | 真实生成 DOCX/XLSX/CSV/PDF/PPTX/HTML/Markdown/ICS，保留版本；可操作在线办公/网页邮箱 | 在线发送或协作需要对应网站登录；生成文件不等于已发送 |
| 电影 | 真实来源检索、片单保存、精确序号详情；网页播放器播放/暂停/跳转并回读状态 | 会员资源、原生 App、电视/音箱需要账号或设备连接 |
| Android GUI | 连接 ADB 后列设备、选择、截图、读取 UI、点击、滑动、输入、按键和启动 App | 当前服务器没有已授权 Android 设备；中文输入还需管理员配置文本接收器 |
| 家居 | 默认 16 台可执行测试设备；另支持配置 Home Assistant 后控制真实实体并回读状态 | 当前没有 Home Assistant 配置；API 回读不等于物理传感器确认 |
| 健康 | 本地结构化档案、版本/删除；复杂周期提醒、后台通知箱、ICS；13 家/21 科室测试挂号目录和共享库存 | 没有真实医院账号、手机推送或临床诊断服务；测试挂号无现实效果 |
| 知识库、记忆 | 网页/文本导入，来源版本、哈希和用户级检索；同用户跨会话偏好 | 原业务私有资料、账号权益和声纹身份仍需接入 |
| 自定义流程 | 由现有业务工具构成的有限流程，逐步执行、停止和重试去重 | 没有无人值守的定时/事件触发器；外部步骤仍依赖账户与设备 |
| 天气和网页信息 | 真实天气预报、1940 年起历史再分析、公开网页搜索和浏览器核验 | 历史再分析不是当时发布的预报；宽泛区域使用代表点 |
| 多模态 | 已配置火山图片模型时生成真实图片文件 | 视频模型、语音和媒体理解尚未接通 |

18 个 skill 对应用户给出的业务角色；coordinator 对应 sysagent。原实现未知，角色对应见 [agent-map.json](data/agent-map.json)。七个数据集领域标签保持原样：`web-search`、`health-advice`、`smart-home`、`device-gui`、`movie-query`、`knowledge-base`、`user-memory`。

工具名的 `sim_` 是兼容名称。默认 `BUSINESS_EXECUTION_MODE=real`，每项结果返回 `status / simulated / source / UserOutputText / UserOutputObject`。家居和挂号的独立验收环境返回 `environment=business_test_system, physical_effect=false`；连接 Home Assistant 后返回 `environment=physical_home` 并区分 API 状态回读与物理效果。

新环境运行 `scripts/setup_browser.py` 安装私有 Chromium 与办公依赖。动态网页可用 `browser_wait`。若系统 DNS 对某个确定的业务站点返回被污染的地址，可由管理员把该站点的完整 origin 加到 `BUSINESS_BROWSER_ALLOWED_ORIGINS`；不要使用通配符。

可选真实设备配置见 `.env.example`：

- Home Assistant：`HOME_ASSISTANT_URL`、`HOME_ASSISTANT_TOKEN`；默认要求 HTTPS。
- Android：`BUSINESS_ADB_EXECUTABLE` 指向 adb，先由运维完成设备授权。
- 网站：使用 `BUSINESS_BROWSER_ALLOWED_ORIGINS=https://exact-site.example` 精确配置受信任 origin。

## 用药提醒的实际渠道

后台将到时事件写入真实通知箱，`health-advice` 的 `notifications` 动作可以查询，也可用 HTTP `/v1/notifications` 读取。`calendar_file.path` 是实际日历文件，包含每日重复和闹钟；导入个人日历后，由该日历应用负责提醒。

日历导入不是自动手机推送；取消本服务提醒不会自动删掉已导入的个人日历。未给剂量时保留“未提供”，不会自行推导处方。工作日、每周、隔日和起止日期已实现；临床处方校验仍需真实医疗系统。`reminder_query` 返回 `scheduler_running`，便于确认是否有运行中的后台。

## 数据集验收

### 正式三阶段入口

完整命令、自动生成的目录和数据流见 [业务离线进化流程](../../../tools/trajectory_evolution/experiments/business-coverage/README.md)。这里的正式流程分为训练积累、GEPA 搜索和版本发布，不靠每次手工改配置切换阶段。

冻结源数据集由 `prepare_dataset.py` 生成到 `tools/trajectory_evolution/experiments/business-coverage/datasets/source/`，位于运行产物目录之外。它从原始业务表选择 400 条 train、200 条 val、200 条 holdout；整段会话和同输入场景组不会跨集合。原表的“智能等级”写入 `attributes.product_source.case_level`，不是进化等级 L0–L4。缺失的意图标签保持 null，不由模型补写。

第一阶段使用原 train 生成训练轨迹、分析/元反思及公私 memory。第二阶段直接引用这份 memory，并只从原 val 的 200 题中划分 GEPA 搜索/候选选择子集；原 holdout 200 题用于最终验收。原 train 不被复制到第二阶段，也不重新执行入库。第三阶段只导出通过门禁的 L3 策略版本。

```bash
# 第一阶段已经完成时不要重跑；新实验首次使用才执行这两条。
python3 tools/trajectory_evolution/experiments/business-coverage/coldstart.py --prepare
python3 tools/trajectory_evolution/experiments/business-coverage/coldstart.py

# 第二阶段先做无模型准备，确认输入后再运行。
python3 tools/trajectory_evolution/experiments/business-coverage/offline_gepa.py --prepare
python3 tools/trajectory_evolution/experiments/business-coverage/offline_gepa.py --status
python3 tools/trajectory_evolution/experiments/business-coverage/offline_gepa.py --run

# 仅在 GEPA 通过最终留出门禁后执行。
python3 tools/trajectory_evolution/experiments/business-coverage/offline_publish.py
```

真实执行链为：原生任务入口 → `run_product_eval.py --evolution-entry` → 原 `agentflowd`/JSON-RPC → v1 coordinator/skills。
该模式只启动正常业务服务和 daemon、转交消息，不另实现 Agent 或调度循环，不读取 Excel、标签或预期答案。
每轮等待真实采集终态后才能推进；真实历史由组件传入，固定时钟等环境事实另行前置。
业务初态只从通用 `TRAJECTORY_EVOLUTION_TRIAL_INITIAL_STATE` 文件读取；每个 trial 独立 SQLite、端口和产物目录，结束清理。
业务内容、前置条件和设备映射均留在本 Agent，进化包没有导入 business_sim、AgentFlow 或具体 skill。
试次 worker 不重载包内 `.env`；普通启动行为不变。离线采集确认默认最多等 5 秒，可通过
`BUSINESS_EVALUATION_POINT_TIMEOUT_MS=1..5000` 调整，成功请求立即返回，不改变线上打点默认 20ms 预算。
入口同时声明 `AF_EXEC_SKILL_KEY`、`AF_COMPLETED_BATCH_KEY` 是公开的 DataBus 关联标识，
避免隐私过滤器因变量名含 KEY 而拦下并发分支的采集。API key、认证令牌等仍照常检查，不关闭脱敏。
生成的裁判配置启用完整重复载荷共享（`judge.share_repeated_inputs=true`）：相同内容引用一次，
不裁掉实际工具结果、执行身份或因果关系；固定业务时钟也用于核验相对日期。

原生报告中，`trials[].evidence.turn_evaluation` 是逐轮判定，`intent_observations` 是去重专业目标观察；
`summary.turn_acceptance` 与 `summary.multi_intent_observation` 为汇总。业务评分、耗时、模型次数、token、无效 loop 等原指标保留。
多意图只观察，不成为进化奖励。逐轮标签和整题结果在训练导入时分开归因，后轮失败不会把前轮成功经验变成反证。

### 业务侧独立诊断（不替代原生学习入口）

```bash
# 读取原始表，逐例对照转换集，更新能力缺口报告；不调用模型
python3 examples/agent/business-simulator/evals/audit_capabilities.py

# 校验完整转换集
python3 examples/agent/business-simulator/evals/run_product_eval.py --validate-only

# daemon 启动后，先跑少量业务用例（消耗配置模型额度）
python3 examples/agent/business-simulator/evals/run_product_eval.py \
  --env-file examples/agent/business-simulator/.env \
  --max-cases 5 --output examples/agent/business-simulator/outputs/product-smoke.json
```

适配器复用 `benchmark/run_eval.py` 的执行器与 judge，每例使用独立身份。业务入口启用逐轮工具证据评分；意图识别默认仅观察，不影响任务成绩。其他 Agent 使用公共 runner 时保持原有默认规则。

默认从 `test_cases/product/*.xlsx` 重新转换，写入 `outputs/product-current.json`，避免复用旧版多轮拆分结果。当前转换结果为 1,074 条、1,936 轮。显式传入 `--evalset` 时使用指定文件；旧 JSON 请先用 `benchmark/converters.py` 重新生成。

原始 RPC 评测调用只发送本轮字符串，而 v1 的首次触发从输入消息数组读取跨轮历史。本包的 `evals/conversation_client.py` 按 Bridge 的输入格式回传**真实用户消息和真实历史答复**，不把预期答案、前置评分标签或 case-id 发送给 Agent。该适配仅作用于本包评测，不修改公共运行时。

如果天气条件不成立，正确行为可能是不派发后续提醒，而静态标签仍列出 `health-advice`。此时由带本轮工具证据的裁判判断业务结果，静态 skill 集合不再作为业务入口的硬性失败条件。GUI 原表要求操作步骤时，以步骤和参数验收，不要求真实手机执行。

### 固定业务世界与实时环境

`run_product_eval.py` 默认使用 `data/evaluation-profile.json`：

- 固定业务时钟，并把时钟事实随对话传给 Agent，使“明天/下周一”的模型解释与后端一致。
- 用家居状态机生成 35 天测试历史，随后每个用例复制独立初态。历史模板按时间和天数缓存；一个用户的动作不会修改其他用户的模板。
- 载入 `data/fixtures.json` 的测试片库、样例产品资料和固定信息。支持推荐→选第二部→播放→暂停→继续，以及用户知识资料查询。样例资料不是已核实的生产产品规范；真实业务验收应由业务方提供资料版本。
- 天气来自 profile 的显式参数；号源在评测用户之间隔离，普通运行仍保留共享库存。需要验证条件真假两条路径时，分别使用 rain=true/false 的 profile。
- 实时时钟不会推进评测设备和提醒；提醒随 `evaluation_world.clock` 推进。普通用户的实时后台不受影响。
- 未配置的外部操作返回 `snapshot_miss`，不自动请求网络或提交真实邮件、支付、设备操作。可在 `external_responses` 增加固定服务回执，每项包含 `skill`、`arguments`、`result`；参数精确匹配，忽略幂等 `request_id`。这些是外部服务测试资料，不应包含用例 ID 或标准答案。
- 报告和工具结果保存 `snapshot_sha256`。比较基线和候选时使用同一 profile 和资料版本，不跨快照直接比较成绩。
- 资料检索只匹配标题、正文、关键词等业务文本，不用日期、ID、来源 URL 或 JSON 字段名作为命中依据；冻结查询也遵守传入的 `topic`、`location`。查询无匹配返回 `not_found` 和空列表，与操作不可用的 `unsupported/snapshot_miss` 区分。仅要求查询并如实展示结果时，正常空结果可以完成查询目标；明确要求价格、非空条目或创建成功时仍须满足相应结果。
- 本例离线 GEPA 的启动检查还会比较当前宿主源码、模型身份与冻结部署。返回 `gepa_host_changed` 时须重新准备匹配当前代码的部署和对照上下文；不会自动覆盖旧实验或把旧基线与新宿主直接比较。

只验证真实联网能力时加 `--live-world`，不载入上述初始历史和资料。普通聊天默认依然是原来的实时模式，评测 profile 只作用于本次评测身份。

```bash
# 不调用模型：重新转换并校验原表
python3 examples/agent/business-simulator/evals/run_product_eval.py --validate-only

# 已启动本 Agent 的 daemon/backend 后，使用固定环境评测 5 条（调用配置模型）
python3 examples/agent/business-simulator/evals/run_product_eval.py \
  --env-file examples/agent/business-simulator/.env --judge --max-cases 5 \
  --output examples/agent/business-simulator/outputs/product-evaluation.json

# 使用另一份业务环境定义
python3 examples/agent/business-simulator/evals/run_product_eval.py \
  --world-profile path/to/profile.json --judge --max-cases 5
```

评测进程和 backend 必须使用同一 `BUSINESS_SIM_DB` 文件；推荐复用 `evals/run_representative.py --label 自定名称 --judge`，它启动隔离的 daemon/backend 并统一数据库。`--no-judge` 可检查执行链路，但内容未评分的轮次标为 `needs_judge`，不作为业务成功。

### 多意图识别观察指标

每个 skill 对应一个专业子 Agent，因此以实际调用的 skill 集合近似意图识别结果，不增加模型调用。

数据集每轮保留：

| 字段 | 含义 |
|---|---|
| `expected_intents` | 原表意图，例如 `["QA", "居家"]`；缺标保持 null |
| `expected_skills` | 按映射表转换的专业子 Agent 集合 |
| `expected_operation_types` | 原表要求的操作码，例如 `["100051"]`；区别于 skill 标签 |
| `expected_behavior` | 该轮业务结果标准；不传给被测 Agent |

每轮报告的 `intent_observation` 保存预期/实际 skill、matched、missing、extra、complete 和 exact。同轮同名 skill 的多次调用只计一次。缺少轨迹时标记 `missing_trace`，不伪造为空调用。

汇总位于报告根部 `multi_intent_observation`：

| 字段 | 定义 |
|---|---|
| `multi_intent_turns` | 预期包含至少两个不同专业 skill 的轮数 |
| `measured_turns` | 有实际轨迹、非条件待核对的可计算轮数 |
| `precision` / `recall` / `f1` | 按各轮去重集合累计的准确率、覆盖率与 F1 |
| `complete_rate` | 实际调用包含全部预期 skill 的轮次比例，额外调用另计 |
| `exact_rate` | 实际集合与预期集合完全相等的比例 |
| `missing_skill_count` / `extra_skill_count` | 漏派与额外派发的累计数量 |
| `conditional_turns` | 静态标签需结合条件核对的轮数，单列，不混入上述分母 |
| `missing_trace_turns` | 不能取得真实派发轨迹的多意图轮数 |
| `observation_only` | 固定 true；不纳入进化目标、业务评分或发布门槛 |

`summary` 中原有的严格意图统计保留，便于与旧报告对照；遇到条件任务，应优先看新增的分组观察结果。此指标不能区分同一 skill 内两个不同业务动作，动作完成度由操作码、参数和业务证据判定。

### 业务评分证据与并发隔离

裁判只接收当前轮新增的业务事件，包括 skill、action、status、操作码、错误和结构化输出；不会每轮重复发送整个状态库。递归限长后总证据约束在 12,000 字符内，保留截断标记，并使用现有隐私脱敏函数。裁判同时看到真实对话和业务标准，失败后成功恢复与条件跳过分开判定。独立业务检查器不再把所有 `CK_GUI` 用例当作充值。

HTTP 工具自动携带 worker 的 `AF_EXEC_SKILL_KEY`（无此值时使用 `AF_SELF_TASK_ID`），浏览器页面和幂等回执按任务分支隔离，用户无需填写参数。播放器是明确共享的会话资源，播放、暂停、继续按该资源串行，保留跨轮控制能力。页面默认空闲 1,800 秒后在下次访问时回收，最多保留 128 个分支页面，可用 `BUSINESS_BROWSER_IDLE_SECONDS` / `BUSINESS_BROWSER_MAX_PAGES` 调整；达到上限时拒绝新增页面，不抢占活跃分支。Chromium 操作仍通过其专有线程执行；本次修复保证分支正确性，没有把浏览器改成并行执行引擎。

## 验证

使用已安装 runtime 的 Python 3.12+：

```bash
python3 -m pytest -q examples/agent/business-simulator/tests
./build/agent_runner examples/agent/business-simulator --validate

# 真实模型、天气、提醒、文档、图片；临时独立进程退出后自动关闭
python3 examples/agent/business-simulator/evals/smoke_real.py
# 只复核指定湿度+导航用例的两轮上下文，避免重复生成图片
python3 examples/agent/business-simulator/evals/smoke_real.py --case-only
```

真实开发验证及已知问题见 [validation.md](docs/validation.md)。全量业务集尚未跑完，不承诺 1,074 例通过。

历史离线业务服务保留为测试替身，**只有显式设置 `BUSINESS_EXECUTION_MODE=fixture` 才启用**；`tests/smoke_daemon.py`、`evals/smoke_live.py` 是该离线模式的旧检查，不能作为真实完成任务的证据。

## 实现入口

- `agent.md`、`harness/skills/*/SKILL.md`：领域路由、依赖、条件、多轮与结果约束。
- `harness/business_sim/contracts.py`：18 个 skill 的结构化参数。
- `real_services.py`：真实执行入口、文档、图片、GUI计划和缺口报告。
- `live_information.py`：公开搜索、天气、日期和来源。
- `reminders.py`：每日调度、日历、补记与去重。
- `device_world.py`：设备控制、时钟演化、遥测、故障与历史。
- `appointments.py`：独立预约系统、日期约束、共享号源、事务与用户隔离。
- `state.py`：身份隔离、事务、请求重试与事件。
- `backend.py`、`scripts/launch.py`：本地业务接口和进程生命周期。
- `evals/audit_capabilities.py`：两份数据来源的能力影响分析；不参与 Agent 执行。

天气参数依据 [Open-Meteo 官方文档](https://open-meteo.com/en/docs)；导航入口依据 [高德 URI 官方文档](https://lbs.amap.com/api/uri-api/guide/travel/route)。无坐标时只生成目的地搜索入口，不能称为已取得避堵路线。

## 固定的 30 条开发验收

```bash
# 无需预先启动daemon；并行运行3个独立daemon，每个内部串行，用全新label保留证据
/root/miniconda3/envs/agentflow-dev/bin/python examples/agent/business-simulator/evals/run_isolated.py --label my-check
```

用例来自原 product.json，清单在 `evals/representative-30.json`。环境不读取评分字段或用例ID。原 scorer 未改；默认不运行 LLM judge，因此原报告“成功”主要表示响应/派发规则满足，**不能单独作为业务完成率**。另用 `evals/check_business_results.py` 检查具体状态和文件。

此前发现的单 daemon 并发身份复用已在公共 `SessionRouter` 修复：路由预留期间不会被其他身份抢占，按身份重新进入已有会话不会重置正在运行的任务。相关 `AgentTest.*` 与隔离测试共 51 项通过。模型评测仍保持每个 daemon 串行，使时延与状态证据更容易复现；生产压力与异常恢复仍需部署环境专项验证。

业务接口与测试环境的操作方式见 [business-backends.md](docs/business-backends.md)。

## 新增外部连接与虚拟设备

- `web-search geocode/route`：直接连接 OpenStreetMap Nominatim 和 OSRM；可做真实地理编码与驾车算路，不包含用户当前位置和实时拥堵。
- `office-work email_send`：配置 `.env.example` 中的 SMTP 变量后发送真实邮件，要求确认和 request_id，并返回 SMTP 接受回执。
- `third-party webhook_post`：只向管理员精确允许的 origin 提交 JSON，要求确认和 request_id，并回读 HTTP 响应。
- `multi-device`：真实 ADB/Home Assistant 与六类虚拟设备显示在同一清单。虚拟手机、平板、电脑、电视、音箱、手表支持在线/离线、选择、广播、数据同步、跨屏接力和持久化历史；所有虚拟结果固定标记 `environment=business_test_system, simulated=true, physical_effect=false`。

真实购物、外卖、订票、医院、支付和账号权益没有跨平台通用公开 API，仍通过真实网站与具体账户连接；缺登录、验证码、实名或付款时准确停在阻塞点。虚拟设备可验收业务流程，但不会伪造真实订单、真实扣款或实体设备变化。
