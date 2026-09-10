# 图集与图示约定

[文档首页](../README.md)

所有图为本地 SVG，可直接查看或编辑，支持随安装包离线使用。
蓝色表示宿主、输入或基础层，绿色表示结果与运行边界，紫色表示后台学习或策略。规划内容单独标注。
橙色表示条件检查、等待或异常状态；时间线 T0–T7 表示事件顺序。

图件采用浅色分区与圆角卡片，按两级记忆、运行生命周期和数据流组织。
图中用户、任务和时间为说明示例。更新接口或配置时，同步维护正文与图件。

| 章节 | 图 | 主题 |
| --- | --- | --- |
| [01](../01-overview.md) | [组件架构：主链与后台分离](01-system.svg) | 两种入口复用学习算法，分别由宿主进程与常驻服务维护状态。 |
| [01](../01-overview.md) | [L0–L4：接入位置与能力分级](01-level-capabilities.svg) | L0–L3 对应采集与控制接口；L4 在后台搜索并验收新策略。 |
| [02](../02-installation.md) | [独立安装包的交付边界](02-package.svg) | 凭据与业务数据不进入安装包；部署配置使用自己的受保护副本。 |
| [02](../02-installation.md) | [部署方式：一个目录，一个写入所有者](02-owners.svg) | 每个 data_dir 由一个原生进程或 serve 服务持有写入权。 |
| [03](../03-configuration.md) | [配置的三层归属](03-config.svg) | sharing 默认 true；auto_approve_experiences 默认 false。 |
| [03](../03-configuration.md) | [配置更新与策略生效时机](03-hot-reload.svg) | 服务权限与规则重新加载；在途 run 使用已固定策略。 |
| [04](../04-quickstart.md) | [第一条任务：求和与真实边界](04-first-run.svg) | 业务答复不等待评测；这条无问题任务允许产生空记忆。 |
| [04](../04-quickstart.md) | [快速开始：三个验收对象](04-results.svg) | 先验证真实边界，再逐步启用模型和策略。 |
| [05](../05-protocol.md) | [身份层次与横向因果](05-identity.svg) | user_id 标识用户，actor_id 标识执行者，scope 划分策略分区。 |
| [05](../05-protocol.md) | [通用协议的四种字段职责](05-contract.svg) | 自定义字段通过规则、提示词或分析器定义语义。 |
| [06](../06-native-runtime.md) | [LocalRuntime：状态留在宿主进程内](06-native.svg) | 实例和后台线程随宿主进程结束；新实例创建新的活跃槽位。 |
| [06](../06-native-runtime.md) | [函数返回与进程退出是两种事件](06-native-lifetime.svg) | 开始快照标记任务，终态发布完整记录；异常退出保留已落盘内容。 |
| [07](../07-http-and-clients.md) | [HTTP 请求：状态先提交，再回复](07-http.svg) | HTTP 有通信与持久化成本；模型评测不在业务响应链路里。 |
| [07](../07-http-and-clients.md) | [相同 JSON，不同生命周期](07-transports.svg) | 根据任务所有者的生命周期选择入口。 |
| [08](../08-instrumentation.md) | [真实接入点：谁负责哪一层](08-mounts.svg) | ReAct 通过事件回调接入统一边界。 |
| [08](../08-instrumentation.md) | [宿主责任与进化责任](08-ownership.svg) | L1–L3 分别接入模型发送、动作执行和循环调度边界。 |
| [09](../09-async-and-multi-agent.md) | [Coordinator 退出，run 仍继续](09-dispatch.svg) | 前提：服务仍活着、任务已登记、消息携带上下文、最终汇总方收尾。 |
| [09](../09-async-and-multi-agent.md) | [并行归因：三种结果分别处理](09-branches.svg) | 待办检查跟踪已登记活动；跨进程作业通过上下文上报。 |
| [10](../10-capture-and-validation.md) | [从边界到可解释轨迹](10-trace.svg) | Episode 与 loop 分别统计，清洗保留原始执行事实。 |
| [10](../10-capture-and-validation.md) | [失败并不只有一种](10-integrity.svg) | 业务状态和采集完整性两条轴独立。 |
| [11](../11-evaluation.md) | [Hybrid Evaluator：规则与模型职责](11-hybrid.svg) | 评测模型状态、业务结果和候选支持分别记录。 |
| [11](../11-evaluation.md) | [模型调用与预算分层](11-budgets.svg) | 逻辑调用、HTTP 尝试和业务模型调用分别计数。 |
| [12](../12-reflection-and-candidates.md) | [从诊断到候选：内容必须有证据](12-candidate.svg) | 元反思基于提供的来源用户和证据事件生成建议。 |
| [12](../12-reflection-and-candidates.md) | [支持、反证与未知](12-counterevidence.svg) | 按独立证据统计支持，按任务对照评估收益。 |
| [13](../13-private-memory.md) | [私库管理：插入、合并、关系、淘汰](13-private.svg) | 策略编译时检查状态、冲突、所有权与授权。 |
| [13](../13-private-memory.md) | [私库三层身份与审计](13-private-fields.svg) | 查看完整批次的插入、合并与淘汰决策。 |
| [14](../14-shared-memory.md) | [两级记忆：来源不公开，经验可共享](14-two-pools.svg) | 跨用户不读对方私库；sharing 默认开启，经验自动注入仍需授权。 |
| [14](../14-shared-memory.md) | [公共状态：支持与争议共同决定](14-public-states.svg) | 后台更新公共状态；已开始的 run 保留原策略快照。 |
| [14](../14-shared-memory.md) | [晋升审查：提炼与复核两次请求](14-review.svg) | 缺独立推广证据不发提炼请求；原始模型正文不进入公共账本。 |
| [14](../14-shared-memory.md) | [私有—公共关系：局部使用与全局信任](14-cross-relations.svg) | 关系绑定双方内容版本，私有经验优先并按资格与授权筛选。 |
| [14](../14-shared-memory.md) | [各用户支持独立过期](14-expiry.svg) | 各用户独立续期；图示采用默认两名支持用户的发布门槛。 |
| [15](../15-policy-snapshots.md) | [一个 run，一个固定策略](15-freeze.svg) | 新 run 采用更新后的策略；已开始的 run 保留原策略版本。 |
| [15](../15-policy-snapshots.md) | [从两库到一次有界注入](15-selection.svg) | 运行时在有界快照中检索，再按条件和预算注入经验。 |
| [16](../16-runtime-controls.md) | [L1：怎样才算经验实际应用](16-applied.svg) | 按实际发送的模型输入核验 applied 回执。 |
| [16](../16-runtime-controls.md) | [L2 与 L3 的作用范围](16-controls.svg) | 动作与循环规则通过部署配置授权，在对应边界执行。 |
| [17](../17-learning-scheduler.md) | [后台忙时，新任务进入下一批](17-queue.svg) | 公共管理与私人学习共享后台工作者，按队列和预算执行。 |
| [17](../17-learning-scheduler.md) | [哪些输入会触发新的学习](17-dedup.svg) | 完整新轨迹按批次进入学习；已成功评测的来源复用报告。 |
| [18](../18-storage-and-recovery.md) | [data_dir：权威源与派生视图](18-storage.svg) | 同名 evolution-memory.pb 的私有/公共 schema 不同。 |
| [18](../18-storage-and-recovery.md) | [持久化与模型的崩溃窗口](18-checkpoint.svg) | 第 3、4 步之间中断时，模型调用结果待确认；重试可能产生额外费用。 |
| [19](../19-datasets-and-benchmarks.md) | [评测集：任务、标准与冻结版本](19-dataset.svg) | 任务评测、策略准入和 GEPA 使用同一执行器与评分标准。 |
| [19](../19-datasets-and-benchmarks.md) | [一次 Trial 的四类结果](19-scoring.svg) | complete 表示全部计划试验均有终态，成功率按验收结果统计。 |
| [20](../20-security-and-limits.md) | [信任边界：数据、建议与权限](20-trust.svg) | 内部账本供后台管理；公共投影只保留审核后的经验内容。 |
| [20](../20-security-and-limits.md) | [有限资源下的不同失败策略](20-resource.svg) | 根据负载监控主链时延、降级计数和采集完整性。 |
| [21](../21-operations.md) | [按阶段定位“为什么没生效”](21-diagnosis.svg) | 结合业务结果、轨迹和后台状态定位问题。 |
| [21](../21-operations.md) | [运维视图：独立查询后台状态](21-status.svg) | 状态查询放在运维路径；业务边界继续使用当前内存策略。 |
| [22](../22-testing-and-extension.md) | [从机制回归到真实收益验收](22-tests.svg) | 组件回归覆盖机制；部署验收覆盖实际框架、模型与负载。 |
| [22](../22-testing-and-extension.md) | [扩展哪一层，需要同步什么](22-extension.svg) | 通用协议保留基础字段，框架专属信息放入扩展字段。 |
| [23](../23-import-and-migration.md) | [历史日志进入同一条学习路径](23-import.svg) | learn 与在线服务按单写入者约束使用 data_dir。 |
| [23](../23-import-and-migration.md) | [旧入口与统一入口的职责对照](23-migration.svg) | 依据原始日志按用户迁移，再通过审核生成公共经验。 |
| [24](../24-evolution-and-gepa.md) | [证据回流：从轨迹到下一次运行](24-loop.svg) | 应用回执记录执行情况，任务级对照评估进化收益。 |
| [24](../24-evolution-and-gepa.md) | [GEPA：反思变异、双亲重组与留出集准入](24-gepa.svg) | 内部节点与其他分支联合生成一个后代，保存多父谱系，最终经留出集准入发布。 |
