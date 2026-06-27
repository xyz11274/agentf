# 038 - feature - harness_agent 通用 Agent Loop 样例

| 项 | 值 |
|----|----|
| 编号 | 038 |
| 类型 | feature |
| 日期 | 2026-06-27 |
| 状态 | implemented |
| 关联 | `examples/agent/harness_agent/`、`docs/enhancements/022/023/024`（next_skills/多响应/定向路由） |

## 1. 背景与目标

在 `examples/agent/` 下新增一个通用的、基于 "harness" 技术的 agent 样例 `harness_agent`，实现：

1. **Agent Loop 循环**：复用已有 dispatcher / coordinator 与可定制 workflow，coordinator 依据输入与历史结果，循环选择"接下来要执行的 skills 清单和顺序"，skill 执行结果回灌 coordinator，直到没有 skill 被选中则退出循环。
2. **访问模型前的上下文填充**：通过 contextupdater 任务在调用模型前组装/填充 prompt，且填充逻辑（模板、变量）可定制。
3. **标准 agent 包 + 可配置 skills 候选清单**：按 `AGENT_PACKAGE_SPEC` 提供全部默认文件，并把 skills 候选清单做成配置文件而非硬编码。

**范围约束**：除本设计文档外，所有实现都在 `examples/agent/harness_agent/` 内，**不修改 AgentFlow 源码**（`src/`、`include/`、根 `CMakeLists.txt`、`tests/e2e/`）。

## 2. 关键洞察（结合 AgentFlow 实现）

阅读 `src/agent/agent.cpp` 与实测确认，框架已原生具备 harness 循环引擎，**本样例 0 框架改动**：

- dispatcher 输出/预配置 `next_skills`（有序列表，支持 `exec_skill_key`）→ 框架 `process_next_skills_launch()` 拉起 skill（`agent.cpp:2330+`；统一处理见 `agent.cpp:1571`，对所有 task 类型生效）。
- 每个 skill 完成后，框架自动重新触发 coordinator（`agent.cpp:1778-1806`），coordinator 读取累计结果再决策。
- coordinator 再输出 `next_skills` 则继续；输出空/无 `next_skills` 则视为完成并回送 `response`（`agent.cpp:1895+`）。
- contextupdater（`scope:"agent"`）在模型访问前装配上下文（`src/agent/context.cpp`）。
- node 声明的 **outputs** 会被框架发布到命名空间中央 DataBus `/session/{sid}/sys/{dialogue}/{key}`（`runtime.cpp:build_framework_key` + `DATABUS_PUBLISH`），`next_skills`/`response` 据此被循环读取。

实测验证（`system_agent_mock` 即 harness 模式：dispatcher 预配置 `next_skills` → skill 动态产出 `next_skills` → coordinator 聚合）：最小驱动 `route_request_message` + `process_all_sessions_until_idle` 即可驱动框架自动循环拉起 skill 并调度 coordinator。

## 3. 备选方案比较与决策

### 决策 A：循环机制
| 选项 | 取舍 |
|---|---|
| A1 复用框架原生 coordinator 重触发循环（coordinator 只输出 next_skills/空） | **选定** |
| A2 workflow 内自建 goto 环 | 复杂、与框架重复，否 |

对齐业界：OpenAI Agents SDK `Runner` 循环、LangGraph `should_continue` 图边——均由框架持有循环，模型/节点只决定"下一步"。

### 决策 B：节点语言
**Python-only**（用户确认）：单语言、可移植、真实 LLM 经 `AF_LLM_*`、离线确定性回退。

### 决策 C：终止与护栏（结合 rubber-duck 评审修订）
- **终止由 coordinator 决策**（非由某个 skill 直接结束）；`finalize` 只产 `final_result`，coordinator 读取后输出空 `next_skills` + `response`。
- **每轮仅拉起 1 个 skill**，避免计数竞态并最能体现"结果回灌→再决策"。
- 护栏：`max_iterations`（默认 8）+ stuck 检测 + 观测截断；护栏触发必带兜底 `response` 与 `terminated_reason`。
- 循环状态/结果按 `{session_id}+{dialogue_id}` 隔离。

对齐业界：OpenAI `max_turns`、LangGraph `recursion_limit`(=25)、AutoGen `max_turns`、CrewAI `max_iter`、OpenHands `max_iterations` + `StuckDetector`、SWE-agent `max_observation_length`。

### 决策 D：结果感知状态机（确保是"循环"而非静态 DAG）
dispatcher 拉起首个技能；coordinator 依累计结构化结果逐步决策（空→retrieve→analyze→compose→finalize→终止）。离线回退实现为**基于结果的确定性状态机**（非固定顺序）。为可单测，决策抽成无副作用纯函数 `decide_next(state, catalog, results, config, available_parameters)`。

### 决策 E：配置化
- `config/skills_catalog.json`：技能候选清单 + `expected_parameters`（结构化工具 schema）。
- `config/context_config.json` + `prompts/*.md`：模型前 prompt 填充（context engineering）。
- `config/harness_config.json`：护栏阈值与 `skill_order`。

### 决策 F：驱动与构建（关键工程决策，需"不改框架源码、全部在 harness_agent"）
**实测发现**：通用 `agent_runner --interactive` 的 `execute` 只直跑单个 workflow，不驱动 agent 级循环；所有"能跑出响应的循环样例"（multi_session_chat / llm_query_agent / claw）都靠专属 TUI（经根 CMake 编译，内部用 `examples/features/chat_bridge_support.h` 编排）驱动。又：node 的**输入**仅从原始 key 水合、worker 的 file DataBus 为**每次执行临时**，故 Python 节点无法靠 DataBus 跨节点累计状态。

据此（自主决策，用户授权）选定 **选项 B**：harness_agent 内自带 `driver/harness_runner.cpp`，由 `scripts/build_and_run.sh` **独立编译**（`c++ -std=c++17 -Iinclude/external -Iinclude/internal driver/harness_runner.cpp -Lbuild/lib -lagentflow`，已验证），以 `runtime->execute_workflow` **顺序编排** context→skill→coordinator workflow（与 `chat_bridge_support.h` 同理），避免异步节点竞态，得到确定可测的循环。跨节点累计采用按 `session+dialogue` 隔离的**文件存储**（`runtime/state/`）+ marker 文件，循环控制（`next_skills`/`response`）仍走框架命名空间 DataBus 的声明式 output。

> 备选 A（根 CMake 加 TUI 目标，最一致）因会改 agentflow 源被排除；备选 C（纯 Python 循环驱动）作为补充思路保留。`agent.json` 仍是合法 AgentFlow 包（可 `agent_runner --validate`）。

## 4. 业界对标（全部为已核实真实链接）

**官方文档/博客**
- Anthropic, *Building Effective Agents*（orchestrator-workers、augmented LLM、agent loop + 停止条件）— https://www.anthropic.com/research/building-effective-agents ；工程版 https://www.anthropic.com/engineering/building-effective-agents
- Anthropic, *How we built our multi-agent research system*（lead/orchestrator + 并行 subagents）— https://www.anthropic.com/engineering/multi-agent-research-system
- OpenAI Agents SDK, *Running agents*（`Runner` 循环；"final output = 有文本输出且无 tool calls"；`max_turns`）— https://openai.github.io/openai-agents-python/running_agents

**论文（arXiv，已核实编号/标题）**
- ReAct（Thought→Action→Observation + finish 动作）2210.03629 — https://arxiv.org/abs/2210.03629
- Reflexion（evaluate-reflect-retry + 记忆）2303.11366 — https://arxiv.org/abs/2303.11366
- SWE-agent / Agent-Computer Interface（工具 I/O 设计是最大杠杆，NeurIPS 2024）2405.15793 — https://arxiv.org/abs/2405.15793
- HuggingGPT（controller 选择有序 skill 序列）2303.17580 — https://arxiv.org/abs/2303.17580
- ToolLLM / DFSDT（候选工具搜索/排序）2307.16789 — https://arxiv.org/abs/2307.16789
- Gorilla（大工具目录上的可靠选择）2305.15334 — https://arxiv.org/abs/2305.15334
- SWE-bench（"harness"=评测脚手架，ICLR 2024）2310.06770 — https://arxiv.org/abs/2310.06770
- AutoGen（manager/group-chat 轮转 + 终止条件）2308.08155 — https://arxiv.org/abs/2308.08155

**开源实现（已核实文件路径，引用建议 pin commit）**
- LangGraph `create_react_agent`：`should_continue`（无 tool_calls→END）、`Send` 有序扇出、`recursion_limit`/`GraphRecursionError` — https://github.com/langchain-ai/langgraph/blob/main/libs/prebuilt/langgraph/prebuilt/chat_agent_executor.py
- OpenAI Agents SDK：`run.py`（`max_turns`/`MaxTurnsExceeded`）、`NextStepRunAgain/Handoff/FinalOutput` — https://github.com/openai/openai-agents-python/blob/main/src/agents/run.py
- SWE-agent：`run()` `while not step_output.done`，`exit`/submission 终止 — https://github.com/princeton-nlp/SWE-agent/blob/main/sweagent/agent/agents.py
- OpenHands：EventStream 控制器、`max_iterations`、`AgentFinishAction→FINISHED`、`StuckDetector` — https://github.com/All-Hands-AI/OpenHands/blob/0.39.0/openhands/controller/agent_controller.py
- AutoGen：`_base_group_chat_manager.py`（`_apply_termination_condition`、`max_turns`、`select_speaker` 返回列表）— https://github.com/microsoft/autogen/blob/main/python/packages/autogen-agentchat/src/autogen_agentchat/teams/_group_chat/_base_group_chat_manager.py
- CrewAI：`crew_agent_executor.py`（`while not isinstance(formatted_answer, AgentFinish)`、`has_reached_max_iterations`）— https://github.com/crewAIInc/crewAI/blob/main/lib/crewai/src/crewai/agents/crew_agent_executor.py
- Aider：`base_coder.py` reflection 循环、`max_reflections` — https://github.com/Aider-AI/aider/blob/main/aider/coders/base_coder.py

**共识 → 本方案映射**：框架持有 iterate-until-done 循环（A）；终止=未选中工具/显式 finish（coordinator 空 next_skills）；必备硬迭代/预算护栏（C）；每轮回灌观测（结果状态机 + 截断）；结构化工具 schema（D `skills_catalog`）；模型前 context engineering（E contextupdater）；有序多 skill / orchestrator-worker（dispatcher/coordinator）。

## 5. 实现产物

```
examples/agent/harness_agent/
  package/  agent.json · package.json · agentCard.md
            config/{skills_catalog,context_config,harness_config}.json
            prompts/{context_prompt,coordinator_prompt}.md
            harness_{context,coordinator,skill_retrieve,skill_analyze,skill_compose,skill_finalize}_workflow.json
            nodes/python/{harness_lib,harness_bus,context_node,coordinator_select,skill_node}.py
  driver/harness_runner.cpp
  scripts/build_and_run.sh
  tests/run_tests.sh · tests/unit/test_harness_core.py · tests/blackbox/test_harness_dialog.sh
  README.md · .gitignore
```

通用技能：`retrieve → analyze → compose → finalize`（finalize 产 `final_result`，coordinator 终止循环）。

## 6. 测试设计（针对 harness 的专项验证）

双层、确定性、离线：

**单元层**（`tests/unit/test_harness_core.py`，stdlib unittest，直接测 `decide_next`）：
- Loop：空→retrieve→analyze→compose→finalize；打乱注入顺序仍得正确下一步；每轮仅 1 skill。
- 终止：有 final_result → 空 next_skills + 非空 response；终止由 coordinator 决策。
- 护栏：max_iterations 触发→空 next_skills + 兜底 response + `terminated_reason`；stuck 收敛；观测截断后外层 JSON 仍可解析。
- 失败/校验：`expected_parameters` 缺失跳过并推进；skill `error/unsupported` 仍能收敛。
- 隔离 & 配置：双 dialogue 状态独立；改 `harness_config`/`skill_order` 行为随之变化。

**黑盒层**（`tests/blackbox/test_harness_dialog.sh`，经 `harness_runner` 跑真实框架循环；UTF-8 断言用 Python，bash grep 仅 ASCII）：
- T1 正常多迭代成功（state 含 4 个结果、iteration≥4、analyze 依赖 retrieve、最终响应含闭环与用户请求）。
- T2 同 session 双 dialogue 隔离（两条响应各自引用各自请求，无串扰）。
- T3 max_iterations 护栏（state `terminated_reason=max_iterations` + 兜底响应）。

入口：`bash examples/agent/harness_agent/tests/run_tests.sh`（聚合单元 + 黑盒，任一失败非零退出）。

## 7. 验证结果

- `tests/run_tests.sh`：单元 15/15 通过；黑盒 T1/T2/T3 全通过；聚合退出码 0。
- 端到端：单条请求触发 `retrieve→analyze→compose→finalize→终止` 完整循环，输出引用原始用户请求；驱动退出码 0。
- `git status` 仅新增 `examples/agent/harness_agent/`，**未触碰任何框架源码**。

## 8. 已知限制 / 后续

- 驱动用文件存储 + marker 编排，适用于本地单机 runtime；分布式/跨进程场景需改用中央 DataBus 命名空间访问（参考 so_library 的 `INodeDataBusClient`）。
- 纯框架自动循环（不经驱动编排）在 Python 异步 coordinator 节点上存在"过早完成→提前清理 dialogue"竞态；本样例以驱动顺序编排规避（与 `chat_bridge_support.h` 一致）。若需纯自动循环驱动，建议后续评估为框架补充"等待 coordinator workflow 完成再判定终止"的同步点（届时需单独决策，涉及框架改动）。
- 驱动在工作完成后以 `std::_Exit(0)` 退出，规避框架全局析构期的 mutex 崩溃（不影响结果，线程已由 `stop()` 回收）。
