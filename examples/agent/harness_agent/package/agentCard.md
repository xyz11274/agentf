# HarnessAgent

通用 harness 技术 Agent 卡片。

## 能力
- Agent Loop：dispatcher 拉起首个技能，coordinator 依据已累计结果循环选择下一个技能，直到无技能可选则终止。
- 访问模型前通过 contextupdater 任务填充可定制 prompt（`prompts/`、`config/context_config.json`）。
- 技能候选清单可配置（`config/skills_catalog.json`）。
- 护栏：最大迭代、stuck 检测、观测截断（`config/harness_config.json`）。

## 默认技能
retrieve → analyze → compose → finalize（finalize 产出最终结果，由 coordinator 终止循环）。

## 输入/输出约定
- 技能输出统一信封：`{skill,status,can_handle,result,error,confidence}`。
- coordinator 输出 `next_skills`（继续）或空 `next_skills` + `response`（终止）。
