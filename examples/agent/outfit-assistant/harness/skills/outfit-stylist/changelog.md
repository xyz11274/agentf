# outfit-stylist changelog

> 记录本 skill 的优化迭代（before/after 量化 + 数据来源 + 状态）。
> 与项目级 `aisf-agent-improver` 技能 changelog（记录评测/方法论）不同，本文件记录 **skill 自身资产的演进**（SKILL.md 指令、preload 工具、目录语义）。
> 方法论见 `self-evoling/启发式自优化/07-LLM轮次收敛.md` 与 `08-工具执行失败作为优化信号.md`。

## 2026-08-12 — 轮次收敛：S2/S4 探查轮归零 + 建档收敛

**背景**：UC1 多轮实测发现 skill 内 LLM 逐轮 bash 探查上传图存在性（S2）与档案目录（S4），各占 1 轮 tool_calls，且建档动作分多轮。根因是提示词显式指令与确定性预加载缺失。

**改动**：
1. `preload_user_context.py` 增加 `workspace_tree` 字段——关键目录浅层树（upload/outputs/memory），`_hint` 声明已返回字段禁查
2. preload 工作根改用 `{AF_AGENT_PACKAGE_DIR}/workspace/{user_id}_{client_id}`（用户数据目录层，含 e2b 双层 workspace 语义），不再依赖从未注入的 `AF_WORKDIR`
3. SKILL.md 删两处显式 "ls uploads/ 看有几张" 指令，改引用 workspace_tree；目录名统一 `uploads/`→`upload/`（真实沙箱目录单数）
4. SKILL.md §0.5 补建档收敛指令——write_file 自动建父目录（host/e2b 双路径），无需 bash mkdir；建档（profile + history）同一轮一次写完

**before / after 量化**（UC1 实测，数据源：scene-uc1-20260812_{191541,212911,214252,225905,234301} raw-results + skill_stderr）：

| 指标 | before（191541）| after（234301）| 变化 |
|------|----------------|---------------|------|
| S2（ls upload/ 探查）| 出现（ls 具体文件）| **0 次** | ✅ 消失 |
| S4（ls memory → DIR_NOT_EXIST）| 出现 | **0 次** | ✅ 消失 |
| bash mkdir（建档建目录）| 1 次 | **0 次** | ✅ 消失 |
| LLM 调用次数 | 8 次 | **7 次** | -1 轮 |
| LLM 合计耗时 | 93.0s | 76.0s | -17.0s（-18%）|
| 端到端耗时 | 105.9s | **88.9s** | -16.0s（-15%）|

**状态**：keep。理由：探查类冗余轮次归零（S2/S4/mkdir），LLM 轮次下降 1，端到端 -15%；剩余主瓶颈是 LLM 生成内容长度抖动与 skill 内多轮 tool_calls 不收敛（非本次改动引入）。

**遗留**：③运行态（examples/agent/）preload 需与 canonical 同步（本次已手动 cp，建议纳入 build/sync 脚本防再漏）。
