# Unified Eval — 数据集与评测框架归一化

归一化三份既有评测数据与两套评测框架，统一为 **evalset.v2 数据集格式** + **统一 runner**：

| 来源 | 原格式 | 原框架 |
|------|--------|--------|
| `trace_output/v1-l4-coldstart/dataset` | JSONL（单轮 task，train/val/holdout） | trajectory_evolution `dataset run` |
| `test_cases/product/*.xlsx` | Excel（多轮编号语料 + 预期意图垂域） | 无（人工核对） |
| `examples/agent/daily-qa-assistant/evals` | evalset.json（多轮 turns） | run_eval.py（JSON-RPC + sessionlog） |

## 目录

```
benchmark/
├── evalset_schema.py     # evalset.v2 schema 定义 / 归一 / 校验
├── converters.py         # converter（coldstart JSONL / product xlsx / daily-qa JSON / hotpotqa JSON）+ 合并 CLI
├── domain_skill_map.json # product 垂域意图 → skill 名映射表（已确认，见下）
├── run_eval.py           # 统一评测 runner（以 daily-qa run_eval.py 为底座扩展）
├── datasets/             # 预生成的 evalset.v2（直接评测，无需自己跑 converter）
│   ├── coldstart.json    #   60 case / 60 轮（单轮任务，无意图 GT，split=train/val/holdout）
│   ├── product.json      #   1074 case / 1910 轮（多轮多意图，GT 已映射为 skill 集合）
│   ├── daily-qa.json     #   25 case / 86 轮（多轮对话）
│   └── hotpotqa.json     # 700 case / 700 轮（HotpotQA 多跳问答，单轮，无意图 GT，split=train/val）
├── reports/              # 评测报告输出
└── README.md
```

## evalset.v2 格式

```jsonc
{
  "name": "...", "version": "...", "schema_version": "evalset.v2",
  "skill_names": ["weather-query", "..."],   // 意图 ground truth 词表（可为空）
  "categories": ["..."],                      // 留空则不限制 category 取值
  "cases": [{
    "id": "...", "category": "...", "difficulty": "easy|medium|hard",
    "goal": "...",                            // judge 背景信息
    "constraints": ["..."],                   // 题目约束（进入 judge 上下文）
    "budget": {"max_steps": 128, "max_tool_calls": 96, "timeout_ms": 1800000},  // 可选
    "checks": [{"id": "...", "op": "action_count", "operation": "image-gen", "max": 1}],  // 可选，确定性检查
    "split": "train|val|holdout",             // 可选（coldstart）
    "level": "L1|L2|L3",                      // 可选（product 智能等级）
    "turns": [{                               // 单轮任务 = turns 长度 1
      "user": "...",
      "expected_skills": ["..."],             // skill 集合；[] = 不应 dispatch（负例）；
                                              // null/缺省 = 无意图 GT（意图指标跳过、不进分母）
      "expected_behavior": "...",             // 判分标准（允许为空 → judge 按通用标准评分）
      "must_include": [], "must_not_include": []
    }],
    "final_rubric": "...",                    // 可选，对话级诊断分
    "provenance": {"kind": "synthetic|product|manual", "...": "..."}
  }]
}
```

## 用法

```bash
cd benchmark

# 校验 / 列出用例（datasets/ 已预生成，无需自己跑 converter）
python3 run_eval.py --evalset datasets/product.json --validate-only
python3 run_eval.py --evalset datasets/product.json --list-cases

# 评测（先启动 daemon，.env 开启 AF_SESSIONLOG_ENABLED=1）
python3 run_eval.py --evalset datasets/daily-qa.json --port 17892 --agent-id daily-qa-assistant
python3 run_eval.py --evalset datasets/coldstart.json --split holdout --no-judge   # 只跑 holdout
python3 run_eval.py --evalset datasets/product.json --category 多意图 --cases QA_GY_HHYL_001

# 指定用例数量（在 --cases/--category/--split 过滤之后截取；大集冒烟测试用）
python3 run_eval.py --evalset datasets/product.json --max-cases 20                  # 原顺序前 20 例
python3 run_eval.py --evalset datasets/product.json --max-cases 20 --shuffle-seed 42  # 随机抽 20 例（同种子可复现）

# 对比基线（配对胜负 + 准入门槛，默认 min_pass_rate=0.8、max_pass_rate_drop=0）
python3 run_eval.py --evalset datasets/product.json --baseline reports/report-old.json

# 需要重新生成数据集时（源数据变更后）：
python3 converters.py ../trace_output/v1-l4-coldstart/dataset/{train,val,holdout}.jsonl -o datasets/coldstart.json
python3 converters.py ../test_cases/product/*.xlsx -o datasets/product.json
python3 converters.py ../examples/agent/daily-qa-assistant/evals/evalset.json -o datasets/daily-qa.json
python3 converters.py ../test_cases/opensource/hotpot_train_500.json ../test_cases/opensource/hotpot_dev_200.json -o datasets/hotpotqa.json
```

## 离线自进化（GEPA）使用说明

离线自进化由 `tools/trajectory_evolution` 内置的 GEPA 反思搜索执行（C++ 实现，不依赖 DSPy），
完整闭环：**补充训练 → 编译 L1–L3 种子 → GEPA 多轮搜索（反思变异 + 双亲重组）→ holdout 准入 → 发布**。
详细设计见 `tools/trajectory_evolution/docs/24-evolution-and-gepa.md`。

### 1. 前置条件

- 已按任务评测指南配置真实宿主命令、评分规则和版本隔离（`config/evaluation.json` 的 `variants[0].command` 指向你的 Agent 评测入口）。
- 任务集已准备非空的 `train` / `val` / `holdout` 三个 split 且全部 review 完成；同一任务族/会话保持在同一 split（`trace_output/v1-l4-coldstart/dataset` 已满足：50/25/25，60 题已 reviewed）。留出任务数 × repetitions 需 ≥ `gate.min_trials`（默认 2）。

### 2. 配置（均在 `tools/trajectory_evolution/config/` 基础上修改）

```bash
cp -r tools/trajectory_evolution/config my-evolution-config   # 保持 runtime.json 与 prompts/ 相对位置
```

| 文件 | 修改点 |
|---|---|
| `runtime.json` | 配自己的反思模型：endpoint、`api_key_env`（凭据只放环境变量）、thinking、请求超时 |
| `agent-evaluation.json` | 增加 `"gepa_config": "gepa.json"`（省略此行则保持普通基线评测，不进化） |
| `service.json` | 保持 `"policy_evaluation_file": "agent-evaluation.json"`；自动学习需 `auto_evaluate=true`，通常配合 `auto_approve_experiences=true`（不绕过发布门禁） |
| `gepa.json` | 搜索预算：`max_rounds` / `max_model_calls` / `max_trials`、`use_merge`（双亲重组）、`proposals_per_round`、`acceptance`、`objective` 成本权重等，默认值已可直接用 |

经验不足时自动补充训练（默认值如下，可不配）：

```jsonc
// agent-evaluation.json
"training": {"enabled": true, "min_experiences": 3, "batch_size": 16, "max_batches": 4}
```

只用 `split=train` 且 reviewed 的题做训练；val/holdout 不进入经验库。

### 3. 执行

```bash
trajectory_evolution learn --config my-evolution-config/service.json
```

- 无需预先有用户会话；命令等待后台队列完成，经验不足或学习失败以非零退出码报告。
- 查看进度：向服务发 `agent_evaluation.status`（返回 `training.status`：`training` / `ready_to_import` / `waiting_for_learning` / `ready`）。
- 修改配置或提示词不需重新编译，但需重启组件建立新实验上下文（避免同一棵树混用不同配置成绩）。

### 4. 产物与验收

- 搜索谱系与检查点在 `agent-evaluation/<job_id>/` 下；通过准入并发布的版本导出到
  `agent-evaluation/<job_id>/search/published-version.json`（未通过验收的不导出）。
- 是否生产生效以 `decision.json.published` 为准；准入门槛即 gate（min_pass_rate=0.8、max_pass_rate_drop=0 等）。

### 5. 离线加载已发布版本

发布包是自包含部署文件（完整策略＋经验快照＋控制规则，不依赖原 memory 库/训练配置）：

```jsonc
// service.json：去掉 policy_evaluation_file，改指发布包
{"policy_version_file": "agent-evaluation/<job_id>/search/published-version.json"}
```

重启即按发布版本运行；已登记发布但缺文件的版本会按原验收记录补导出，不重跑 Agent。

### 6. 恢复与故障

- 进程异常退出：同上下文中运行中的 GEPA 任务自动重新排队，已完成评测缓存经报告哈希校验后复用；反思调用中断的节点记 `interrupted`，预算保留。
- 评分/反思 API 返回 429：作业以 `gepa_model_rate_limited` 停止，保留当前代与检查点；限流解除后用 `agent_evaluation.retry` 接续，已耗预算不退还。
- 训练批次在执行失败/中断后用 `agent_evaluation.retry` 显式重试，不自动无限重试。

## 离线自进化前后测评

本目录的 runner 负责进化前后的**独立对照测评**（GEPA 流程见上节），
"进化前后"= daemon 加载的 agent 策略（agent.md / skills / 注入经验）变化前后。

```bash
cd benchmark

# ── 进化前：存基线报告（迭代用 val，验收用 holdout）──
python3 run_eval.py --evalset datasets/coldstart.json --split holdout \
  --port 17892 --output reports/pre-evolution.json
# product / daily-qa 同理各存一份基线

# ── 进化产物落地、重启 daemon 后：带 --baseline 复测 ──
python3 run_eval.py --evalset datasets/coldstart.json --split holdout \
  --port 17892 --output reports/post-evolution.json \
  --baseline reports/pre-evolution.json
```

报告 `comparison` 段给出对照结论（与 GEPA 准入口径一致：min_pass_rate=0.8、max_pass_rate_drop=0）：

```jsonc
"comparison": {
  "paired_cases": 15, "wins": 3, "losses": 1, "ties": 11,   // 逐 case 配对胜负
  "case_success_rate_delta": 0.13,
  "intent_exact_accuracy_delta": 0.06,
  "pass_rate_drop": -0.13,
  "gate": {"passed": true, "failures": []}                  // 准入判定
}
```

实践要点：

| 要点 | 做法 |
|---|---|
| 数据隔离 | 迭代调参用 `--split val`，最终验收才用 `--split holdout`，避免对 holdout 过拟合（GEPA 内部同纪律） |
| 可比性 | 前后两次必须用同一 evalset 文件、同一 agent-id、同一 judge 模型与阈值，否则配对失真 |
| 判分口径 | 基线与候选的规则 checks 必须一致（规则失败不能被 judge 覆盖，两边同配置才公平） |
| 两轨分工 | GEPA 内部准入看 coldstart holdout（trajectory_evolution `dataset run`）；本 runner 做跨数据集（product/daily-qa）独立回归，口径已对齐可互相印证 |

注意：本 runner 测的是 **daemon 实际部署形态**。GEPA 发布包（published-version.json）若只作用于
trajectory_evolution 注入链路，需先把进化结果（改写后的经验文本/分发策略）落到 agent.md 或
skill 中并重启 daemon，benchmark 的"进化后"复测才能测到差异。

trajectory_evolution 原生对照（coldstart 单轨，供参考）：

```bash
# 进化前：L0 基线（variants[0] 配自己的评测入口，policy=null）
trajectory_evolution dataset run ../trace_output/v1-l4-coldstart/dataset \
  --config my-evaluation.json --output-root out/baseline-v1
# 进化后：service 配置改指 "policy_version_file": ".../published-version.json"，
# 同一 dataset、同一配置重跑 dataset run，与 out/baseline-v1/report.json 对比；
# 只换 judge 模型/提示词时用 --regrade-report 避免重复执行 Agent。
```

## product 垂域 → skill 映射（已接入 daily-qa-assistant）

`domain_skill_map.json`（2026-09-16 确认）：QA→`web-search`（已有，真实 API）；
观影→`movie-query`、GUI→`device-gui`、健康→`health-advice`、居家→`smart-home`、
知识库→`knowledge-base`、记忆→`user-memory` —— 后 6 个为 **mock skill**（固定成功返回
示例数据），位于 `examples/agent/daily-qa-assistant/harness/skills/`，已在 `agent.md`
注册并写入分发策略。替换为真实实现时只需改写对应 scripts/*.py，数据集与映射表不变。

## 报告指标（四组）

| 组 | 指标 |
|---|---|
| 任务成功率 | `case_success_rate` / `turn_success_rate`（规则 AND judge≥阈值）、`rule_pass_rate`、`deterministic_checks_failed`、judge 通过率/均分 |
| 意图识别 | `intent_exact_accuracy`、`single/multi_intent_accuracy`、`no_dispatch_accuracy`、`false_dispatch_rate`、per-skill P/R/F1（仅统计有意图 GT 的轮次） |
| 效率/成本 | `turn_latency_avg_s` / `turn_latency_p95_s`（实测 wall-clock）、`dispatch_total`、`llm_calls_total` / `tokens_total`（依赖 sessionlog usage，**缺失记 null 不补零**） |
| 对比/进化 | `comparison`：配对胜/负/平、各率 delta、`pass_rate_drop`、`gate` 准入判定 |

判分优先级与 trajectory_evolution 对齐：**规则失败不能被 judge 覆盖**；sessionlog 缺失时
action_count 等确定性检查记 `error`（未知不当零），意图指标分母不含缺失轮。

## 数据源转换说明

- **coldstart**：单轮 case；`expected_behavior`→判分标准，`constraints/budget/split/provenance` 保留；
  无意图 GT（`expected_skills=null`）。
- **product xlsx**：`测试步骤（语料）`按编号拆轮（支持行内 `1. xxx。2. yyy` 与换行编号）；
  `预期意图`经 `domain_skill_map.json` 映射为 skill 集合，**空标注（如 `1.`）视为未标注（null），
  不是负例**；`智能等级 L1-L3`→difficulty + `level` 字段；约 1/3 用例无预期结果列，
  对应轮次由 judge 按通用标准评分（加载时告警统计）。
- **daily-qa**：原生兼容，`rubric` 自动映射为 `expected_behavior`。

## 待办

- 效率指标中的 llm_calls/tokens 依赖 sessionlog 事件携带 usage；当前 runtime 事件普遍不带，
  报告中体现为 null + `usage_missing_cases`，后续可在 runtime 侧补采。
- 6 个新 skill（movie-query / device-gui / health-advice / smart-home / knowledge-base /
  user-memory）是 mock 实现（固定成功返回），用于评测链路联通；接真实服务时替换对应
  scripts/*.py 即可。
- product 的 `观影`/`GUI` 类用例涉及设备操控，当前 runner 只能驱动对话式 agent，
  这类 case 的判分依赖 judge 对回复文本的评估，不核对真实设备动作。
