# 当前工作硬性约定（用户确认于 2026-09-25）

以下约定优先于本文件下方保留的 GitNexus 自动生成模板。

- 以最快速度开发：能并行的定位、实现和定向测试立即并行；优先完成可验证的关键链路，避免重复扫描、重复基线、无关重构和非必要文档工作。不为追求速度伪造结果、跳过必要验证或混用不同版本成绩。
- 不运行影响分析、图变更检查、图索引检查或重建；不再为这些检查安装、启动或调用 GitNexus。直接定位、开发和修复，以必要的定向功能测试验证，避免重复全量回归拖慢迭代。
- 持续开发、修复或监控期间，每隔不超过 60 秒向用户播报一次，使用表格；没有新结果时明确说明正在做什么及阻塞，不编造进展。
- 实验播报须对比 L0、父节点、当前节点，包含完成数、质量、平均无效 loop、时延、严格通过数以及 L1/L2/L3 实际应用次数。只能比较同题同口径的结果；局部样本注明范围，未生成的节点或尚无指标写“待生成／暂无”。
- 用户明确要求加速交付：优先修复阻止真实迭代、有效选优和发布的缺陷；必要验证通过后及时恢复实验。保留原始数据、权威 memory 和可追溯成绩，不以降低验收门槛或修改旧结果冒充成功。

<!-- gitnexus:start -->
# GitNexus — Code Intelligence

This project is indexed by GitNexus as **AgentFlow** (19631 symbols, 34951 relationships, 192 execution flows).

> Index stale? Run `node .gitnexus/run.cjs analyze --index-only` from the project root — it auto-selects an available runner. No `.gitnexus/run.cjs` yet? Bootstrap with `npx`, `bunx`, or `pnpm dlx` — e.g. `bunx gitnexus@latest analyze` (npm 11 npx crash; #1939).

## Always Do

- **MUST run impact analysis before editing.** Use `impact({target: "symbolName", direction: "upstream"})` (MCP) or `node .gitnexus/run.cjs impact "symbolName" --direction upstream --repo .` (CLI fallback); report callers, processes, and risk. Never substitute grep for graph analysis.
- **MUST analyze graph changes before committing.** Use `detect_changes({scope: "all"})` (MCP) or `node .gitnexus/run.cjs detect-changes --scope all --repo .` (CLI fallback). `partial: true` or `truncated: true` is not a clean check — a zero means unseen, not unaffected; re-run it. For regression review: `detect_changes({scope: "compare", base_ref: "main"})` or `node .gitnexus/run.cjs detect-changes --scope compare --base-ref "main" --repo .`.
- **MUST warn the user** if impact analysis returns HIGH or CRITICAL risk before proceeding with edits.
- **MUST treat `risk: UNKNOWN` as unresolved, not as low.** An empty caller set is not evidence the symbol is unused — it can also mean the callers are not resolvable by the index (plain-object property access, dynamic dispatch, cross-language calls). `impact` pairs `UNKNOWN` with a `riskNote` saying so. Confirm with a text search before treating the symbol as safe to change or delete; do not proceed on the strength of a zero.
- When exploring unfamiliar code, use `query({search_query: "concept"})` to find execution flows instead of grepping. It returns process-grouped results ranked by relevance.
- When you need full context on a specific symbol — callers, callees, which execution flows it participates in — use `context({name: "symbolName"})`.
- For security review, `explain({target: "fileOrSymbol"})` lists taint findings (source→sink flows; needs `analyze --pdg`).

## Never Do

- NEVER edit a function, class, or method before MCP/CLI impact analysis.
- NEVER ignore HIGH or CRITICAL risk warnings from impact analysis, and never read `UNKNOWN` as an all-clear — it means the walk could not answer, which is the one verdict that requires confirming by other means.
- NEVER rename symbols with find-and-replace — use `rename` which understands the call graph.
- NEVER commit before MCP/CLI graph change analysis.

## Resources

| Resource | Use for |
| --- | --- |
| `gitnexus://repo/AgentFlow/context` | Codebase overview, check index freshness |
| `gitnexus://repo/AgentFlow/clusters` | All functional areas |
| `gitnexus://repo/AgentFlow/processes` | All execution flows |
| `gitnexus://repo/AgentFlow/process/{name}` | Step-by-step execution trace |

## CLI

| Task | Read this skill file |
| --- | --- |
| Understand architecture / "How does X work?" | `.claude/skills/gitnexus-exploring/SKILL.md` |
| Blast radius / "What breaks if I change X?" | `.claude/skills/gitnexus-impact-analysis/SKILL.md` |
| Trace bugs / "Why is X failing?" | `.claude/skills/gitnexus-debugging/SKILL.md` |
| Rename / extract / split / refactor | `.claude/skills/gitnexus-refactoring/SKILL.md` |
| Tools, resources, schema reference | `.claude/skills/gitnexus-guide/SKILL.md` |
| Index, status, clean, wiki CLI commands | `.claude/skills/gitnexus-cli/SKILL.md` |

<!-- gitnexus:end -->
