#!/usr/bin/env python3
"""Compare raw Excel and converted cases, then inventory execution gaps per turn.

Static, explainable rules only. Counts describe affected scenarios, not pass rates.
Runtime agents never import this module or read these expectations.
"""
from __future__ import annotations
from collections import Counter, defaultdict
import hashlib
import json
from pathlib import Path
import re
import sys

PACKAGE=Path(__file__).resolve().parents[1]; REPO=PACKAGE.parents[2]
sys.path.insert(0,str(REPO/'benchmark'))
from converters import read_xlsx_rows, parse_intents, split_steps, convert_product

CAPABILITIES={
 'home_control':('家居控制','基础测试系统已实现','16台测试设备、批量事务及时间演化；没有实体家庭连接，不代表全部业务场景验收通过'),
 'home_habits':('家居使用习惯', '测试记录统计已实现', '按日期/设备的耗电、小时使用时长、同时运行统计与开关时刻；保留100053，仍缺原家庭历史和实体电表'),
 'phone_actions':('APP实际操作','适配器已实现，当前未连接设备','ADB 可观察、截图、点击、滑动、输入、按键和启动应用；当前服务器没有已授权 Android，原生任务只能产出步骤'),
 'gui_plan':('GUI操作计划', '22类操作计划已实现', '包含收藏/安装/签到/评论/打车、发送对象及正文；网站使用真实浏览器，原生手机尚未接入'),
 'navigation':('当前位置及实时避堵路线','部分实现','已有步骤与高德入口；缺用户定位、具体医院及实时路况/算路接口'),
 'hospital':('医院预约及查询', '测试系统已实现', '13家测试医院、21科室、时间与患者过滤、医生/号型、费用、库存与改约；不连接同名真实医院'),
 'reminder':('用药提醒', '本地执行已实现', '保留ID更新、饭点偏移、工作日/间隔、筛选、去重、调度/通知箱/ICS；尚无手机推送'),
 'health_records':('健康档案', '本地管理已实现', '患者/id过滤、有效否定事实、版本、软删除；仍需用户提供原医疗资料'),
 'triage':('健康咨询与导诊','部分实现','LLM一般建议和本地病史；缺原业务导诊服务与医学质量专项验收'),
 'movie_recommend':('影片推荐与多轮片单', '网页与片单管理已实现', '真实检索/网页核实、保存片单与精确序号详情；没有原授权片库，质量需逐例验收'),
 'playback':('音视频实际播放', '网页播放器已实现', 'Chromium实播/暂停/跳转并回读媒体状态；原生APP、实体电视、会员资源需连接/权限'),
 'private_knowledge':('原业务知识与账户数据', '导入与检索已实现', '公开网页、用户资料导入、来源版本与知识库检索；原私有规则/账号权益未提供'),
 'weather':('天气湿度与预报', '预报及历史接口已实现', '真实预报及1940年起历史再分析；代表点不等于全地域、缺定位不能猜用户位置'),
 'live_business_data':('班次票价、库存、营业状态等', '公开网页执行可用', '真实浏览器搜索/详情页核查；目标站点登录、反自动化或网络仍可能阻止结果'),
 'transaction':('充值/订单/发送', '网页操作需账户与逐站验收', '真实表单/点击/上传/下载及提交回读；没有预置账号或支付方式，未验收真实付款/邮件发送'),
 'voice_identity':('声纹身份与跨设备账户','缺失','文本框架用户身份不能替代声纹识别或原用户账户'),
 'memory':('偏好与记忆', '本地用户级记忆已实现', '同用户跨会话共享偏好，其他用户隔离；没有原声纹身份连接'),
 'original_protocol':('原业务端侧结构化下发','部分实现','工具轨迹有UserOutputObject和操作码；没有原客户端协议传输、端侧落库与执行回执'),
 'text_content':('问答、写作、推理','已实现待业务验收','真实模型完成；正确性和多轮表现仍需业务集逐例评分'),
 'image_generation':('图片生成','已实现待业务验收','已接火山真实图片模型；本业务数据没有明确生成图片任务标签'),
 'video_generation':('视频生成','缺失','当前配置下已试的视频模型返回UnsupportedModel'),
 'media_understanding':('图片/音视频理解','缺失','未接媒体上传与识别链路；纯文本参考视频资料不属于识别'),
}


def analyze_turn(case, turn, number, row):
    user=turn['user']; expected=turn.get('expected_behavior',''); text=user+'\n'+expected
    skills=turn.get('expected_skills') or []; category=case['category']
    numbered=parse_intents(row.get('预期操作类型',''))
    codes=re.findall(r'\b\d{6}\b',','.join(numbered.get(number,[])))
    if not numbered and len(case['turns'])==1:
        codes=re.findall(r'\b\d{6}\b',row.get('预期操作类型',''))
    findings={}
    def add(key,evidence,confidence='explicit'):
        findings.setdefault(key,{'capability':key,'status':CAPABILITIES[key][1],'reason':CAPABILITIES[key][2],
                                 'evidence':[],'confidence':confidence})['evidence'].append(evidence)
    code_map={'100051':'home_control','100053':'home_habits','100101':'playback','200002':'movie_recommend',
              '300001':'gui_plan','100151':'reminder','100152':'health_records','100153':'health_records',
              '100154':'hospital','100155':'triage','100156':'hospital','100157':'reminder'}
    for code in codes:
        if code in code_map:add(code_map[code],'原表第%d轮操作码%s'%(number,code))
    if 'smart-home' in skills or (turn.get('expected_skills') is None and category in ('智家服务','居家操控','居家习惯')) or re.search(r'(?:开|关|调|设).{0,10}(?:空调|窗帘|客厅.*灯|卧室.*灯|加湿器)',user):
        add('home_habits' if re.search(r'习惯|habitTime|modeList',text) else 'home_control','家居意图/场景或明确控制词', 'rule')
    if 'device-gui' in skills or category=='GUI泛化' or re.search(r'taskPlan|任务规划步骤|操作步骤|300001',expected):
        add('gui_plan','GUI意图/任务步骤契约','rule')
    if 'gui_plan' in findings:
        # A completed plan can satisfy this specific converted expectation. Actual
        # clicking remains a separate product requirement, not an automatic score failure.
        add('phone_actions','真实完成APP任务仍需执行器；若本轮只考步骤，缺口不直接等于评分失败','scope')
    if re.search(r'导航|避堵|避开拥堵|避开堵|最快路线|定位当前位置',text):
        add('navigation','导航/定位/避堵要求','rule')
    if re.search(r'挂号|预约.{0,8}(?:医生|科室|门诊)|医院.{0,8}预约',text) or category in ('挂号','挂号查询'):
        add('hospital','挂号语义或挂号场景','rule')
    if re.search(r'(?:用药|吃药|服药|药品|药物).{0,8}提醒|提醒.{0,12}(?:吃|服用|维生素)|medicationReminder|100151|100157',text) or category.startswith('吃药提醒'):
        add('reminder','提醒对象/提醒场景','rule')
    if re.search(r'健康档案|healthRecord|100152|100153',text):add('health_records','档案要求','rule')
    if category=='健康导诊' or '100155' in codes:add('triage','导诊场景/操作码','rule')
    if 'movie-query' in skills or re.search(r'aiMovieData|电影推荐列表|返回.*多部《|核心看点',expected):add('movie_recommend','观影意图/推荐契约','rule')
    if re.search(r'(?:播放|接着看|开始看|放一下|放个).{0,20}(?:电影|影片|那部|这部|第二部|第一部|一部|音乐|歌曲|视频|集|首歌)|(?:播放|观看)(?:第一|第二|第三|刚才)',user) or '100101' in codes:
        add('playback','要求实际播放或播放操作码','rule')
    if 'knowledge-base' in skills or category=='知识库问答' or re.search(r'云手机|话费还剩|账户余额|套餐余量|剩余流量',text):add('private_knowledge','产品知识/账户语义','rule')
    if re.search(r'天气|湿度|下雨|降雨|气温|空气质量|高温预警',user):add('weather','天气语义；部分只是背景，需逐例复核','rule')
    if re.search(r'班次|票价|余票|航班|库存|营业时间|酒店价格|房价|订票|实时路况',text):add('live_business_data','实时业务数据语义','rule')
    if re.search(r'充值|缴费|(?:交|充)\d+元话费|下单|帮我.{0,8}(?:买票|订票|订餐|订酒店)|发送.{0,8}(?:邮件|消息|短信)',user):add('transaction','交易/对外发送动作语义','rule')
    if 'user-memory' in skills or re.search(r'记住我|我的.*偏好',user):add('memory','偏好/记忆要求','rule')
    if number==1 and ('声纹' in row.get('预置条件（可选）','') or row.get('声纹ID','').strip()):add('voice_identity','原表声纹前置条件或声纹ID')
    if re.search(r'(?:生成|制作|画).{0,10}(?:图片|插画|海报|图像)',user):add('image_generation','明确生成图片','rule')
    if re.search(r'(?:生成|制作).{0,10}视频',user):add('video_generation','明确生成视频','rule')
    if re.search(r'(?:识别|分析|看一下).{0,10}(?:这张图|上传的图|这个视频)',user):add('media_understanding','明确识别媒体','rule')
    if codes or 'UserOutputObject' in expected:add('original_protocol','原表操作码或UserOutputObject契约')
    if 'web-search' in skills or not findings:add('text_content','QA意图或无法从现有标注确定其他执行需求','rule')
    return {'turn':number,'user':user,'expected_behavior':expected,'expected_skills':turn.get('expected_skills'),
            'raw_operation_codes':codes,'findings':list(findings.values()),
            'missing_intent_label':turn.get('expected_skills') is None,'missing_expected_behavior':not expected,
            'plan_only_expectation':bool(re.search(r'规划|步骤|计划',expected)) and 'gui_plan' in findings}


def audit():
    dataset_path=REPO/'benchmark/datasets/product.json';dataset=json.loads(dataset_path.read_text())
    mapping=json.loads((REPO/'benchmark/domain_skill_map.json').read_text())
    raw={};fresh={};sources=[];warnings=[]
    for path in sorted((REPO/'test_cases/product').glob('*.xlsx')):
        rows=read_xlsx_rows(path)
        sources.append({'file':str(path.relative_to(REPO)),'rows':len(rows)-1,'sha256':hashlib.sha256(path.read_bytes()).hexdigest()})
        for index,values in enumerate(rows[1:],2):
            row=dict(zip(rows[0],values));raw[(path.name,row.get('用例编号'))]={**row,'excel_row':index}
        for case in convert_product(path,mapping,warnings)['cases']:fresh[(path.name,case['id'])]=case
    comparisons=[];items=[];present=set();renamed=[]
    for case in dataset['cases']:
        source=Path(case['provenance']['source']).name
        raw_id=case['id'];prefix=f'product:{Path(source).stem}:'
        if raw_id.startswith(prefix) and (source,raw_id[len(prefix):]) in raw:
            raw_id=raw_id[len(prefix):]
            renamed.append({'normalized_id':case['id'],'raw_id':raw_id,'source':source,'reason':'existing converter duplicate-id namespace'})
        key=(source,raw_id);present.add(key)
        row=raw.get(key,{});converted=fresh.get(key)
        differences=[]
        if not row or not converted:differences.append('raw_source_missing')
        else:
            if len(case['turns'])!=len(converted['turns']):differences.append('turn_count')
            for number,(a,b) in enumerate(zip(case['turns'],converted['turns']),1):
                for field in ('user','expected_behavior','expected_skills'):
                    if a.get(field)!=b.get(field):differences.append(f'turn{number}.{field}')
            for field in ('goal','category','difficulty','level'):
                if case.get(field)!=converted.get(field):differences.append(field)
        if differences:comparisons.append({'id':case['id'],'source':key[0],'differences':differences})
        items.append({'id':case['id'],'category':case['category'],'source':key[0],'excel_row':row.get('excel_row'),
                      'raw_id':raw_id,'raw_precondition':row.get('预置条件（可选）',''),'raw_operation_types':row.get('预期操作类型',''),
                      'raw_device_type':row.get('设备类型',''),'raw_voiceprint_id':row.get('声纹ID',''),
                      'turns':[analyze_turn(case,t,i,row) for i,t in enumerate(case['turns'],1)]})
    counts={};all_turns=[t for c in items for t in c['turns']]
    for key,(name,status,reason) in CAPABILITIES.items():
        affected=[(c,t) for c in items for t in c['turns'] if any(f['capability']==key for f in t['findings'])]
        counts[key]={'name':name,'status':status,'reason':reason,'cases':len({c['id'] for c,t in affected}),'turns':len(affected),
                     'examples':[{'id':c['id'],'turn':t['turn']} for c,t in affected[:5]]}
    return {'notice':'静态影响清单，规则推断不是通过率或失败率。含条件分支/上下文/仅计划任务，必须结合实际运行复核。',
            'sources':sources,'normalized_file':str(dataset_path.relative_to(REPO)),
            'normalized_sha256':hashlib.sha256(dataset_path.read_bytes()).hexdigest(),
            'cases':len(items),'turns':len(all_turns),'raw_rows':sum(s['rows'] for s in sources),
            'conversion':{'differences':comparisons,'raw_only':[{'source':s,'id':i} for s,i in sorted(raw.keys()-present)],
                          'normalized_only':[{'source':s,'id':i} for s,i in sorted(present-raw.keys())],
                          'warnings':warnings,'renamed_ids':renamed,'operation_codes_preserved_in_normalized':False},
            'label_gaps':{'no_expected_skills':sum(t['missing_intent_label'] for t in all_turns),
                          'no_expected_behavior':sum(t['missing_expected_behavior'] for t in all_turns),
                          'plan_only_gui_turns':sum(t['plan_only_expectation'] for t in all_turns)},
            'capability_summary':counts,'case_details':items}


def markdown(report):
    lines=['# 业务能力缺口与用例影响','',report['notice'],'',
           '最新深度复查见 [能力缺口复查](capability-gap-recheck.md)：包含隔离接口复现、具体用例及修复优先级。下表为涉及范围，不能把基础接口存在当成所有用例已覆盖。','',
           '## 数据对照','',f"三份原表共 {report['raw_rows']} 行；product.json 共 {report['cases']} 个用例、{report['turns']} 轮。",
           f"按源文件名与用例编号匹配，重新使用现有转换器对照用户输入、预期行为、意图、轮次和用例字段，差异 {len(report['conversion']['differences'])} 项；原表独有 {len(report['conversion']['raw_only'])} 项，JSON 独有 {len(report['conversion']['normalized_only'])} 项。",'',
           f"转换器为 {len(report['conversion']['renamed_ids'])} 个重名用例加了来源前缀，本报告按来源正确匹配；这不是丢失用例。",'',
           '原表的预期操作类型没有独立保存在 product.json；本报告重新读取了它们。前置条件、设备类型、声纹字段也参与核查。',
           f"{report['label_gaps']['no_expected_skills']} 轮没有意图标签，{report['label_gaps']['no_expected_behavior']} 轮没有预期行为；不能把这些空标注当作能力已覆盖。",'',
           '重转字段一致只说明文件同步，不能说明原始表的轮次编号、意图或预期行为标注都正确。', '', '## 能力与影响范围','', '| 能力 | 当前状态 | 涉及用例/轮次 | 缺口及范围 | 例子 |','|---|---|---:|---|---|']
    for row in report['capability_summary'].values():
        example='、'.join(f"{e['id']} 第{e['turn']}轮" for e in row['examples'][:2]) or '未发现明确对应任务'
        lines.append(f"| {row['name']} | {row['status']} | {row['cases']} / {row['turns']} | {row['reason']} | {example} |")
    lines += ['', '同一个用例可能涉及多个能力，表中数量不能相加。关键词规则可能把背景描述纳入影响范围；每个命中都有具体依据，可在 JSON 中逐项复核。', '',
              '## 对验收的实际影响','',
              '- 家居和挂号已接可运行的独立业务测试系统，能用设备状态/预约库存验收执行链路；结果标记 business_test_system、physical_effect=false。它们不是原家庭或同名真实医院的操作。网页媒体已能真实播放、暂停和回读状态；原生播放器仍需 Android 设备。',
              '- GUI 原表操作码300001考有序任务规划。生成步骤可以满足这类输出要求；配置 ADB 设备后可观察并点击真实 Android，否则不能声称 APP 已操作。',
              '- 用药提醒已具备真实本地调度、通知事件和日历文件；若验收要求下发原客户端或手机主动弹窗，还缺通道。',
              '- 天气按实际日期与地点变化。用例标题/示例预期中的“湿度不大”不是固定事实；实际湿度偏大时应跳过条件动作，静态 expected_skills 仍可能要求 health-advice，需要把意图分数与条件正确性分开看。',
              '- 文本质量、健康建议、资料可信度、多轮指代和路由准确率仍需模型逐例验收；本报告没有把接口存在当成测试通过。',
              '- 全量详细条目见 [capability-audit.json](../evals/capability-audit.json)：每例原表行号、原操作码、每轮输入/预期、命中能力、原因与标注缺口。', '',
              '## TGJK_YD2_010','',
              '第一轮先查真实湿度；只有条件成立才创建08:00/20:00提醒。独立导航任务可产出完整步骤，但当前没有定位和实时算路，不能给出虚构最快路线。第二轮必须沿用前文天气判断徒步条件；不能再次创建提醒。', '',
              '当前缺口：手机推送、用户定位、医院目的地消歧、实时路况与路线计算；手机 APP 执行取决于是否配置已授权 ADB 设备。缺口不影响真实天气查询、本地提醒持久化和后续查询。','']
    return '\n'.join(lines)


if __name__=='__main__':
    report=audit();out=PACKAGE/'evals/capability-audit.json';out.write_text(json.dumps(report,ensure_ascii=False,indent=2)+'\n')
    docs=PACKAGE/'docs';docs.mkdir(exist_ok=True);(docs/'capability-gaps.md').write_text(markdown(report))
    print(json.dumps({k:v for k,v in report.items() if k not in ('case_details','sources')},ensure_ascii=False,indent=2))
