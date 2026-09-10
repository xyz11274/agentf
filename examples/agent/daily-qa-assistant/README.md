# Daily QA Assistant

通用问答助手 — 基于 AgentFlow 框架的 coordinator-driven agent，帮助用户解决日常生活问题：**天气查询、高德地图周边搜索、公交信息查询、网页实时搜索、文生图**。

架构与 `examples/agent/outfit-assistant` 完全一致（dispatcher → dummy_pre_skill hook → coordinator loop → 通用 skill-runner），仅替换业务 skills。执行架构细节见 outfit-assistant 的 README「完整执行全景图」。

## 能力一览

| Skill | 能力 | 工具 | 数据源 |
|-------|------|------|--------|
| `weather-query` | 实时天气 + 未来 3 天预报 | `weather_query` | 心知天气 v3 |
| `amap-nearby` | 地址转坐标 / 周边 POI / 公交站点线路 | `geocode` `nearby_poi_search` `bus_route_query` | 高德 Web 服务 |
| `web-search` | 网页实时信息检索 | `web_search` | compatible-mode `enable_search` 内置联网（qwen-plus） |
| `image-gen` | 文生图 | `generate_image` | multimodal-generation (wan2.7-image) |

coordinator（agent.md）按问题意图自主 dispatch 一个或多个 skill 并行执行，汇总后回答；纯常识类问题直接回答，不消耗 skill 调用。

## 目录结构

```
daily-qa-assistant/
├── agent.json                          # 4 task 定义（与 outfit-assistant 相同拓扑）
├── agent.md                            # model 配置 + 业务意图 + skills 列表
├── package.json                        # workflow 注册
├── manifest.jsonc                      # 打包 includes/excludes
├── .env / .env.example                 # API key 配置
├── requirements.txt                    # Python 依赖
├── setup.sh                            # 沙箱环境初始化（随包上传）
├── setup-host.sh                       # 宿主环境初始化（.venv + agentflow-runtime + sentinel）
├── coordinator_workflow.json           # coordinator task workflow
├── dummy_pre_skill_workflow.json       # 前置 hook workflow
├── skill_workflow.json                 # skill-runner task workflow
├── nodes/                              # 自动生成入口（实际实现在 agentflow-runtime pip 包）
└── harness/
    ├── hooks/sessionlog_hook.py        # 会话轨迹落盘（通用，直接复用）
    └── skills/
        ├── weather-query/
        │   ├── SKILL.md
        │   └── scripts/weather_query.py
        ├── amap-nearby/
        │   ├── SKILL.md
        │   ├── data/city_code.json     # 城市名 → adcode 映射（bus/stopname 需要）
        │   └── scripts/{geocode,nearby_poi_search,bus_route_query}.py
        ├── web-search/
        │   ├── SKILL.md
        │   └── scripts/web_search.py
        ├── image-gen/
        │   ├── SKILL.md
        │   └── scripts/generate_image.py
        ├── dummy_pre_skill/SKILL.md    # hook 占位
        └── skill-runner/SKILL.md       # 占位
```

所有工具脚本零第三方依赖（标准库 urllib，代理不可达自动回退直连）；skill-scoped 工具由 skill_executor 从 `harness/skills/{skill}/scripts/` 动态加载（模块契约：`TOOL_NAME` / `TOOL_DESCRIPTION` / `TOOL_PARAMS` / `execute(args)`）。

## 快速开始

### 1. 环境初始化

```bash
cd examples/agent/daily-qa-assistant
bash setup-host.sh               # 创建 .venv + 装依赖 + agentflow-runtime + sentinel
cp .env.example .env             # .env.example 已带可用 key，按需替换
```

### 2. 启动 daemon

在项目根目录执行：

```bash
cd /path/to/AgentFlow

set -a; . examples/agent/daily-qa-assistant/.env; set +a

PYTHON_PATH="$(pwd)/examples/agent/daily-qa-assistant/.venv" \
  ./build/agentflowd \
  --listen 0.0.0.0:17892 \
  --package "$(pwd)/examples/agent/daily-qa-assistant"
```

> `PYTHON_PATH` 必须用绝对路径（框架 worker 用它找 `.venv/bin/python3`）。

### 3. 测试

```bash
python3 examples/agent/harness_agent_v2/test_client.py \
  --port 17892 \
  --agent-id daily-qa-assistant \
  --message "东莞今天天气怎么样？适合穿什么"

# 其他示例：
#   "松山湖附近有什么好吃的川菜馆？"
#   "深圳华为站有哪些公交线路？"
#   "帮我画一只在樱花树下打盹的橘猫"
```

## 评测

长程任务测评集（任务执行成功率 / 多意图识别准确率）与评测 runner 见 [evals/README.md](evals/README.md)：

```bash
python3 evals/run_eval.py --port 17892            # 全量 25 例（约 1~3 小时）
python3 evals/run_eval.py --port 17892 --category multi-intent   # 只跑多意图分类
```

> 依赖 sessionlog 轨迹核对意图分发，`.env` 中 `AF_SESSIONLOG_ENABLED=1`（已配置）。

## 环境变量

| 变量 | 用途 | 必填 |
|------|------|------|
| `AF_LLM_API_KEY` / `API_KEY` | LLM API key | 是 |
| `AF_LLM_BASE_URL` / `AF_LLM_MODEL` | LLM 配置（覆盖 agent.md） | 否 |
| `SENIVERSE_API_KEY` | 心知天气 key（weather-query） | 天气需要 |
| `AMAP_API_KEY` | 高德 key（amap-nearby） | 地图/公交需要 |
| `IMAGE_API_KEY` / `IMAGE_BASE_URL` / `IMAGE_MODEL` | 文生图端点（image-gen） | 画图需要 |
| `WEBSEARCH_API_KEY` / `WEBSEARCH_BASE_URL` | 网页搜索（web-search） | 搜索需要 |
| `WEBSEARCH_MODEL` | 联网搜索模型（默认 qwen-plus） | 否 |
| `PYTHON_PATH` | .venv 目录（框架 worker 用） | 启动 daemon 时必填 |

## 加新 skill

与 outfit-assistant 相同：新建 `harness/skills/my-skill/SKILL.md`（+ 可选 `scripts/` 工具），在 agent.md 的 `skills:` 列表加一行，不改任何代码。

## 已知事项

| 问题 | 说明 |
|------|------|
| LLM 推理泄漏到最终回复 | 继承自框架 coordinator 的已知问题（见 outfit-assistant README） |
| 文生图模型 key 权限 | 当前 key 仅开通 `wan2.7-image`（默认）；`qwen-image-3.0-pro` 等返回 403 AccessDenied.Unpurchased，开通后用 `IMAGE_MODEL` 切换 |
| `web-search` 走 enable_search 插件 | 本端点的 AgentStudio 仅管理面、web_search MCP 未上架（实测 2026-09-04），故用 compatible-mode `enable_search: true`（qwen-plus/qwen-max 实测可用） |
