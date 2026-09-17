# Business Agent

用当前 AgentFlow 的 **coordinator → skill-runner** 承担原业务 **sysagent → 业务 Agent** 的派发和执行职责。沿用 `business-simulator` 目录名和 agent-id 以保持评测配置兼容；**联网、提醒、文件等直接执行；家居与预约连接独立、可运行的业务测试系统，验收检查实际状态**。

验收与能力说明：

- [本轮30例验收与5例修复复核](docs/acceptance-30.md)：业务状态、原规则分数及剩余问题。
- [18 个 skill 的真实执行能力](docs/real-world-capabilities.md)：逐项说明已验证效果与所需外部资源。
- [能力缺口及受影响用例](docs/capability-gaps.md)：对照三份原始 Excel 和 `benchmark/datasets/product.json`。
- [逐用例、逐轮明细](evals/capability-audit.json)：原表行号、原操作码、输入/预期、能力缺口和判定依据。

原始表和转换集共 **1,074 个用例、1,910 轮**。能力清单是静态影响分析，**不是通过率**；尚未执行全量模型验收。

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

适配器复用原 `benchmark/run_eval.py` 的评分、judge 和意图规则，不改标准答案。每例使用独立身份；只准备原表明确的前置条件，证据写入报告 `business_execution`。

原始 RPC 评测调用只发送本轮字符串，而 v1 的首次触发从输入消息数组读取跨轮历史。本包的 `evals/conversation_client.py` 按 Bridge 的输入格式回传**真实用户消息和真实历史答复**，不把预期答案、前置评分标签或 case-id 发送给 Agent。该适配仅作用于本包评测，不修改公共运行时。

严格意图评分和任务完成度应分别看：如果实际天气使条件不成立，正确行为可能是不派发后续提醒，而静态标签仍列出 `health-advice`。另外，GUI 的很多原始用例只要求操作步骤，步骤达标不证明手机实际执行。

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
