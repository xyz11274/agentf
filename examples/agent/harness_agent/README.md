# harness_agent — 通用 Agent Loop（Harness）样例

基于 AgentFlow 的 **dispatcher / coordinator / 可定制 workflow** 实现的通用
"harness" 运行时样例：coordinator 依据输入与已累计的技能结果，**循环**选择
接下来要执行的技能，技能结果回灌 coordinator，直到没有技能被选中则**终止**。

- 节点全部为 **Python**（真实 LLM 经 `AF_LLM_*` 环境变量接入，未配置时走确定性回退）。
- 访问模型前通过 **contextupdater** 任务填充可定制 prompt。
- 技能候选清单、prompt 模板、循环护栏均为**配置文件**驱动。
- 设计文档：[`docs/enhancements/038-feature-harness-agent-...md`](../../../docs/enhancements/)。

> 约束：本样例的全部实现都在 `examples/agent/harness_agent/` 内，**不修改 AgentFlow 源码**。

## 目录结构

```text
harness_agent/
  package/                      # 标准 Agent 包（可被 agent_runner --validate 校验）
    agent.json                  # contextupdater + dispatcher + coordinator + 4 个 skill
    package.json / agentCard.md
    config/
      skills_catalog.json       # 可配置技能候选清单（含 expected_parameters）
      context_config.json       # prompt 填充配置
      harness_config.json       # 护栏：max_iterations / 截断长度 / stuck 阈值 / skill_order
    prompts/                    # 可定制 prompt 模板
    harness_*_workflow.json     # context / coordinator / 4 个 skill workflow
    nodes/python/
      harness_lib.py            # 纯函数决策核心 decide_next + 护栏 + 信封 + LLM 桥接
      harness_bus.py            # 环境/共享存储（按 session+dialogue 隔离）+ marker
      context_node.py / coordinator_select.py / skill_node.py
  driver/harness_runner.cpp     # 自带 C++ 驱动（独立编译，链接预构建 libagentflow）
  scripts/build_and_run.sh      # 构建库 + 编译驱动 + 运行
  tests/
    run_tests.sh                # 入口：单元测试 + 黑盒测试
    unit/test_harness_core.py   # decide_next 的 loop/护栏/终止/隔离/配置 单测
    blackbox/test_harness_dialog.sh  # 经驱动跑真实框架循环的端到端断言
```

## 运行

```bash
bash examples/agent/harness_agent/scripts/build_and_run.sh
# 交互模式：输入文本回车；/quit 退出
# 或单条：... build_and_run.sh   （脚本默认进入交互；可改用驱动 --input）
```

直接用驱动单条执行：

```bash
./examples/agent/harness_agent/build/harness_runner \
    examples/agent/harness_agent/package --input "请帮我规划一次三天的旅行"
```

输出（示例）：

```
HARNESS_RESPONSE: 最终答复：…… 结论：已完成对「请帮我规划一次三天的旅行」的检索-分析-综合-定稿闭环。
```

## 循环机制

```
用户请求
  └─ contextupdater：填充 prompt/上下文（模型访问前）
  └─ dispatcher：拉起首个技能 retrieve
        retrieve ──▶ coordinator.decide_next ──▶ next_skills=[analyze]
        analyze  ──▶ coordinator.decide_next ──▶ next_skills=[compose]
        compose  ──▶ coordinator.decide_next ──▶ next_skills=[finalize]
        finalize ──▶ coordinator.decide_next ──▶ next_skills=[]  + response（终止）
```

- **终止由 coordinator 决定**（输出空 `next_skills` + `response`）。
- **每轮仅选 1 个技能**，且选择严格依据"已累计结果"（结果感知状态机，而非静态 DAG）。
- **护栏**：`max_iterations`（默认 8）+ stuck 检测 + 观测截断；护栏触发时输出兜底 `response` 与 `terminated_reason`。

### 驱动说明

通用二进制 `agent_runner --interactive` 只直跑单个 workflow，不驱动 agent 级循环；
故本样例自带 `harness_runner`，以 `runtime->execute_workflow` **顺序编排** coordinator 与
skill workflow（与 `examples/features/chat_bridge_support.h` 驱动 multi_session_chat 同理），
从而避免异步节点竞态，得到确定、可测试的循环。`agent.json` 同时定义了完整的
dispatcher/coordinator/skill agent，可被框架装载与校验。

## 定制点

| 想改什么 | 改哪里 |
|---|---|
| 技能候选清单 / 顺序 | `config/skills_catalog.json`、`config/harness_config.json` 的 `skill_order` |
| 模型前 prompt 填充 | `prompts/*.md`、`config/context_config.json` |
| 护栏阈值 | `config/harness_config.json`（`max_iterations` / `max_observation_length` / `stuck_repeat_threshold`） |
| 技能业务逻辑 | `nodes/python/skill_node.py` |
| 选择/终止策略 | `nodes/python/harness_lib.py` 的 `decide_next` |

## 测试

```bash
bash examples/agent/harness_agent/tests/run_tests.sh
```

- **单元层**（`tests/unit/test_harness_core.py`）：直接验证 `decide_next` 的
  loop / 终止 / 护栏（max_iterations、stuck、观测截断）/ 参数校验 / 失败处理 /
  dialogue 隔离 / 配置生效。
- **黑盒层**（`tests/blackbox/test_harness_dialog.sh`）：经 `harness_runner` 跑真实框架循环，
  断言多迭代成功、双 dialogue 隔离、max_iterations 护栏。

无外部 LLM 时全部走确定性回退，可在 CI 离线稳定通过。

## LLM 配置（可选）

```bash
export AF_LLM_BASE_URL="https://api.openai.com/v1"
export AF_LLM_API_KEY="sk-..."
export AF_LLM_MODEL="gpt-4o-mini"
```

未配置时使用规则化回退；配置后技能/上下文节点会尝试调用真实模型，失败自动回退。
