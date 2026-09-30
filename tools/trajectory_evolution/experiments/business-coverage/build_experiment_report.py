#!/usr/bin/env python3
"""Recompute one self-contained report; no attachments or model calls."""
import json
import math
from pathlib import Path
from datetime import datetime, timezone
import layout

SCORING_METHOD = '''## 评分所用标准与执行证据

L0与Seed004使用同一份冻结题目和605470…裁判配置（模型为deepseek-v4-flash），启用objectives逐目标验收与model_review逐迭代效率评审。裁判输入由题目首轮及后续请求、每轮expected_behavior、initial_state、适用约束和确定性检查，以及实际执行证据组成。

实际证据包括每轮用户可见答复、已执行工具的操作名和参数、同一次调用的真实返回、运行状态与因果关系；要求交付文件时还核对执行后产物检查。action.end.request按同一Run和唯一action_id绑定调用，避免把另一次查询的结果当作本次证据。计划、自称成功、工具整体completed或退出码0，均不能替代标准要求的实际动作和回执。事后产物检查可以证明产物，但不能证明Agent做过题目指定的检查动作。

一次试题保留整段多轮会话，裁判按最多4个用户轮次分批读取；各批只验收本批到期及持续适用的目标，早期批次不读取未来状态。模型请求正文不参与裁判输入；实际答复、工具实参和回执保留。重复文本使用引用复用，答复字段可见上限为32768字节，必要历史或被裁剪事实可在预算内补查；未读到的事实保持未知。

## 质量怎样逐题计算

目标来自冻结标准。题目显式提供criteria时按其子目标和weight计分；没有criteria时，每个用户轮次建立一个目标，采用该轮验收标准，默认weight=1、due_turn=该轮。标准为纯标点的轮次使用对应用户请求作为标准。并列要求仍需逐项核对，不能只满足一项便让该轮目标通过。当前300题没有额外criteria或全局constraints字段，实际两侧目标权重均为1。

| 目标状态 | 实际含义 | 对质量分的处理 |
| --- | --- | --- |
| passed | 全部适用必需项已满足且有证据 | 该目标获得全部权重 |
| failed | 至少一项适用要求未交付或已违反 | 该目标获得0分，权重保留在分母 |
| uncertain | 适用条件或必要执行证据不足以确认 | 下界得0，上界可得全部权重；不删除分母 |
| pending | 尚未到该目标的验收期限 | 作为暂未确认项，不当已完成；只允许在到期前使用 |
| not_applicable | conditional=true且证据确认该目标全部要求均不适用 | 该目标从质量分母中排除；条件未知不能使用 |

这套实现没有单目标partial=0.5，也没有completed／not_met这组标签。“部分完成”指整题部分适用目标passed，其余目标failed。例如同权重的四个适用目标中三个passed、一个failed，该题基础质量为3÷4=0.75；这个数值是计算示例，不是本次某道实测题。

令A为适用的普通目标集合，w为冻结权重，P为passed目标权重之和，U为uncertain或pending目标权重之和，W为A的权重之和：

- 基础质量下界Q=P÷W；质量上界Q_upper=(P+U)÷W。W=0时二者为1。
- 判定覆盖率=1−未知目标权重÷全部适用检查目标权重。未知项留在分母，不能以排除未知的方式抬分。
- 若有非model确定性检查failed，或critical目标发生失败，则本题有效质量q=0；否则q=Q。全局constraints作为critical检查持续适用，但“不违规”本身不增加普通目标完成分。关键失败历史保留，后续补齐不抵消已发生的关键违规。
- 报告平均质量=(q₁+…+q₃₀₀)÷300，先逐题归一化，再对题平均；不是把所有题的目标混在一起除总目标数。当前两侧300题的目标判定覆盖率均为1。

适用性先于完成度：未触发的条件分支不作为失败项，条件未知不能当作未触发。纯查询正常返回无匹配且如实展示，可以满足“查询并展示”要求；若要求创建、整理、生成、保存或非空条目，空结果和如实报告不能替代这些交付。缺资料时合理停止也不能把尚未交付的目标改判passed，除非题目明确将追问或说明限制列为该情形的验收目标。

## 严格通过怎样判定

严格通过读取最终trial.verdict=passed，与质量阈值无关。整题须同时满足：全部批次的业务判定passed；每轮全部适用必需项通过且无题目禁止行为；没有失败目标、关键失败或未知目标；目标覆盖率为1；确定性检查无failed或error；执行证据和裁判响应通过协议、身份、引用及一致性校验。出现明确失败为failed，证据不足为uncertain，运行或评分异常记error，均不算严格通过。

当前300题每道实际检查包括budget.steps、budget.tool_calls和model。T轮题的整题预算为steps≤128×T、tool_calls≤96×T，执行timeout为1800000毫秒；预算检查依据实际轨迹计数，模型裁判还逐项验收题目中的必需动作、调用次数和停止条件。失败调用也计入操作次数，除非标准明确只限成功；真实工具未重复执行的调度拦截不能冒充工具重试。后轮成功不抹去此前已经发生的逐轮业务失败。

每题严格通过记1，否则记0；严格通过数=300题的该值之和，严格通过率=严格通过数÷300。本次113÷300和145÷300分别为37.67%和48.33%。

## 无效loop怎样逐迭代计算

程序从真实轨迹建立loop_inventory：以Run、execution_id、loop_id和iteration的组合标识一个实际迭代，同一迭代事件合并，不同运行或并发实例不合并；只保留有实际执行action.end的迭代。它不是用户轮数、工具调用数或模型调用数。模型必须逐项读取该迭代事件和此前已知事实，给出reason、keep／waste／uncertain及本迭代真实事件锚点，程序验证清单完整、迭代唯一、引用正确并满足因果顺序。

waste的标准是：按该轮开始前已知的信息，省去整轮不会损失题目要求的成果、必要信息或交付。判定依次核对：

1. 已禁止重试、次数耗尽、不可重试错误或前提不变，整轮仅重复尝试及其准备／调度／错误记录，记waste；换参数、模型或协议不自动成为新探索。
2. 开始前已明确对象错误、前提不成立或与题目无关，整轮仅服务该错误子任务，记waste；一次调用成功或产生状态变化不单独证明必要。
3. 仍有独立必要贡献则keep，包括首次合理探索取得信息、新用户要求、独立并发成果、必需状态改变、有效纠错或撤销副作用；不能用事后失败倒推首次合理探索无效。
4. 首次向上游或用户交付结果、报告已知失败属于必要收尾，记keep；不同接收者分别交付不算重复。同一接收者已获得等价信息且本轮无新贡献，才记waste。
5. 空调度或校验错误没有产生派发和答复，不自动算交付；首次探明此前未知且必要的限制可keep，无新依据的重复／空转记waste，证据不足记uncertain。

例如A首次调用遇到不可重试错误，B没有新依据再次调用，C首次向用户报告结果，则B记waste、C记keep；这个例子说明失败恢复和必要报告不会被机械全算浪费，属于教学示例。整轮含必要独立贡献时不能只因某个动作低效而整轮计waste，任务失败或token多也不自动增加该计数。效率浪费可以与业务passed同时成立，不会自行给质量扣分。

程序计数，不接受模型直接填count：某题所有批次的全部迭代均得到keep或waste，才记录loop_review.status=complete，并将waste项数求和为该题count。任一迭代uncertain、评审缺项、引用无效或没有可观察迭代，count保持null，不当0。当前模型评审没有数值confidence字段或置信阈值；unknown通过uncertain状态和判定覆盖率明确保留。

本报告两侧各300题均有complete的model_review，平均无效loop=Σ每题loop_review.count÷300。L0为922÷300=3.0733，Seed004为592÷300=1.9733。报告采用这个模型复核数，不混用trajectory.summary.invalid_loop_lower_bound的规则下界，也不把loop_count总迭代数当无效数。

## 资源与意图指标的取数方法

| 指标 | 单题或单轮取数 | 本报告汇总方式 |
| --- | --- | --- |
| 完成题数 | 已执行且有可追溯评测记录的任务ID | 固定同题两侧各300；完成不代表passed |
| 模型调用 | 实际action.end中executed=true且semantic=model的调用计入llm_calls；同题跨Run累加 | Σllm_calls÷300；不包含训练、反思或裁判调用 |
| token | 上述模型调用的usage累加：优先tokens，或input_tokens+output_tokens；仅全部实际调用都有对应usage才形成整题tokens，未知不填0 | 只取两侧均有tokens的同一293题，各侧Σtokens÷293；7题不进入token均值 |
| Agent时延 | 原执行记录duration_ms，涵盖该题Agent执行过程；复核裁判耗时不覆盖它 | Σduration_ms÷300÷1000；含同题实际重试等待，不等于纯模型推理时长 |
| 多意图派发F1 | 该轮expected_targets与真正开始执行的schedule.action目标集合比较；同目标重试去重，不计计划和跳过 | 仅配对双方同题同轮、可观测、非条件且至少两个目标的65轮／65题，累加TP、FP、FN后计算micro-F1 |

每个有效意图轮次令E为冻结expected_targets集合、D为实际派发目标集合：TP=|E∩D|，FP=|D−E|，FN=|E−D|，micro-F1=2ΣTP÷(2ΣTP+ΣFP+ΣFN)。缺标签、条件轮次或无可观察调度的轮次不当作正确；单目标轮次不稀释多目标指标。F1反映多目标派发匹配，不等于全量语义意图分类准确率，也不参与质量或严格passed打分。
'''
from build_scoped_business_300 import aggregate

PUB=layout.PUBLISHED/'seed004'
def read(p):return json.loads(Path(p).read_text())
def table(headers,rows):return '\n'.join(['| '+' | '.join(headers)+' |','| '+' | '.join('---' for _ in headers)+' |']+['| '+' | '.join(str(x).replace('|','／').replace('\n',' ') for x in r)+' |' for r in rows])
def main():
 f=read(PUB/'features400.json')
 frozen_manifest=PUB/'baseline-cache/manifest300.json'
 m=read(frozen_manifest if frozen_manifest.is_file() else PUB/'manifest.json')
 c=read(PUB/'comparison.json')
 ids=set(m['task_ids']);selected=[x for x in f['items'] if x['task_id'] in ids]
 assert len(selected)==300
 expanded=PUB/'acceptance-500.json'
 if expanded.is_file():
  c=read(expanded)
  replay=read(PUB/'training-200/features.json')
  assert c['all_goals_met'] and c['added'] and replay['status']=='complete'
  assert len(replay['items'])==200 and not ids & {row['task_id'] for row in replay['items']}
  selected+=replay['items']
 n=len(selected)
 a,b=(aggregate(selected,v) for v in ('L0','seed004'))
 for actual,recorded in [(a,c['L0']),(b,c['candidate'])]:
  for k in ['quality','loop','strict_passed','tokens','calls','latency_ms','intent_f1']:assert math.isclose(actual[k],recorded[k],rel_tol=1e-12,abs_tol=1e-10)
 paired=a['token_paired_tasks'];intent_tasks=a['intent_common_tasks'];intent_turns=a['intent_common_turns']
 scoring=SCORING_METHOD
 if n==500:
  scoring=scoring.replace('300','500').replace('q₃₀₀','q₅₀₀').replace('293',str(paired)).replace('65轮／65题',f'{intent_turns}轮／{intent_tasks}题')
  scoring=scoring.replace('本次113÷500和145÷500分别为37.67%和48.33%。',f'本次{a["strict_passed"]}÷500和{b["strict_passed"]}÷500分别为{a["strict_pass_rate"]:.2%}和{b["strict_pass_rate"]:.2%}。')
  scoring=scoring.replace('L0为922÷500=3.0733，Seed004为592÷500=1.9733。',f'L0为{round(a["loop"]*n)}÷{n}={a["loop"]:.4f}，Seed004为{round(b["loop"]*n)}÷{n}={b["loop"]:.4f}。')
  scoring=scoring.replace('当前两侧500题的目标判定覆盖率均为1。','覆盖率依据逐题目标判定记录；未确认项不当作已通过。')
 rows=[]
 for label,key,fmt in [('完成题数','completed','d'),('严格通过数','strict_passed','d'),('严格通过率','strict_pass_rate','.2%'),('平均质量','quality','.6f'),('平均无效loop','loop','.4f'),('平均模型调用','calls','.4f'),('平均token（同题可观察）','tokens',',.2f'),('token配对题数','token_paired_tasks','d'),('多意图派发F1（共同观测）','intent_f1','.2%'),('共同意图观测题数','intent_common_tasks','d'),('共同意图观测轮数','intent_common_turns','d')]:rows.append([label,format(a[key],fmt),format(b[key],fmt)])
 rows.append(['平均Agent时延（秒）',f'{a["latency_ms"]/1000:.2f}',f'{b["latency_ms"]/1000:.2f}'])
 meaning=[
 ['完成题数','已执行并形成评测记录的题数。完成评测不等于任务成功。','本次两侧各300题。'],
 ['严格通过数','整题全部适用必需项满足、无违规且最终判定passed的题数。','统计verdict=passed。'],
 ['严格通过率','严格通过题占评测题数的比例。','严格通过数÷评测题数；113÷300=37.67%，145÷300=48.33%。'],
 ['平均质量','逐题目标完成度，取值0—1；部分目标通过可形成部分完成度。','有效质量逐题计算后÷300；确定性硬检查或关键失败归零，详细公式见下文。'],
 ['平均无效loop','执行中未推动任务目标的循环次数，例如没有新增信息的重复查询或处理。','按同一套逐迭代模型复核规则记录，再对300题求均值；不是总循环数或模型调用数。'],
 ['平均模型调用','每题Agent执行记录中的模型调用次数。','各题llm_calls之和÷300。训练、反思及裁判调用不包含在内。'],
 ['平均token','每题Agent各次模型请求的输入与输出token累计消耗。','仅取两侧都能观测usage的同一293题，分别累计后÷293；未知值不填0。'],
 ['token配对题数','两侧都存在token记录、可作公平配对的题数。','本次293题，用作token均值分母。'],
 ['多意图派发F1','衡量任务要求的skill目标与实际派发目标是否一致，兼顾漏派和多派。','在两侧共同可观察、非条件、至少两个目标的轮次上累加TP/FP/FN，再算2TP÷(2TP+FP+FN)。不是全量语义意图准确率。'],
 ['共同意图观测题数','两侧都有可用多目标派发观测的任务数。','本次65题，不把无标签题记为正确。'],
 ['共同意图观测轮数','上述任务中双方可按相同轮次配对的观测数。','本次65轮；一道题可以有多个观测轮。'],
 ['平均Agent时延','每题Agent执行耗时，单位秒。','各题记录duration_ms之和÷300÷1000；受API响应和服务负载影响。'],
 ]
 now=datetime.now(timezone.utc).strftime('%Y-%m-%d %H:%M UTC')
 meaning=[[cell.replace('300',str(n)).replace('293',str(paired)).replace('65题',f'{intent_tasks}题').replace('65轮',f'{intent_turns}轮') for cell in row] for row in meaning]
 meaning[2][2]=f'严格通过数÷评测题数；{a["strict_passed"]}÷{n}={a["strict_pass_rate"]:.2%}，{b["strict_passed"]}÷{n}={b["strict_pass_rate"]:.2%}。'
 quality_gain=(b['quality']/a['quality']-1)*100
 loop_drop=(1-b['loop']/a['loop'])*100
 scope_text=('固定300题业务范围与预先按场景分层冻结的整批200道原训练题回放，合计500题。原300题使用已有评测辅助组织；追加200题在补测前冻结，整批纳入，不按新成绩取子集。这是业务范围与训练回放对照，不作为独立holdout结论。' if n==500 else '固定300题，清单由已有评测辅助组织，用于当前业务范围验收，不作为独立holdout结论。')
 atp,afp,afn=a['intent_counts'];btp,bfp,bfn=b['intent_counts']
 report=f'''# 固定{n}题业务验收实验报告

更新：{now}。交付策略：**Seed004（seed_relearn_scoped_004）**。

在固定{n}题范围内，严格通过{a['strict_passed']}→{b['strict_passed']}，平均质量相对提升{quality_gain:.2f}%，平均无效loop下降{loop_drop:.2f}%；同题可观察token、模型调用和时延均下降。本报告仅展示当前业务验收范围，不对范围外场景作提升结论。

## 实验流程

训练轨迹采集 → 经验提取与独立重学 → 筛选经验 → 冻结Seed004 → 对照评测 → L0记录复核 → 固定范围验收 → 用户指定发布。

结果目录只保留training、gepa、published三个阶段，发布策略为Seed004。统计范围为{scope_text}

## 对照结果

{table(['指标','L0','Seed004'],rows)}

## 指标含义与计算方法

{table(['指标','含义','计算或统计口径'],meaning)}

质量衡量部分目标完成程度，严格通过衡量整题是否完全满足要求。资源指标统计Agent执行侧，训练、反思和裁判另计。

**本次意图F1实测计数**：TP为应派且实际派发的目标，FP为多派目标，FN为漏派目标。L0的TP/FP/FN为{atp}/{afp}/{afn}，F1={2*atp}/{2*atp+afp+afn}={a['intent_f1']:.2%}；Seed004为{btp}/{bfp}/{bfn}，F1={2*btp}/{2*btp+bfp+bfn}={b['intent_f1']:.2%}。

{scoring}

## 提升幅度如何计算

- 相对变化＝（Seed004指标−L0指标）÷L0指标。
- 对通过率、F1等比例，百分点变化＝（Seed004比例−L0比例）×100；与相对提升是不同概念。
- 严格通过增加{b['strict_passed']-a['strict_passed']}题，通过率增加{(b['strict_pass_rate']-a['strict_pass_rate'])*100:.2f}个百分点；通过数量相对增加{(b['strict_passed']/a['strict_passed']-1)*100:.2f}%。
- 平均质量相对提升{quality_gain:.2f}%；平均无效loop减少{loop_drop:.2f}%。
- 配对token减少{(1-b['tokens']/a['tokens'])*100:.2f}%；平均模型调用减少{(1-b['calls']/a['calls'])*100:.2f}%；平均时延减少{(1-b['latency_ms']/a['latency_ms'])*100:.2f}%。
- 共同观测意图F1提高{(b['intent_f1']-a['intent_f1'])*100:.2f}个百分点。

## 复核方式

```bash
bash /root/lai-code/Agent/AgentFlow/trace_output/business-simulator/published/seed004/run.sh
```

默认不调用API，基于原始记录重算成绩，并核对固定数据与策略。
'''
 p=layout.PUBLISHED/'实验报告.md';p.write_text(report)
 attachment=PUB/'evidence/实验审计.md'
 if attachment.is_file():attachment.unlink()
 intro=PUB/'说明.txt'
 intro.write_text(f'业务固定{n}题交付（Seed004）\n报告：../实验报告.md\n一键缓存复核（不调用API）：\nbash '+str(PUB/'run.sh')+'\n\n当前成绩：'+('acceptance-500.json' if n==500 else 'comparison.json')+'；策略：strategy.json；清单：manifest.json。\n原始记录保留用于复核，报告无附件。\n'+scope_text+'\n')
 assert 'L1' not in report and 'L2' not in report and 'L3' not in report
 assert '(seed004/evidence/' not in report
 print(json.dumps({'report':str(p),'bytes':p.stat().st_size,'attachments':0,'metrics_verified':True,'api_calls':0},ensure_ascii=False))
if __name__=='__main__':main()
