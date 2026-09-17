---
name: dummy_pre_skill
description: 前置处理 hook 占位 — dispatcher 之后、coordinator 之前的 passthrough
---

# Dummy Pre Skill

前置处理 hook 点。纯 passthrough，不执行任何业务逻辑。

## 用途

承接 dispatcher 的第一条 next_skills，作为用户请求进入 coordinator 决策前的 hook 点。
完成后 framework 自动 forward 给 coordinator task（从 user_prompt 恢复原始输入）。
未来可扩展：输入校验、鉴权、富化、遥测等。

## 行为

- 接收用户原始输入（AF_INPUT）
- 不修改、不写 DataBus、不输出 next_skills
- framework 自动 forward 给 coordinator task

## Hook 契约

- 禁止修改用户输入
- 禁止输出 next_skills（否则会走 launch 路径丢失原始 input）
- 禁止写对话历史
