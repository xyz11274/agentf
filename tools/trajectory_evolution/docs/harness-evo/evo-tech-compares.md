# 自进化 Agent 技术与框架竞争分析报告

> 版本：2026-09 整理
> 范围：当前主流自进化（Self-Evolving / Recursive Self-Improvement）Agent 框架与技术路线
> 对比对象：**Trajectory Evolution（自研引擎）**、openJiuwen RSI、DeepSeek Harness(DSH)+DSH-Evolve、EvoAgentX、SE-Agent、AgentFactory、PenguinHarness、Hermes Agent、CORAL、ADAS、EvolveR、Darwin-Gödel-Machine

---

## 一、总体判断

当前自进化 Agent 的主流共识已经从"微调模型权重"转向"**进化 Harness / 工作流 / 技能库**"，核心闭环是：

```
执行轨迹 → 反思归因 → 变异/补丁 → 评测筛选 → 保留最优
```

也就是：进化的对象主要是 Agent 的运行策略（提示词、工具规则、技能、工作流拓扑），而不是模型参数本身。参数层面的 SFT/RL 自进化（如 STaR、V-STaR）在工程落地中仍属少数，成本高、易灾难性遗忘，通常只在闭环稳定、评估信号可靠之后才考虑内化到权重。

---

## 二、技术对比总表

| 框架 | 开发主体 | 进化核心对象 | 核心算法/范式 | 架构特点 | 主打场景 | Benchmark | 开源&生态 | 核心优势 | 短板 |
|---|---|---|---|---|---|---|---|---|---|
| **Trajectory Evolution（自研引擎）** | AgentFlow 自研 | ✅ Harness 完整策略：经验文本（L1 注入）、动作规则（L2 Allow/Reject＋必填参数护栏）、循环规则（L3 Continue/Replan/Stop） | GEPA 反思式 DAG 搜索的 C++ 受限实现：多父谱系、双亲重组、逐题/多目标 Pareto 前沿、真实 Agent 试验筛选、Holdout 准入；规则＋模型 Hybrid 评测；Sørensen–Dice 确定性记忆治理 | 主链/后台分离、外挂低侵入；C++ 内核（约 2.4 万行）＋原生 ABI/HTTP 双入口；8 语言客户端；5 份 protobuf＋OpenAPI；每 run 冻结策略快照，授权字段入不可变 hash | 企业内 Agent/Harness 策略持续优化、防劣化准入、私有化离线进化 | 冻结任务集 train/val/holdout 自建基准（任务入口绑定真实 Agent） | 随 AgentFlow 开源；约 40 个测试文件、1.5 万行测试；不依赖 DSPy/Python/向量库 | **「采集→评测→反思→两级记忆→受控应用→回执核验→搜索→Holdout 准入→发布」全链路代码闭环**；L0–L4 渐进、可停可回退；应用回执按实际输入核验；撤权即时失效；基线/准入/搜索口径统一；可纯离线运行（learn CLI＋published-version 包） | 不进化 Artifacts 交付物（不及 RSI 双维度）；不进化 Agent 源码（不及 DGM/AgentFactory）；不进化多 Agent 拓扑（保留宿主调度，不及 EvoAgentX/CORAL）；评估仍靠规则＋模型 Judge（非 ADAS 无监督）；新引擎，暂无公开第三方跑分；C++ 协议层扩展需重新编译 |
| **openJiuwen RSI** | 华为 2012 实验室 | ✅ Harness（Prompt/Skill/Tool/Rail）<br>✅ Artifacts（论文/代码交付物）**双维度** | PUCT/Frontier 搜索 + 文本梯度优化；四层架构（任务层/迭代优化层/执行层/基础层）；版本化 + 沙箱验证闭环 | 平台化、原生多 Agent Swarm 蜂群；内置基线、版本管理、资源预算控制、可观测；与 WorkSwarm 深度绑定 | 企业办公、文档/论文、算法代码、多智能体协同 | 自研办公+代码任务集，正在接入通用基准 | 开源 openJiuwen 体系；国内云/鸿蒙生态强 | **唯一同时进化 Agent 策略 + 任务产出物**；原生多 Agent 协同自进化；工程化完整（沙箱、版本、回滚、资源管控齐全）；国产算力亲和，KV Cache 协同调度 | 发布时间新，外部社区案例少；偏向华为栈，异构大模型适配弱；SWE-bench/GAIA 公开跑分不多；偏重办公场景，通用开放域弱 |
| **DeepSeek Harness (DSH) + DSH-Evolve** | DeepSeek 团队 | Harness（提示词、工具规则、子 Agent 调度策略） | Harness 补丁迭代，轨迹归因，LLM-as-judge 评测；插件式进化，不侵入 DSH 内核 | 插件化 Harness 运行时；独立 Evolve 插件；人工闸门，补丁隔离上线 | 通用 Agent、命令行、代码 Agent、复杂推理 | Terminal-Bench、GAIA、HotpotQA | 开源，社区活跃，大量第三方插件 | Harness 生态成熟；模型无关，可接任意 LLM；上手资料多，社区案例丰富；适合独立研究自进化 Harness | **不进化交付产物 Artifacts**；原生不是多 Agent 蜂群；进化成本偏高，token 消耗大；缺少原生版本基线管理，需要自己实现 |
| **EvoAgentX** | 独立开源团队 | 多 Agent 工作流拓扑、子 Agent 分工、提示词 | 遗传进化：变异、重组、筛选；轨迹树搜索；AFlow/MIPRO 底层 | 以 Agent 工作流图为种群；自动生成多 Agent 团队，评估择优 | 多 Agent 编排，复杂多跳推理 | HotpotQA、MATH、MBPP、GAIA | 开源，海外社区 | 专门进化 **Agent 拓扑结构**；可从零自动搭建多 Agent 流水线；支持轨迹树多分支探索 | 不支持 Artifacts 迭代；工程配套弱，缺少原生沙箱、版本管理；生产落地成本高，偏向学术 demo |
| **SE-Agent** | NeurIPS 学术项目 | 任务推理执行轨迹（轨迹树） | 轨迹修正 Revision、轨迹交叉 Recombination、精简 Refinement；遗传式轨迹进化 | 以单任务推理轨迹作为种群，搜索更好推理路径 | 代码修复、SWE 类任务 | SWE-bench Verified | 学术开源原型 | 轨迹树理论最成熟，在 SWE-bench 上效果突出；专注优化推理路径 | 纯学术原型，工程能力缺失；没有 Harness/Skill 管理，不能长期持续迭代；无多 Agent 原生支持 |
| **AgentFactory** | 开源社区 | 可执行 Python 子 Agent / 代码技能单元 | Meta-Agent 分析失败轨迹，自动修改代码技能，沉淀技能库 | 技能 = 可执行代码；自动生成、测试、归档代码子 Agent | 软件自动化、代码 Agent、SWE-bench | SWE-bench | 开源 | 技能直接是可运行代码，不是 prompt；代码能力强，可沉淀可复用工具 Agent | 不优化 prompt/工作流 Harness；非代码场景弱；缺少平台化基线管理 |
| **PenguinHarness** | LlamaFactory 作者 | Harness（提示词、技能策略） | 轻量 Harness 补丁、轨迹蒸馏 | 极简轻量化，剥离复杂平台组件，降低自进化上手门槛 | 本地小规模实验、快速跑通 Harness 进化闭环 | GAIA、简单推理基准 | 开源 | 轻量、算力成本低；上手简单；适配 Qwen/DeepSeek 本地模型 | 功能精简，缺少版本、资源管控；无多 Agent 协同；无 Artifacts 优化，生产级能力不足 |
| **Hermes Agent（Harmes）** | NousResearch | Harness、技能库、系统提示、工具代码 | GEPA 遗传帕累托提示进化 + DSPy；执行轨迹离线变异 | 三级技能存储架构；任务中持续构建/修改技能；沙箱测试 + 人工 PR 闸门 | 通用自动化、数据分析、工具调用 Agent | GAIA、自定义任务集 | 开源，社区增长快 | 边运行边进化技能，不需要离线训练；多粒度技能沉淀；低成本 API 进化，不需要微调模型 | 原生多 Agent 弱；复杂科研长任务稳定性差；缺少全局基线管理 |
| **CORAL** | Human-Agent-Society（COLM 2026） | 多 Agent 组织架构、任务方案、实验脚本 | 分层 Grader 评测、隔离 Workspace、共享 Hub 记忆；多智能体组织进化 | 多 Agent 组织架构；独立隔离工作区、分级打分器、持久共享记忆；兼容 OpenCode/ClaudeCode | 自主科研、代码实验、自动做实验、开放发现 | Erdos 最小重叠问题、代码实验基准 | 开源 | 面向科研多 Agent 组织进化；收敛速度远快于 AlphaEvolve；内置分级评估器 | 偏向科研实验场景；普通办公/文档场景适配差；部署配置较重 |
| **ADAS（Agent-Forge）** | Agent-Forge 项目组 | Agent 种群、多 Agent 系统本身（元进化） | 自主无监督评估、Quality-Diversity 档案、Ouroboros 自进化引擎 | 黑板通信 + 分层多智能体；**不需要人工标注评价信号**；持续种群演化 | 通用 Agent 种群进化、元 Agent、开放域探索 | 自定义连续进化基准 | 开源生态 | 解决自进化最大瓶颈：脱离人工打分；支持元进化（进化进化器本身） | 评估器幻觉风险高；计算开销巨大；落地工程配套薄弱，偏向理论研究 |
| **EvolveR** | 独立研究项目 | 递归自改进 Agent 策略、规划器 | 递归进化，每代改进元进化策略；自参照反思 | 分层元 Agent 架构，进化策略本身，嵌套进化循环 | 复杂推理、多步骤规划、开放域任务 | HotpotQA、MATH | 研究原型 | 递归嵌套进化，优化进化算法本身；强反思归因能力 | 公开资料少，社区小；工程原型，沙箱/安全机制简陋 |
| **Darwin-Gödel-Machine (DGM)** | Sakana AI | Agent 自身完整 Python 代码（含进化逻辑） | 开放进化存档采样，从历史存档挑选父代做变异；实证评测替代哥德尔机形式化证明 | 维护全版本 Agent 存档库；自修改 Agent 源码；沙盒 + 人工监督 | 代码 Agent、SWE-bench 代码修复 | SWE-bench Verified、Polyglot | 开源论文工程 | **可以修改 Agent 自身完整代码（含进化逻辑）**；存档采样跳出局部最优，SWE-bench 提升显著 | 只聚焦代码任务；进化开销极大；开放域/文档场景几乎不可用；安全风险高 |

---

## 三、各框架一句话定位

1. **openJiuwen RSI**：**企业生产级双维度自进化平台**，同时进化 Agent Harness 与任务交付物 Artifacts，原生多 Agent 蜂群，面向办公/论文/代码交付。
2. **DeepSeek Harness (DSH) + DSH-Evolve**：**Harness 补丁进化的事实标准**，插件式、模型无关，靠轨迹归因生成提示/工具规则补丁，社区最成熟。
3. **EvoAgentX**：**多 Agent 工作流拓扑自动进化**，把 Agent 编排图当种群做遗传变异/重组，从零自动搭多智能体流水线。
4. **SE-Agent**：**轨迹树进化的学术代表**，把单任务推理轨迹当种群做修正/交叉/精简，SWE-bench 上验证突出。
5. **AgentFactory**：**代码即技能**，Meta-Agent 自动生成、修改、测试可执行 Python 子 Agent，沉淀可复用代码技能库。
6. **PenguinHarness**：**轻量 Harness 自进化入门框架**，极简、低成本、本地可跑，适合快速验证 Harness 进化想法。
7. **Hermes Agent**：**技能优先的 Harness 自进化**，在线持续沉淀技能，不需要离线训练，最适合工具自动化。
8. **CORAL**：**科研多 Agent 组织进化**，多智能体团队自动做实验、迭代方案，主打 autoresearch。
9. **ADAS**：**自主评估引擎**，解决 LLM-as-judge 需要人工标注的痛点，可以进化 Agent 种群甚至进化器本身。
10. **EvolveR**：**递归嵌套进化**，进化的不光是 Agent，连进化策略本身一起迭代，偏理论研究。
11. **DGM 达尔文哥德尔机**：**代码级自我重写**，Agent 直接修改自己的 Python 源码，开放进化存档，SWE-bench 标杆。
12. **Trajectory Evolution（自研）**：**代码闭环最完整的"受控进化"引擎**——把 GEPA 反思搜索用 C++ 落地为可核验、可准入、可离线加载的生产策略，L0–L4 渐进分级，主打"进化不劣化上线"。

---

## 四、技术路线归类

| 路线 | 代表框架 | 进化对象 |
|---|---|---|
| Harness / Prompt 进化 | Hermes、DSH、PenguinHarness | 提示词、工具规则、调度策略 |
| 轨迹 / 推理路径进化 | SE-Agent、EvoAgentX | 单任务推理轨迹树 |
| 代码 / 技能单元进化 | AgentFactory、DGM | 可执行代码子 Agent、Agent 自身源码 |
| 多 Agent 组织 / 种群进化 | CORAL、ADAS | 多智能体团队架构、Agent 种群 |
| 双维度（Harness + 交付物）平台化生产路线 | openJiuwen RSI | Agent 策略 + 任务 Artifacts |
| Harness 全策略受控进化（采集→受控应用→准入工程闭环） | **Trajectory Evolution（自研）** | 经验文本＋动作/循环规则（完整 RuntimePolicy） |
| 递归元进化 | EvolveR、ADAS、DGM | 进化策略 / 进化器本身 |

---

## 五、openJiuwen RSI 核心差异化（竞争要点）

### 5.1 独有：Harness + Artifacts 双维度进化
其他框架几乎全部只进化 **Agent 本身（Harness / 工作流 / 轨迹）**；RSI 可以同时：
1. 进化 Agent 怎么干活（Harness：提示词、技能、工具调用规则、约束 Rail）；
2. 进化 Agent 产出的交付物 Artifacts（论文、算法代码等成果本身）。

> 适合办公场景：Agent 不仅学会更好的写作流程，还能持续迭代论文/代码成品，不断打磨交付物。

### 5.2 原生多蜂群 Swarm 协同自进化
openJiuwen 本身就是 **多 Agent Swarm 平台**，RSI 支持双层演进：单个 Agent 技能 + 整个蜂群团队分工一起进化。
- DSH / PenguinHarness 以单 Agent Harness 为核心，多 Agent 是外挂；
- EvoAgentX 虽然进化多 Agent 拓扑，但没有企业级 Swarm 协同编排。

### 5.3 完整工程四层闭环（生产导向）
任务层 → 迭代优化层 → 执行层 → 基础层，内置：基线建立、候选方案生成、沙箱隔离、版本归档、资源预算控制、失败经验沉淀、回滚机制。

> 其他很多框架是**算法原型**，沙箱、版本、资源限制需要开发者自己手写。RSI 是平台原生自带，面向企业落地。

### 5.4 短板与竞争劣势
1. **场景锁定**：优先办公、文档、论文、算法代码；通用开放域、网页浏览、操作系统 Agent 不是重点。
2. **社区历史短**：RSI 2026 年 9 月刚发布，外部独立评测、第三方案例远少于 DSH、EvoAgentX。
3. **生态绑定**：深度和 openJiuwen WorkSwarm 平台绑定；异构大模型、第三方工具链适配不如 DSH 灵活。
4. **公开学术基准数据偏少**，更多是内部办公任务验证，SWE-bench/GAIA 公开跑分不多。

---

## 六、自研 Trajectory Evolution 引擎：代码级技术对比

> 代码位置：`tools/trajectory_evolution/`（C++ 内核 `src/`、24 章组件文档 `docs/`、5 份 protobuf `proto/`、8 语言客户端 `clients/`）。以下结论均可回溯到具体源码。

### 6.1 基本档案

| 项 | 实现情况 |
|---|---|
| 定位 | 外挂式、低侵入的 Harness 自进化引擎；模型与工具仍由宿主执行，引擎只在执行边界采集、学习、授权和控制 |
| 规模 | C++ 内核约 2.4 万行；最大模块 `evolution_pipeline.cpp`（3749 行）、`integration_service.cpp`（2392 行）、`gepa_search.cpp`（1079 行） |
| 接入面 | ① 进程内原生 ABI（LocalRuntime，主链低延迟）② 独立 HTTP 服务（跨短进程、服务持有状态）③ `learn` CLI（历史日志导入、无会话离线进化） |
| 跨语言 | Python、JS/TS、Rust、Go、Java、C#、C/C++ 共 8 种客户端；统一 `point` 打点：`call/acall/allow_loop`，只需传入已有任务 ID、调用 ID、父调用 ID |
| 协议固化 | 5 份 protobuf（轨迹、Hybrid 评测、评测数据集、运行时策略、影子策略）＋ 6 份 JSON Schema ＋ OpenAPI |
| 模型无关 | 反思/评测模型经 `runtime.json` 配置 endpoint、`api_key_env`、thinking、超时；换模型、改提示词不需重新编译 |
| 测试 | 约 40 个测试文件、1.5 万行：算法单测（C++）＋真实隔离 Agent 进程的集成测＋8 语言探针 |

### 6.2 进化对象：只进化"被授权的策略空间"

引擎明确不做三件事：**不改模型权重、不进化交付 Artifacts、不改宿主/Agent 源码**。可变异的只有完整 RuntimePolicy 中获授权的三类组件：

| 组件 | 内容 | 变异边界 |
|---|---|---|
| 经验文本（L1） | 注入模型输入的结构化经验（scope＋触发条件＋正文） | 经 `experience_overrides` 按 `memory_id/content_revision` 覆盖正文；来源、触发条件、检索与冲突筛选不变 |
| 动作规则（L2） | 显式 deny／required 参数规则；另有从真实工具 JSON Schema 自动提取的顶层必填项**固定契约** | 固定契约不是搜索维度：不把可选参数升级为必填、不从自然语言推断权限 |
| 循环规则（L3） | Continue/Replan/Stop 阈值（最大轮数、无进展轮数、replan 预算） | 只可在已批准经验上**收紧**基础规则，不放松上限，不改宿主业务代码 |

> 对比：DGM/AgentFactory 改代码、openJiuwen 同时改 Artifacts——本引擎用"窄搜索空间"换可控性与安全可发布性。

### 6.3 L0–L4 能力分级与完整闭环

```
L0 边界采集 → 结构校验(DAG/Episode) → Hybrid 评测 → 反思候选 → 元反思 → 私有/公共两级记忆
                                                                        ↓ 编译
L4 GEPA 后台搜索（反思变异/双亲重组）→ Holdout 准入 → 发布版本 → L1–L3 受控应用 → 应用回执核验
                                                                        ↑ 回执+新轨迹回流
```

- **L0**：只采集学习，不改变宿主任何行为；规则无模型也可跑，语义提炼才调模型。
- **L1**：仅当 `memory_id` 与 `content_revision` 同时匹配批准项才注入，逐条过 `max_context_bytes`，带 policy_id 边界标记。
- **L2**：动作派发前 Allow/Reject；保留宿主原有串行/并行调度，不重写编排。
- **L3**：有在途动作（pending_actions）时永不 stop/replan；只做循环级决策并核验宿主实际执行。
- **L4**：后台搜索，主 Agent 不等待；每个试验由组件启动独立 Agent 实例、复用原启动命令与统一评分器。
- **每个 run 冻结固定策略快照**：新 run 用新版本，已开始的 run 不换版；`set_enabled(false)`＋epoch 隔离实现随时可停。

### 6.4 L4 核心算法：反思式 DAG 搜索（GEPA 的 C++ 受限实现）

引擎没有移植 DSPy/HERMES 代码，而是在 `gepa_search.cpp` / `gepa_strategy.cpp` 中重新实现了一套受授权空间约束的 GEPA（谱系格式 `gepa-reflective-dag-v2`）：

1. **冻结实验上下文**：绑定任务集版本、宿主 revision、评测/反思配置指纹与控制授权；生产基线未发布时用 L0。
2. **反思变异（单父）**：模型看到当前经验、选中组件、训练题输入/输出/判分/轨迹（失败案例优先；val/holdout 正文不进入反思），产出结构化改写。
3. **双亲重组（MergeProposer 思路）**：要求两节点在同一冻结实验的有效前沿上、**有共同祖先、非祖先-后代关系、各至少一题优于祖先**；按组件继承（一方改取该方、双方不同取验证更优方、平分稳定取第一方），重组本身不调模型。
4. **前沿与选父**：逐题 Pareto 前沿（instance/objective/hybrid/cartesian 四种前沿），去冗余后按占据前沿频次抽样；内部节点与叶节点同资格——不使用 MCTS 的 UCT/随机 rollout。
5. **筛选**：minibatch 上默认 strict_improvement（先保质量再看 fitness），可选 improvement_or_equal 或 pareto 保留局部优势；多提案按相对各自父版本的**平均增益**排名，消除末批较短的偏置。
6. **真实试验＋缓存**：全部成绩来自真实 Agent 执行；同版本/split/上下文的逐题成绩按报告哈希缓存复用、只补跑缺题。
7. **预算硬约束**：`max_rounds=8`、`max_model_calls=8`（仅计反思模型）、`max_trials=400`（含 repetition/基线/重试）、`timeout_ms=30min`、`patience=4`；预算预留最终 holdout 成本，不足不再启动试验。
8. **独立 Holdout 终验**：先固定 winner 再在留出集与生产基线配对比较；**holdout 失败只保留原版本，不把失败详情回喂反思继续搜同一留出集**（防过拟合）。
9. **发布检查**：该用户/等级仍是最新候选、生产父版本未改变且准入通过才登记新版本。

逐题 fitness 为质量优先的有界成本式：

```text
fitness = task_pass
          - 0.2 × invalid_loops / (1 + invalid_loops)
          - 0.05 × tokens / (10000 + tokens)
          - latency_weight × duration_ms / (latency_scale_ms + duration_ms)
```

启用循环目标后额外要求：同任务集合、同口径的原始无效 loop 均值不得上升，再比较通过率——不能用归一化饱和掩盖真实浪费。可选 **ComBEE 分组反思**（map-reduce：先 k=√n 组局部改进，再汇总为一个组件），所有分组/汇总调用统一计入模型预算。

### 6.5 六项代码级差异化技术

| # | 差异化技术 | 代码依据 |
|---|---|---|
| 1 | **应用回执核验（applied-by-verification）**：业界普遍接受"注入了即生效"的声明式回执；引擎按**实际发送的模型输入**重算应有增强并核验——注入文本须在 system 消息中恰好出现 1 次、动作须出现在 skipped 且不在 executed、stop 决策须被观察到，产出 applied/failed/rejected 三态 | `policy_runtime.cpp:341`（verify）、`:134`（request_contains 恰好一次） |
| 2 | **授权不可变 hash**：经验批准（memory_id＋content_revision）、request binding 等授权字段全部进入策略 SHA-256；撤权、改写、删除、不兼容关系立即使旧版本失去后续 run 资格 | `policy_runtime.cpp:29`（确定性序列化＋SHA-256 出 policy_id） |
| 3 | **多父谱系＋双亲重组**：谱系为 DAG，每节点存独立不可变完整策略；可把不同分支的改进组合进同一后代，记录共同祖先、组件来源与冲突 | `gepa_strategy.cpp`（ancestors/recombine）、`tree.json` nodes[].parents/component_sources |
| 4 | **口径统一评测底座**：L0 基线、L1–L3 准入、L4 搜索筛选共用同一个 Hybrid Evaluator；规则确认的无效结论在融合时保留，避免"观测一套口径、优化另一套口径" | `hybrid_evaluator.cpp`；`gepa_search.cpp` 复用同一评测 worker |
| 5 | **无向量库的确定性记忆治理**：字节三元组 Sørensen–Dice 系数（2｜A∩B｜/(｜A｜+｜B｜)）取 top-8 短名单，再由模型判 DUPLICATE/RELATED/CONTRADICTS/DISTINCT，冲突两端同时排除；无外部向量库即可私有化部署 | `evolution_pipeline.cpp:1448`（byte_ngrams）、`:1491`（Dice）、`:1671`（shortlist，默认上限 8） |
| 6 | **冻结快照＋渐进分级**：每 run 固定策略视图，启用/停用以 epoch 隔离；L0→L4 逐级开启、随时可停、可回退、可审计，把自进化从一次性切换变成渐进工程 | `compile_runtime_policy`（policy_runtime.cpp:150）；暂停恢复见 docs/24 |

配套工程机制：公共记忆晋升须过隐私正则门＋四张语义闸门（privacy_safe/generalizable/supported/non_overriding）＋**独立用户计票**（默认 ≥2 独立用户或训练组支持）＋争议暂停＋证据过期淘汰（`shared_memory.cpp:74`）；落盘全程临时文件＋fsync＋原子替换、拒绝符号链接、目录 0700；429 限流时以 `gepa_model_rate_limited` 挂起并保留预算；中断节点保留已耗预算、恢复后只补评分或补缺题。

### 6.6 与主流框架的能力矩阵对比

| 能力维度 | **Trajectory Evolution（自研）** | DSH-Evolve | Hermes | openJiuwen RSI | EvoAgentX | DGM/AgentFactory |
|---|---|---|---|---|---|---|
| 进化对象 | 经验文本＋动作规则＋循环规则（完整策略） | Harness 补丁 | 技能（DSPy） | Harness＋**Artifacts** | **工作流拓扑** | **Agent/技能源码** |
| 多父谱系、跨分支重组 | ✅ DAG＋双亲重组 | ❌ 补丁单链 | ✅ GEPA | ✅ | ✅ 遗传交叉 | 部分（存档采样） |
| 真实 Agent 试验（非代理指标） | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |
| 应用回执按实际输入核验 | ✅ 三态 | ❌ 声明式 | ❌ | 部分 | ❌ | ❌ |
| 撤权即时失效（授权入 hash） | ✅ | ❌ | ❌ | 版本化 | ❌ | ❌ |
| 基线/准入/搜索统一评测口径 | ✅ | 部分 | 部分 | ✅ | ❌ | ❌ |
| 记忆治理 | 两级＋确定性短名单（**无向量库**） | 无原生 | 三级技能存储 | 平台记忆 | 弱 | 弱 |
| 离线进化、版本可移植 | ✅ learn CLI＋published-version 包 | 部分 | 部分 | 部分 | ❌ | 部分 |
| 多 Agent 拓扑进化 | ❌（保留宿主调度） | ❌ | 弱 | ✅ 原生蜂群 | ✅ | ❌ |
| Artifacts 交付物进化 | ❌ | ❌ | ❌ | ✅ 独有 | ❌ | ❌ |
| 框架/语言无关接入 | ✅ 8 语言＋ABI/HTTP | 插件式 | Python | 平台绑定 | Python | Python |
| 权重微调 | 不做 | 不做 | 不做 | 不做 | 不做 | 不做 |

### 6.7 与上游 GEPA / Hermes 的关系

- **继承的思想**：真实执行反馈驱动反思改写、逐任务前沿＋覆盖去冗余、MergeProposer 双亲重组、提案筛选策略、ComBEE 分组反思。
- **主动划清的边界**：① 上游可搜索任意字符串组件，本引擎只改写已批准经验与授权控制规则，手工策略与宿主程序原样不动；② 上游常以单一总分比较，本引擎质量优先、再压无效循环/token/时延，多提案按平均增益排序；③ 不引入 DSPy/LangChain 适配器与 Python callable 依赖，对外只有统一协议；④ 搜索分数不直接覆盖记忆正文、不越过授权、不把组合收益归因到单条记忆。

### 6.8 短板与边界

1. **进化对象广度受限**：不碰 Artifacts（openJiuwen 的双维度优势无法覆盖）、不碰源码（DGM/AgentFactory 的代码进化无法覆盖）、不碰多 Agent 拓扑（EvoAgentX/CORAL 的组织进化无法覆盖）。
2. **评估仍需配置 Judge**：不是 ADAS 式无监督评估；Hybrid 融合能缓解模型 Judge 的漂移与幻觉，但不消除，评分模型本身须显式配置并纳审计。
3. **真实试验成本不低**：预算为真实试跑量级（默认 400 trials / 30 分钟墙钟）；单评测 worker、反思提案顺序生成，吞吐是刻意的稳定性取舍。
4. **C++ 扩展成本**：换模型/提示词/配置可热换，但新增协议字段、规则语义、控制语义仍需改码重编。
5. **保守的 Holdout 策略**：holdout 失败不回喂模型，可能在候选接近门槛时持续保留基线，进化偏保守。
6. **公开验证空白**：新引擎，外部独立评测、第三方案例与公开 benchmark 跑分尚待积累。

---

## 七、横向竞争要点总结

- **openJiuwen RSI** 是**唯一平台化生产级方案，Harness + 产物双进化 + 原生蜂群**；其余大多只进化 Agent 逻辑，不进化交付成果。
- **DGM** 是**代码 Agent 天花板**，直接修改自身源码，但场景极窄，只适合代码。
- **CORAL** 主打**科研多 Agent 组织**，适合自动跑实验，不是办公自动化。
- **ADAS** 的核心卖点是**无人工评估**，但评估幻觉是硬伤。
- **Hermes** 适合中小规模工具 Agent，轻量技能进化，上手成本低。
- **Trajectory Evolution（自研）** 是**代码闭环最完整的受控进化路线**：不追求进化对象的广度（不碰产物/源码/拓扑），而把"策略不劣化上线"做到可核验、可回退、可离线部署——与 openJiuwen 的差异是"窄而深的 Harness 策略" vs "宽而全的双维度平台"。

---

## 八、选型决策建议

| 场景 | 推荐框架 |
|---|---|
| 已有 Agent/Harness，想低侵入接入"可准入、可回滚、可离线部署"的策略进化，不换模型、不改宿主代码 | **Trajectory Evolution（自研）** |
| 私有化/离线环境（无向量库、无 DSPy/Python 依赖）跑通反思进化、Holdout 准入与版本发布 | **Trajectory Evolution（自研）** |
| 企业办公、论文/文档 + 代码交付，多 Agent 蜂群协同，要求稳定版本管控、沙箱安全 | **openJiuwen RSI** |
| 自由切换大模型，只研究 Harness 策略进化，不需要交付产物迭代 | **DSH + DSH-Evolve** |
| 研究轨迹树、多 Agent 工作流拓扑自动生成 | **EvoAgentX / SE-Agent** |
| 纯代码自动化，希望 Agent 自动生成代码子 Agent 技能 | **AgentFactory** |
| 本地快速跑通 Harness 自进化原型、低成本实验 | **PenguinHarness** |
| 在线持续沉淀技能、工具自动化 | **Hermes Agent** |
| 自主科研、多 Agent 自动做实验 | **CORAL** |
| 研究无人工评估的种群/元进化 | **ADAS** |
| 研究递归元进化理论 | **EvolveR** |
| 代码 Agent 自修改源码、SWE-bench 冲榜 | **DGM** |

---

## 九、关键基础组件（所有自进化 Agent 基本都需要）

1. **Meta-Agent / Evolver**：元智能体，负责看轨迹、找问题、提出修改方案（变异器）。
2. **Evaluator（LLM-as-judge）**：评测器，隔离沙盒里测试新版本 Agent，打分、回滚坏版本，是自进化最关键瓶颈。
3. **轨迹存储与归因**：保存完整 action/observation/thought 轨迹，做失败归因（规划错？工具错？prompt 错？）。
4. **沙盒隔离**：新版本 Agent 必须在隔离环境验证，防止进化出破坏性策略（HOP 可信架构就是解决这个安全问题）。

---

## 十、常用 Benchmark

- **SWE-bench Verified**：代码修复类 Agent 最常用。
- **GAIA、HotpotQA**：复杂多跳推理。
- **Terminal-Bench**：命令行/终端 Agent（Self-Harness 基准）。
- **τ²-Bench**：专门评估持续进化、防止能力遗忘的基准。
- **MATH、MBPP**：数学与代码生成。

---

> 说明：本报告基于公开资料与社区讨论整理，部分框架（ADAS、EvolveR、CORAL、openJiuwen RSI）发布时间较新，公开跑分与第三方独立评测仍在积累中，具体数据以官方最新发布为准。

