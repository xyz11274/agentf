#!/usr/bin/env python3
"""Read-only dataset comparison; writes independent audit artifacts, never model input."""
from __future__ import annotations
import argparse
from collections import Counter,defaultdict
import json,math,re,unicodedata
from pathlib import Path

ROOT=Path(__file__).resolve().parents[4]
DATA=Path(__file__).resolve().parent/'datasets'
AUDIT=ROOT/'trace_output/business-simulator/audit'
STAMP='20260929'

# Independently reviewed source-task comparisons; no evaluation scores used.
MANUAL_REVIEWS={'QA_GUI_YDYL_008': {'training_ids': ['TGJK_YD2_012'], 'judgment': 'shared_core_mechanism_found', 'shared': '薛定谔解释与爱奇艺视频搜索、并行QA+GUI及后续问答', 'difference': '训练额外含腹痛分支；评测新增第4/5轮知识追问，非仅凭操作集合可认定新协调机制'}, 'SYS_DYT_03_143': {'training_ids': ['SYS_DYT_03_241'], 'judgment': 'shared_core_mechanism_found', 'shared': '失眠咨询后，只有医生建议用药才创建提醒的条件链', 'difference': '独立第三分支由挂号换成家电习惯；该变化不能证明核心条件机制未见'}, 'TGJK_WYD_001': {'training_ids': ['QA_GUI_WYYL_002'], 'judgment': 'shared_core_mechanism_found', 'shared': '北京天气、话费、穿衣/带伞后续问答与并行分支', 'difference': '评测额外挂号查询支路，不能单凭整题操作集合差异判新依赖机制'}, 'SYS_DYT_03_101': {'training_ids': ['SYS_DYT_02_023'], 'judgment': 'shared_core_mechanism_found', 'shared': '挂号后查询挂号记录的先后顺序', 'difference': '评测另含健康档案查询、科室实体不同；实体变化不构成机制证据'}, 'ZJ_GY_KSP_01_021': {'training_ids': ['CK_GUI_FHCK_00_126', 'QA_GY_HHYL_005', 'QA_GY_WYYL_010'], 'judgment': 'weak_joint_mechanism_support_needs_confirmation', 'shared': '分别存在GUI历史列表第一项、电影推荐跨轮指代、按明确片名播放的原语', 'difference': '尚未找到movie-query推荐列表→第二项→播放器ID的完整训练链；不等于没学过列表指代'}, 'ZJ_GY_KSP_01_027': {'training_ids': ['CK_GUI_FHCK_00_126', 'QA_GY_HHYL_005', 'QA_GY_WYYL_010'], 'judgment': 'weak_joint_mechanism_support_needs_confirmation', 'shared': '与021同输入/同机制；训练分别含列表指代、推荐和播放原语', 'difference': '完整推荐列表→第二项→播放器ID链支持薄弱；与021不能算两个独立机制'}, 'QA_JK_HHYL_004': {'training_ids': ['SYS_DYT_03_012', 'DH_YT_CYFF_01_009'], 'judgment': 'weak_joint_mechanism_support_needs_confirmation', 'shared': '训练含档案生成+设提醒（并列）以及物品列表→备忘录与后续问答', 'difference': '评测要求新写入档案→读取医嘱药名/剂量/周期→生成提醒的依赖；当前相似训练不足以证明整链已见'}, 'SYS_DYT_02_086': {'training_ids': ['SYS_DYT_02_081', 'SYS_DYT_03_200'], 'judgment': 'shared_core_mechanism_found', 'shared': '两个独立只读查询并行；训练分别出现提醒列表+习惯查询、档案+天气+习惯查询。', 'difference': '将独立查询支路重新配对，没有跨结果参数传递或条件门。', 'annotation_warnings': [], 'limitations': '本会话定向语义阅读完整用户请求与后续轮次的控制/状态依赖；有机制对应不证明已学到可用经验、触发成功或整题通过。并列任务重组不要求训练有精确操作码组合；同资源读写顺序不自动等价。'}, 'SYS_DYT_03_065': {'training_ids': ['SYS_DYT_02_033', 'SYS_DYT_03_059'], 'judgment': 'shared_core_mechanism_found', 'shared': '提醒创建+档案查询、提醒创建+家电习惯查询均在训练联合出现。', 'difference': '第三个独立支路重新组合；没有要求用档案或习惯结果生成提醒。', 'annotation_warnings': ['存在仅标点的expected_behavior，动作要求主要依赖用户请求和操作码；本判读不证明验收标签充分。'], 'limitations': '本会话定向语义阅读完整用户请求与后续轮次的控制/状态依赖；有机制对应不证明已学到可用经验、触发成功或整题通过。并列任务重组不要求训练有精确操作码组合；同资源读写顺序不自动等价。'}, 'SYS_DYT_03_066': {'training_ids': ['SYS_DYT_03_070', 'SYS_DYT_03_037'], 'judgment': 'shared_core_mechanism_found', 'shared': '训练03_070已有创建提醒后列出提醒，03_037已有提醒+电影推荐。', 'difference': '将独立的导诊/档案支路换为电影推荐，核心提醒写后读已有对应。', 'annotation_warnings': ['存在仅标点的expected_behavior，动作要求主要依赖用户请求和操作码；本判读不证明验收标签充分。'], 'limitations': '本会话定向语义阅读完整用户请求与后续轮次的控制/状态依赖；有机制对应不证明已学到可用经验、触发成功或整题通过。并列任务重组不要求训练有精确操作码组合；同资源读写顺序不自动等价。'}, 'SYS_DYT_03_080': {'training_ids': ['SYS_DYT_03_167', 'SYS_DYT_03_111'], 'judgment': 'shared_core_mechanism_found', 'shared': '03_167明确分别查询出门、回家习惯；03_111已有习惯查询+档案生成。', 'difference': '双习惯查询的参数分组已有对应，独立第三支从挂号换档案，不应按100053重复次数判新机制。', 'annotation_warnings': ['存在仅标点的expected_behavior，动作要求主要依赖用户请求和操作码；本判读不证明验收标签充分。'], 'limitations': '本会话定向语义阅读完整用户请求与后续轮次的控制/状态依赖；有机制对应不证明已学到可用经验、触发成功或整题通过。并列任务重组不要求训练有精确操作码组合；同资源读写顺序不自动等价。'}, 'SYS_DYT_03_083': {'training_ids': ['SYS_DYT_02_040', 'SYS_DYT_03_012'], 'judgment': 'shared_core_mechanism_found', 'shared': '02_040已有先读旧档案再生成新档案；03_012已有档案生成+提醒。', 'difference': '增加独立提醒支路；用户没有声明依据新档案医嘱填提醒参数。', 'annotation_warnings': ['存在仅标点的expected_behavior，动作要求主要依赖用户请求和操作码；本判读不证明验收标签充分。'], 'limitations': '本会话定向语义阅读完整用户请求与后续轮次的控制/状态依赖；有机制对应不证明已学到可用经验、触发成功或整题通过。并列任务重组不要求训练有精确操作码组合；同资源读写顺序不自动等价。'}, 'SYS_DYT_03_084': {'training_ids': ['SYS_DYT_03_087', 'SYS_DYT_03_092'], 'judgment': 'shared_core_mechanism_found', 'shared': '03_087已有普通提醒再另加晚上提醒；03_092已有电影推荐+提醒。', 'difference': '把独立空调支路替换为电影推荐；保留两个提醒需求而非误去重，是已有训练机制。', 'annotation_warnings': ['存在仅标点的expected_behavior，动作要求主要依赖用户请求和操作码；本判读不证明验收标签充分。'], 'limitations': '本会话定向语义阅读完整用户请求与后续轮次的控制/状态依赖；有机制对应不证明已学到可用经验、触发成功或整题通过。并列任务重组不要求训练有精确操作码组合；同资源读写顺序不自动等价。'}, 'SYS_DYT_03_088': {'training_ids': ['SYS_DYT_03_044', 'SYS_DYT_03_006'], 'judgment': 'shared_core_mechanism_found', 'shared': '03_044已有查旧挂号记录后预约新号，03_006有预约+提醒。', 'difference': '新请求将设提醒放在预约后；顺序组合有支撑，但未声明提醒参数必须来自预约结果。', 'annotation_warnings': ['存在仅标点的expected_behavior，动作要求主要依赖用户请求和操作码；本判读不证明验收标签充分。'], 'limitations': '本会话定向语义阅读完整用户请求与后续轮次的控制/状态依赖；有机制对应不证明已学到可用经验、触发成功或整题通过。并列任务重组不要求训练有精确操作码组合；同资源读写顺序不自动等价。'}, 'SYS_DYT_03_100': {'training_ids': ['SYS_DYT_02_057', 'SYS_DYT_03_271', 'SYS_DYT_02_011'], 'judgment': 'shared_core_mechanism_found', 'shared': '训练覆盖咨询后按推荐科室挂号、导诊+档案、挂号+档案。', 'difference': '本题医院科室已明确，三项未指定数据传递；失眠/医院实体替换不是机制缺口。', 'annotation_warnings': ['存在仅标点的expected_behavior，动作要求主要依赖用户请求和操作码；本判读不证明验收标签充分。'], 'limitations': '本会话定向语义阅读完整用户请求与后续轮次的控制/状态依赖；有机制对应不证明已学到可用经验、触发成功或整题通过。并列任务重组不要求训练有精确操作码组合；同资源读写顺序不自动等价。'}, 'SYS_DYT_03_112': {'training_ids': ['SYS_DYT_02_056', 'SYS_DYT_03_034'], 'judgment': 'shared_core_mechanism_found', 'shared': '02_056有关灯后档案生成，03_034有档案生成+档案查询。', 'difference': '新增体检历史查询支路；需按用户限定查旧报告，不能把刚生成档案当旧报告，但同资源读写已有训练对应。', 'annotation_warnings': ['存在仅标点的expected_behavior，动作要求主要依赖用户请求和操作码；本判读不证明验收标签充分。'], 'limitations': '本会话定向语义阅读完整用户请求与后续轮次的控制/状态依赖；有机制对应不证明已学到可用经验、触发成功或整题通过。并列任务重组不要求训练有精确操作码组合；同资源读写顺序不自动等价。'}, 'SYS_DYT_03_164': {'training_ids': ['SYS_DYT_03_181', 'SYS_DYT_02_040'], 'judgment': 'shared_core_mechanism_found', 'shared': '电影推荐+档案查询及先查旧档案再生成新档案均已有联合训练。', 'difference': '合并两个已有关系；用户未引入电影结果驱动档案操作的条件或数据依赖。', 'annotation_warnings': ['存在仅标点的expected_behavior，动作要求主要依赖用户请求和操作码；本判读不证明验收标签充分。'], 'limitations': '本会话定向语义阅读完整用户请求与后续轮次的控制/状态依赖；有机制对应不证明已学到可用经验、触发成功或整题通过。并列任务重组不要求训练有精确操作码组合；同资源读写顺序不自动等价。'}, 'SYS_DYT_03_169': {'training_ids': ['SYS_DYT_03_087', 'SYS_DYT_03_073'], 'judgment': 'shared_core_mechanism_found', 'shared': '训练已有空调打开+设提醒，以及设提醒+查挂号。', 'difference': '三个独立支路组合，未引入新的先决条件。', 'annotation_warnings': ['存在仅标点的expected_behavior，动作要求主要依赖用户请求和操作码；本判读不证明验收标签充分。'], 'limitations': '本会话定向语义阅读完整用户请求与后续轮次的控制/状态依赖；有机制对应不证明已学到可用经验、触发成功或整题通过。并列任务重组不要求训练有精确操作码组合；同资源读写顺序不自动等价。'}, 'SYS_DYT_03_172': {'training_ids': ['SYS_DYT_02_046', 'SYS_DYT_02_055', 'SYS_DYT_03_181'], 'judgment': 'shared_core_mechanism_found', 'shared': '档案查询+挂号、推荐电影+挂号、推荐电影+档案查询三种配对均在训练出现。', 'difference': '三项并列，未声明跨结果依赖；悬疑片/医院实体不构成新机制。', 'annotation_warnings': ['存在仅标点的expected_behavior，动作要求主要依赖用户请求和操作码；本判读不证明验收标签充分。'], 'limitations': '本会话定向语义阅读完整用户请求与后续轮次的控制/状态依赖；有机制对应不证明已学到可用经验、触发成功或整题通过。并列任务重组不要求训练有精确操作码组合；同资源读写顺序不自动等价。'}, 'SYS_DYT_03_176': {'training_ids': ['SYS_DYT_03_185', 'SYS_DYT_03_162', 'SYS_DYT_02_057'], 'judgment': 'shared_core_mechanism_found', 'shared': '训练已有查挂号记录+咨询，以及咨询/查询医院后挂指定科室，甚至根据推荐科室挂号。', 'difference': '本题额外查询旧记录且科室已给定；不比现有导诊到预约链引入新的参数依赖。', 'annotation_warnings': ['存在仅标点的expected_behavior，动作要求主要依赖用户请求和操作码；本判读不证明验收标签充分。'], 'limitations': '本会话定向语义阅读完整用户请求与后续轮次的控制/状态依赖；有机制对应不证明已学到可用经验、触发成功或整题通过。并列任务重组不要求训练有精确操作码组合；同资源读写顺序不自动等价。'}, 'SYS_DYT_03_178': {'training_ids': ['SYS_DYT_03_093', 'SYS_DYT_03_070', 'SYS_DYT_02_033'], 'judgment': 'weak_joint_mechanism_support_needs_confirmation', 'shared': '03_093有先查提醒再调整；03_070有新建提醒后查询；02_033有提醒与档案查询。', 'difference': '评测是先查已有提醒、再新建另一提醒，同时读档案；训练分别覆盖读后更新和新建后读，未确认先读后新增且保留已有项的整链，不能把反向顺序当等价。', 'annotation_warnings': ['存在仅标点的expected_behavior，动作要求主要依赖用户请求和操作码；本判读不证明验收标签充分。'], 'limitations': '本会话定向语义阅读完整用户请求与后续轮次的控制/状态依赖；有机制对应不证明已学到可用经验、触发成功或整题通过。并列任务重组不要求训练有精确操作码组合；同资源读写顺序不自动等价。'}, 'TGJK_WYD_006': {'training_ids': ['QA_GY_WYYL_003', 'CK_GUI_FHCK_00_109', 'QA_JK_WYYL_012'], 'judgment': 'shared_core_mechanism_found', 'shared': '硬币问题及所有后续推理轮在QA_GY_WYYL_003已出现；当前歌曲收藏请求在CK_GUI_FHCK_00_109逐字出现；提醒查询有独立训练。', 'difference': '把电影推荐替换为GUI收藏与提醒查询两个独立支路，主QA上下文机制相同；并行三分支本身已有训练，但此完整组合未出现。', 'annotation_warnings': ['第三轮硬币推理的如果被标conditional=true，不等同于工具执行条件门。'], 'limitations': '本会话定向语义阅读完整用户请求与后续轮次的控制/状态依赖；有机制对应不证明已学到可用经验、触发成功或整题通过。并列任务重组不要求训练有精确操作码组合；同资源读写顺序不自动等价。'}, 'SYS_DYT_03_189': {'training_ids': ['SYS_DYT_03_218', 'SYS_DYT_03_120', 'SYS_DYT_02_055'], 'judgment': 'shared_core_mechanism_found', 'shared': '训练包含窗帘/灯操作+导诊，导诊+电影推荐，以及多项并列派发。', 'difference': '本题三个独立任务；训练部分带较强顺序，评测不要求从导诊结果决定是否关窗帘/推荐电影。', 'annotation_warnings': ['存在仅标点的expected_behavior，动作要求主要依赖用户请求和操作码；本判读不证明验收标签充分。'], 'limitations': '本会话定向语义阅读完整用户请求与后续轮次的控制/状态依赖；有机制对应不证明已学到可用经验、触发成功或整题通过。并列任务重组不要求训练有精确操作码组合；同资源读写顺序不自动等价。'}, 'SYS_DYT_02_058': {'training_ids': ['SYS_DYT_03_317', 'SYS_DYT_03_337'], 'judgment': 'uncertain', 'shared': '03_317有电影推荐成功后查提醒；03_337有养生内容建议后建提醒；条件控制与动作原语均有训练。', 'difference': '评测具体门是有不错影片才创建提醒；两个训练的门与动作配对不同，且本题conditional=false、expected_behavior仅分号，无法确认评分器是否按真实条件分支验收。', 'annotation_warnings': ['存在仅标点的expected_behavior，动作要求主要依赖用户请求和操作码；本判读不证明验收标签充分。', '用户表达条件，但conditional=false；相关训练条件标签也有缺失，必须独立核验验收。'], 'limitations': '本会话定向语义阅读完整用户请求与后续轮次的控制/状态依赖；有机制对应不证明已学到可用经验、触发成功或整题通过。并列任务重组不要求训练有精确操作码组合；同资源读写顺序不自动等价。'}, 'SYS_DYT_03_023': {'training_ids': ['SYS_DYT_02_008', 'SYS_DYT_03_012', 'SYS_DYT_02_025'], 'judgment': 'shared_core_mechanism_found', 'shared': '训练已有档案生成+提醒，以及提醒+导诊的联合任务。', 'difference': '用户说先档案后提醒，但未声明医嘱数据传递；泛化顺序与并列咨询已有训练，不能夸大为需要抽取新档案药品的机制。', 'annotation_warnings': ['存在仅标点的expected_behavior，动作要求主要依赖用户请求和操作码；本判读不证明验收标签充分。'], 'limitations': '本会话定向语义阅读完整用户请求与后续轮次的控制/状态依赖；有机制对应不证明已学到可用经验、触发成功或整题通过。并列任务重组不要求训练有精确操作码组合；同资源读写顺序不自动等价。'}, 'SYS_DYT_03_030': {'training_ids': ['SYS_DYT_02_011', 'SYS_DYT_02_056', 'SYS_DYT_03_265'], 'judgment': 'shared_core_mechanism_found', 'shared': '训练有预约+建档、家居操控+建档，且相同医院科室+空调已出现。', 'difference': '去掉训练中条件支路后为三项并列；温度/设备位置是参数替换。', 'annotation_warnings': ['存在仅标点的expected_behavior，动作要求主要依赖用户请求和操作码；本判读不证明验收标签充分。'], 'limitations': '本会话定向语义阅读完整用户请求与后续轮次的控制/状态依赖；有机制对应不证明已学到可用经验、触发成功或整题通过。并列任务重组不要求训练有精确操作码组合；同资源读写顺序不自动等价。'}, 'SYS_DYT_03_048': {'training_ids': ['SYS_DYT_02_033', 'SYS_DYT_02_046', 'SYS_DYT_03_053'], 'judgment': 'shared_core_mechanism_found', 'shared': '提醒+档案查询、档案查询+挂号、提醒+挂号均有配对支持。', 'difference': '三项并列而无新状态依赖，科室变化不影响核心派发机制。', 'annotation_warnings': ['存在仅标点的expected_behavior，动作要求主要依赖用户请求和操作码；本判读不证明验收标签充分。'], 'limitations': '本会话定向语义阅读完整用户请求与后续轮次的控制/状态依赖；有机制对应不证明已学到可用经验、触发成功或整题通过。并列任务重组不要求训练有精确操作码组合；同资源读写顺序不自动等价。'}, 'SYS_DYT_03_051': {'training_ids': ['SYS_DYT_03_012', 'SYS_DYT_03_111', 'SYS_DYT_03_059'], 'judgment': 'shared_core_mechanism_found', 'shared': '建档+提醒、习惯查询+建档、提醒+习惯查询均在训练联合出现。', 'difference': '三项无条件并列；没有要求习惯结果或建档结果成为提醒参数。', 'annotation_warnings': ['存在仅标点的expected_behavior，动作要求主要依赖用户请求和操作码；本判读不证明验收标签充分。'], 'limitations': '本会话定向语义阅读完整用户请求与后续轮次的控制/状态依赖；有机制对应不证明已学到可用经验、触发成功或整题通过。并列任务重组不要求训练有精确操作码组合；同资源读写顺序不自动等价。'}, 'SYS_DYT_03_057': {'training_ids': ['SYS_DYT_03_012', 'SYS_DYT_03_271', 'SYS_DYT_02_025'], 'judgment': 'shared_core_mechanism_found', 'shared': '建档+提醒、建档+导诊、提醒+导诊均已有训练。', 'difference': '嗓子干与既有症状只是导诊输入变化，本题没有医生建议用药才设提醒的条件。', 'annotation_warnings': ['存在仅标点的expected_behavior，动作要求主要依赖用户请求和操作码；本判读不证明验收标签充分。'], 'limitations': '本会话定向语义阅读完整用户请求与后续轮次的控制/状态依赖；有机制对应不证明已学到可用经验、触发成功或整题通过。并列任务重组不要求训练有精确操作码组合；同资源读写顺序不自动等价。'}, 'SYS_DYT_03_098': {'training_ids': ['SYS_DYT_02_040', 'SYS_DYT_03_081', 'SYS_DYT_02_046'], 'judgment': 'shared_core_mechanism_found', 'shared': '02_040有读旧档案再生成新档案；03_081有挂号+建档；02_046有档案查询+挂号。', 'difference': '插入独立预约任务，旧档案读取和建档关系已有对应；最新就诊信息不等于尚未发生的预约就诊。', 'annotation_warnings': ['存在仅标点的expected_behavior，动作要求主要依赖用户请求和操作码；本判读不证明验收标签充分。'], 'limitations': '本会话定向语义阅读完整用户请求与后续轮次的控制/状态依赖；有机制对应不证明已学到可用经验、触发成功或整题通过。并列任务重组不要求训练有精确操作码组合；同资源读写顺序不自动等价。'}, 'SYS_DYT_03_103': {'training_ids': ['SYS_DYT_03_092', 'SYS_DYT_03_203', 'SYS_DYT_03_053'], 'judgment': 'shared_core_mechanism_found', 'shared': '电影推荐+提醒、云手机功能问答+提醒、三分支派发均有对应。', 'difference': '独立知识库问答替换原独立挂号/搜索，未引入条件。', 'annotation_warnings': [], 'limitations': '本会话定向语义阅读完整用户请求与后续轮次的控制/状态依赖；有机制对应不证明已学到可用经验、触发成功或整题通过。并列任务重组不要求训练有精确操作码组合；同资源读写顺序不自动等价。'}, 'SYS_DYT_03_105': {'training_ids': ['SYS_DYT_03_044', 'SYS_DYT_03_058', 'SYS_DYT_03_071'], 'judgment': 'shared_core_mechanism_found', 'shared': '训练有查旧挂号记录后挂号，以及挂号+提醒列表查询。', 'difference': '把独立音乐/电影支路换提醒查询；医院和科室还有几乎逐字训练支持。', 'annotation_warnings': ['存在仅标点的expected_behavior，动作要求主要依赖用户请求和操作码；本判读不证明验收标签充分。'], 'limitations': '本会话定向语义阅读完整用户请求与后续轮次的控制/状态依赖；有机制对应不证明已学到可用经验、触发成功或整题通过。并列任务重组不要求训练有精确操作码组合；同资源读写顺序不自动等价。'}, 'SYS_DYT_03_109': {'training_ids': ['SYS_DYT_02_090', 'SYS_DYT_03_167', 'SYS_DYT_02_055'], 'judgment': 'shared_core_mechanism_found', 'shared': '习惯查询+推荐电影、习惯查询+挂号、电影推荐+挂号均有联合训练。', 'difference': '三个独立支路组合；无查询结果约束挂号参数。', 'annotation_warnings': ['存在仅标点的expected_behavior，动作要求主要依赖用户请求和操作码；本判读不证明验收标签充分。'], 'limitations': '本会话定向语义阅读完整用户请求与后续轮次的控制/状态依赖；有机制对应不证明已学到可用经验、触发成功或整题通过。并列任务重组不要求训练有精确操作码组合；同资源读写顺序不自动等价。'}, 'SYS_DYT_03_170': {'training_ids': ['SYS_DYT_03_199', 'SYS_DYT_03_087', 'SYS_DYT_02_049'], 'judgment': 'shared_core_mechanism_found', 'shared': '训练有习惯查询+家居控制、家居控制+提醒创建、习惯查询+提醒创建。', 'difference': '独立提醒从查询换创建，窗帘替换空调；没有条件或参数传递。', 'annotation_warnings': ['存在仅标点的expected_behavior，动作要求主要依赖用户请求和操作码；本判读不证明验收标签充分。'], 'limitations': '本会话定向语义阅读完整用户请求与后续轮次的控制/状态依赖；有机制对应不证明已学到可用经验、触发成功或整题通过。并列任务重组不要求训练有精确操作码组合；同资源读写顺序不自动等价。'}, 'SYS_DYT_03_187': {'training_ids': ['SYS_DYT_03_218', 'SYS_DYT_03_120', 'SYS_DYT_03_152'], 'judgment': 'shared_core_mechanism_found', 'shared': '家居调暗+失眠咨询与健康咨询+推荐电影有对应。', 'difference': '评测三个并列支路不依赖咨询成功，训练中部分带顺序/条件但基本多意图机制已有。', 'annotation_warnings': ['存在仅标点的expected_behavior，动作要求主要依赖用户请求和操作码；本判读不证明验收标签充分。'], 'limitations': '本会话定向语义阅读完整用户请求与后续轮次的控制/状态依赖；有机制对应不证明已学到可用经验、触发成功或整题通过。并列任务重组不要求训练有精确操作码组合；同资源读写顺序不自动等价。'}, 'SYS_DYT_03_188': {'training_ids': ['SYS_DYT_03_200', 'SYS_DYT_02_081', 'QA_JK_WYYL_012', 'SYS_DYT_03_299'], 'judgment': 'shared_core_mechanism_found', 'shared': '档案+其他查询、提醒+其他查询、提醒+联网搜索、附近中医院搜索均有训练。', 'difference': '独立只读查询组合，没有从档案或提醒内容决定医院检索条件；不能仅因档案与提醒没同题出现判全无机制。', 'annotation_warnings': [], 'limitations': '本会话定向语义阅读完整用户请求与后续轮次的控制/状态依赖；有机制对应不证明已学到可用经验、触发成功或整题通过。并列任务重组不要求训练有精确操作码组合；同资源读写顺序不自动等价。'}, 'TGJK_WYD_011': {'training_ids': ['TGJK_WYD_005', 'TGZS_WYYL_003', 'QA_JK_WYYL_012'], 'judgment': 'shared_core_mechanism_found', 'shared': 'TGJK_WYD_005已有QA连续对话+酷狗签到+健康提醒三分支；TGZS_WYYL_003有碳中和与碳达峰两轮；提醒查询有单独训练。', 'difference': 'QA主题替换、健康动作从创建换查询；主对话连续性与并行GUI/健康分支机制已有。', 'annotation_warnings': [], 'limitations': '本会话定向语义阅读完整用户请求与后续轮次的控制/状态依赖；有机制对应不证明已学到可用经验、触发成功或整题通过。并列任务重组不要求训练有精确操作码组合；同资源读写顺序不自动等价。'}}

def load_rows(path):
    return [json.loads(line) for line in path.read_text().splitlines() if line.strip()]

def utterance(task):
    return '\n'.join([task['task_input']]+task.get('followup_inputs',[]))

def excerpt(task,n=125):
    s=utterance(task).replace('\n',' / ')
    return s if len(s)<=n else s[:n]+'…'

def turns(task):
    return task['attributes'].get('turn_expectations',[])

def union(task,key):
    return tuple(sorted({x for t in turns(task) for x in (t.get(key) or [])}))

def slots(task):
    return tuple(task['attributes']['product_source'].get('scenarios',[]))

def family(task):
    return 'SYS_DYT_slots' if task['task_id'].startswith('SYS_DYT_') else 'other_source_taxonomy'

def taxonomy(task):
    s=slots(task)
    return s if family(task)=='other_source_taxonomy' and len(s)>=3 and all(x!='无' for x in s) else None

def dep(task):
    if taxonomy(task):
        for x in slots(task):
            if x in {'意图依赖','无意图依赖','混合依赖','依赖、转折多意图'}:return x
    return None

def meaningful(s):
    return any(x.isalnum() for x in str(s or ''))

def grams(task):
    s=re.sub(r'[^\w]+','',unicodedata.normalize('NFKC',utterance(task)).lower())
    return Counter(s[i:i+n] for n in (2,3,4) for i in range(len(s)-n+1))

def jac(a,b):
    return len(set(a)&set(b))/len(set(a)|set(b)) if a and b else 0.

def contrast(a,b):
    return {'same':sorted(set(a)&set(b)),'missing':sorted(set(a)-set(b)),'extra':sorted(set(b)-set(a))} if a and b else {'unavailable':True}

def build():
    view=json.loads((DATA/'gepa-view.json').read_text())
    train=load_rows(DATA/'training-reviewed/train.jsonl')
    val={t['task_id']:t for t in load_rows(DATA/'source/val.jsonl')}
    groups={'search150':[val[x] for x in view['search_task_ids']], 'selection50':[val[x] for x in view['selection_task_ids']], 'holdout200':load_rows(DATA/'source/holdout.jsonl')}
    assert len(train)==400 and [len(x) for x in groups.values()]==[150,50,200]
    ef=[union(t,'expected_effects') for t in train];tg=[union(t,'expected_targets') for t in train]
    effects_count=Counter(e for e in ef if e); target_count=Counter(e for e in tg if e)
    td=Counter((taxonomy(t),tg[i]) for i,t in enumerate(train) if taxonomy(t) and tg[i])
    dd=Counter((dep(t),tg[i]) for i,t in enumerate(train) if dep(t) and tg[i])
    terms=[grams(t) for t in train];df=Counter(g for c in terms for g in c);idf={g:math.log(401/(n+1))+1 for g,n in df.items()}
    def vector(c):
        w={g:n*idf[g] for g,n in c.items() if g in idf};norm=math.sqrt(sum(x*x for x in w.values()))
        return {g:x/norm for g,x in w.items()} if norm else {}
    inv=defaultdict(list)
    for i,c in enumerate(terms):
        for g,w in vector(c).items():inv[g].append((i,w))
    allrows=[];summary={}
    for split,tasks in groups.items():
        summary[split]=Counter()
        for t in tasks:
            effects=union(t,'expected_effects');targets=union(t,'expected_targets');intents=union(t,'expected_intents');sc=taxonomy(t)
            support=effects_count[effects] if effects else None
            state='annotation_unknown' if support is None else 'operation_combination_unseen' if support==0 else 'operation_combination_sparse' if support<5 else 'operation_combination_seen'
            label_unknown=not targets or not intents
            sparse_behavior=not any(meaningful(z.get('expected_behavior')) for z in turns(t))
            summary[split][state]+=1;summary[split]['total']+=1
            if label_unknown:summary[split]['targets_intents_unknown']+=1
            if not effects:summary[split]['effects_empty_targets_unknown' if label_unknown else 'effects_empty_targets_known']+=1
            if sparse_behavior:summary[split]['behavior_all_punctuation_or_empty']+=1
            taxonomy_support=td[(sc,targets)] if sc and targets else None
            dependency_support=dd[(dep(t),targets)] if dep(t) and targets else None
            if taxonomy_support is not None:
                summary[split]['taxonomy_target_eligible']+=1
                summary[split]['taxonomy_target_seen' if taxonomy_support else 'taxonomy_target_unseen']+=1
            scores=defaultdict(float)
            for g,w in vector(grams(t)).items():
                for i,tw in inv.get(g,[]):scores[i]+=w*tw
            def rank(i):
                # Explicit weighted proxy similarity, never an inferred mechanism label.
                components=[(.25,scores[i]),(.10,float(t['category']==train[i]['category']))]
                if effects:components.append((.30,jac(effects,ef[i])))
                if targets:components.append((.20,jac(targets,tg[i])))
                if sc:components.append((.15,float(sc==taxonomy(train[i]))))
                return sum(w*s for w,s in components)/sum(w for w,s in components)
            ranked=sorted(range(400),key=lambda i:(-rank(i),-scores[i],train[i]['task_id']))
            manual=MANUAL_REVIEWS.get(t['task_id'])
            manual_indices=[next(i for i,q in enumerate(train) if q['task_id']==task_id) for task_id in manual['training_ids']] if manual else []
            indices=(manual_indices+[i for i in ranked if i not in manual_indices])[:3]
            candidates=[]
            for i in indices:
                q=train[i]
                candidates.append({'id':q['task_id'],'manual_reference':i in manual_indices,'request':excerpt(q,105),'proxy_score':round(rank(i),4),'text_cosine':round(scores[i],4),'effects':contrast(effects,ef[i]),'targets':contrast(targets,tg[i]),'same_category':t['category']==q['category'],'source_labels':{'same':sorted((set(slots(t))&set(slots(q)))-{'无'}),'missing':sorted((set(slots(t))-set(slots(q)))-{'无'}),'same_kind':family(t)==family(q)}})
            reasons=[]
            if support is None:reasons.append('操作码未标注，无法判断该维度；不等于无需动作')
            elif not support:reasons.append('非空操作码精确集合在train400未见；各单码已见，尚不能判定机制缺失')
            elif support<5:reasons.append(f'相同操作码集合仅{support}题训练支持；不是机制支持数')
            else:reasons.append(f'相同操作码集合有{support}题训练支持；依赖/前提机制仍待确认')
            if label_unknown:reasons.append('targets/intents缺标注，不计为无调用或已覆盖')
            if sparse_behavior:reasons.append('预期行为仅标点或为空，需核查原表语义')
            if taxonomy_support is not None:reasons.append(f'非SYS来源同targets+同原始taxonomy元组{taxonomy_support}题；仅标签证据')
            allrows.append({'split':split,'task_id':t['task_id'],'request':excerpt(t),'category':t['category'],'effects':list(effects) if effects else None,'targets':list(targets) if targets else None,'intents':list(intents) if intents else None,'source_labels':list(slots(t)),'source_label_kind':family(t),'turns':1+len(t.get('followup_inputs',[])),'annotation_unknown':{'targets_intents':label_unknown,'effects':not bool(effects),'behavior':sparse_behavior,'some_turn_targets_missing':any(not z.get('expected_targets') for z in turns(t))},'train_support':{'effects_exact_set':support,'targets_exact_set':target_count[targets] if targets else None,'non_sys_taxonomy_and_targets':taxonomy_support,'non_sys_dependency_label_and_targets':dependency_support},'classification':state,'mechanism_status':manual['judgment'] if manual else 'not_semantically_reviewed','evidence_level':'A_literal_fields_B_direct_semantic_comparison' if manual else 'A_literal_fields_B_candidate_analogy_U_mechanism','reasons':reasons,'nearest_training':candidates,**({'manual_review':manual} if manual else {})})
    return allrows,summary

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output-dir',type=Path,default=AUDIT)
    parser.add_argument('--review-file',type=Path,action='append',default=[],help='Merge completed semantic reviews; never generate model feedback.')
    args=parser.parse_args()
    existing=args.output_dir/f'evaluation-training-correspondence-{STAMP}.jsonl'
    if existing.exists():
        for record in load_rows(existing):
            if record.get('manual_review'):
                MANUAL_REVIEWS[record['task_id']]=record['manual_review']
    for review_file in args.review_file:
        for review in json.loads(review_file.read_text()):
            review=dict(review)
            task_id=review.pop('task_id')
            MANUAL_REVIEWS[task_id]=review
    rows,summary=build();assert len(rows)==400 and len({r['task_id'] for r in rows})==400
    args.output_dir.mkdir(parents=True,exist_ok=True);base=args.output_dir/f'evaluation-training-correspondence-{STAMP}'
    path=base.with_suffix('.jsonl');path.write_text(''.join(json.dumps(r,ensure_ascii=False,separators=(',',':'))+'\n' for r in rows))
    lines=['# 业务评测逐题训练对应审计（2026-09-29）','','完整检查search150、selection50、holdout200，共400题；只读原始题目与标注，对照train400。未用模型、评测成绩或反思结果，未改数据/分片/运行进程。','',f'逐题清单：`{path.name}`。每题含请求摘要、标签、支持数、3个训练候选及共有/缺失操作码和目标。','','## 可确认的字段事实','','|范围|操作组合未见|仅1–4支持|≥5支持|操作码缺标注|targets/intents缺标注|行为仅标点|','|---|---:|---:|---:|---:|---:|---:|']
    for name,s in summary.items():lines.append(f"|{name}|{s['operation_combination_unseen']}|{s['operation_combination_sparse']}|{s['operation_combination_seen']}|{s['annotation_unknown']}|{s['targets_intents_unknown']}|{s['behavior_all_punctuation_or_empty']}|")
    lines+=['','“操作组合未见”是非空expected_effects集合的精确比较；忽略顺序，仅能说明验收操作码组合没在训练整题出现。所有评测单个操作码在训练均出现。**37题操作组合差异不等于37题机制未见。**这37题已逐题语义复核（由本会话完成）：32题找到共享核心机制，4题联合链条支持薄弱待确认，1题因条件/验收标注问题仍不确定；这不等于32题已学会或会通过。另82题无effects记录，其中59题仍有targets/intents，只有23题两者均无；未知字段逐项列出，不能将82题统称完全无标签。它不是“无机制对应”结论。空标签单独计数，不能当无调用或已覆盖。','','## 非SYS来源标签核验','','原转换器仅复制S1–S4列；SYS_DYT通常为多意图槽位，不能当统一层级。本报告只对其他来源且原始标签完整、targets非空的题，核对“同原始标签元组+同targets”；依赖标签另列。仍不等于标准机制簇。','','|范围|可核对题数|同目标+同原始标签有训练对应|无精确标签对应|','|---|---:|---:|---:|']
    for name,s in summary.items():lines.append(f"|{name}|{s['taxonomy_target_eligible']}|{s['taxonomy_target_seen']}|{s['taxonomy_target_unseen']}|")
    lines+=['','## 需要优先语义确认的37条操作组合差异','','|范围|题号|简短请求|操作码|首个训练候选|','|---|---|---|---|---|']
    for r in rows:
        if r['classification']=='operation_combination_unseen':
            request=r['request'][:70].replace('|','／')
            lines.append(f"|{r['split']}|{r['task_id']}|{request}|{','.join(r['effects'])}|{r['nearest_training'][0]['id']}|")
    reviewed=[r for r in rows if r.get('manual_review')]
    lines+=['',f'## 逐题语义复核（由本会话完成，{len(reviewed)}/400题）','',
            '逐题对照完整请求、后续轮次及所列训练候选。共享核心机制不代表整题已学会；支持薄弱表示当前未找到充分整链对应，并非证明训练绝无相关案例。所有逐题依据保存在JSONL，以下只列需要优先处理的差异。','',
            '|范围|已复核|核心机制有对应|整链支持薄弱|无法确认|','|---|---:|---:|---:|---:|']
    for name in summary:
        subset=[r for r in reviewed if r['split']==name]
        counts=Counter(r['mechanism_status'] for r in subset)
        lines.append(f"|{name}|{len(subset)}|{counts['shared_core_mechanism_found']}|{counts['weak_joint_mechanism_support_needs_confirmation']}|{counts['uncertain']}|")
    lines+=['','|范围|评测题|简短请求|训练证据|判断|共享/差异|','|---|---|---|---|---|---|']
    for record in reviewed:
        r=record['manual_review']
        if r['judgment']!='shared_core_mechanism_found':
            request=record['request'][:70].replace('|','／')
            shared=str(r['shared']).replace('|','／')
            difference=str(r['difference']).replace('|','／')
            lines.append(f"|{record['split']}|{record['task_id']}|{request}|{', '.join(r['training_ids'])}|{r['judgment']}|共享：{shared}；差异：{difference}|")
    lines+=['','## 证据等级和使用限制','','- A：可从原JSONL复算的标签与支持数。证明字段事实，不证明机制缺失。','- B：候选训练类比。权重为操作码Jaccard 0.30、targets Jaccard 0.20、原taxonomy精确匹配0.15、同类别0.10、纯自然文本cosine0.25；缺字段权重移除后重归一化。候选不保证语义等价。有定向语义核对的题优先使用其训练引用，列出并标manual_reference（指本会话定向语义阅读，不代表其他人类审阅），其他位置按上述分数排序。','- U：机制尚无统一标签，未逐题语义阅读的363题保留not_semantically_reviewed；不能把400条字段比较冒称400条完整机制语义裁决。','- 文本使用仅train拟合的字符2–4gram TF-IDF，未知词项被忽略；只辅助找候选。无system/schema/expected_behavior进入相似度。','- 操作码不补意图，场景元组不补机制，空值不补成功覆盖。报告仅供独立审计，不回流GEPA或改变候选排名。','','来源：`datasets/training-reviewed/train.jsonl`、`datasets/source/val.jsonl`、`datasets/source/holdout.jsonl`及`datasets/gepa-view.json`，相对本实验目录。原revision2审计保持不变。','']
    lines=[line.replace('未逐题语义阅读的363题保留not_semantically_reviewed',f'未完成语义阅读的{len(rows)-len(reviewed)}题保留not_semantically_reviewed') for line in lines]
    base.with_suffix('.md').write_text('\n'.join(lines));print(json.dumps({'rows':len(rows),'summary':summary,'semantic_reviewed':len(reviewed),'semantic_counts':dict(Counter(r['mechanism_status'] for r in reviewed)),'jsonl':str(path),'bytes':path.stat().st_size},ensure_ascii=False))
if __name__=='__main__':main()
