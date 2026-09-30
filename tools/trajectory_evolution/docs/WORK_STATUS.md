# 当前工作状态（2026-09-29）

## 当前任务，先读此节

### 最新恢复与seed迭代（2026-09-29 16:55 UTC）

- L0 val200完整有效、0errors：Q.531238095238、strict91、loop2.875、101.022648s、token106593.635、calls16.53、intent.92857143/32题32轮。holdout200正在audit/l0-400-20260929/sealed-holdout运行，不用于选优。
- L0复用已修：原82合并report交native导致contract_invalid，曾错误fresh重试；安全暂停保留88行，从首次有效来源筛得126有效，再只补74。索引trial-index.json保留task/trial/source，最终best_strategy和deadline_finish均接完整L0缓存，额外执行成本不可隐藏。
- 新96题/196轮与跨域32题/64轮Agent采集已完成。旧原生memory有跨动作支持污染，不当作修复版。新relearn已缓存191份逐轮分析、补1012真实工具契约，不copy旧candidate/private/public memory。
- 支持污染修复staging=/tmp/te-support-contract-20260929/build；全ABI重编和3定向目标/真实batch replay通过。同tool/action不同措辞可合并，detail/recommend及movie/reminder不互借支持。formal与staging同seed artifact-version一致。
- 新relearn首63轮分析已完成、private66，但shared0modelcalls即失败：遗漏service.sharing的4个relative prompt文件。16:55修好copy，9tests通过，tmux business-targeted-relearn已从原checkpoint恢复。
- tmux business-seed-iteration等新memory/memory/evolution-memory.json的experiences发布后立即运行validate_targeted_seed.py --id seed_relearn_001 --native-build /tmp/te-support-contract-20260929/build --run；不用等完整重学。registry位于audit/targeted-seed-candidates/registry.json，完整同50成绩才能注册，低分保留不隐藏。
- 用户授权统一评测延后1h；监督器trajectory-deadline已于16:42实际换为19:00UTC（北京03）冻结，硬截止01UTC（北京09）不变。15项监督器tests通过（含external node=None冻结和L0零新增运行）。
- latestcheck_resume_route和child曾usage-limit退出，root接管监控；best_candidate_scan仍可处理只读候选审查。不要把代理失联当实验停滞。

### 当前主线（2026-09-29 16:12 UTC）

- 用户改为强seed优先、GEPA锦上添花。旧业务GEPA job26d4于15:12按主代理安排暂停，Node12待生成；不误报为运行卡死。Node9全池异常绕过修复和真实差分支持修复已安装，旧成绩保留。
- 新96会话/196轮定向训练已在tmux business-targeted-seed运行，输出targeted-seed-20260929，当前64/96采集、batch002学习。首批32/32有效，strict27、Q.921875、loop2.25为训练诊断；首批共享导出7条，仍须内容审核和独立seed验证。
- 跨域补充32会话/64轮已工具契约验证，tmux business-crossdomain-seed等待96完成后复制7项权威memory到独立目录续学；7项本地恢复/等待/隔离测试通过。等待不调用模型，原96 memory不改。
- 原48补训筛除问题经验后生成仅含6条L1的新seed6293d114…，32题pool评测在tmux targeted-seed-clean-pool32运行；尚无完整成绩，不能先称改善。
- L0同32题已完整：Q.5411458333、strict14、loop3.15625、85.827959s、97099.78125token、16.03125calls、intent .9230769231/9题9轮；L1-L3无计数遥测。原始报告在audit/targeted-repair-20260929/runs/L0-e9fe2e6348ba-218642fe2294/attempt-2。
- 用户授权L0先缓存400题。baseline400_cache.py准备82条复用+318新增，tmux business-l0-400-20260929于16:11启动命令成功。第一次审批未识别授权而拒绝；补充用户明确plan/v3与400题授权后同一命令重新审批通过。后续seed用固定32search快筛+既有50selection，不重跑L0、不用holdout选优。
- BFCL新seen-scenarios-v1继续独立训练，最近267/400；batch018已采用memory_review_parallelism16（此前默认4），尚未测量加速倍数。
- 18:00UTC搜索/候选冻结、次日01:00UTC最终交付不变。deadline监督器正在运行；新增外部strong seed须接入val50候选注册与最终选择，避免仅从旧树选best。该接入由best_candidate_scan实施中，尚未声称完成。
- 每60秒以内实际墙钟表格播报，禁止GitNexus和bwrap。仍16并发、plan/v3、实验直连无代理；不打印/提交密钥。

### 最新实测与修复（2026-09-29 14:53 UTC）

- 当前同一业务job `26d4b5788f9bfba8835c9befce0ebf1c4325f47e0f8ec6b3c5f2cf8a5f3fefb0`，runner1037656，tmux business-gepa，入口仍 `/tmp/te_resume_business_after_review_budget.py`。Node9正在独立val50；Node8已结束。
- Node8 val attempt2完整50/50有效、0错误：Q0.4784761905、strict20、loop4.48、116026.236917ms、115103.48token、18.3calls；intent F1=1.0/1题1轮；L1=374/L2动作=3/L2编排=0/L3=22。tree状态unobserved_changes，原因no_attributed_validation_gain，不是没有跑val。L0同50题Q0.4082857143/loop3.14，Node8尚未同时达标。
- Node9同32题pool：父Node0→Node9 Q0.5234375→0.5、strict13→12、loop4.46875→3.84375、lat100.747s→97.482s、token100772→96095、calls17.53125→15.90625；intent .904762→.9，各9题9轮。父报告1f76460b…包括26条新执行+6缓存，不能只取26条混比。
- Node9反思流程发现实质漏洞：map第6组component_not_authorized冒泡到外层单题repair，18/32题分析后绕过剩余map/reduce/review。正在修复同组受限repair和全池未完整返回不得进入外层单题repair；尚未部署，旧成绩保留，不能声称全池完成。
- 候选实际差分支持及相关成功/失败案例选择修复已编译测试，尚未安装；本次将与Node9异常路径修复一并定向验证后安全切换，避免反复重启。
- Node2独立val50已结束（attempt1）：Q0.454952381、strict19、loop3.3、118401.457632ms、113216.04token、18.02calls、0错误；与Node5独立审计均使用source数据，正在核实source/view仅review_status不同后复用，不因指纹不同直接重跑。
- evaluation_cost.py及6项无模型测试完成，Node8 pool两次报告64行去重为33次Agent执行/32题。含额外失败执行均值109717.852973ms、111939.28125token、16.25calls；不得仅用最后32条均值掩盖重试成本。此工具仅计Agent，不包含judge/GEPA调用成本。
- deadline监督器已于13:42:56真实启动在tmux trajectory-deadline，status位于audit/deadline-finish-20260929/status.json；14:47:34心跳正常。18:00UTC搜索截止，次日01:00UTC交付截止。后续重启继续用原runner入口，避免监督器身份不匹配。
- BFCL新seen-scenarios-v1训练最近确认209/400，memory157；第14批学习CPU持续增长，非已停止。继续每60秒内表格播报；L0缺少实际计数遥测必须写暂无，不能按关闭策略推断零次。

### 最新实测（2026-09-29 13:26 UTC）

- Node5独立val50补判attempt2结束：50/50有效、0error，Q0.4435238095、strict21、loop4.04、119017.714ms、112908.46token、17.98calls；多目标intent F1=1.0/1题1轮，L1=352/L2a=0/L2s=0/L3=5。原attempt1五条judge错误保留。与父0比loop-18.22%，与L0比Q+3.5238pp但loop/资源仍较高，未达标。
- Node2旧pool32真实同题：父1/子2 Q均0.56770833、strict均15；loop4.09375→3.78125、token123382.5→120074.56、calls18.78125→18.125、lat120.929→123.778s；多目标intent F1 .871795→.842105，各9题9轮。现独立val50已用tmux business-node2-val启动，purpose=node2_val50_audit，不改旧tree/gate；早期5/50不作效果结论。
- Node7父pool25/25结束13:16:39，约8分钟本地单核满载预处理后于13:24:53实际counterexample review请求58957bytes、13:25:01收到响应、13:25:03生成7.draft.json。新预算修复已在真实新节点生效，未复现超限。当前待解析/同题子节点评测。
- L0/seed0/Node1当前val50多目标intent重算均F1=1.0、覆盖1题1轮，不能继续用旧全目标F1 .913/.936/.941。测量稀疏，不据此宣称意图提升。
- deadline_finish.py已13项无模型测试通过，监督器准备部署；best_strategy候选/冻结/复用修复在收尾；L0复用应指向audit/val-regrade-20260929/l0-split/retry-1/report.json，不能假设其在tree.evaluations内。

### 当前续跑（2026-09-29 13:11 UTC）

- 已修复Node6最后counterexample review重复receipt状态快照造成的64KiB超限。仅在超限时缩128字节诊断excerpt，保留32题outcomes、真实成功/失败pair、全部receipt身份/计数、typed必要动作下界；full_pool_reflection定向正负/恢复测试通过，Node6原始最高reduce层回放257626→60230 bytes。
- 正式CLI/archive已安装/tmp/te-review-budget-20260929构建；原业务runner964683于13:09:32安全暂停，13:10:34同job/Node7恢复，new runner1037656，脚本/tmp/te_resume_business_after_review_budget.py，日志gepa-review-budget-resume-20260929.log，tmux business-gepa。Node6原error状态保留；不手改成成功。部署证据audit/restart-20260929/review-budget-fix.json。
- BFCL第6批学习13:10:13结束进入第7批capture；刚才约7分钟是学习/关系整理，training_failed=0、pending=0，非卡死。仍16并发、直连plan/v3，继续独立并行。

### 最新更正与并行进度（2026-09-29 13:05 UTC）

- 以下12:30记录中的f69c3394…是父Node0的22题pool复评，不是Node5。真实Node5同题报告为09857750…/attempt-2：Q0.593939对父0.515152、loop3.590909对4.090909；被拒的实际原因是时延比1.105517超过1.10，通用pool_quality_regression标签具有误导性，不能据此说质量退步。
- Node5独立val50已采集50题，但45有效、5 evaluation_judge_failed，不能当完整结果。仅同45题：L0 Q0.386984/loop3.222222/strict16；父0 Q0.439153/loop4.777778/strict17；Node5 Q0.455767/loop3.977778/strict20。待仅补失败判分，不改tree.validation或旧gate。
- 独立val目录：published/best-strategy-runs/26d4b5788f9bfba8835c9befce0ebf1c4325f47e0f8ec6b3c5f2cf8a5f3fefb0/node-5-5f73b2e061-node5_val50_audit-val-50-1e114ace0f-candidate。
- Node6覆盖32/32题、两级reduce已完成，13:02报gepa_feedback_budget_too_small；失败在最终counterexample review压缩后仍超65536 bytes，不是策略效果失败。子代理修复；Node7 parent0正在25题pool评测。
- BFCL已90/400题采集、6批进入学习、私有memory80（动态），2旧trace_output_mismatch；新版本连续第5/6批31题零新增校验错误。训练指标仅诊断。
- trace_output_mismatch根因是protobuf double额外JSON往返精度变化。task_evaluation.cpp局部改为原始protobuf Value精确比较，保留类型/字段/邻接浮点差异校验，未改全局parser/评分。3项定向测试通过；CLI/archive约12:43原子安装，旧进程仍用原inode。新BFCL批次已使用修复。
- 两线最终交付须按完整独立val50冻结候选与best，不能holdout后重选；业务best_strategy旧接口存在holdout重排，正在修。所有原始分数保留，禁止把best_observed冒充正式发布达标。
- 搜索最迟截止UTC2026-09-29 18:00（北京时间次日02:00），为UTC次日01:00硬截止留7小时完整最终评测与失败补齐。deadline监督脚本仅草案，尚未运行。每60秒内表格播报。

### 实际恢复与并行实验（2026-09-29 12:30 UTC）

- 业务同job `26d4b5788f9bfba8835c9befce0ebf1c4325f47e0f8ec6b3c5f2cf8a5f3fefb0`已实际恢复，runner964683、tmux business-gepa。tree=searching，active Node5，新的22题pool评测f69c3394…已启动，不是仅进程存活。
- 修复gepa_search.cpp的guard→schedule跨step真实执行归因；asynchronous_target_replay与terminal_failure_identity定向通过，新增1正9反断言；3条原始Node2轨迹QA_GUI_WYYL_010/QA_GUI_YDYL_012/SYS_DYT_03_126分别找回1次actions改动。未改评分/门槛，旧Node2状态保留并另附审计。
- 新CLI和静态库已原子安装，备份后缀.pre-delegated-guard-20260929；构建/tmp/te-gepa-attribution-20260929，审计audit/restart-20260929/guard-attribution-fix.json。aggregate提示同步修改源模板和业务运行副本，plan记录已更新。API仍plan/v3新key、无代理、16并发。
- 重启脚本/tmp/te_resume_business_after_guard_fix.py；日志trace_output/business-simulator/gepa-guard-fix-resume-20260929.log。它绑定原job，不新开树，不改API切换状态。
- 用户已要求并行BFCL-V3；tmux bfcl-v3-seen已用新seen-scenarios-v1划分、全新memory训练，12:29至少32/400采集，私有memory39、公有0。BFCL宿主run.end前固定应用计数，5项本地测试通过；2条L0 trace_output_mismatch根因仍在只读定位，不能算已修复。
- BFCL最佳策略兜底由独立bfcl_best_strategy.py实现中；必须在holdout前按完整val冻结3候选和best ID，不可根据holdout再选best。严格保留BFCL宿主、initial_state和bfcl_valid确定性检查。
- 硬截止北京时间2026-09-30 09:00=UTC01:00；当前已过12:30UTC，剩12.5小时。原定30节点仍尽量做，但最终完整评测须提前留4–6.5小时，不能搜到最后才发现无时间。

### 当前人工审查（2026-09-29 11:38 UTC）

- runner于11:13:47已安全paused，当前job不变，active Node5/proposed；Node5 pool22已有8条有效评分，其余14条未评分（含取消），不得将取消或裁判错误当策略失败。恢复后补齐原节点。
- 用户随后放宽固定30%无效节点限制，改为代理合理判断；观察最近10个、至少6个完成、60%触发人工诊断，不机械停机，不写入搜索自动流程。
- Node2删除device-gui max_attempts=1规则，预检5题有差分，实际归因漏掉guard与后续schedule.action不同step的执行关联；正在最小修复，不能按状态标签直接认定无效。
- Node3 cap8而真实同actor最大iteration8，无执行差分；Node4缺结构化恢复条件却把max_replans1改0。并行修正汇总提示中把结构校验误称执行验证的表述，后续节点使用；不重写旧节点分数或策略。
- 恢复前只做必要关联正/负例测试和定向构建；不重复L0、原始轨迹或全量回归。api仍plan/v3新key、无代理、16并发、tmux。

### 最新 API 切换与续跑（2026-09-29 09:59 UTC）

- 当前 job 仍是 `26d4b5788f9bfba8835c9befce0ebf1c4325f47e0f8ec6b3c5f2cf8a5f3fefb0`，没有重开树。Node1 val50结束后于09:54:30切换新key，tmux `business-gepa`、runner859326、native859350；Node2父节点为Node1，正在先跑本轮32题parent pool，09:59:47已有效完成23题。
- 真实传输端点切换前后均为 `plan/v3/chat/completions`、模型 `deepseek-v4-flash`，本次仅换凭据；`OPENAI_BASE_URL=coding/v3` 是run.py保留的历史评分身份字段，不能用其推断实际路由。实际应核对runtime endpoint和`TRAJECTORY_MODEL_ENDPOINT_OVERRIDE`。
- 新key在plan端点测试HTTP200，在coding端点401。用户明确授权全部plan；native的AF/OPENAI key与.env匹配、实际override为plan；Agent使用AF配置且不继承OPENAI key是正常的。实验无代理，16并发。
- 新口径val50：L0 Q=0.4082857143 / strict19 / loop3.14 / 108022.42ms / 108446.04token / 17.14calls；Node0 Q=0.4352380952 / strict19 / loop4.94 / 116584.08ms / 118292.06token / 18.62calls；Node1 Q=0.4819047619 / strict22 / loop3.90 / 123187.39ms / 118869.76token / 18.56calls；三者有效50/50、0错误。
- Node1 status=accepted且validation_gain_attributed=true，允许继续派生，但loop/资源仍高于L0，未达到全部最终目标。Node1反思覆盖32/32题、15次模型调用。意图测量覆盖不同，F1不可直接称为提升。
- 切换审计：`trace_output/business-simulator/audit/restart-20260929/api-key-switch.json`；凭据暂存文件在成功后删除；不得打印或提交密钥。以下旧job、pending及暂停状态都是历史，以最新tree/status为准。

### 最新实际启动（2026-09-29 09:05:19 UTC）

- 新GEPA job `26d4b5788f9bfba8835c9befce0ebf1c4325f47e0f8ec6b3c5f2cf8a5f3fefb0` 已运行seed50题；L0新口径50/50已导入、0错误。
- 种子来自旧Node5，旧job已显式取消以防自动恢复抢占；旧树/报告保留。
- 复核已拆分为目标理由与loop证据两类独立请求，定向11/11通过。详见 `trace_output/business-simulator/audit/RESTART_STATUS_2026-09-29.md`。
- 下方旧job暂停与48/50复评描述均为历史诊断，当前以新job及receipt为准。继续每60秒内表格播报、人工审查无效节点比例。


- 权威要求：`EXPERIMENT_GOALS.md`。最新诊断：`trace_output/business-simulator/audit/LOOP_EVALUATION_CONSISTENCY_2026-09-29.md`；下方9月26日数据仅为历史记录，不能当作当前树。
- 当前GEPA job：`278d33b0ccd013ebb150d1ef0af0749db238a61b69c2b53ab269d88a216bf66f`。Node7 pending、parent5，runner paused，已结束候选5个，另1个生成错误；Node0种子单列，实际父前沿[0,5]。实验人工暂停，尚未恢复。
- 混合反思失败证据/必要动作保留、反思断点按实际发送输入复用已修，stage4正式CLI/库已安装；合成32题恢复、真实Node3归因、真实Node6 32题19次本地map复放通过。不能把这些功能测试当作实际策略收益。
- 质量播报已按原生硬失败/critical_failure归零，16项定向测试通过；Node6 pool质量0.6619791667。L0/Node0/Node4/Node5 val质量未改变。
- 当前阻塞：同裁判同题出现理由承认无效却keep；原一致性审查只包含业务goals，未含loop。keep会影响必要动作/轮次下界。必须先修语义一致性并复用旧轨迹统一复评，禁止混用新旧标准、覆写旧分数或自动降低门槛。
- 用户要求定位提取/注入/进化/评测四段问题并找到有效方法。正在并行：经验内容与实际receipt对齐；loop一致性最小修复；现成单经验消融入口。策略修改证据仅取train/pool，val/holdout不回流反思。优先做可归因的小规模同题对照，不承诺随机模型一定提升。
- bwrap不可用：shell全部require_escalated；不使用GitNexus。每60秒内表格播报。无效节点比例由主代理人工审查，不写自动停机。保留16并发、tmux、无代理及原始证据。

## 以下为2026-09-26历史状态

恢复上下文时先读 `EXPERIMENT_GOALS.md`、本文件和 `25-business-holdout-effect-audit-20260926.md`。

## 旧实验终态

- 旧 GEPA job：`8d505e...`，`finished/rejected`，`published=false`，当前无运行进程。
- Validation 50：L0 `.470857/2.36/83.249s/20`；父 Node6 `.584952/2.36/94.509s/27`；Node27 `.598190/2.00/77.526s/28`。这些不能称为 Holdout。
- 真实 Holdout 共享 L0 200；Node27/15/18 各 candidate 200，三份配对报告均 400 行（baseline 200 + candidate 200）。
- L0：quality `.519522`，strict `85/200`，invalid loop `2.56`，latency `77.893s`，tokens `99851.7`，intent F1 `.880`。
- Node27：quality `.519494`，strict `89/200`，invalid loop `2.40`，latency `90.229s`，tokens `101065.4`，intent F1 `.9123`；拒因 `quality_regression/max_latency_ratio/loop_target_not_met`。
- Node15：quality `.517573`，strict `90/200`，invalid loop `2.49`，latency `88.951s`，tokens `99908.6`，intent F1 `.88496`；真实配对 wins/losses/ties `24/19/157`，仍拒绝。
- Node18：quality `.503284`，strict `83/200`，invalid loop `2.18`，latency `88.569s`，tokens `96387.1`，intent F1 `.88789`；配对 `19/21/160`，含 critical goal failure，仍拒绝。

## 已完成代码改动

- `agent_evaluation.cpp`：cached pair 缺少嵌套 objective pairing 时回退顶层配对字段；不改分数和阈值。
- `task_evaluation.cpp`：接受 `artifact_retention`；轻量模式完成报告后清除重复 `trials`，可关闭 `report.md/results.jsonl`；默认配置保持兼容。
- `offline_gepa.py`：新实验打开轻量产物配置、`max_token_ratio=1.0`，并把 GEPA 搜索的 invalid-loop/token/latency 权重与发布目标对齐。
- `offline_experiment.py`：非程序员单入口 `--fresh/--status/--check`。
- 定向证据：标准 report/manifest 测试 1/1 通过；轻量模式实测保留 `report.json/report.pb/manifest` 且清除重复物。

## 待完成且不可宣称达标

1. 合并 cached pair 的 objective paired 统计和 final_holdout 元数据修复，避免 validation retrospective 冒充 Holdout。
2. 完成无损归档工具和 SHA256 核对；随后再删除旧 GEPA 重复物/重置 GEPA 树，不删除训练记忆和输入数据。
3. 定向编译/测试后，用简化入口重新跑完整离线链条；每 60 秒表格播报。
4. 只有真实 Holdout 同时满足质量 +5%、invalid loop 至少 -15%、latency/token/calls 不增加、intent 不下降，才可以发布；否则保留 rejected 证据。
