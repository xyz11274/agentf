# 15 · 记忆检索、策略快照与版本

[文档首页](README.md)

## 本章目标

区分 memory_id、库 revision、内容版本、快照 ID 和运行策略版本。

## 设计思路

每个 run 冻结当前用户私有库加可用公共库的合格视图，再按精确授权、scope 和当前条件检索。词面检索与模型关系判定是两个阶段，不会每次模型调用都重新审查整个记忆库。

![一个 run，一个固定策略](figures/15-freeze.svg)

读图：新 run 采用更新后的策略；已开始的 run 保留原策略版本。 [放大 SVG](figures/15-freeze.svg)。

## 怎样把记忆编译成可查询的经验快照

### 启用评测准入后的版本流程

在 service 配置中指定 `policy_evaluation_file` 后，后台先从合格记忆和授权规则编译候选，
在独立 Agent 实例中执行固定任务集，通过准入后登记发布。新 run 从内存中的已发布版本创建本次运行策略；
后台尚未完成时沿用仍兼容的已发布版本，没有可用版本则按 L0 原始逻辑继续，不等待评测。

`agent_version_id` 是可跨 run 比较的行为版本：包含 Agent 部署修订、用户、运行等级、经验及控制规则；
hash 排除本次 run/conversation ID、快照 ID 和记忆库修订等运行或管理元数据。
`policy_id` 仍标识绑定到当前 run 的具体运行策略。两个 run 可以有不同 policy_id、相同 agent_version_id。

候选、父版本、任务报告及发布指针保存在 `data_dir/agent-evaluation/`，重启继续使用相同发布记录。
应用前重新核对等级、规则、经验内容授权与关系；撤权、改写、删除或不兼容关系使旧版本失去后续 run 的应用资格。
未启用此配置时仍按本章原有精确授权路径运行。完整操作见[任务评测](19-datasets-and-benchmarks.md)。

### Memory 不会原样塞进模型

`src/shadow_runtime.cpp` 的 `compile_shadow_policy` 为当前 Run 筛选经验。
候选类型有多种，但当前自动注入只读取符合新契约的 EXPERIENCE_INJECTION。
RETRY_POLICY、LOOP_STOP_CONDITION 等候选仍可保存、分析，**不会自动翻译成 L2/L3 控制规则**。

每条**私人**经验必须同时通过：

- scope 在当前部署授权范围内。
- 有支持证据，支持用户数为 1，所有支持证据都属于当前用户。
- 类型为 EXPERIENCE_INJECTION，生成器和字段符合新经验契约。
- 无反证，也没有 CONTRADICTS 关系；冲突关系的两端都会被排除。
- entry.status 是 CANDIDATE 或 VALIDATED，且 candidate.status 是 PROPOSED。
- 内容与安全检查通过，并未超过快照容量。

所以一条记忆已写入 PB，但支持不足、类型不对或冲突，快照里没有它，是正常筛选。
entry.status 还是 CANDIDATE，但内部 candidate 已 PROPOSED 时，则可以在满足其他条件后进入快照；不必先自动晋升为 VALIDATED。

被排除原因进入 `excluded_counts`，例如 ownership、target_type、experience_contract、conflict、status、capacity。
它们比“记忆库有几条”更能解释为什么实际没注入。
公共经验通过单独的受信 SharedExperienceStore 输入，不放宽上面的私人归属检查，具体机制见[公共与私有协作：晋升、冲突、信任与淘汰](14-shared-memory.md)。

### 快照保存什么，版本怎样计算

快照保存本次身份、允许的 scope、私人 memory_revision、公共 shared_revision、限制选项和经过筛选的经验。
经验包含 memory_id、条件、行为和检索词项；私人经验保留来源 candidate IDs，公共经验设置 application_shared，
不携带他人的来源 ID 或历史。跨库重复关系只保留入选条目间的、版本一致的关系；容量裁剪同步移除孤立关系。

```text
content_revision = SHA256(该经验除 content_revision 外的稳定序列化内容)
snapshot_id = "shadow-v1:" + SHA256(快照除 snapshot_id 外的稳定序列化内容)
```

在线快照使用 schema_version=2；读取契约时以该字段为准，ID 使用 shadow-v1 前缀。
旧 schema 1 可离线读取，在线使用时转换到 schema 2。

content_revision 不只是建议正文的 hash，来源别名、条件等快照字段变化也可能使它改变。
授权按内容版本精确匹配，版本变化后重新核对授权。
容量不足时，当前实现按 memory_id 稳定遍历后截取，不按收益或置信度挑出全库最优集合。
随包配置最多 128 条经验、256 KiB 快照，并同时检查 Protobuf 与 JSON 的大小。

### 关键词怎样提取

只从 `trigger_pattern` 提取适用条件关键词，不从历史失败描述或建议正文提取。
否则“以前提到过视频失败”容易让所有视频任务误命中一条特定前置条件经验。

- ASCII 字母/数字/下划线组成的词，转小写，长度至少 2。
- 连续汉字生成相邻双字词，例如“缺少图片素材”产生“缺少、少图、图片、片素、素材”。
- 非汉字边界会中断汉字组合；不使用中文语义分词或词向量。
- 去掉配置中的 stop_terms，去重后保留最多 max_match_terms 项。

策略检索使用**中文字符双元组 + ASCII 词**；记忆短名单使用**字节三元组 Dice**。


![从两库到一次有界注入](figures/15-selection.svg)

读图：运行时在有界快照中检索，再按条件和预算注入经验。 [放大 SVG](figures/15-selection.svg)。

## 怎样检索经验并生成策略

### 检索是确定性的词面匹配

`ShadowRuntime::query_advice` 先核对用户/会话/Run 身份、scope 和输入预算，再对 `task + state` 提取词项。
对每条同 scope 经验计算：

```text
matched_terms = 经验条件词项 ∩ 当前任务/状态词项
score = |matched_terms|
```

命中数至少为 `min_matched_terms` 才入选，默认 2。
先按有效授权过滤，再优先私人经验、同类按命中数降序和 memory_id 升序排列；跨库重复只选一份，
最后受数量与总字节预算约束，随包最多 3 条、4096 字节。未授权的私人经验不会耗掉公共经验的 top-K 名额。
纯 Shadow 查询没有 Runtime 授权概念，仍只提供未应用建议。
这里没有把 memory.confidence、历史收益、余弦距离或 BM25 加权到运行时排序中。

命中只说明词面相关。例如“已经提供图片素材”也可能命中包含“缺少图片素材”的词项。
词面检索生成 shadow 建议，再按权限和上下文条件筛选。
注入模板提示宿主模型核对当前条件、权限和预算。

### 从影子建议到获准策略

`src/policy_runtime.cpp` 的 `compile_runtime_policy` 将以下内容编译为固定策略：

1. 已验证的 schema 2 经验快照。
2. 用户与可选 conversation 白名单。
3. 宿主已声明能力与请求等级。
4. `approved_experiences` 中的 memory_id + content_revision 精确授权。
5. 外部配置的动作规则、循环规则、注入模板与大小限制。

能力逐级要求模型输入边界、动作过滤边界、循环决定边界。
策略编译支持 strict 和 downgrade：前者在接入能力不足时拒绝，后者降到宿主已满足的等级。
通用接入的 `model/actions/loop` 能力会映射为内部策略能力；宿主仍必须正确使用返回值。

`policy_id = runtime-v2:<固定策略内容的 SHA-256>`，包含本次快照与配置。
每次查询在这个策略中决定，不在主链重新读记忆文件或调用模型。

默认 `auto_approve_experiences=false`，没有匹配的版本授权就不注入。
显式开启自动批准时，系统在建立 Run 时为通过快照筛选的经验添加当前版本授权。
该开关自动授权 L1 经验使用；L2 动作规则和 L3 循环规则通过 runtime_options 分别配置。

## 新记忆何时影响下一次执行

当前本地路径的时间顺序：

```text
Run A 开始：从内存记忆 R10 编译策略 P-A，并冻结
Run A 执行：只查 P-A，产生真实调用与应用回执
Run A 结束：先返回业务，后台保存并排队；满足批次条件后评测、反思和入库
后台提交私人记忆 R11；公共管理独立更新公共修订 S，成对刷新私人＋公共内存视图
Run B 开始：从已刷新的 R11 编译 P-B，重新执行筛选和授权
```

如果 Run B 在后台完成前已开始，它仍使用旧记忆，这是有意的低延迟与一致性取舍。
后台更新不替换进行到一半的 Run 的策略，所以回放时可以解释“这轮到底用了哪套规则”。
P-A/P-B 包含运行身份，ID 不同不一定意味着经验正文改变；判断经验内容变化应看 content_revision。

这里有不同的版本概念：私人 memory_revision 和公共 shared_revision 是持久化修订号；policy_id 是本次运行策略快照标识；
memory_version 为进程内缓存失效计数，重启后可重置。
L1–L3 的完整策略快照保存在 runs/<run-id>.json 的 policy 中，本地路径由 writer 异步落盘，
未落盘就崩溃仍可能丢失。GEPA 的实验版本另外同步保存于 `agent-evaluation/<job-id>/search/versions/`，
父子关系、逐题成绩与搜索进度保存在同目录的 `tree.json`；版本行为 ID 不受本次运行身份影响。
已发布的完整策略包可通过 `policy_version_file` 离线加载，详见[GEPA 与版本持久化](24-evolution-and-gepa.md)。

## 公共与私有经验的联合筛选

编译私有工作集先做归属/冲突/状态/契约检查，再接收受信 SharedExperienceStore。
公共条目与当前合格私有条目按双方内容版本核验关系；任何同 scope 必需关系未知时，当前用户暂不合用该公共条目。
快照先保留私有，再加入安全公共经验，条数与 JSON/PB 字节预算都计算，重复关系也计入容量。

运行时过滤实际批准的 memory_id/content_revision 后，才做检索、排序和 top-K。
私有优先决定同次检索的选择次序，各条目的来源和所有权保持独立。
已入选的私有重复项优先；私有项未获授权或未命中时，继续筛选可用公共项。

## 版本定位表

| 版本 | 谁生成/保存 | 何时改变 |
| --- | --- | --- |
| 私库 revision | 私人入库提交，保存在 EvolutionMemoryStore | 成功批次/容量等实际写入 |
| 公共 revision | 公共 manager，保存在账本与投影 | 可用公共经验投影变化 |
| content_revision | 策略编译器，对实际经验内容计算 | 参与内容变化，精确授权需重核 |
| snapshot_id | 影子快照编译器 | 身份、库视图、选项或候选集变化 |
| policy_id | 运行策略编译器 | 快照或运行时授权/规则等变化 |
| 用户内存视图版本 | IntegrationService 的进程内通知 | 私库或该用户公共/关系视图变化 |

用户关系变化不一定增加根公共 revision，但会影响该用户的可编译视图。
公共账本关系中的内容摘要和 ShadowExperience 的 content_revision 使用不同对象计算，
approved_experiences 使用对应经验的策略内容版本。

策略版本固定后，任务输入、网络数据与模型输出仍按各次运行分别记录；
要复现与对照，需同时保存任务集、真实输入、实际模型配置和工具边界。

## 实现定位

- `src/shadow_runtime.cpp::compile_shadow_policy / ShadowRuntime::query`
- `src/policy_runtime.cpp::compile_runtime_policy`
- `src/local_runtime.cpp`
- `proto/shadow_policy.proto`
- `proto/runtime_policy.proto`

---

[文档首页](README.md) · [上一章：公共与私有协作：晋升、冲突、信任与淘汰](14-shared-memory.md) · [下一章：L1–L3：经验注入、动作约束和循环控制](16-runtime-controls.md)
