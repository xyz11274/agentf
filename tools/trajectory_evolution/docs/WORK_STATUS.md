# 当前工作状态（2026-09-29）

## 当前任务，先读此节

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
