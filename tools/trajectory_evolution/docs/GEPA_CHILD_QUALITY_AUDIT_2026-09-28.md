# GEPA 子节点质量审计（2026-09-28）

## 审计范围

- Job：`1bf403e7f6655c570f4d9c991c78d1687c3a150f9cd6d643c3afd580cd98268c`
- 树：`trace_output/business-simulator/gepa/workspace/agent-evaluation/<job>/search/tree.json`
- 状态：Node27 完成后暂停；Node28 尚未评测；未恢复运行。
- 数据口径：训练 pool 逐题配对、50 题 Val、L0 基线；不使用 holdout 反向影响搜索。

## 事实结果

| 项目 | 数据 |
|---|---:|
| 节点总数 | 29（Node0 + 28 个派生节点） |
| 派生节点进入 pool 配对评测 | 17 个，404 个同题配对 |
| 派生节点进入 Val | 4 个（Node4/7/18/22） |
| accepted | 3 个（Node7/22 加上 Node0） |
| 派生节点最终可发布 | 0 个 |
| duplicate | 7 |
| rejected_minibatch | 6（5 个 pool_quality_regression，1 个 no_observed_training_gain） |
| unobserved_changes | 5（3 个 new_control_not_applied，2 个 no_attributed_validation_gain） |
| rejected_merge | 4（合并批次只有 16 题） |
| untriggerable_controls | 2 |
| error | 1（control binding） |
| pending | 1（Node28） |

逐题 pool 配对的 404 个样本中，质量下降 59 次、严格通过下降 44 次、invalid loop 增加 155 次、时延增加 191 次、token 增加 196 次。说明“整体均值上升”主要被未改题的随机波动掩盖，不能证明新控制有效。

Val 的代表性结果：Node7 为质量 `0.4695`、严格通过 `20/50`、invalid loop `2.34`、时延 `87.49s`、token `96,614`；Node22 为质量 `0.4782`、严格通过 `21/50`、invalid loop `3.64`、时延 `111.56s`、token `112,222`。Node22 相对 L0 质量提高约 8.2%，但 loop 增加约 67.0%、时延增加约 30.8%、token 增加约 16.2%，所以 optimization `feasible=false`，不能发布。

## 根因排序

### 1. pool 允许“质量局部改善换来 loop/尾部退化”

`classify_training_screening` 使用 Pareto `local_gain`。`screening_quality_safe` 只比较全批次平均 quality，不保护逐题尾部；因此 Node7 这类候选可以在改变题上增加 loop，仍被送入 Val。当前 `no_worse` 也主要是均值判断，和业务目标“质量提高且 invalid loop 下降”不一致。

### 2. 未归因的 Val 节点仍能成为探索父节点

`exploration_eligibility` 只要求训练局部收益，并允许相对基线最多 15% 的 Val 质量下降；它不要求 Val 的控制应用带来归因收益，也没有 loop、时延、token 守门。因此 Node4（Val quality `0.412`）和 Node18（Val quality `0.4708`、loop `3.26`）仍进入 frontier 并继续派生，放大了低质量分支。

### 3. 反思的训练证据太少，且组件轮询会主动制造无证据候选

配置将 `max_feedback_trials` 设为 8 作为初始上限，`minibatch_size=32`、`component_selector=round_robin`。多数节点实际应用变化只有 1–2 道题；经验 Node9/11/27 没有应用，schedule 节点反复遇到独立意图，loop 节点遇到缺少可编辑绑定。反思应复用 pool 中与当前组件和失败类型相关的训练证据，并审计这 8 条实际上有多少进入反思输入；上限不代表实际入模数。当前这些结果只说明相关性和候选表现，不能据历史分数宣称因果已证明。跨题成功对照仍需补足，才能检查规则是否只记住个别题目。

### 4. 经验记忆缺少反馈闭环

训练私有候选库有 321 条，全部是 `CANDIDATE` 状态；公开共享库有 39 条，其中 24 条 published、13 条 pending_support、2 条 disputed。当前 compact tree 的 `memory_feedback` 非空，共 10 条：`conflicting=5`、`regressed=3`、`insufficient_support=1`、`supported=1`。这些状态是反馈记录的归类，不能单凭历史分数证明记忆对结果的因果作用。后续应利用同题 pool 证据验证记忆应用与收益的关系，再以跨题成功对照检查可迁移性。

### 5. accepted 语义与可发布语义混淆

Node7/22 的状态是 accepted，但二者 `optimization.passed=false`；accepted 只是探索/归因状态，不是发布成功。播报和父节点选择若不区分这两个状态，会误把成本退化节点当成优质父节点。

### 6. 绑定异常被算作 error，浪费一个完整搜索轮

Node17 的 `source_type_missing_or_ambiguous`/`budget_binding_unavailable` 没有降级为 `untriggerable_controls` 或空提案，异常节点被保留为 error，后续无法复用该轮反馈。

### 7. merge 预算与证据不匹配

4 次 merge 只用 16 题，且没有形成两个父分支的逐题贡献，产生了较高的 rejected_merge 比例；这部分预算没有转化为有效子节点。

## 必须在恢复前修复

1. pool gate 增加同题尾部守门：质量下降、严格通过下降、invalid loop 增加达到硬阈值时拒绝；Pareto 局部质量收益不能绕过 loop/成本约束。
2. 探索父节点只保留根节点、Val 有归因收益且相对 L0 不退化的节点；单独保留显式的质量/loop 专长通道，不把未归因节点当通用父节点。
3. 候选至少形成 2 个同题应用证据；经验必须真实命中，控制规则必须有 typed binding。未命中直接标记 `untriggerable`，不消耗 Val。
4. 组件选择从无条件 round-robin 改为证据驱动：没有对应 `training_opportunities` 就跳过该组件；schedule 要求真实 typed binding，明确的资源依赖同样有效，不能仅按顺序词过滤。
5. 经验只加载 published 且有支持的条目；继续保留 pool/Val 的同题正负反馈到轻量 `memory_feedback`，并在审计中报告每条记录的应用与证据，不把历史分数描述成因果证明；禁止把未验证候选直接作为搜索轴。
6. 绑定失败走一次受限修复；修复仍失败则记录诊断并降级为 `untriggerable_controls`，不得中止搜索。
7. 播报明确区分 `exploration accepted` 与 `publishable`，发布候选必须同时通过质量、loop、时延、token 门禁。

## 通过标准

- 至少产生一个派生节点进入 Val，并在同题口径上相对 L0 质量提高至少 5%。
- invalid loop 相对 L0 至少下降 15%；时延、token、调用次数不增加。
- Val 和 holdout 的节点播报均包含完成数、质量、严格通过、invalid loop、时延、token、意图识别率，以及 L1/L2 action/L2 orchestration/L3 实际应用次数。
- 单次实验只保留可复用的轻量中间索引和必要轨迹；恢复不依赖版本指纹阻拦。
- 反思优先复用相关的 pool 训练证据；`max_feedback_trials=8` 是初始输入上限，每轮都要测量并报告实际进入反思的条数。建立跨题成功对照，避免把同题 pool 结果当作因果证明。
- Validation 与 holdout 数据及评分不得泄露进 GEPA 反思输入；validation 仅用于搜索期选优，holdout 只用于冻结候选后的最终审计和门禁。
- 当前环境的 `bwrap` 不可用；记录为执行环境限制，不通过安装或替代启动方式绕开。

## 反思修复实施（2026-09-28，效果待新实验验证）

- 业务准备脚本固定详细反馈上限 8、输入上限仍为 65,536 字节；上限不冒充实际纳入数。
- 提供整批已评测 pool 题目的轻量指标概览，详细证据优先跨题覆盖并包含成功对照；不追加模型调用或评测，不使用 val/holdout 内容。
- 控制轨迹保留完整 typed 字段、必要工作下界和编排依赖，压缩长输出/评语，再跨 Run 选取可执行机会；后段冗余不能被前 N 条截断隐藏。
- 经验支持按实际入模的不同任务、同一 memory 计算；单题重复回执不算多题支持。历史反证用于修订条件，不永久禁用整个经验。
- 实际发出的请求在节点记录详细题数和字节数，播报直接读取；修复调用使用其真实输入记录。
- `artifact_retention` 使用 const 读取，避免注入 null 改变后续缓存键。
- 恢复已验证快照的自动批准，保持未验证记忆过滤；关闭自动批准而未给显式审批会阻止自动评测启动。
- 新实验只清 GEPA 树和生成配置，保留 training、权威记忆、源数据和同口径 L0 报告；使用 tmux，evaluator/service 均为 16 并发。

## 真实旧报告只读重建验证

同一父 Node7、Node27 的 32 题训练 pool 报告，不调用模型：旧 Node27 实际只入模 2 题、63,569B；新请求均附 32 题指标概览，action 详细 6 题/61,335B/1 个 binding，loop 详细 4 题/60,193B/0 个 budget binding，schedule 详细 6 题/63,522B/6 个 binding。全部小于 65,536B。8 是详细反馈上限，不是每个请求保证值；loop 无 budget binding 不等于无其他原语。此项验证只证明证据覆盖与结构有效，不证明新节点质量已提高。

## 启动前回归发现与修复

- 分组汇总请求可以没有 `feedback`，实际入模统计需先检查数组类型。
- 部分失败轨迹没有 `events`，经验应用抽取应视为无观测，不能抛出 `array_required`。
- 经验应用计数的预算预留改为本批实际计数上界，避免极大占位数字挤掉本可纳入的证据。
- 中性循环记录保留最低优先级作为上下文，不因为缺少可修改机会而删空整个运行，也不据此授予修改权限。旧测试访问空数组会段错误，现先检查非空，再验证详细动作上限。
- Pareto 探索仍允许单项局部收益，但中性无收益不算通过；同步更新旧测试预期。
