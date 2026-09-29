#!/usr/bin/env python3
"""Generate reviewed, train-only business practice cases; never read evaluation splits.

The cases are authored from train primitives and public tool contracts. A case is a
whole conversation; no turn or branch is independently assigned to a dataset split.
No model calls, tool execution, or writes to the authoritative learning memory.
"""
from __future__ import annotations

import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path

HERE = Path(__file__).resolve().parent
DEFAULT_OUTPUT = HERE / 'datasets/supplemental-20260929'
INTENTS = {'movie-query': '观影', 'health-advice': '健康', 'smart-home': '居家',
           'custom-workflow': '工作流', 'web-search': 'QA', 'knowledge-base': '知识库'}
REFERENCES = {
    'binding': ['QA_GY_HHYL_005', 'SYS_DYT_03_012'],
    'entity': ['QA_GY_HHYL_005', 'QA_GY_WYYL_010'],
    'idempotency': ['SYS_DYT_02_040', 'SYS_DYT_03_070', 'SYS_DYT_03_087'],
    'predicate': ['SYS_DYT_03_241', 'TGZS_YDYL_006'],
    'partition': ['SYS_DYT_02_040', 'SYS_DYT_03_012'],
    'stop': ['QA_GY_WYYL_010', 'SYS_DYT_03_070'],
    'repair': ['SYS_DYT_03_012', 'SYS_DYT_02_057'],
    'revision': ['QA_GY_HHYL_005', 'SYS_DYT_03_087'],
}


def turn(user, skills, effects, behavior, conditional=False):
    return {'user': user, 'expected_targets': skills,
            'expected_intents': list(dict.fromkeys(INTENTS[s] for s in skills)),
            'expected_effects': effects, 'expected_behavior': behavior,
            'conditional': conditional}


def authored_cases():
    """Six structurally different instances per mechanism, not name substitutions."""
    H, M, S, C = ['health-advice'], ['movie-query'], ['smart-home'], ['custom-workflow']
    cases = []
    def add(mechanism, instance, *turns):
        cases.append((mechanism, instance, turns))

    add('binding', 'record_fields_to_schedule',
        turn('请记一条我的健康档案：2026年8月12日复诊，医生让我每天09:10吃维生素B2，用量按医嘱；请先只记录，不创建提醒。', H, ['100152'], '保存用户给定 visit_date、medication、doctor_advice；保留09:10，不创建提醒或推导剂量。'),
        turn('现在读取刚才保存的那条档案，按里面的药名和时刻建每日提醒，剂量没有给出就标明未提供。', H, ['100153', '100151'], '先读取本次保存档案，再将其药名和09:10绑定到创建参数；不得从无关旧记录取值；提醒成功保留ID和时区。'),
        turn('只告诉我刚才提醒的药名和时刻，不需要再查或再建。', [], [], '引用已有成功回执回答，不新增提醒、不重复调用。'))
    add('binding', 'movie_result_to_named_note',
        turn('从测试片库推荐三部喜剧，列出片名，先不要播放或保存片单。', M, ['200002'], '返回真实推荐列表；无额外保存、详情或播放。'),
        turn('把这次返回的第一部片名和核心看点存成备忘录，标题叫周五看片候选。', C, [], '使用前轮实际第一项片名和看点传给 custom-workflow.save，不重新生成片单、不猜片名；保存结果如实报告。'))
    add('binding', 'reminder_result_to_note',
        turn('给我建一个每天18:40吃乳糖酶片的提醒，剂量我稍后自己看医嘱。', H, ['100151'], '按给定药名和18:40创建；剂量保持未提供，不额外追问阻断或自行推导。'),
        turn('读取这条提醒，把实际编号、药名、时刻和是否启用存进题为晚间提醒信息的备忘录。', H+C, ['100157'], '读取真实提醒后再将返回ID、药名、times、active写入备忘录；下游不能在上游结果到达前编造字段。'))
    add('binding', 'device_result_to_note',
        turn('将书房台灯亮度设为35%，除此之外不要改其他设备。', S, ['100051'], '先获取设备ID，仅设置书房台灯brightness=35，保留真实控制回执。'),
        turn('查一下现在书房台灯的状态，把查询到的设备名、开关和亮度存成备忘录灯光记录。', S+C, [], '状态查询结果先于保存；用实际设备名、power和brightness生成正文，不将初始状态当当前回执。'))
    add('binding', 'selected_record_to_note',
        turn('替测试患者小禾存档：2026年8月16日体检，医生建议一周后复查，其他检查数值我没有提供。', H, ['100152'], '存小禾的visit_date和doctor_advice；不生成检查数值。'),
        turn('查出小禾刚存的档案，把就诊日期与原文复查建议摘到备忘录小禾复查，别推算具体复诊时间。', H+C, ['100153'], '按患者及实际记录定位后，把已存日期和原文医嘱传到save；不替代为当前用户记录、不推导预约。'))
    add('binding', 'failed_lookup_blocks_dependent_only',
        turn('查一下测试患者南枝的健康档案；只有查到记录才把原文存成备忘录南枝档案。同时打开书房台灯。', H+S, ['100153', '100051'], '南枝无档案应如实说明；不创建空备忘录、不虚构档案；独立台灯控制仍完成。', True),
        turn('既然没有档案，就先不存任何东西。把刚才灯的处理结果告诉我即可。', [], [], '沿用已有台灯回执，明确未保存档案；无需再查、再开灯或占位派发。'))

    add('entity', 'filtered_list_index',
        turn('请按测试片库给我三部科幻片，只列结果，不播放。', M, ['200002'], '保留本次筛选推荐列表顺序和真实实体。'),
        turn('播放刚才这份列表的最后一部，并告诉我实际播放的片名。', M, ['100101'], '从本次列表确定最后一项，用有效index或title绑定播放，核对player.title；不能按全片库位置选片。'))
    add('entity', 'list_refresh_reference',
        turn('先给我两部喜剧推荐。', M, ['200002'], '返回两部真实喜剧。'),
        turn('改看科幻，重新列三部，不要播放上一份里的任何片子。', M, ['200002'], '重新筛选科幻并更新当前推荐上下文，不播放旧列表。'),
        turn('播放新列表排第一的那一部。', M, ['100101'], '指代最新科幻列表首项，不能使用旧喜剧列表；回报测试播放器实际片名。'))
    add('entity', 'out_of_range_then_valid',
        turn('只推荐两部适合全家看的电影。', M, ['200002'], '用family_friendly筛选并保留至多两项列表。'),
        turn('我选第九部。', [], [], '已知列表不足九项时指出越界并请求有效选择；不猜电影、不重新凑九部、不假报播放。'),
        turn('那就播放第一部吧。', M, ['100101'], '从此前两项列表定位第一项并实际播放，不能因上一轮越界终止整段对话。'))
    add('entity', 'detail_keeps_selected_identity',
        turn('推荐三部动画片，保留列表顺序。', M, ['200002'], '返回动画推荐列表。'),
        turn('查询列表中最后一部的详情，告诉我是哪部，暂不播放。', M, ['200003'], 'detail定位该列表最后实体；不得把详情对象替换为全目录同序号。'),
        turn('就播放你刚刚查详情的那部。', M, ['100101'], '承接具体详情实体，核验player.movie_id/title，不再重新推荐。'))
    add('entity', 'record_identity_by_patient',
        turn('分别替测试患者小松和小桃存档：小松8月3日体检，小桃8月7日复诊，两人都还没有提供用药。', H, ['100152'], '新建两个不同patient记录，visit_date分别2026-08-03和2026-08-07，不能相互覆盖或填药。'),
        turn('只查小桃那条，告诉我患者、日期和记录编号。', H, ['100153'], '按患者/真实ID定位小桃并返回实际ID与日期，不能按创建顺序猜小松为目标。'))
    add('entity', 'reminder_identity_by_medicine',
        turn('创建两个独立提醒：每天07:20吃维生素C、每天19:10吃乳糖酶片，剂量都没有提供。', H, ['100151'], '按两种medicine和times建立两个独立提醒，保留真实ID。'),
        turn('暂停乳糖酶片那一条，维生素C那条保持原样。', H, ['100157'], '从已有回执或查询确定乳糖酶片真实ID再pause；不按位置猜ID，不取消/暂停维生素C。'),
        turn('查询两条现在的状态。', H, ['100157'], '如实展示不同ID与active状态；不重新创建暂停的提醒。'))

    add('idempotency', 'explicit_retry_same_reminder',
        turn('每天12:25提醒我吃维生素C，剂量未提供。', H, ['100151'], '创建一次每日提醒并返回实际ID。'),
        turn('网络像是卡了一下，我只是确认上一条办好了没有，不是要再加一条。', [], [], '已有成功回执就复述ID和时刻，不重复创建；只有真实未知才有针对性读取，不能把确认当新意图。'))
    add('idempotency', 'read_absent_then_add',
        turn('先查询有没有每天16:15吃乳糖酶片的提醒，只查询。', H, ['100157'], '返回当前实际提醒列表，初态为空；不创建。'),
        turn('如果确实没有，就新增这个每日16:15的提醒；有的话保留原来的。', H, ['100151'], '依据此前空结果创建一次；未知不能当不存在，已存在则不能重复创建。', True),
        turn('再查询一次，告诉我符合这个药名和时刻的提醒有几条。', H, ['100157'], '只读取并确认匹配数量为1，不为核验再次新增。'))
    add('idempotency', 'distinct_second_time_is_new',
        turn('先建一个每天08:35吃维生素B2的提醒。', H, ['100151'], '创建08:35一条。'),
        turn('查一下已有的，再另外新增每天20:35同药的独立提醒；早上的不要修改。', H, ['100157', '100151'], '先查已有，新增不同时间定义的第二条，保留早上ID与时刻；不可误去重或覆盖。'))
    add('idempotency', 'same_record_confirmation',
        turn('请记录：我2026年8月9日体检，尚未服药，没有提供检查数值。', H, ['100152'], '保存真实visit_date和尚未服药事实，不写虚构数值。'),
        turn('还是刚才同一次体检，只想确认记录在，不要建立另一份。', H, ['100153'], '按本次真实ID读取确认，不生成重复记录或用allow_duplicate绕过去重。'))
    add('idempotency', 'update_record_same_id',
        turn('记录我的2026年8月10日复诊，医生建议规律作息。', H, ['100152'], '保存visit_date和doctor_advice，返回ID。'),
        turn('给同一次复诊补充过敏史：青霉素过敏。其余内容保留。', H, ['100152'], '先定位原ID后record_save更新allergies，保留日期医嘱和ID，不能新建另一条。'),
        turn('读取这条，核对编号没变，原来的复诊日期和建议还在。', H, ['100153'], '读取同ID并如实展示原字段与新增过敏史；不得声称更新未改ID却无证据。'))
    add('idempotency', 'pause_resume_preserve_id',
        turn('给我建立每天10:45吃维生素C的提醒。', H, ['100151'], '创建并保留实际ID。'),
        turn('先暂停这条，不要删掉。', H, ['100157'], 'pause同ID，不能cancel或重建。'),
        turn('现在恢复原来这条，时间不要改。', H, ['100157'], 'resume原ID，时间10:45保持；不新建同定义提醒。'))

    add('predicate', 'positive_record_evidence',
        turn('存一条2026年8月18日的医嘱记录：每天17:05吃维生素C，剂量未提供。先不设置提醒。', H, ['100152'], '只记录给定医嘱，不提前创建提醒。'),
        turn('读取这条；只有医嘱同时写了药名和时刻，才创建对应每日提醒。', H, ['100153', '100151'], '先核验记录包含medicine和17:05，条件成立后创建一次；剂量不编造。', True))
    add('predicate', 'negative_empty_record',
        turn('查询测试患者闻溪的档案，如果完全没有记录，才把书房台灯调为20%；查到记录就不动灯。', H+S, ['100153', '100051'], '先查闻溪得到空记录才执行灯brightness20；不得把查询失败当空；两支互斥。', True),
        turn('只解释你执行了哪一个分支，沿用刚才结果，不要再操作。', [], [], '引用空记录证据及已执行控制回执说明负向分支，不再次query/control。'))
    add('predicate', 'unknown_required_time',
        turn('请存我的就诊信息：2026年8月19日复诊，药名是维生素B2，医生还没告诉我服药时间。', H, ['100152'], '保存已有药名及时间未知事实，不猜早晚时刻。'),
        turn('读取医嘱。只有记录里有明确时刻才设提醒，否则先告诉我缺什么。', H, ['100153'], '记录缺明确时刻，不能默认08:00/20:00或创建空提醒；指出时刻缺失。', True),
        turn('现在补充：每天14:20。按这个时间建提醒即可，不用修改其他记录。', H, ['100151'], '使用新增明确时刻和原药名创建，剂量保持未提供；不因之前缺项永久失败。'))
    add('predicate', 'catalog_empty_no_play_independent_home',
        turn('在测试片库找标题叫《雾港铅笔巡游》的电影，找到才播放；同时把客厅灯调到60%。', M+S, ['100051'], '使用合法查询或detail核验不存在片名后不play、不捏造片源；失败查询未必有影片效果码，独立灯光控制继续。', True),
        turn('找不到就不看了，只复述灯光操作的结果。', [], [], '复用现有控制回执，不反复搜索同一不存在影片、不再开灯。'))
    add('predicate', 'nested_success_then_write',
        turn('把书房台灯亮度设为45%。只有控制成功且回执亮度为45%，才推荐两部喜剧。', S+M, ['100051', '200002'], '先确认控制成功回执，再推荐；控制失败不能触发推荐。', True),
        turn('如果刚才确实返回了两部片名，就把这两个片名存进备忘录周末候选；不足两部先说明。', C, [], '依据真实返回列表长度决定save；正文保留那两项实体，不臆造第二项。', True))
    add('predicate', 'false_numeric_gate_then_revision',
        turn('把客厅空调温度设为24度，然后查看回执；只有温度高于28度才把书房台灯打开，否则不动台灯。', S, ['100051'], '设为24后根据实际回执判断24不高于28，不执行后件；不能把成功条件偷换成温度条件。', True),
        turn('修改条件：不高于24度时才打开书房台灯。先前空调不用重复设置。', S, ['100051'], '依据已有24度事实执行新的非大于谓词，只打开台灯，不重复设置空调。', True))

    add('partition', 'two_visit_months',
        turn('分别存两份档案：2026年6月11日体检，建议规律运动；2026年8月23日复诊，建议复查。不是同一条的更新。', H, ['100152'], '以不同visit_date新建两条并保留各自医嘱，不复用同ID覆盖。'),
        turn('只查2026年6月1日至6月30日的记录，别混入八月那份。', H, ['100153'], '按record_fields.visit_date范围筛选，返回六月档案，不能按写入时间筛掉全部。'))
    add('partition', 'same_date_different_patient',
        turn('2026年8月14日两人都体检：测试患者小岚说无过敏史，小乔说花粉过敏。分别存档。', H, ['100152'], '按patient分成不同记录；同一天不是同一患者，不混合过敏史。'),
        turn('只给我看小岚的档案。', H, ['100153'], 'patient小岚筛选，返回无过敏史，不泄露或拼接小乔资料。'))
    add('partition', 'distinct_sources_same_patient',
        turn('我的两次就诊请分开：2026年7月2日体检建议运动；2026年7月20日口腔复诊建议按时刷牙。', H, ['100152'], '两份不同就诊来源分别新建，保持日期和原文建议绑定。'),
        turn('只给7月20日那次补充一句：下月复查。7月2日那份不变。', H, ['100152'], '定位7月20日原ID更新doctor_advice，保留原建议并追加用户新事实；另一记录不变。'),
        turn('查询七月份两份记录，分行展示日期、建议和编号。', H, ['100153'], '七月范围返回两条不同ID，分别展示，不能合并为一条。'))
    add('partition', 'bounded_reminder_periods',
        turn('建立两条独立提醒：2026年10月1日至3日每天08:10吃维生素C；10月5日至7日每天20:10吃乳糖酶片。剂量都未提供。', H, ['100151'], '两条各有独立药名、times、start_date/end_date；不合并为连续1日至7日。'),
        turn('查询它们，分别告诉我起止日期和每天几点。', H, ['100157'], '读取两条并保持时间区间各自绑定，不丢掉中间空档、不自行续期。'))
    add('partition', 'independent_note_keys',
        turn('分别存两个备忘录：晨间事项写浇花和通风；晚间事项写收衣服和关窗。不要合并。', C, [], '使用两个不同标题/条目保存正文，不能后一条覆盖前一条。'),
        turn('读取晨间事项，确认它只有早上的两件事。', C, [], '查询准确条目，返回浇花和通风，不混入晚间事项。'))
    add('partition', 'range_boundary_inclusive',
        turn('存三次体检记录，日期分别为2026年6月30日、7月1日、7月31日，医嘱均为规律作息，但三次不是重复提交。', H, ['100152'], '不同visit_date建立三条记录，不因相同医嘱误去重。'),
        turn('仅查询2026年7月1日至7月31日，边界两天都算在内。', H, ['100153'], '按含首尾的日期范围返回7月1日和7月31日，不含6月30日；不是按数据写入时间判断。'))

    add('stop', 'recommendation_only',
        turn('只给我推荐两部动画片；这次不需要详情、收藏或播放。', M, ['200002'], '取得真实列表即交付，不额外detail/save/play或重复推荐。'),
        turn('收到，就到这里，不用继续操作。', [], [], '直接简短回应，无run_skills空数组或占位任务。'))
    add('stop', 'control_receipt_is_sufficient',
        turn('把客厅灯亮度设为55%，控制完成后只告诉我回执里的状态。', S, ['100051'], '查设备后set亮度，成功回执已经含power/brightness时直接报告，不额外on确认。'),
        turn('只复述亮度是多少，不要再控制。', [], [], '从成功回执给出55%，不再set/on或为复述重复查询。'))
    add('stop', 'reminder_created_no_duplicate_verification',
        turn('每天15:40提醒我吃维生素C，成功后告诉我编号和时刻即可。', H, ['100151'], '创建一次并按真实成功回执交付ID/times，不用再次创建证明成功。'),
        turn('谢谢，保持原样。', [], [], '不再创建、修改或派占位技能。'))
    add('stop', 'explicit_note_save',
        turn('保存备忘录桌面整理，内容是归还书籍、清理便签；不要额外保存完成标记。', C, [], '保存真实条目并报告；不得新增无内容完成标记或重复保存相同正文。'),
        turn('我只问刚才标题叫什么，不用读取文件。', [], [], '依据既有上下文回答桌面整理，不额外query。'))
    add('stop', 'empty_record_is_final_read',
        turn('查一下测试患者海棠的健康档案，有什么就展示什么。', H, ['100153'], '不存在则如实报告空记录，不重复无参record_query，不为了查而写。'),
        turn('那先不补资料，也不要创建档案。', [], [], '接受停止，不把待补清单写成健康档案。'))
    add('stop', 'pause_success_then_resume_new_intent',
        turn('在测试播放器播放《星际穿越》。', M, ['100101'], '实际更新测试播放器并保留片名与状态；不误称用户实体电视已播放。'),
        turn('暂停它，成功后就结束这轮。', M, ['100101'], 'pause一次，以paused回执完成，不把同一暂停反复执行。'),
        turn('现在继续播放。', M, ['100101'], '新一轮明确resume是新意图，应执行；不能用上一轮已停止规则拒绝。'))

    add('repair', 'missing_medicine_then_complete',
        turn('请给我建一个每天11:15的吃药提醒。', [], [], '药名缺失，直接询问药名；不得用默认药名创建或把缺参调用循环重试。'),
        turn('药名是维生素B2，剂量还没提供。', H, ['100151'], '结合前轮11:15创建；剂量未提供可保持缺省，不继续问阻断。'))
    add('repair', 'missing_time_then_complete',
        turn('帮我设一个每天吃乳糖酶片的提醒，时间稍后告诉你。', [], [], '等待具体时刻，不默认08:00/20:00、不创建无时间提醒。'),
        turn('每天21:35，直到我取消；剂量未提供。', H, ['100151'], '合并前轮药名与新时刻，创建21:35每日提醒，不猜剂量/结束日期。'))
    add('repair', 'invalid_time_corrected',
        turn('每天25:80提醒我吃维生素C。', [], [], '识别时刻非法并请用户更正，不把25:80截断/猜为合法时刻，不循环尝试。'),
        turn('刚才输错了，是23:10。', H, ['100151'], '采用纠正后的23:10与原药名，创建一次；不保留非法值。'))
    add('repair', 'control_range_corrected',
        turn('把书房台灯亮度设置为140%。', [], [], '识别brightness仅0到100，询问合法值；不能静默截断或宣称设置成功。'),
        turn('那就改为70%，其余灯别动。', S, ['100051'], '仅控制书房台灯brightness70，完成后停止；不再使用140。'))
    add('repair', 'wrong_movie_title_then_recover',
        turn('在测试播放器播放《纸船轨道之夜》，找不到就告诉我。', M, [], '真实查询/播放返回not_found后准确说明，不伪造播放器状态，不反复试同片。'),
        turn('换成《星际穿越》播放。', M, ['100101'], '新片名是可修正输入，应执行成功播放，不因先前not_found拒绝新请求。'))
    add('repair', 'missing_record_content_then_complete',
        turn('我想创建一份健康档案，目前没有提供就诊内容。', [], [], '询问至少一项真实健康内容，不能保存空档案或把缺项清单当内容。'),
        turn('补充：2026年8月27日体检，我尚未服药，医生建议规律睡眠。', H, ['100152'], '保存明确visit_date、尚未服药与医嘱，缺检查数值不臆造；新增信息使任务可继续。'))

    add('revision', 'correct_reminder_time_keep_id',
        turn('建立每天06:50吃维生素C的提醒。', H, ['100151'], '创建并保留真实ID。'),
        turn('我刚才说早了，改成07:40，是修改原来的那条，不是新增。', H, ['100151'], 'reminder_update原ID，times改07:40，保留药名，不另建。'),
        turn('读回这条确认旧时间没有保留。', H, ['100157'], '读取原ID，展示07:40且不再包含06:50，若更新失败不得伪称完成。'))
    add('revision', 'different_order_same_skill_reentry',
        turn('先把书房台灯打开；然后保存备忘录今天顺序，写明先开灯再记事；保存成功后才将客厅灯亮度调为30%。', S+C, ['100051'], '严格smart-home→custom-workflow→smart-home；两个家电动作不能合成前置批次，不跳过保存成功门。', True),
        turn('只按刚才实际执行的顺序复述结果，不再做一遍。', [], [], '引用各步骤真实回执和先后，不能只给计划或重新执行。'))
    add('revision', 'later_cancel_does_not_erase_old_action',
        turn('在测试播放器播放《星际穿越》。', M, ['100101'], '播放并报告当前真实状态。'),
        turn('改主意了，暂停播放，别声称刚才没播放过。', M, ['100101'], 'pause当前实体，承认上一轮已播放，不能用新要求抹去既有动作。'))
    add('revision', 'patient_correction_before_write',
        turn('我要给测试患者小杉建立健康档案，但先别存，内容等下一条。', [], [], '等待内容，不创建空记录。'),
        turn('患者名字更正为小棠，2026年8月25日复诊，医生建议散步。现在存这一条。', H, ['100152'], '以纠正后的patient小棠新建；不为旧名小杉创建，不混合患者。'),
        turn('只查小棠，确认记录在正确的人名下。', H, ['100153'], '按小棠读取，不能用无筛选查询冒充精确身份核对。'))
    add('revision', 'device_correction_changes_only_requested_field',
        turn('把客厅空调设为26度、送风模式。', S, ['100051'], '一次批量提交温度与mode，保留真实设备回执。'),
        turn('温度不变，只把模式改为睡眠。', S, ['100051'], '同设备仅改mode睡眠，保留temperature26；不把风速字段当模式或重做无关动作。'),
        turn('现在查询它的温度和模式。', S, [], '从真实query状态展示26和睡眠，不能仅复述目标冒充查询。'))
    add('revision', 'old_selected_entity_survives_new_list',
        turn('给我推荐两部科幻电影，把第一部的片名明确告诉我，暂不播放。', M, ['200002'], '保留第一份科幻列表及所指具体片名。'),
        turn('再列两部喜剧作为备选，不要改变我刚才选中的科幻片。', M, ['200002'], '可更新工具last_movies为喜剧，但会话中保留用户已选科幻实体。'),
        turn('现在播放我最初选中的那部科幻片，不是最新列表的第一部。', M, ['100101'], '用最初实体title/id播放，不能使用已被覆盖的index=1；核验播放器片名。'))
    return cases


def build_tasks(train_path):
    original = [json.loads(line) for line in train_path.read_text().splitlines() if line.strip()]
    original_ids = {row['task_id'] for row in original}
    if len(original) != 400 or any(row['split'] != 'train' for row in original):
        raise ValueError('Expected the original train400 source only')
    missing = {x for ids in REFERENCES.values() for x in ids} - original_ids
    if missing:
        raise ValueError(f'Train provenance IDs missing: {sorted(missing)}')
    source_fingerprint = hashlib.sha256(train_path.read_bytes()).hexdigest()
    profile_hashes = {r['initial_state']['profile_sha256'] for r in original}
    if len(profile_hashes) != 1:
        raise ValueError('Original train must use one fixed business profile')
    tasks, counts = [], Counter()
    for mechanism, instance, turns in authored_cases():
        counts[mechanism] += 1
        task_id = f'SUPP_20260929_{mechanism.upper()}_{counts[mechanism]:02d}'
        expectations = [{k: v for k, v in t.items() if k != 'user'} | {'turn': i}
                        for i, t in enumerate(turns, 1)]
        if mechanism == 'repair' and not expectations[0]['expected_targets']:
            expectations[0]['expected_behavior'] += '允许一次有目的的专科技能核验后澄清；不强制额外调用，不重复已知缺参的相同请求。'
        # The decision itself (including unknown/no-op/failure reporting) is
        # always required. Do not let a conditional turn disappear from scoring.
        for e in expectations:
            e['criteria'] = [{'id': f"turn-{e['turn']}-decision", 'due_turn': e['turn'],
                              'expected_behavior': e['expected_behavior'],
                              'conditional': False, 'weight': 1}]
        tasks.append({'task_id': task_id, 'group_id': f'business-supplement:20260929:{mechanism}:{instance}',
            'task_input': turns[0]['user'], 'followup_inputs': [t['user'] for t in turns[1:]],
            'expected_behavior': '逐轮满足 turn_expectations；后续纠正只覆盖对应要求，不抹去已发生行为。测试工具失败须如实报告，不能用计划冒充完成。',
            'category': mechanism, 'difficulty': 'medium',
            'initial_state': {'schema': 'business-simulator.initial-state.v1',
                'profile_sha256': next(iter(profile_hashes)), 'precondition': '', 'device_type': '', 'voiceprint_id': ''},
            'budget': {'max_steps': 128*len(turns), 'max_tool_calls': 96*len(turns), 'timeout_ms': 1800000},
            'split': 'train', 'review_status': 'reviewed',
            'provenance': {'kind': 'synthetic', 'learning_exposed': True,
                           'source_fingerprints': [source_fingerprint]},
            'attributes': {'turn_expectations': expectations,
                'supplemental_training': {'mechanism': mechanism, 'instance': instance,
                    'train_reference_ids': REFERENCES[mechanism], 'authoring_basis': 'train-primitives-and-tool-contracts',
                    'use': 'training-only', 'review': 'session-authored-contract-review'}}})
    validate(tasks)
    return tasks


def validate(tasks):
    if len(tasks) != 48 or Counter(t['category'] for t in tasks) != Counter({k: 6 for k in REFERENCES}):
        raise ValueError('Expected eight mechanisms and six conversations each')
    for field in ('task_id', 'group_id'):
        if len({t[field] for t in tasks}) != len(tasks):
            raise ValueError(f'Duplicate {field}')
    inputs = set()
    for task in tasks:
        turns = [task['task_input'], *task['followup_inputs']]
        if not 2 <= len(turns) <= 5 or task['split'] != 'train':
            raise ValueError('Whole 2–5-turn conversations must remain training-only')
        if tuple(turns) in inputs:
            raise ValueError('Duplicate complete conversation')
        inputs.add(tuple(turns))
        expectations = task['attributes']['turn_expectations']
        if len(turns) != len(expectations):
            raise ValueError('Missing per-turn rubric')
        for i, e in enumerate(expectations, 1):
            if e['turn'] != i or len(e['expected_behavior']) < 6 or not isinstance(e['conditional'], bool):
                raise ValueError('Invalid turn rubric')
            for field in ('expected_targets', 'expected_intents', 'expected_effects'):
                if not isinstance(e[field], list) or any(not x for x in e[field]):
                    raise ValueError(f'Unlabeled {field}')
            if set(e['expected_targets']) - set(INTENTS):
                raise ValueError('Unknown target')
        if set(task['initial_state']) != {'schema', 'profile_sha256', 'precondition', 'device_type', 'voiceprint_id'}:
            raise ValueError('Rubrics or unsupported state injected into initial_state')


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--train-source', type=Path, default=HERE/'datasets/source/train.jsonl')
    p.add_argument('--output', type=Path, default=DEFAULT_OUTPUT)
    p.add_argument('--check', action='store_true', help='Validate and compare existing output, without writes')
    p.add_argument('--refresh', action='store_true', help='Explicitly regenerate this authored source during review')
    args = p.parse_args(argv)
    tasks = build_tasks(args.train_source)
    payload = ''.join(json.dumps(t, ensure_ascii=False, separators=(',', ':'))+'\n' for t in tasks)
    if args.check:
        if (args.output/'train.jsonl').read_text() != payload:
            raise SystemExit('Generated source differs from existing train.jsonl')
    else:
        args.output.mkdir(parents=True, exist_ok=True)
        path = args.output/'train.jsonl'
        if path.exists() and path.read_text() != payload and not args.refresh:
            raise SystemExit('Refusing to overwrite a changed source; choose a new --output directory')
        path.write_text(payload)
        (args.output/'source-manifest.json').write_text(json.dumps({
            'name': 'business-supplemental-training-20260929', 'split': 'train', 'tasks': len(tasks),
            'turns': sum(1+len(t['followup_inputs']) for t in tasks),
            'mechanisms': dict(Counter(t['category'] for t in tasks)),
            'source': 'train.jsonl', 'original_train_tasks': 400,
            'evaluation_inputs_read_by_generator': False,
            'design': 'Six separately authored state/dependency/branch instances per mechanism; not a statistical independence claim.',
            'capability_limits': 'Fixed business test environment only; no claim of real taxi, external routes or physical devices.',
            'generator': '../../generate_supplemental_training.py'}, ensure_ascii=False, indent=2)+'\n')
        # `golden` here selects the native local-file reader; Builder::add keeps
        # nonempty task.provenance.kind='synthetic'. Native group finalization
        # forces all learning_exposed/requested-train groups to train. Positive
        # split percentages are mandatory config syntax and are never used here.
        (args.output/'build-config.json').write_text(json.dumps({
            'schema_version': 1, 'name': 'business-supplemental-training-20260929',
            'split_seed': 'supplemental-training-only-20260929', 'split_percent': [80, 10, 10],
            'max_tasks': 48, 'max_model_calls': 0,
            'budget': {'max_steps': 640, 'max_tool_calls': 480, 'timeout_ms': 1800000},
            'learning_groups': [t['group_id'] for t in tasks],
            'sources': [{'kind': 'golden', 'path': 'train.jsonl'}]}, ensure_ascii=False, indent=2)+'\n')
    print(json.dumps({'tasks': len(tasks), 'turns': sum(1+len(t['followup_inputs']) for t in tasks),
                      'mechanisms': dict(Counter(t['category'] for t in tasks)),
                      'bytes': len(payload.encode()), 'output': str(args.output)}, ensure_ascii=False))


if __name__ == '__main__':
    main()
