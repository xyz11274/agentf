# Daily QA Assistant 长程任务评测集

验证两个核心指标：

| 指标 | 定义 | 判定依据 |
|------|------|----------|
| **任务执行成功率** | case（整段对话）/ turn（单轮）回答达成的比例 | 规则检查（响应非空 + must_include/must_not_include + 无 error）AND LLM-as-judge ≥ 阈值 |
| **多意图识别准确率** | 每轮期望 dispatch 的 skill 集合与实际 dispatch 集合完全匹配的比例 | sessionlog 轨迹中 `skill.dispatch` 事件的 `data.skill_names` 与 `expected_skills` 集合比对 |

另输出：意图集合完全匹配率（分单意图 / 零意图负例）、误分发率、per-skill P/R/F1、分 category 指标、judge 均分。

## 目录

```
evals/
├── evalset.json    # 25 个长程多轮用例（86 轮），5 个 category
├── run_eval.py     # 评测 runner（零第三方依赖）
├── reports/        # 评测报告输出（JSON）
└── README.md
```

## 用例分类（25 例 / 86 轮）

| category | 数量 | 考察点 |
|----------|------|--------|
| `single-intent` | 5 | 单 skill 意图识别（覆盖全部 4 个 skill）+ 负例轮 |
| `multi-intent` | 6 | 单轮 2~3 个意图并行 dispatch（多意图识别主战场） |
| `cross-turn` | 5 | 跨轮指代与省略（"那明天呢"/"换成火锅呢"），考察上下文承接后仍正确分发 |
| `long-horizon` | 5 | 4~5 轮链式任务（出游规划/搬家考察等），跨 4 个 skill 的长程执行 |
| `negative` | 4 | 纯常识/写作不应 dispatch（含诱饵词陷阱，考察误分发） |

每个 case 为 3~5 轮的多轮对话，`expected_skills` 为意图 ground truth（`[]` 表示不应 dispatch），
`must_include` 为响应必须包含的关键词（宽松，主判定交给 judge），`rubric` 为 judge 评分检查点。

## 前置条件

1. **daemon 启动**（见包 README「快速开始」），并且 `.env` 中已开启轨迹采集（本包 `.env` 已配置）：

   ```bash
   AF_SESSIONLOG_ENABLED=1
   ```

   若 daemon 已在旧环境上运行，改完 `.env` 后需重启 daemon。

2. **LLM judge 配置**：复用 `.env` 的 `AF_LLM_BASE_URL` / `AF_LLM_API_KEY` / `AF_LLM_MODEL`（OpenAI 兼容端点）。
   judge 与被测 agent 同模型时建议 `--judge-model` 换一个模型以避免自评偏置。

3. sessionlog 缺失时轮次的意图无法核对（计入 `sessionlog_missing_turns`，不进准确率分母），runner 会在报告中标注。

## 用法

```bash
cd examples/agent/daily-qa-assistant

# 校验评测集格式 / 列出用例
python3 evals/run_eval.py --validate-only
python3 evals/run_eval.py --list-cases

# 全量评测（25 例，含 judge；整跑约 1~3 小时，视模型与图片生成速度）
python3 evals/run_eval.py --port 17892

# 只跑某个分类 / 指定用例；关闭 judge 只做规则判分
python3 evals/run_eval.py --port 17892 --category multi-intent,long-horizon
python3 evals/run_eval.py --port 17892 --cases MI-001,LH-001 --turn-timeout 300
python3 evals/run_eval.py --port 17892 --no-judge

# 并发（daemon session 池为 3，建议 ≤3）；指定 judge 模型与阈值
python3 evals/run_eval.py --port 17892 --concurrency 3 --judge-model qwen-plus --judge-threshold 4
```

运行结束在 stdout 打印摘要，完整报告写入 `evals/reports/report-<时间戳>.json`
（可用 `--output` 覆盖），包含每轮的用户输入 / 助手回复 / 期望与实际 skills / 规则检查 / judge 得分与理由。

## 报告指标速读

```jsonc
{
  "summary": {
    "case_success_rate":  0.92,   // 任务执行成功率（整段对话）
    "turn_success_rate":  0.95,   // 任务执行成功率（单轮）
    "multi_intent_accuracy": 0.88,// 多意图识别准确率（期望 ≥2 个 skill 的轮次）
    "intent_exact_accuracy": 0.93,// 全部轮次 skill 集合完全匹配率
    "single_intent_accuracy": 0.96,
    "no_dispatch_accuracy": 0.95, // 负例轮正确不 dispatch 的比例
    "false_dispatch_rate": 0.04,  // 误分发率（多调的 skill 实例 / 总调用量）
    "judge_pass_rate": 0.9, "judge_avg_score": 4.3,
    "sessionlog_missing_turns": 0
  },
  "per_category": { "...": "分 category 的成功率与意图准确率" },
  "per_skill":    { "...": "每个 skill 的 dispatch P/R/F1" }
}
```

## 评测集格式

```jsonc
{
  "skill_names": ["weather-query", "amap-nearby", "web-search", "image-gen"],
  "cases": [
    {
      "id": "MI-001", "category": "multi-intent", "difficulty": "medium",
      "goal": "case 级考察点（judge 背景信息）",
      "turns": [
        {
          "user": "深圳今天天气怎么样？附近有什么好吃的川菜馆？",
          "expected_skills": ["weather-query", "amap-nearby"],
          "must_include": ["天气", "川菜"],
          "must_not_include": [],
          "rubric": "本轮 judge 检查点"
        }
      ],
      "final_rubric": "对话级 judge 检查点（可选，产出 dialogue_judge_score 诊断分）"
    }
  ]
}
```

## 扩展用例

- 新 case 追加到 `evalset.json` 的 `cases` 数组即可；`id` 唯一、`expected_skills` ⊆ `skill_names`，
  `run_eval.py --validate-only` 会做 schema 与 ground truth 校验。
- 意图 ground truth 依据包 `agent.md` 的分发策略：天气/穿衣 → `weather-query`；附近/地点/公交 →
  `amap-nearby`；新闻/价格/政策等实时信息 → `web-search`；明确画图 → `image-gen`；纯常识/写作 → `[]`。
- 注意多轮用例中后轮的省略表达（"那明天呢"）应允许 agent 承接上下文，但"明天"的数据需要重新查询，
  不能以上一轮缓存作答 —— 这是 cross-turn 分类的核心考察点。

## 已知事项

- judge 与被测 agent 共用端点时，judge 调用走 `AF_LLM_*`，与 agent 执行互不影响，但会增加耗时。
- `LH/MI` 类用例含真实文生图（wan2.7-image），单轮可能耗时 1~2 分钟，`--turn-timeout` 默认 240s。
- 外部 API（心知/高德/qwen）偶发限流会导致个别轮失败，建议失败 case 单独重跑（`--cases`）后再汇总。
