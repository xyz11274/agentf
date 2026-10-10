#!/usr/bin/env python3
"""Source content for the advisor report, kept separately from slide layout."""
from pathlib import Path
import json
R=Path(__file__).resolve().parent
S=[]
P='tools/trajectory_evolution/'
RT='packages/agentflow-runtime/src/agentflow_runtime/'
AW=P+'experiments/appworld/README.md'
CMP='trace_output/appworld/gepa/seed-search-71-stage500/comparison.json'
L0='trace_output/appworld/gepa/l0-current-stage500/result.json'
MAN='trace_output/appworld/published/strategy/manifest.json'
TREE='trace_output/appworld/gepa/workspace/agent-evaluation/f0d143a0298773c1bb0b0fbe1733f68feab42c2e53cf49b33e005c1d46f193b2/search/tree.json'
STATUS=P+'docs/WORK_STATUS.md:1-7（历史执行记录；不是本次重跑）'
def add(title,section,subtitle,notes,sources,**kw):
    S.append(dict(title=title,section=section,subtitle=subtitle,notes=notes,sources=sources,
    source_short=kw.pop('source_short','证据：'+' · '.join(x.split('（')[0].rsplit('/',1)[-1] for x in sources[:2])),
                  seconds=kw.pop('seconds',60),**kw))
def cards(*items): return [dict(title=a,body=b) for a,b in items]
def steps(*items): return [dict(title=a,body=b) for a,b in items]

add('AgentFlow 自进化系统\n当前机制、实验进展与研究方向','',
    '从真实执行轨迹到可验证的行为策略\n系统梳理 · 实验审计 · 下一步研究讨论',
    '这次汇报回答三个问题：系统现在如何运行和学习，现有结果证明到哪一步，下一步应该研究什么。分析基于2026年10月9日的当前仓库，提交f44bbaf4。系统机制用当前源码核对，实验结论用本机保留的逐题摘要复算，并把缺失的原生报告、历史运行日志与未经验证的计划分别标明。本次没有重跑模型实验，也没有把历史测试记录当作当前回归。',
    ['Git HEAD f44bbaf4a9531731bfbd3eb6c115fe4210df95b1；分支 xyj/harness_agent_poc_evo',AW],
    type='cover',seconds=30)

add('当前结论：工程闭环已形成，研究证据仍需补强','01 / 汇报主线',
    '先讲已经能够确认的结论，再展开机制、实验与研究问题。',
    '系统已形成轨迹采集、评测、经验管理、运行策略、后台搜索与交付的工程链路。AppWorld现存摘要显示冻结seed相对L0增加16道严格通过题，平均无效loop下降，但质量提升4.968944%仍低于5%的仓库门槛。更需要强调，完整500题已经用于开发反馈，不是独立留出评测；后续30轮GEPA也没有新增accepted节点。我的判断是，这一阶段最有价值的工作是建立了可追踪的执行和策略机制，下一阶段应将实际应用、泛化收益和完整成本变成可重复证明的研究证据。',
    [AW,MAN,TREE],cards=cards(
    ('系统已有闭环','执行 → 轨迹 → 评测 → 记忆 → 策略 → 后续执行'),
    ('开发对照已有增益','322 → 338 / 500；无效 loop 下降 46.32%'),
    ('既定目标尚未达标','严格通过与质量相对提升 4.968944% < 5%'),
    ('下一步先补证据','独立留出、应用消融、稳定性与完整成本')),
    takeaway='核心讨论：怎样把“可运行的自进化”推进为“可证明的自进化收益”？')

add('30 分钟汇报路线','01 / 汇报主线',
    '主讲 30 页，答疑附录补充源码定位、状态语义与复现边界。',
    '前六分钟说明AgentFlow的总体架构和请求执行方式，中间十分钟讲从边界打点到L1、L2、L3再到GEPA的具体机制。随后八分钟集中核对AppWorld成绩、搜索状态和业务评测的偏差，最后六分钟讨论研究问题、实验设计与阶段安排。源码位置和原始证据路径留在每页备注与附录，避免主讲时逐个念函数名。',
    ['本汇报第4—18页：源码机制；第19—25页：实验与问题；第26—30页：下一步方向'],
    source_short='汇报导航 · 详细源码、结果与边界见逐页备注',
    cards=cards(('系统底座','仓库地图、请求链路、在线与后台职责'),('自进化机制','打点、评测、记忆、L1–L3、GEPA与准入'),('实验审计','500题成绩、搜索失败、成本与选择偏差'),('研究讨论','可回答的问题、对照设计、交付顺序')),
    takeaway='主线始终围绕：系统做了什么、证据支持什么、下一步验证什么。')

add('仓库由执行底座、Agent Harness 与演化组件组成','02 / 系统架构',
    'C++ 管理执行与状态；Python 驱动模型与工具；独立组件负责学习和策略。',
    '仓库不是一个单独的提示词优化脚本。src与include中是C++服务、会话、任务、工作流、Worker和DataBus；packages/agentflow-runtime中是Python协调器、技能执行器和LLM调用；tools/trajectory_evolution是可独立构建的C++组件，有协议、SDK、记忆、策略和搜索。frontend/bridge提供REST和SSE接口，experiments提供专用评测入口。根构建产生libagentflow与agentflowd，演化组件另行构建。这一划分让执行系统可以不依赖每次请求中的后台学习。',
    ['CMakeLists.txt:355,429',P+'CMakeLists.txt:4,155,217,240',RT],
    type='table',headers=['模块','主要位置','职责'],rows=[
    ['执行与通信','src/agent、workflow、runtime、daemon','身份、调度、并发、DataBus、RPC'],
    ['Agent Harness','packages/agentflow-runtime','LLM 决策、技能、工具与循环'],
    ['演化组件','tools/trajectory_evolution','轨迹、评测、记忆、策略与 GEPA'],
    ['接口与宿主','frontend/bridge；examples/agent','业务 API、工具环境、评测接线'],
    ['实验与验收','benchmark；experiments；tests','对照协议、来源校验、分层验证']],
    widths=[.15,.40,.45],table_font=15,
    takeaway='理解源码先沿一轮真实任务走，再沿同一条轨迹走进后台学习。')

add('一次请求怎样跨语言执行并返回结果','02 / 系统架构',
    '下面是受测 v1 Harness 的代表路径；Bridge 之外也有 SDK 与实验入口。',
    '用户请求从Bridge chat_rest进入JSON-RPC agent.execute；C++端通过AgentRegistry与SessionRouter找到Session和Task，Scheduler与Worker执行工作流节点。Python Coordinator选择技能，C++维护批次并发、exec_skill_key和ALL join，Skill Executor调用模型和工具。DataBus保存结果，ResponsePoller关联请求，Bridge返回内容。要区别两个Coordinator：C++协调任务派发和批次状态，Python协调下一步模型决策。框架completed仅说明执行终态，业务成功要由独立验收决定。',
    ['frontend/bridge/routes/chat_rest.py:471','src/daemon/rpc_handler.cpp:418,646','src/agent/agent.cpp:3133,3228,3346',RT+'skill_executor.py'],
    type='flow',steps=steps(('请求入口','Bridge / RPC\n稳定用户与会话身份'),('任务调度','Session / Task\nScheduler / Worker'),('模型与技能','Python Coordinator\nSkill → LLM / Tool'),('批次与结果','DataBus\nexec key / ALL join'),('对外返回','ResponsePoller\nBridge / SessionLog')),
    below='分支结果、模型观察与最终交付共同构成轨迹；单个 success 字段不足以判断任务成功。',
    takeaway='执行控制、模型决策和任务验收各有职责。')

add('自进化是在线执行与后台学习之间的反馈环','02 / 系统架构',
    '一次 run 使用固定策略；新经验与新版本影响后续 run。',
    '在线路径负责用固定快照执行任务，在模型、动作、调度与循环位置进行有界查询和控制。后台路径接收轨迹、校验、评测、提炼经验、审核，再编译候选和搜索。在线不会为了每次建议同步运行完整反思模型。原生LocalRuntime在run创建时固定PolicyRuntime，后台增加记忆不直接替换正在执行的策略。跨短进程节点的HTTP服务负责共享run所有权；原生SDK适合生命周期完整的常驻宿主。',
    [P+'src/service/local_runtime.cpp:292,340,368',P+'docs/01-overview.md',P+'docs/17-learning-scheduler.md'],
    type='lanes',lanes=[
    dict(label='在线：当前任务',steps=[['冻结策略','版本 / 授权'],['执行与查询','L1 / L2 / L3'],['应用回执','真实输入 / 动作'],['任务收尾','关闭 / 持久化']]),
    dict(label='后台：后续学习',steps=[['轨迹校验','Episode / 缺口'],['评测与反思','质量 / loop'],['经验与候选','审核 / 编译'],['验收与交付','GEPA / 新版本']])],
    below='反馈：后台批准并交付的新版本 → 后续 run 加载；运行中版本保持固定。',
    takeaway='进化对象是行为策略及经验，当前链路不更新模型权重。')

add('L0–L4：收集事实、改变行为、搜索新策略','03 / 自进化机制',
    '运行授权上限是 L1–L3；L4 用来描述后台优化能力。',
    'L0是基线和轨迹收集，L1在模型输入中加入已批准经验，L2控制工具参数、重试与技能依赖，L3控制继续、重规划和停止。L4不是在某个新运行边界里执行的第四种在线控制，而是后台GEPA生成、比较、重组和交付L1到L3策略的能力。虽然Proto枚举有L4，validate_options接受的运行策略级别是L1到L3。配置max_level=L3也不意味着三种能力都曾实际生效。',
    [P+'src/runtime/policy_runtime.cpp:112-113',P+'proto/trajectory_trace.proto:11-18',P+'docs/16-runtime-controls.md'],
    type='levels',rows=[['L0','采集与原始基线','记录任务 / 模型 / 动作 / loop，不施加演化增强'],['L1','经验输入增强','检索符合身份、作用域与观察条件的批准经验'],['L2','动作与编排控制','参数完整性检查、重试限制、拒绝规则、技能依赖'],['L3','循环控制','continue / replan / stop；遵守分支及待执行动作条件'],['L4','后台候选搜索','反思变异、多父重组、Frontier、验证与准入']],
    takeaway='代码存在 → 配置开启 → 实际应用 → 同题收益 → 独立留出，逐层核验。')

add('统一边界协议把执行事实与应用回执关联起来','03 / 自进化机制',
    'evolution.boundary.v1 覆盖任务、模型、动作、调度与循环边界。',
    '统一协议的意义是让不同语言与不同进程表达同一类事件。标准Identity标明agent、用户、conversation和run；底座session另属宿主路由身份。point与parent链标明分支，request_id支持幂等，输入、观察和错误成为后续评测的证据。模型前可以查L1，动作前查L2，loop前查L3，但最终必须看到实际请求或派发结果的应用回执。并发事件不能只按时间戳排序来理解，exec_skill_key和caused_by帮助关联真实前因。',
    [P+'api/boundary-v1.schema.json',P+'docs/05-protocol.md',P+'docs/08-instrumentation.md','src/agent/agent.cpp:3346,3497'],
    cards=cards(('身份与分支','agent / user / conversation / run；point 与 parent'),('边界事实','模型请求、动作输入、工具观察、loop决策'),('因果与幂等','request_id、caused_by、exec_skill_key关联事实'),('真实应用','before 建议 → 实际执行 → advice.applied核验')),
    takeaway='打点提供观察与可追溯性；建议产生次数不能当实际应用次数。')

add('轨迹先经过完整性校验，再进入评测与学习','03 / 自进化机制',
    '错误轨迹、缺失收尾与采集降级需要显式记录，不能补成健康数据。',
    '从边界事件到经验之间不是简单把日志交给模型。首先规范化轨迹与Episode，检查身份、事件引用、顺序、收尾、缺口和预算。之后Evaluator生成任务结果、质量和loop诊断，再据有来源的失败与恢复提炼候选。缺失事件或capture_degraded会限制可评价范围，不能将缺失计为0错误或0无效loop。训练中允许真实模型失败，但存储完整性故障要单独处理。',
    [P+'docs/10-capture-and-validation.md',P+'src/analysis/evolution_pipeline.cpp',P+'docs/11-evaluation.md',P+'docs/12-reflection-and-candidates.md'],
    type='flow',steps=steps(('边界事件','任务 / 模型\n工具 / loop'),('规范轨迹','统一结构\n保留输入观察'),('完整性校验','身份与引用\n缺口 / 隔离'),('评测诊断','质量 / strict\n无效loop'),('学习候选','经验与控制\n绑定证据来源')),
    below='候选必须保留证据、触发条件和适用范围，经过审核后才能进入运行策略。',
    takeaway='可学习事实的完整性是策略质量的前提。')

add('评测同时衡量任务成功、有效质量与无效 loop','03 / 自进化机制',
    'AppWorld 单目标样本的正式 Q 为 0 / 1；官方部分检查比例另作辅助。',
    '严格通过以原生verdict=passed为准，要求确定性检查和模型严格验收共同满足。正式有效质量Q不是直接照抄模型assessment.quality，确定性检查失败或关键违规可能使其归零。AppWorld当前每题一个普通目标，因此完整Q均值等于正式严格通过率。无效loop来自complete的loop_review，计算评审的无效迭代总数除以评审题数，并不是工具调用次数或总轮数。原始报告中即使raw质量为1，正式验收仍可能失败。官方TGC和仓库更严格的组合验收必须分别报告。',
    [AW,P+'docs/19-datasets-and-benchmarks.md',P+'docs/INTERMEDIATE_ARTIFACTS.md',L0,CMP],
    cards=cards(('严格通过','原生 verdict=passed；确定性检查与模型验收'),('有效质量 Q','普通目标完成度；检查失败 / 关键违规影响有效分'),('平均无效 loop','完整评审中的无效迭代总数 ÷ 有效评审题数'),('资源与覆盖','时延、token、调用和额外尝试分别报覆盖')),
    takeaway='“质量更高”“轮数更少”“官方任务完成”需要明确各自定义。')

add('记忆是带来源与资格的经验库，不是自由追加文本','03 / 自进化机制',
    '私有经验与公共经验分层管理；批准状态、作用域和版本决定可应用资格。',
    '后台将诊断转为有来源、scope、触发条件和支持证据的候选，经过合并、反证、审核和管理后进入经验库。私有记忆服务同一用户，公共记忆支持经过治理的跨用户协作，包括晋升、冲突和淘汰。置信度或入库条数不能当收益证据，未批准经验不能直接执行。配置模板常见auto_approve_experiences=false，公共共享默认开启，因此正式实验应显式隔离用户、数据目录和共享写回。公共经验的published状态与运行策略交付不是同一语义。',
    [P+'docs/12-reflection-and-candidates.md',P+'docs/13-private-memory.md',P+'docs/14-shared-memory.md',
     'trace_output/appworld/training/evolution/by-user/appworld-training/memory/evolution-memory.json'],
    type='flow',steps=steps(('真实证据','任务失败与恢复\n可追溯来源'),('候选经验','内容 / scope\n条件 / 支持'),('审核与管理','合并 / 冲突\n反证 / 资格'),('分层记忆','用户私有\n受控公共'),('运行资格','已批准且匹配\n才能编译应用')),
    takeaway='入库、批准、发布、加载、应用、收益是不同状态。')

add('快照冻结内容；检索用当前观察筛选适用经验','03 / 自进化机制',
    '当前在线检索以确定性词面匹配为主，不调用模型做语义检索。',
    '运行时先按批准状态与授权编译快照，保存memory_id、content_revision与策略版本，再按当前任务和状态做词面检索。检索副本排除系统指令、既有经验和工具定义，保留当前用户请求及可见助手调用和工具消息；这些消息并不全是已执行事实。默认至少两个独立命中词，具体规则与数量字节预算还会进一步筛选。部分经验可添加observed_operations，要求同分支已执行动作提供证据。L2和L3结构化控制独立于L1 top-K。词面命中不能保证识别否定与阶段语义，这是误触发和漏触发的重要研究空间。',
    [P+'docs/15-policy-snapshots.md:92-136',P+'src/service/local_runtime.cpp:292,368',P+'src/runtime/policy_runtime.cpp'],
    cards=cards(('冻结的部分','来源版本、经验正文、授权选项和策略身份'),('变化的部分','当前 task + state、已执行动作和分支观察'),('筛选与预算','批准 / scope → 独立词项 → 排序 → 数量与字节'),('当前限制','否定、任务阶段与长上下文语义不由词面保证')),
    takeaway='可讨论的改进：后台编译结构化触发器，在线只做轻量且可审计的判定。')

add('L1：经验必须进入真实模型请求才算应用','03 / 自进化机制',
    '查询命中 → 准备增强输入 → 实际发送 → 复核正文，形成完整证据链。',
    'L1最容易被误报，因为检索命中不代表LLM真正看到了内容。当前LLM wrapper在环境版本v1且EVOLUTION_POINT_CONFIG有效时调用演化组件，真实trace_call使用返回的request.messages和tools；组件记住prepared query并复核正文。AppWorld启用preserve_runtime_policy_state，原生宿主每run也保留固定对象；这两种场景中每条memory_id只准备一次新注入，同prepared query重放不算新注入。HTTP默认不保留该对象，不能把once推广到所有配置或跨崩溃。后续技能重建消息还涉及宿主保留逻辑。即便注入正确，模型也可能不遵循，所以回执只是介入证据。595次L1应用不能解释为595题改善。',
    [RT+'llm_call.py:89,101',P+'src/runtime/policy_runtime.cpp:514-560',P+'src/service/integration_service.cpp:1767-1777',P+'experiments/appworld/layout.py:286',CMP],
    type='flow',steps=steps(('匹配经验','批准 / scope\n真实观察条件'),('准备请求','经验进入\nmessages / tools'),('发送模型','真实 call 使用\nprepared request'),('核验回执','注入正文一致\nadvice.applied'),('比较任务','成功与退步\n另行配对验收')),
    takeaway='最佳500题策略 L1=595 次；应用频次是机制证据，不是收益幅度。')

add('L2：在动作与派发位置落实结构化约束','03 / 自进化机制',
    '动作约束和技能编排分别记回执，不能只统计生成了多少规则。',
    'L2分为动作和编排两类。动作控制检查必填参数、失败与重试预算并决定allow或reject，当前不重写动作参数；编排控制对技能派发及依赖关系生效，由宿主绑定接受的exec_skill_key和batch_id。学习生成的规则必须有批准来源并符合scope，不能靠后台模型任意引入未授权控制；部署的基础规则另有配置语义。当前AppWorld最佳500题策略L2动作与编排应用均为0，说明这个结果没有支持L2对收益的贡献。后续需要专门设计有实际控制机会的任务，并设置独立消融。',
    [RT+'skill_executor.py:83,1359',RT+'coordinator_decide.py:1776,1787',P+'docs/16-runtime-controls.md',CMP],
    cards=cards(('工具动作','参数完整性检查、重试限制、动作拒绝'),('技能编排','依赖条件、派发顺序、accepted exec key与batch关联'),('合法性边界','批准来源、scope、级别与宿主可落实能力'),('已有实验','最佳500题 L2动作=0、L2编排=0；收益贡献待验证')),
    takeaway='要证明 L2 有效，需要“可触发任务 + 实际介入 + 同题消融”。')

add('L3：用真实进展控制 continue / replan / stop','03 / 自进化机制',
    '循环控制作用于本分支，并需避免干扰仍在执行的动作。',
    'L3在下一轮开始前检查是否继续、重规划或停止。Coordinator和Skill Executor都在v1循环边界调用allow_loop，控制依赖该分支的近期观察和进展条件。同分支存在在途动作时固定continue，不stop或replan；L3也不是把整个Session强制结束。replan.commit本身不计应用，要在下一次真实模型请求中核验到replan context。关键风险是把暂时缺少新观察误认为无进展，从而提前停止仍可能成功的任务。最佳500只有4次L3应用，不能据此说系统性证明循环策略收益。应将提前停止失败与正常减少重复操作分开统计。',
    [RT+'coordinator_decide.py:1362',RT+'skill_executor.py:834',P+'docs/16-runtime-controls.md',CMP],
    cards=cards(('Continue','无适用停止 / 重规划条件，或仍有在途动作'),('Replan','符合重规划条件；上下文进入下一真实模型请求'),('Stop','满足停止条件，保存原因与真实终态')),
    takeaway='最佳500题 L3=4 次；需重点检验误停止、漏触发和任务成功保持。')

add('一条经验怎样走到真实行为：以 API 参数错误为例','03 / 自进化机制',
    '示意基于现有产物摘录；本机缺失原轨迹，不能把摘录当完整重验。',
    '核心产物文档展示过一条真实训练错误：工具接口只接受关键字参数，Agent用了位置参数导致TypeError。后台可从错误及后续修复提炼有来源的经验，审核后编译成策略，在符合scope和观察条件的模型边界注入。接下来需要核验LLM请求是否真正含有经验、下一次调用是否按接口修复、最终任务是否通过。这里展示的是证据链的读法，而不是本轮证明该经验对哪道题产生了因果提升。文档引用的原轨迹在当前副本缺失，这一限制必须明确。',
    [P+'docs/INTERMEDIATE_ARTIFACTS.md:22-84',RT+'llm_call.py:89,101',P+'docs/15-policy-snapshots.md'],
    type='flow',steps=steps(('观察错误','位置参数调用\n触发 TypeError'),('提炼经验','核对公开接口\n使用正确参数'),('批准与冻结','来源 / scope\ncontent revision'),('运行介入','匹配真实错误\n注入与回执'),('任务验收','动作是否修复\n任务是否成功')),
    below='需要同时保存：原始失败 → 经验来源 → 冻结策略 → 实际请求 → 最终验收。',
    takeaway='这条链路解释可追溯机制；收益归因仍需同题对照或消融。')

add('GEPA 在后台反思、变异、重组并比较候选','03 / 自进化机制',
    '仓库实现借鉴反思与 Pareto 搜索思想；本仓库收益须用自身实验验证。',
    'GEPA原论文使用轨迹反思提出并测试提示更新，并从Pareto前沿组合互补经验。本仓库进一步把经验正文、触发条件、动作和循环控制纳入候选。父策略执行后提供正式质量、严格通过、无效loop、应用证据和失败观察，模型生成合法变异，多父候选可重组；训练筛选和val再决定保留。搜索预算、来源绑定、授权和输入预算都限制可选空间。代码有Frontier、多父重组和ComBEE路径，不意味着每个实验节点都执行了所有搜索机制，也不应直接移用论文中的提升数字。',
    [P+'src/gepa/gepa_search.cpp',P+'src/evaluation/agent_evaluation.cpp',P+'docs/24-evolution-and-gepa.md',
     'GEPA原论文：https://arxiv.org/abs/2507.19457（仅用于算法背景）'],
    source_short='证据：gepa_search.cpp / agent_evaluation.cpp · GEPA原论文 arXiv:2507.19457',
    type='search',body='1. 变更是否合法且有来源？\n2. 新控制有没有实际触发？\n3. 受影响任务是否真正改善？\n4. val 能否支持进入下一阶段？',
    takeaway='生成节点、应用变更、通过验证、完成最终对照，不能混为一次“进化成功”。')

add('准入与交付：通过门禁才能称为合格策略','03 / 自进化机制',
    '组件支持 train / val / holdout 分工；独立性还取决于实际实验使用方式。',
    '后台流程有训练筛选、val比较、最终对照与交付。当前训练筛选对平均loop和资源相对父策略有启发式上界，并检查受影响任务和关键违规。现有注释中的逐题约束不能理解为每道题loop都不退化，因为相关函数还比较平均数。AppWorld正式发布要求同题完整500中strict和Q相对提升至少5%，平均无效loop相对下降至少15%。最终require_level_coverage默认false，所以门禁通过也不必然证明L1、L2、L3每级都参与。截止交付可以明确导出best_available_not_qualified，交付文件存在不等于目标达成。',
    [P+'src/gepa/gepa_strategy.cpp:53-75',P+'src/gepa/gepa_search.cpp:7504-7544',
     P+'src/evaluation/agent_evaluation.cpp:970-973',AW,MAN],
    type='flow',steps=steps(('训练筛选','合法 / 实际触发\n收益与退化检查'),('验证选优','固定 val\n候选比较'),('最终对照','同题 / 同裁判\n完整覆盖'),('门禁判断','strict与Q ≥5%\nloop下降 ≥15%'),('明确交付','qualified 或\nbest available')),
    takeaway='算法结构有 holdout 不等于本次500开发对照具有独立留出性质。')

add('AppWorld 协议：500 题结果属于开发对照','04 / 实验进展与问题',
    '官方环境与评分适配已具备；当前自定义范围不能直接当官方榜单复现。',
    'AppWorld提供可执行的应用世界和状态评分，适合观察跨工具、多步任务。仓库当前训练集134题，包括90道官方TRAIN和44道公开TRAIN环境补充题；当前原树冻结在132题，选定seed又有自己的不可变来源，因此三者要分开。val为12题，最终500包含131 test_normal、327 test_challenge和42 dev。32和100题是这500的开发前缀，而且开发中参考过失败结果。因此即便目录叫holdout.jsonl，500也不是独立留出；它是当前开发对照。未来应先锁定训练与开发协议，再使用不反复查看的确认评测。',
    [AW,P+'experiments/appworld/datasets/manifest.json',MAN,TREE,
     'AppWorld官方仓库：https://github.com/StonyBrookNLP/appworld'],
    type='table',headers=['范围','当前数量与用途','可得结论 / 限制'],rows=[
    ['TRAIN','134：官方TRAIN90 + 补充44','学习来源；原树冻结132，seed来源另核'],
    ['VAL','12','搜索与候选选优'],
    ['开发验证','32 → 100','固定500的前缀子集'],
    ['500开发对照','131 normal + 327 challenge + 42 dev','已用于开发反馈，独立性不成立'],
    ['未来确认评测','待设计并冻结','一次性验收泛化与稳定性']],
    widths=[.14,.40,.46],table_font=14.5,
    takeaway='正文写“500题开发对照”，避免把文件名 holdout 当独立性证明。')

add('AppWorld 流程脚本已交付，当前成绩未达目标','04 / 实验进展与问题',
    '已有真实 API 全链路复现说明；本机运行环境与原生报告仍需补齐。',
    'reproduce.sh串联训练、原生学习审核、冻结seed、32和100门禁、GEPA、至多三个候选的500对照和发布，也提供resume与只读status。已有全链路脚本与复现说明，但本机AppWorld虚拟环境、world与相关build产物缺失，不能说当前已可直接运行或本轮已从头复现。manifest记录delivered、best_available_not_qualified、goal_passed=false、gepa_completion_claimed=false、independent_holdout=false。导出0模型调用仅指交付这一步，不能说完整实验0费用。原GEPA搜索已取消，没有新合格完整500。历史运行Git commit不能证明等于当前HEAD，现存摘要仅保留Host v22、Judge d81与冻结策略身份。',
    [AW,P+'experiments/appworld/reproduce.sh',MAN,TREE],
    type='flow',steps=steps(('训练与学习','真实轨迹\n来源审核入库'),('冻结 seed','不可变策略\n来源与版本'),('开发门禁','32 → 100\n同题复用'),('后台搜索','最多30轮\ntrain / val'),('500与交付','当前seed已评分\n未达核心门槛')),
    takeaway='当前交付状态：best_available_not_qualified；GEPA未宣称完成。',takeaway_color='D47E31')

add('最佳已评分 seed：成功增加，loop 明显减少','04 / 实验进展与问题',
    '同一500题；数值由当前两份逐题摘要复算，原始轨迹与部分原生报告缺失。',
    '两边均有500条完成和评分摘要。正式严格通过从322增加到338，正式有效质量从0.644到0.676，绝对增加3.2个百分点；相对提升16除以322，为4.968944%。配对结果78题改善、62题退步、360题不变，净增16，因此收益与退化都要保留。平均无效loop从0.380降到0.204，降幅46.315789%。这支持冻结seed在当前开发范围具有观察到的改善，但5%的质量门槛需要至少339道严格通过题，338不能四舍五入视为通过。由于不是独立留出，也没有多次重复实验，这些数值不能直接证明泛化、稳定或因果收益。',
    [CMP,L0,MAN],source_short='证据：AppWorld comparison.json / L0 result.json；500条摘要复算',
    type='bars',metrics=[
    dict(title='正式严格通过 / 500',categories=['L0','冻结 seed'],values=[322,338],max=400,delta='+16 题；仍需 339 才过门槛',color='D47E31'),
    dict(title='正式有效质量 Q',categories=['L0','冻结 seed'],values=[.644,.676],max=.85,format='0.000',delta='+3.2 个百分点 / +4.968944%',color='D47E31'),
    dict(title='平均无效 loop',categories=['L0','冻结 seed'],values=[.380,.204],max=.52,format='0.000',delta='下降 46.315789%')],
    takeaway='配对：78题改善 / 62题退步 / 360题不变；净增16题，strict与Q门槛仍未通过。',takeaway_color='D47E31')

add('完整指标表：冻结 seed 与当前搜索节点分开列','04 / 实验进展与问题',
    '父策略是当前树 Node0；历史标签“71”不是当前树的 Node71。',
    '当前树Node0使用的策略版本与历史seed-search-71完整500策略一致。Node30的parent也是0，因此比较时父策略就是这一冻结seed，但Node30未生成完整500成绩。完整500表只能列L0和父seed；当前节点对应列保持暂无。父seed真实应用L1为595次、L2为0、L3为4。候选原执行时延500题均值172.43秒，token和调用只覆盖497题；L0缺完整资源统计，不能用已知片段补零。原执行均值也不能代替包含重试、评测、反思和学习的实验总成本。',
    [CMP,L0,TREE,AW],type='table',headers=['同题500指标','L0','父策略：冻结 seed','当前：Node30'],rows=[
    ['执行 / 有效评分','500 / 500','500 / 500','暂无新500结果'],
    ['严格通过 / 正式 Q','322 / 0.644','338 / 0.676','暂无'],
    ['平均无效 loop','0.380','0.204','暂无'],
    ['时延（秒）','暂无完整统计','172.43（500题）','暂无'],
    ['token / 模型调用','暂无完整统计','172,923.98 / 25.72（497题）','暂无'],
    ['L1 / L2 / L3应用','0 / 0 / 0','595 / 0 / 4','暂无同500指标']],
    widths=[.28,.20,.31,.21],table_font=14,
    takeaway='局部32题、完整500题与不同冻结版本不能混成一张“进化提升”成绩。')

add('30 轮搜索：尚未产生新的 accepted 节点','04 / 实验进展与问题',
    'Node26与Node28分别与父Node0在各自同题TRAIN32上比较；两组32题不同。',
    '当前树共31个节点，包含accepted seed0与30次搜索，记录395次优化器反思调用。30个新节点状态error13、duplicate5、untriggerable_controls4、interrupted3、inconclusive2、unobserved_changes2、rejected_minibatch1，新accepted为0。Node26全32 strict22到24，Q .6875到.75，loop都0，但实际变更影响8题strict均6、Q均.75，并出现关键违规1，因此拒绝；时延134.608到142.609秒，子应用L1=33/L2=L3=0。Node28另一32 strict25到26，Q .78125到.8125，loop .5625到.28125，时延120.735到111.960秒，但新增L3应用0，子只有旧L1=32，因此unobserved_changes。最新Node30局部数字只来自WORK_STATUS：同31父子strict20、Q20/31，loop4/31到11/31，时延与同31应用次数暂无，不能复算原报告。作业cancelled，final_holdout.running是遗留字段。',
    [TREE,STATUS],type='cards',cards=cards(
    ('30个新节点的产出','新 accepted：0\n反思调用：395\nerror13 / duplicate5\n不可触发4 / interrupted3\n不确定2 / 未观测变更2\n训练拒绝1'),
    ('Node26：全池净增2','父Node0 → 子26，同32题\n全池 strict：22 → 24\n实际影响8题：6 → 6\n新增关键违规1，节点被拒\n全池改善不等于变更收益'),
    ('Node28：新控制未介入','父Node0 → 子28，另一32题\n全池 strict：25 → 26\n新增 L3 实际应用：0\n既有 L1 应用：32 次\n终态 unobserved_changes')),
    takeaway='TRAIN32不是val或500；候选合法、实际介入和同题收益需要分别核验。',takeaway_color='D47E31')

add('业务实验显示：成绩选择可能掩盖整体退步','04 / 实验进展与问题',
    '业务与 AppWorld 独立展示；下表不是跨基准比较。',
    '业务seed004的300题子集文件明确写method=performance_informed_business_acceptance，limitations说明子集依据已测成绩选择。在这个300子集上，Q从0.4690到0.5578，严格通过113到145，loop3.073到1.973。然而同文件full400的Q从0.5284降到0.5117，strict177到168，loop2.870到2.955。最终500又由已选300加200训练回放组成，并标independent_holdout=false。因此all_goals_met=true只能描述这个协议下的门禁结果，不能作为泛化成功。不同分母不能拼接比较，也不以早期不完整评分推翻后续结果。',
    ['trace_output/business-simulator/published/seed004/baseline-cache/comparison300.json:1；/L0、/candidate、/full400、/limitations',
     'trace_output/business-simulator/published/seed004/comparison.json:1；manifest.json；independent_holdout=false'],
    source_short='证据：业务 seed004 comparison300.json 的 L0/candidate/full400/limitations',
    type='table',headers=['同范围业务对照','L0','候选','结果边界'],rows=[
    ['已选300：Q / strict','0.469022 / 113','0.557833 / 145','按已测成绩选子集'],
    ['已选300：无效loop','3.07333','1.97333','开发选择结果'],
    ['full400：Q / strict','0.528392 / 177','0.511717 / 168','完整400范围退步'],
    ['full400：无效loop','2.870','2.955','重复无效迭代增加'],
    ['最终500范围','业务300 + 回放200','all_goals_met=true','非独立留出']],
    widths=[.28,.23,.23,.26],table_font=14,
    takeaway='下一轮必须先冻结评测范围，再看候选成绩；不能按成绩反向选题。',takeaway_color='D47E31')

add('目前的主要问题集中在四个证据缺口','04 / 实验进展与问题',
    '将已确认的事实、静态限制和待检验假设分开处理。',
    '第一是覆盖和归属：AppWorld实际应用记录以L1为主，L2零应用、L3四次；各层收益贡献尚待消融，新增控制可能不触发。第二是泛化：500用于开发反馈，业务子集又按成绩选择。第三是稳定性和成本：反思调用不少，但30个搜索节点没有新accepted；L0与额外尝试费用缺失，不能说效率提升。第四是运行和证据闭合：v2完整接线未验证，当前副本缺失原报告与运行环境；原生及bufferedHTTP的接受回执也不等于落盘。以上事实指向研究设计和工程准备，但没有故障注入证据的恢复风险只能列为静态边界。',
    [CMP,TREE,AW,RT+'llm_call.py:89',P+'src/service/local_runtime.cpp:671',P+'src/service/integration_service.cpp:1340,2743'],
    cards=cards(('介入与收益未闭合','L2=0、L3=4；生成规则和实际改变行为脱节'),('泛化评测不独立','开发反馈污染；业务子集存在成绩选择偏差'),('稳定性与成本不足','0新accepted；基线与额外尝试费用不完整'),('运行与来源边界','v1/v2接线差异；原报告缺失；回执≠落盘')),
    takeaway='优先补可验证关键链路，再扩候选空间或增加搜索预算。')

add('下一步研究问题：触发、控制与泛化如何闭合？','05 / 下一步研究讨论',
    '以下是依据当前限制提出的研究建议，尚未实现或验证。',
    '建议先聚焦一个主问题：在冻结模型与预算下，基于真实观察、作用域和阶段的经验触发机制，能否比固定经验注入稳定提升独立留出成功率，并减少无效loop？它可以分为三条可检验问题：条件编译是否减少误触发和漏触发，L2或L3实际介入是否贡献任务改善，训练与开发得到的策略能否迁移到未观察任务族。贡献定位应放在可验证行为自进化，而不是声称我们实现了全新的GEPA，或把更多搜索轮数当成研究贡献。',
    [P+'docs/15-policy-snapshots.md:94-136',P+'docs/24-evolution-and-gepa.md',CMP,TREE],
    source_short='研究建议 · 由当前词面检索、应用覆盖与评测限制推导',
    cards=cards(('RQ1：触发准确性','观察 + 任务阶段 + scope，能否减少误注入和漏注入？'),('RQ2：行为控制价值','实际L2/L3介入，能否减少无效loop并保持成功？'),('RQ3：跨任务泛化','冻结策略能否改善未参与开发的任务族？')),
    takeaway='推荐主线：证据约束的条件触发与行为控制；把GEPA作为候选优化基础。')

add('方向一：让经验在正确阶段触发，并留下可核验证据','05 / 下一步研究讨论',
    '保留来源和授权；将轻量条件编译与词面基线进行比较。',
    '当前词面检索是可解释、低开销的基线，但否定表达、长上下文和阶段变化会引起误匹配。建议后台从训练观察生成有限条件：必要错误或状态、已执行操作、阶段、作用域与否定条件，在线只读取结构化事实而不增加同步反思调用。每条候选保存触发依据、prepared query、真实模型请求与后续动作差分。先做训练上的命中与负样本诊断，再在固定dev比较，最后冻结后进入确认集。任何新约束仍要经过已有批准和预算流程，不能手写测试答案进入经验。',
    [P+'docs/15-policy-snapshots.md:124-153',P+'docs/16-runtime-controls.md',RT+'llm_call.py:89,101'],
    source_short='拟议方法 · 当前已有 observed_operations 与作用域机制可作为基础',
    type='flow',steps=steps(('训练事实','错误与恢复\n正 / 负样本'),('后台编译','条件 / 阶段\n有限结构化规则'),('在线匹配','分支真实状态\n有界低开销'),('应用证据','query → 请求\n动作差分'),('独立验收','冻结后评测\n成功 / loop / 误触发')),
    takeaway='把“是否相关”推进为“何时适用”，并单独测量触发准确性与任务收益。')

add('方向二：用消融和配对实验检验真实收益','05 / 下一步研究讨论',
    '模型、宿主、任务、预算与裁判保持一致；评测不向记忆回写。',
    '建议对照至少包含L0原始Agent、固定批准经验、带条件的L1策略、以及按任务需要加入L2或L3的策略。对完整策略做关闭L2或关闭L3的消融，才能识别控制层贡献。使用相同环境快照、冻结裁判和相同任务，运行多次随机重复，保留逐题赢输与退化。按任务族划分训练、开发和未触碰确认集；各组memory和data_dir隔离，测试不写回。主指标使用正式strict与官方TGC分别报告，辅助指标为Q、无效loop、提前停止、触发错误与总成本，并给区间而不是只看一次均值。',
    [AW,P+'docs/19-datasets-and-benchmarks.md',CMP],
    source_short='拟议实验 · 需与导师确认预算与一次性确认集方案',
    type='table',headers=['组别','改变的因素','要回答的问题'],rows=[
    ['A：L0','不启用策略增强','原始成功率与成本'],
    ['B：固定经验','同一批准经验快照','经验内容是否有用'],
    ['C：条件L1','仅改变触发机制','阶段条件是否优于词面基线'],
    ['D：L1 + L2/L3','受控动作或loop策略','控制对成功与loop的增量贡献'],
    ['消融 / 重复','逐层关闭；多随机重复','归属、稳定性、退化与总成本']],
    widths=[.21,.34,.45],table_font=15,
    takeaway='先验证介入，再验证收益；用未触碰确认集检验泛化。')

add('建议按证据闭合顺序推进，而非继续堆搜索轮数','05 / 下一步研究讨论',
    '阶段与耗时为规划建议；以验收条件为准，不作完成承诺。',
    '第一阶段先整理一个可迁移的证据包，保留原生报告、冻结模型与裁判身份、环境版本、memory和策略，修清默认模板与受测v1的接线边界，不重写旧成绩。第二阶段在训练和固定dev验证条件触发与应用回执，统计正负触发、提前停止和逐题退化。第三阶段做等预算消融和多次重复，并预先冻结确认集。第四阶段一次性确认，形成结果表、失败分类和完整成本。若选择可靠性为独立方向，还需先定义可接受丢失窗口，再进行故障注入；当前静态恢复能力不能当已验证保证。',
    [AW,P+'docs/18-storage-and-recovery.md',P+'docs/22-testing-and-extension.md',MAN],
    source_short='下一阶段规划建议 · 各阶段以验收条件决定是否进入下一步',
    type='table',headers=['建议阶段','主要产物','进入下一步的条件'],rows=[
    ['1：证据准备','可迁移原报告 / 版本 / 配置 / 成本包','来源可重验；范围与接线明确'],
    ['2：小范围机制','条件触发与回执验证；错误分类','真实介入，误触发和退化可量化'],
    ['3：开发消融','等预算对照与多次重复','开发集稳定收益，策略冻结'],
    ['4：一次确认','未触碰确认集结果；区间与总成本','完成独立验收，保留失败和未达标']],
    widths=[.18,.43,.39],table_font=15,
    takeaway='优先级：可重验证据 → 条件触发 → 层级消融 → 独立确认。')

add('请导师帮助确定三个取舍','05 / 下一步研究讨论',
    '带着清楚的系统基础与证据边界，讨论一个可收敛的研究目标。',
    '今天希望确定三个取舍。第一，主贡献放在条件触发与可验证行为控制，还是放在后台搜索效率，建议优先前者，因为当前数据直接暴露了实际应用和收益之间的缺口。第二，AppWorld作为主任务环境，业务任务作为机制诊断，是否合适；两者分开报告，不做跨基准分数比较。第三，能投入多少重复评测和确认集预算，并允许哪些L2或L3干预。得到这三项决定后，可以用阶段验收推进，而不是无限增加节点。当前可以诚实报告的基础是工程闭环与开发范围改善，尚待解决的是稳定、泛化和成本。',
    [CMP,TREE,MAN],source_short='讨论页 · 建议方案与待决定事项',seconds=90,
    cards=cards(('研究主线','条件触发 + 行为控制\n或后台搜索效率？'),('任务与边界','AppWorld主评测；业务机制诊断\n确认集如何冻结？'),('预算与验收','允许的控制范围、重复次数\n总成本与阶段退出条件？')),
    takeaway='拟议定位：基于真实执行证据的、可验证的 Agent 行为自进化。')

# Appendix: accessible details without overloading the main talk.
add('源码阅读路线：先运行链，再学习链','附录 A / 仓库掌握',
    '按问题导航；路径与符号以当前提交为准。',
    '第一遍沿一轮任务读Bridge、RPC、SessionRouter、Batch、Worker与Python技能循环；第二遍沿一条轨迹读LocalRuntime或IntegrationService、评测、候选、memory、PolicyRuntime与GEPA；第三遍只读AppWorld实验控制器和冻结产物。不要从历史设计文档的函数名推断当前路径，也不要用README完成百分比替代源代码与测试证据。构建、单元、集成与真实模型验收分开进行，本次梳理没有执行这些运行验证。',
    ['frontend/bridge/routes/chat_rest.py','src/daemon/rpc_handler.cpp','src/agent/agent.cpp',RT,P+'src/'],
    type='table',headers=['要掌握的问题','优先阅读'],rows=[
    ['请求与批次','chat_rest.py → rpc_handler.cpp → session_router.cpp → agent.cpp'],
    ['模型与工具','llm_call.py → coordinator_decide.py → skill_executor.py'],
    ['采集与持久化','local_runtime.cpp / integration_service.cpp'],
    ['学习与运行策略','evolution_pipeline.cpp → memory → policy_runtime.cpp'],
    ['后台选优与验收','agent_evaluation.cpp → gepa_search.cpp → gepa_strategy.cpp'],
    ['可复现实验','experiments/appworld/README.md → reproduce.sh → manifest']],
    widths=[.22,.78],table_font=14.5,seconds=45,
    takeaway='日常构建 ./scripts/build.sh dev；组件另行构建；测试范围不能混算。')

add('接入范围：v1、v2、配置与入口必须分开核验','附录 B / 运行边界',
    '默认模板 v2，与当前已有直接演化打点的 v1 路径存在差异。',
    '当前模型和工具wrapper以AF_COORDINATOR_VERSION环境值判断：显式v2时不接演化，缺省按v1。默认模板缺省选择v2脚本，却不export该环境值；v2 coordinator_agent_loop又复用同一call_llm，因此环境未设且演化配置有效时，模型路径仍可能间接进入打点。当前未见v2同等的直接dispatch/loop接线，完整v2适配和默认环境一致性未做真实验证。专用BFCL、AppWorld及v1复现runner明确使用v1。EVOLUTION_POINT_CONFIG缺失时这组隐式wrapper不启用；Bridge仅在专门评测开关下绑定aevaluate。实验成功不能推定默认业务模板加载同一策略。',
    [RT+'llm_call.py:89',RT+'skill_executor.py:83',
     'frontend/bridge/templates/agent_template/harness_agent/nodes/coordinator_decide.sh:24',
     RT+'coordinator_agent_loop.py:32,370','frontend/bridge/main.py:234',P+'clients/python/evolution_configured.py:141',
     'configs/reproduction/v1-l3/manifest.json:29','examples/agent/business-simulator/appworld/appworld_agent_runtime.py:341'],
    cards=cards(('当前直接接线','v1：模型 / 工具 / 派发 / loop'),('默认模板','脚本缺省v2；wrapper环境缺省v1，需核验'),('启用条件','实际环境值、EVOLUTION_POINT_CONFIG、服务/身份'),('评测入口','专用runner显式v1；Bridge评测有专门开关')),
    seconds=45,takeaway='报告“组件已集成”时，同时说明宿主版本、配置与真实应用证据。')

add('持久化与恢复：接受、落盘与业务续跑不同','附录 C / 可靠性边界',
    '下面是静态源码确认的能力边界，未做本轮故障注入。',
    '原生LocalRuntime用有界内存接收事件，drain写一次active初始快照和closed最终快照，中间事件不是逐条durable journal。HTTP支持buffered和durable模式，buffered save只入队并返回persisted=false，需要run.status或flush确认。启动时将已落盘未关闭运行标为producer_interrupted并abort或隔离，后台学习和评测作业可恢复，但不是从上次工具动作继续业务。maintain_runs按10000阈值向archive移动，归档不释放磁盘。这些静态事实提示恢复责任和磁盘预算需要设计，不能据此断言已复现数据丢失。',
    [P+'src/service/local_runtime.cpp:671-738',P+'src/service/integration_service.cpp:669-695,844,1340,2685,2743,2804'],
    type='table',headers=['位置','当前能确认','不能据此宣称'],rows=[
    ['原生采集','内存接受；初始 / 最终快照落盘','每个中间事件都可恢复'],
    ['HTTP buffered','队列接受；status / flush确认落盘','run.end成功必定已持久化'],
    ['启动恢复','已落盘不完整运行 abort / quarantine','故障后从原工具动作自动续跑'],
    ['后台作业','恢复学习 / 评测检查点','复原未落盘的全部业务事件'],
    ['归档','移入archive；可历史读取','自动释放磁盘或具备总字节配额']],
    widths=[.20,.40,.40],table_font=14.5,seconds=45,
    takeaway='可靠性研究前先定义允许丢失窗口、恢复责任与磁盘保留策略。')

add('状态字典：同一个“完成”不能覆盖整条链路','附录 D / 状态语义',
    '这些状态需要各自的证据，不能依次推定。',
    'completed表示执行终态；persisted表示指定采集状态已落盘；evaluated表示评分完成；approved表示经验获得应用资格；published在公共memory中可指经验发布，在策略交付中又有不同语义；loaded_version表示宿主加载版本；advice.applied表示核验到控制进入真实边界；passed或goal_passed才回答任务与实验门槛。client.delivery还只声明server_side_observation，并不是远端客户端已确认收到。这样区分能避免把一张交付文件或一个活进程解释成实验成功。',
    [P+'docs/README.md',MAN,'frontend/bridge/services/sessionlog_delivery.py:266-267'],
    type='table',headers=['状态','回答的问题'],rows=[
    ['completed / persisted','执行是否终态 / 采集是否已落盘'],
    ['evaluated / approved','评分是否完整 / 经验是否获得应用资格'],
    ['published / delivered','哪个产物被交付？合格还是最佳可用？'],
    ['loaded_version / advice.applied','加载了哪个版本 / 实际使用了何种增强'],
    ['passed / goal_passed','任务严格通过 / 实验门槛是否达成'],
    ['client.delivery','服务端观察；client_acknowledged=false']],
    widths=[.36,.64],table_font=15,seconds=45,
    takeaway='实现、启用、应用、收益和泛化分别建立证据。')

add('证据包：现有摘要可复算，原生来源仍需补齐','附录 E / 可迁移与复现',
    '当前本机证据与原运行产物分开描述，避免把旧路径当现存文件。',
    '当前AppWorld正式目录保留13个JSON，约6.4MB，含L0逐题摘要、候选comparison、tree、策略、manifest和memory。manifest引用六份候选native report和一份L0原报告，在旧root前缀映射到当前仓库后仍不存在。因此能复算两边500摘要的strict、Q、loop、应用次数和候选部分资源，不能从本机原轨迹核验每次工具行为或重新执行完整native来源校验。建议保存版本清单、环境与模型身份、脱敏配置、报告、官方IO、真实请求和全部尝试费用，利用相对路径或显式映射支持迁移。',
    [MAN,CMP,L0,TREE,P+'docs/INTERMEDIATE_ARTIFACTS.md'],
    cards=cards(('本机已保留','策略 / memory / tree / 两边500逐题摘要'),('本次可复算','strict、Q、loop、L1-L3；候选部分资源'),('本机缺失','6份候选native report + L0原报告等'),('下一步归档','环境 / 版本 / 脱敏配置 / 官方IO / 全尝试成本')),
    seconds=45,takeaway='摘要自洽是必要条件；可审查原始事实和可重跑复现还需原生来源。')

add('官方完成率与仓库严格验收分开报告','附录 F / 指标口径',
    '仓库的正式 strict/Q 加入了额外严格验收，不直接等于官方 TGC。',
    '官方任务状态检查与仓库模型严格验收会给出不同结果。L0原摘要official_strict_passed为323、候选为339，而正式strict分别为322和338。两边各有一题官方满分但组合严格验收未通过；原报告缺失，本次不能追查额外失败理由。正式Q在这批单目标任务里又等于正式严格通过率，不能把两者作为两项独立统计证据。official_quality是部分检查比例，只是辅助。所有对外成绩须注明当前自定义500范围、混合验收协议和非独立开发用途，不能放到AppWorld官方排行榜直接比较，更不能以官方339替代正式338通过门槛。',
    [L0,CMP,AW,'AppWorld官方仓库：https://github.com/StonyBrookNLP/appworld'],
    cards=cards(('正式严格验收','322 → 338；确定性与模型严格条件共同满足'),('官方任务完成','323 → 339；官方状态字段，按独立口径报告'),('正式有效 Q','本批单目标Q为0/1；均值等于正式strict率'),('辅助检查比例','official_quality为部分检查比例；不替代严格通过')),
    seconds=45,takeaway='主图 64.4% → 67.6% 是仓库正式口径；公开汇报须保留这一说明。')

add('可复现入口与本次核查范围','附录 G / 验证边界',
    '命令作为入口说明；本次没有执行模型实验、服务启动或构建回归。',
    'AppWorld README给出fresh真实API全流程、resume同作业恢复、plan与status只读查看，以及deliver-best-available只导出旧评分结果。fresh会重跑模型和可能清理实验空间，不是本次梳理需要做的动作。本机相关build与AppWorld虚拟环境缺失，脚本存在不等于当前可直接运行。本文只做当前源码静态核对、现存JSON复算、来源路径存在性检查以及PPT生成和渲染验证。历史专项测试不是本轮测试结果。BFCL当前结果缺失；tau2只保留控制脚本，宿主、数据和运行依赖未闭合，不能声称已完成tau2接入或报完整实验成绩。',
    [AW,P+'experiments/appworld/reproduce.sh',P+'experiments/bfcl-v3/README.md',P+'experiments/tau2-retail-verified/tau2_experiment.py'],
    type='table',headers=['动作','用途','当前报告边界'],rows=[
    ['--fresh','真实训练 → 学习 → 搜索 → 对照 → 发布','会执行模型；本次未运行'],
    ['--resume','按冻结身份恢复原作业','不是新树或改旧成绩'],
    ['--plan / --status','只读阶段安排与状态','状态字段仍需原产物核验'],
    ['--deliver-best-available','导出现有完整已评分策略','不代表目标达标或GEPA完成'],
    ['本轮核查','静态源码、摘要复算、来源与文稿渲染','未跑构建、回归或真实模型验收']],
    widths=[.25,.40,.35],table_font=14.5,seconds=45,
    takeaway='可复现脚本存在、运行成功、成绩稳定复现是三个不同结论。')

add('导师可能追问的五个问题','附录 H / 答疑准备',
    '回答时先给结论，再给证据边界。',
    '如果问是否在训练模型，回答没有，当前演化经验和行为控制。如果问是否自进化成功，回答有冻结seed开发对照改善，但尚未达到既定质量门槛，也没有独立留出证明。如果问是否L3带来收益，回答只有四次应用，需要消融和触发诊断。如果问GEPA30轮是否有效，回答当前没有新accepted，工程问题与不可触发控制占据许多节点。如果问贡献在哪里，回答建议聚焦观察与阶段约束的条件触发以及行为介入证据，再用独立实验检验泛化与成本。',
    [CMP,TREE,MAN,P+'docs/24-evolution-and-gepa.md'],
    type='table',headers=['问题','可据当前证据回答'],rows=[
    ['是否训练了模型？','当前不更新权重；演化经验、动作与loop策略。'],
    ['自进化已经成功吗？','开发范围改善；质量门槛未达；独立泛化待验证。'],
    ['L2/L3贡献有多大？','L2=0、L3=4；尚需真实触发、配对消融和重复实验。'],
    ['GEPA30轮有效吗？','当前0新accepted；保留真实状态，不把搜索次数当收益。'],
    ['下一步贡献是什么？','条件触发与可验证控制；独立验收稳定收益及完整成本。']],
    widths=[.29,.71],table_font=15,seconds=45,
    takeaway='已实现机制可以讲清楚；尚未证明的收益作为下一步研究问题。')

assert len(S)==38, len(S)
assert sum(x['seconds'] for x in S[:30])==1800
(R/'deck_content.json').write_text(json.dumps(dict(metadata=dict(date='2026-10-09',head='f44bbaf4a9531731bfbd3eb6c115fe4210df95b1',main_slides=30,appendix_slides=8,verification='static source audit and existing summary recomputation; no experiment rerun'),slides=S),ensure_ascii=False,indent=2),encoding='utf-8')
print(f'Wrote {len(S)} slides; main talk 1800 seconds')
