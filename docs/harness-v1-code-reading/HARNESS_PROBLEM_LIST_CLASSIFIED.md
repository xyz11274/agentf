# Harness v1 分类问题清单

## 一、会直接改变执行结果、造成错误回答或流程挂起

| 问题 | 位置 | 诱发原因 | 可能导致的结果 | 具体错误场景 |
|---|---|---|---|---|
| Tool Call 语义恢复正常情况下不会重试 | packages/agentflow-runtime/src/agentflow_runtime/skill_executor.py:753-794 | continue 写在 response_push 的 except 中；进度上报成功时直接落入错误返回 | 可恢复的 Tool Call 被直接终止；Loop 行为与配置不一致 | 配置允许恢复 2 次。第 1 轮 LLM 因 token 截断返回不完整的 write_file.arguments；执行器追加恢复提示并扩大 token budget，但“正在恢复”进度写入成功后没有 continue，而是立即返回 tool_call_error。日志显示进入过恢复分支，实际只有一次 LLM 请求。 |
| Skill 逻辑失败被上报为成功 | skill_executor.py:476-507、516-543、668-698、714-739、753-794、840-904、1082-1103；src/agent/agent.cpp:1863-1876 | Skill 输出失败 status 后正常 return，Python 退出码仍为 0；C++ 只根据 workflow_completed 设置 skill_success | Coordinator 把失败结果当成功处理；错误样本进入成功轨迹 | Skill 输出 status=llm_error 后正常退出；Worker 因退出码 0 将节点标成成功，Runtime 发送 workflow_completed，下一轮 Coordinator 收到 AF_SKILL_SUCCESS=true，并使用成功决策提示处理一个实际失败的结果。 |
| 并行 Skill 共用结果键 | src/runtime/runtime.cpp:106-119、1193-1293、1643-1691 | build_framework_key 忽略 workflow_id；同一 dialogue 的 Skill 都写 skill_result、skill_name、artifacts_json | Skill 结果、业务名和产物互相覆盖；Coordinator 基于错误结果决策 | 穿搭 Skill A 写入 result=A，图片 Skill B 随后写入 result=B。A 的 workflow completion 稍后读取共享键时拿到 B，于是 Coordinator 收到两次图片结果，却没有收到穿搭结果。 |
| 处理一个 Skill 完成事件时可能清掉兄弟完成事件 | src/agent/agent.cpp:1913-1919；src/agent/task.cpp:183-207、293-307 | clear_data_exchange_messages 无条件清空整个队列，没有按 execution id 或消息类型过滤 | 并行 completion 丢失；pending 无法归零；对话永久等待 | A、B completion 已同时排队。处理 A 时 pending 从 2 降到 1，随后 clear 删除尚未处理的 B。B 不会再次上报，C++ 始终认为还有一个 Skill 在运行，前端一直收不到 final。 |
| 最终响应可能绕过 pending Skill 屏障 | src/agent/agent.cpp:2326-2381、2473-2545；coordinator_decide.py:920-940 | C++ 只在 found_next_skills 分支检查 pending；Coordinator correction loop 会删除 next_skills | Skill 尚未完成就发送 final；final 后后台继续执行 | A 仍在运行，B 完成后触发 Coordinator。模型先重复调度 B，重复拦截分支删除 next_skills，下一次内部决策输出 final。C++ 进入 no-next_skills 分支，未检查 pending A 就结束对话；几十秒后 A 又完成并再次触发 Coordinator。 |
| Dispatch 写入不是事务，且忽略写入结果 | coordinator_decide.py:217-258；databus_client.py:98-109、146-162 | manifest、last_prompt、next_skills 分多次写入；put 返回 False 仍返回 dispatched | 出现假调度、孤儿 manifest 或缺失输入的 Skill | dispatch manifest 因 UDS 抖动写入失败，next_skills 随后写入成功。Coordinator 仍返回 status=dispatched，C++ 启动 Skill，但 Skill Executor 找不到对应 manifest，实际执行内容已与原委派脱节。 |
| run_skills 参数缺少运行时校验 | coordinator_decide.py:794-845、214-227 | 没有检查 skills 类型、spec.name、prompt、load_skills 和白名单 | JSON 合法但结构错误时 Coordinator 崩溃；非法 Skill 进入调度 | 模型返回 {"skills":[{"prompt":"生成图片"}]}。JSON 可以正常解析，但 spec 缺少 name，handle_run_skills_call 在 spec["name"] 处抛 KeyError，无法通过 Tool correction 让模型重新输出。 |
| Hook 阻断的 Skill 仍被记为已调度 | coordinator_decide.py:823-845、867-919 | Skill 名先加入 _dispatched_skills，之后 Hook 才设置 blocked；阻断后没有清理 | 显示假进度并等待不存在的 Skill | 安全 Hook 阻断 image-gen，但 image-gen 已留在 _dispatched_skills。Coordinator 仍发出 skills_dispatched 和“正在执行 image-gen”，随后 return；实际上没有 next_skills，也没有 Skill 进程会完成。 |
| 重复调度只检查最近完成的 Skill | coordinator_decide.py:180-204、381-389、804-817 | 只比较当前 completed_skill 和最后一个 prompt，没有完整 pending/completed ledger | 已完成 Skill 被重复执行；增加无效 Loop 和费用 | weather-search 以 prompt=P 完成后，outfit-stylist 又完成，使当前 completed_skill 变成 outfit-stylist。模型再次以相同 prompt=P 调用 weather-search 时不会被拦截，完全相同的查询被重复执行。 |
| Dispatch manifest 丢失时继续执行 unknown-skill | skill_executor.py:330-370 | manifest 缺失、DataBus 失败或 JSON 非法时选择通用回退，而不是终止 | 调度断链被伪装成正常执行；产生无关结果 | C++ 传入 dispatch:D:1，但 Skill 启动时该 manifest 已不可读。执行器设置 skill_name=unknown-skill，把原始 AF_INPUT 当任务继续调用通用工具，最后甚至可能输出 status=completed。 |
| after_tool Hook 改写结果不检查数量和类型 | skill_executor.py:943-959 | 改写结果直接与 tool_calls 使用 zip，没有校验长度、tool_call_id 和元素结构 | Tool response 缺失、错配或解包崩溃 | 本轮有 call_read 和 call_bash 两个 Tool Call，Hook 误只返回一个结果。zip 只回灌 call_read；下一轮请求仍包含 call_bash，却没有对应 role=tool，模型服务可能直接返回 400。 |
| 空最终文本仍标记 completed | skill_executor.py:1020-1032 | output.status 先固定为 completed；空文本只记录 WARN | 空结果被当成成功；Coordinator 沿成功路径继续 | Provider 返回 finish_reason=stop、content=""，但 usage 不为 0，因此没有命中“usage 全 0”的空响应检查。Skill 最终输出 status=completed、skill_result=""，C++ 再上报 skill_success=true。 |
| Timeout 和 Max Rounds 返回的不是可靠汇总 | skill_executor.py:476-497、1082-1094 | 直接把 messages 最后一条 content 当 skill_result；最后一条可能是 Tool JSON 或恢复提示 | 内部协议或原始 Tool 数据被当成最终业务结果 | 第 8 轮 bash 返回一大段 JSON 后刚好耗尽 max_rounds，系统没有再调用 LLM 总结，而是把最后一条 role=tool JSON 直接作为 skill_result 交给 Coordinator。 |
| SpanWriter 写失败会反过来打断业务 | trace/emit.py:70-82；trace/span_writer.py:68-89 | emit_span 的 finally 直接写文件，没有异常隔离 | 模型和工具已经成功，仍因 Trace 文件异常而 workflow_failed | LLM 已生成正确答案，Skill 结果也已打印；关闭 span 时只读目录触发 PermissionError，异常继续向外传播，Python 非零退出，C++ 最终把整个 Skill 标成失败。 |
| Tool registry 覆盖实现后仍保留旧 schema | tool_registry.py:205-226 | 同名 Tool 覆盖 _modules，但 _tools 继续保留所有旧 schema | LLM 按一个 schema 生成参数，执行器却调用另一个实现 | 全局 bash 接收 command，Skill 目录的同名 bash 接收 script。LLM 同时看到两个 bash 定义并返回 {"command":"ls"}，执行阶段实际调用后加载的 Skill 版本，因缺少 script 报错。 |

## 二、执行可能继续，但轨迹会丢失、串线或无法构建正确父子关系

| 问题 | 位置 | 诱发原因 | 可能导致的结果 | 具体错误场景 |
|---|---|---|---|---|
| Skill 完成回传丢失 dispatch 身份 | coordinator_decide.py:217-247；src/runtime/runtime.cpp:1627-1702；src/agent/agent.cpp:1880-1904 | exec_skill_key 只传到 Skill 输入，完成 metadata 没有带回 | SkillCompleted 无法唯一连接到 DispatchRequested | 同一批两次调用 web-search，分别搜索北京和上海，生成两个 dispatch key。完成记录都只剩 business_skill_name=web-search；如果上海先完成，采集器无法确定它对应哪一个请求节点。 |
| 同批 skill-runner 的 task_id 可能相同 | src/agent/agent.cpp:1230-1256、2840-2851 | task_id 构造没有包含 exec_skill_key | 多个执行在日志和 Recorder 中显示为同一 task | outfit-stylist 和 forge-image-gen 同批启动，C++ 规格名都为 skill-runner，message_id 也相同，最终两个进程获得相同 AF_SELF_TASK_ID。轨迹树会把两个执行合并成一个节点。 |
| LLM trace index 并发写会丢项 | packages/agentflow-runtime/src/agentflow_runtime/recorder.py:427-438 | 多进程同时“读列表—append—覆盖写”，没有 CAS 或原子 append | 实际 LLM Call 无法被索引枚举；轨迹缺轮次 | A、B 同时读取 index=[call_1]；A 写 [call_1,call_2]，B 后写 [call_1,call_3]。call_2 的详细记录还存在，但轨迹拉取器按 index 永远看不到它。 |
| emit_event 没有真正写入 NDJSON | observability.py:49-79；trace/emit.py:87-118 | span_event 只输出 stderr，没有加入活动 TraceSpan | 重试、错误、超时、Tool Call 等事件无法从 NDJSON 回放 | Skill 第 4 轮 node timeout。stderr 中存在 node_timeout 及 round=4，但 NDJSON 只有 Skill span 的 ERROR 状态，没有 stop reason 和轮次；离线无法判断为何终止。 |
| Tool 调用和结果缺少可靠持久事件 | skill_executor.py:796-802、970-987；trace/emit.py:87-118 | Tool 信息主要写 messages 和 stderr，没有独立 ToolCall 事件 | 无法证明 Tool 是否真正执行，也不能安全回放 | write_file 成功生成 report.html 后进程立即被杀。Recorder 只保存上一轮 LLM 发出的 tool_call，没有 ToolCompleted、返回值和 ArtifactProduced，回放时不知道应重做还是跳过。 |
| Recorder 记录值可能和真正执行值不同 | recorder.py:337-396；coordinator_decide.py:626-630；skill_executor.py:661-666 | response 先落盘，after_llm Hook 后续可原地修改同一 choice | 离线回放分支与线上实际分支不同 | Recorder 保存 run_skills；after_llm Hook 因预算不足原地删除 tool_calls 并改为向用户澄清。线上没有启动 Skill，离线按 Recorder 却会走 dispatch 分支。 |
| 最终响应只自动附加最近完成 Skill 的产物 | coordinator_decide.py:261-284、725-727、782 | 最终附件只使用当前 completion 的 AF_SKILL_ARTIFACTS，没有 batch 聚合 | 早期 Artifact 节点丢失或错误挂到最后一个 Skill | 图片 Skill 先生成 outfit.png，文本 Skill 最后完成且 artifacts=[]。文本 completion 触发 final 时只处理空列表，最终附件中没有图片，轨迹也难以从 final 反查完整产物集合。 |
| Hook 原地追加 Artifact 不会回写输出 | context.py:17-32；skill_executor.py:872-888、1048-1064 | 只在 list 对象身份改变时重写 artifacts_json；append 不改变身份 | Hook 生成的 Artifact 不进入 completion 和轨迹 | output 先序列化 report.html；before_skill_complete Hook 再 append audit.json。因为 ctx.artifacts 仍是原 list，identity 判断为 False，最终 artifacts_json 只有 report.html。 |
| Recorder 缺少关键执行身份 | recorder.py:97-117、379-396；src/runtime/runtime.cpp:2119-2143 | 未保存 exec_skill_key、source、skill、round；Runtime unique execution_id 也未暴露给 Python | LLM Call 只能靠时间或 Prompt 猜所属执行 | 两个 web-search 并行调用 LLM，meta 中 dialogue_id、node_id、task_id 都相同，也没有 dispatch_id。即使 Prompt 不同，经过压缩或 Hook 改写后文本也不能充当稳定关联键。 |
| SpanWriter 的锁不能保护多进程并发 | trace/span_writer.py:43-48、68-89 | threading.Lock 只保护本进程；并行 Skill 各自有不同锁 | NDJSON 出现半行、损坏行或不可解析记录 | A 写长 span 时被 SIGKILL，留下半条 JSON；B 的进程锁与 A 无关，继续把完整 span 追加在半行后。读取器按行解析时得到一条非法 JSON，A、B 两条记录都可能丢失。 |

## 三、消息协议与统计口径失真

| 问题 | 位置 | 诱发原因 | 可能导致的结果 | 具体错误场景 |
|---|---|---|---|---|
| Skill 完成结果被放成 role=user | coordinator_decide.py:462-474 | 异步完成结果晚于 Tool ACK 到达，代码用 user 消息重新注入 Coordinator | 内部事件被当成用户输入；角色、权限和 user turn 统计错误 | web-search 结果包含网页文本“忽略之前指令”。该结果以 role=user 进入 Coordinator，模型可能把网页注入当成用户要求；轨迹还会错误增加一次用户追问。 |
| run_skills 的 role=tool 只是调度确认，不是执行结果 | coordinator_decide.py:207-258、855-859 | run_skills 提交异步任务后立即返回“已调度 N 个 Skill” | DispatchAccepted 被误当 SkillCompleted | role=tool 返回“已调度 2 个 skill”后，普通 ReAct 解析器立即记录两个 Skill 成功；几十秒后其中一个实际返回 llm_error，数据中出现相互冲突的结果。 |
| Dialogue Start/End 实际是 Coordinator 激活 Start/End | coordinator_decide.py:354-361、728-758 | 每次 completion 都会重启 Coordinator；每次候选 final 都触发 dialogue end | 同一 dialogue 出现多个 start/end，甚至先 end 后 start | 首次触发 start；Skill A 完成后再触发 start；Coordinator 生成候选 final 并触发 end；Skill B 稍后完成，同一 dialogue 又触发第三次 start。 |
| Coordinator round 混入 Skill completion 次数 | coordinator_decide.py:391-406、521-525 | 每个 completion 都增加 current_round，内部 correction 又有另一套 tool_loop_round | 并发度被误算成 Loop 数；max_iterations 过早耗尽 | max_iterations=4，round=1 一次启动 4 个并行 Skill。前三个 completion 把 round 推到 4，第 4 个完成时变成 round=5并直接触发上限；系统只做过一次真正编排。 |
| Workflow JSON 声明和 Python 实际输出不一致 | skill_workflow.json:15-24；coordinator_workflow.json:52-64；skill_executor.py:1019-1025 | 声明 response，实际输出 skill_result、skill_name、rounds_used；coordinator_status 也未稳定输出 | Python/Rust ABI 不一致；Typed Protobuf adapter 无法确定字段 | Rust 按 workflow schema 只反序列化 response，但 Python 基线只输出 skill_result。当前 C++ 动态字段机制能跑，Rust 重写却得到空业务结果。 |
| LLM retry 次数记录永远为 0 | recorder.py:260、349-396；openai_compatible_client.py:121-267 | retry loop 位于 HTTP client，Recorder 没有获得真实 attempt 数 | 网络重试、语义恢复和 Agent Loop 无法区分 | 设置最大重试 2 次；请求前两次连接失败、第三次成功。HTTP client 实际尝试 3 次，Recorder 仍写 retries_used=0，离线成本和稳定性统计少算两次请求。 |

## 四、正式复杂实验新增问题（2026-08-28）

| 状态 | 问题 | 位置 | 诱发原因 | 可能导致的结果 | 具体错误场景 |
|---|---|---|---|---|---|
| 已修复（CHG-20260828-080） | Local Sandbox 多实例共用固定端口并串线 | `configs/dev/agentflowd.json:sandbox_http_port`；`src/sandbox/sandbox_local.cpp:LocalSandbox/launch_http_server/wait_for_server_ready` | 每个实例都收到同一端口；后启动子进程 bind 失败后，父进程仍可能把旧实例在该端口的任意响应当作自己的就绪信号 | 跨用户业务数据串线；首个 server 退出后其他实例统一 `Connection refused`；轨迹归属错误 | 用户 A 的 Sandbox 已占用 8080。用户 B 的子进程因端口冲突退出，但 B 的父进程探测到 A 的 server 返回 200 后仍宣告启动成功；随后 B 的 Tool 实际进入 A 的工作区，A server 退出后 B 又持续连接拒绝。 |
| 已修复（CHG-20260829-081） | Local Sandbox 工作目录与宿主绝对路径契约冲突 | `packages/agentflow-runtime/src/agentflow_runtime/executors/http.py`；`docker/sandbox/http_server/sandbox_http_server.py:_exec_tool`；`src/sandbox/sandbox_local.h`、`sandbox_docker.h` | Local server 以独立临时 cwd 为默认边界，但业务文件路径属于当前用户的宿主 workspace；Local/Docker 又都被注入成 `local`，无法安全地只为共享宿主文件系统的 Local 做映射 | 合法档案、历史和产物被 `Path escape rejected`；若粗暴放宽到 Agent 包根则会允许读取其他用户 workspace | 用户 A 的 `write_file("outputs/lookbook.html")` 本应写入 `workspace/A_conversation/outputs`，旧实现却落到临时 Sandbox；绝对用户路径又被越界检查拒绝。修复后仅 A 的 workspace 获得读写根，skill 静态目录只读，用户 B 路径仍被拒绝，Docker 保持容器路径语义。 |
| 已修复（CHG-20260829-081） | Sandbox 文件工具的内部二次校验会把真实失败归一化成成功 | `docker/sandbox/http_server/sandbox_http_server.py:_exec_tool` | Server 已按请求 workdir 校验路径，但委托 runtime wrapper 时后者又按 server 临时 cwd 拒绝；错误字典没有 `success` 字段，而归一化使用 `result.get("success", True)` | 文件没有写入却返回 `exit_code=0`；Skill、Artifact 和轨迹记录假成功 | `write_file` 返回 `success=true` 和可发布路径，但磁盘上没有文件；后续 Artifact 收集得到 missing。修复后 Server 只把已校验绝对路径交给内部工具，缺少 `success` 的 error 结果按失败处理。 |
| 已修复（CHG-20260829-082） | HTTP Sandbox 缺少 `list_dir` 分发与实现 | `packages/agentflow-runtime/src/agentflow_runtime/executors/http.py:list_dir`；`docker/sandbox/http_server/sandbox_http_server.py:TOOL_DISPATCH` | HTTPExecutor 会发送 `tool=list_dir`，但 server 分发表没有该工具，runtime tools 目录也没有 `list_dir.py` | Preload 的 workspace tree、history 列表和 skills 探测全部静默为空；轨迹把“工具不可用”误写成“目录不存在” | 用户已有 profile history 和 upload 图片；Hook 连续调用 `list_dir`，每次 server 都返回 `Unknown tool: list_dir`，HTTPExecutor 丢弃 error 并返回 `[]`，Coordinator 因而把老用户判断成新用户。 |
| 已修复（CHG-20260829-083） | HTTP Sandbox 未注册已有的 `web_fetch` 工具 | `packages/agentflow-runtime/src/agentflow_runtime/executors/http.py:fetch_url`；`docker/sandbox/http_server/sandbox_http_server.py:TOOL_DISPATCH`；`harness/tools/web_fetch.py` | Client 已实现 `fetch_url`，runtime 也已有安全 `web_fetch.py`，但 server 分发表漏掉 `web_fetch` | Local/Docker 模式下所有正常网络抓取都以 unknown tool 失败；模型可能重复尝试或错误归因为网络故障 | Skill 请求公开 HTTPS 页面，HTTPExecutor 正常 POST `tool=web_fetch`，server 却返回 `Unknown tool: web_fetch`；轨迹只看到多轮网络失败，实际没有发出任何外网请求。 |
| 已修复（CHG-20260829-084） | 活跃对话会被固定 300 秒 Session TTL 清理 | `include/internal/agent/session.h:timeout_ms`；`src/agent/session.cpp:203-220`；`src/agent/session_router.cpp:253-275` | Session 活跃时间只在外部消息入队时刷新，路由 GC 不检查对话、Workflow 或 Skill 是否仍在运行 | 长任务在执行中被销毁；DataBus key 被清空；Skill 和 LLM 终态丢失 | 一轮多媒体任务 17:34:38 入队，Skill 仍在重试；17:39:38 Session 恰好到 300 秒被销毁并清掉 84 个 key，轨迹留下 `skill.start` 和 `llm.request`，没有对应 end/response。 |
| 已修复（CHG-20260829-085） | Bridge 已取得 Session 后等待响应超时却不取消底层 Dialogue | `frontend/bridge/routes/chat_rest.py:_chat_turn_events` | `agent.execute` 已返回 `session_id`，但两个等待响应超时出口只发送 error/`[DONE]` 并丢弃 Bridge 队列，没有复用已有的 `DIALOGUE-INTERRUPT` | 客户端已经结束，后台 Skill 仍继续调用工具并持有 dialogue lock；下一轮被阻塞；客户端与底层终态矛盾 | Bridge 420 秒先超时并结束 T03，底层仍继续；修复后同步与 SSE 共用幂等取消状态，在清理响应队列前按真实 `session_id/dialogue_id` 下发一次中断。 |
| 已修复（CHG-20260829-086） | Bridge 轨迹投递契约已移除 `session_id`，回归测试仍要求该字段 | `frontend/bridge/tests/test_chat_rest_sse.py:test_streaming_records_final_and_done_delivery/test_sync_returns_public_content_but_persists_raw_artifact_url` | 采集侧已改用 user/client/dialogue，生产调用不再向 `client.delivery` 传内部 `session_id`，但两条旧断言未同步 | 全量 Bridge 测试持续假红，掩盖真正回归，也可能诱导维护者把废弃字段重新写回轨迹 | 超时取消改动后完整测试显示 2 failures，业务代码实际正确；旧断言对 `delivery_event["session_id"]` 取值抛 `KeyError`。修复后测试改为明确断言字段不存在，32 项全部通过。 |
| 已修复（CHG-20260829-089） | `agent.execute` 前置超时后 Bridge 无法定位底层 Session 进行取消 | `frontend/bridge/routes/chat_rest.py:_cancel_dialogue/_chat_turn_events` | execute RPC 超时时没有返回 `session_id`，旧路径直接结束客户端流；按 user 猜测 Session 又可能误伤同一用户的其他 client | daemon 已接收请求时后台继续运行并占用 dialogue lock；没有接收时盲目取消可能创建或伤及错误 Session | T03 execute 响应超时。修复后 Bridge 用已有 `session.list(agent,user)` 查找 user+client 唯一精确匹配，再按原 dialogue_id 发中断；不同 client、零匹配或多匹配均不发送。 |
| 已修复（CHG-20260829-090） | Skill 节点剩余时间没有约束下一次 LLM 调用预算 | `packages/agentflow-runtime/src/agentflow_runtime/skill_executor.py:round loop/call_llm`；`examples/agent/outfit-assistant/skill_workflow.json:timeout_ms=240000` | 节点只在每轮顶部检查 elapsed；硬压缩和主 LLM 各自可再占配置的 120 秒，请求开始前不按节点剩余时间收缩 | C++ 节点硬超时可能在 LLM 中途杀进程，`llm.request` 缺少 response/error，Skill span 与 batch 悬挂 | Skill 已执行 230 秒再进入 LLM。修复后独立 Skill 子进程将 `AF_LLM_TIMEOUT` 收缩为配置值与剩余预算的较小者；压缩后重新计算，并预留 result、`skill.end`、span 落盘时间。 |
| 已修复（CHG-20260829-091） | Skill 节点剩余时间没有约束 Tool 调用预算 | `packages/agentflow-runtime/src/agentflow_runtime/skill_executor.py:parallel tool execution`；`tool_policies.py:execute_in_batches` | Tool 在 LLM 后启动，旧调度器没有节点 deadline；视频轮询可达 600 秒，节点硬上限仍为 240 秒 | Tool 进程或 HTTP 请求被底座中途杀掉，`tool.start` 无 `tool.end`，已产生的 Artifact 状态不确定，Skill/batch 轨迹悬挂 | 节点剩余 30 秒时请求 `generate_video(poll_timeout=600)`。修复后业务/未知 Tool 的串行批次在预留落盘时间前被真实中断；当前及尚未启动调用各得到保序的结构化 `node_timeout` 结果，Tool batch、Skill 与 span 均闭合。并行白名单 Tool 的声明 timeout 同时收缩到该 deadline。 |
| 已修复（CHG-20260829-087） | Bridge 把错误轮次的 progress 全量保存为 assistant 历史 | `frontend/bridge/routes/chat_rest.py:SSE error 分支` | 流式路径累计所有进度块；异常分支发送 error 和 `[DONE]` 后仍把整个 `sse_chunks` 交给 legacy store 与 SessionProvider | 下一轮 Coordinator 把调试进度和工具日志当成助手正式回答；历史与轨迹污染 | T03 先输出 `skill still running`，随后超时。修复前该 progress 成为 T03 assistant response；修复后 error 与 transport done 仍进入 `client.delivery` 轨迹，但两套对话历史均不创建这条未完成回答。 |
| 已修复（CHG-20260829-088） | `[DONE]` 的传输结束语义与业务成功语义没有显式分离 | `frontend/bridge/routes/chat_rest.py:SSE error/final 出口` | 正常与异常流都按兼容协议发送 `[DONE]`；成功帧已有 `_final=true`，但错误帧只有 error 对象，没有显式的负终态和 dialogue identity | 只按 sentinel 建树会把失败样本误算为成功，wire 错误难与具体 dialogue 关联 | T03 收到 error 后仍以 `[DONE]` 结束。修复后 `[DONE]` 明确仅表示 transport EOF；error 帧携带同一 `_dialogue_id`、`_final=false`、`_terminal_status=error`，成功只认 `_final=true`，SessionLog 继续记录 `error/done_after_error`。 |
| 已解决（CHG-20260829-092 复核；根因由 CHG-084/085/089/090/091 覆盖） | 第二批执行中断后缺少 Batch 与 Dialogue 终态（原误归因为部分失败重试） | 旧轨迹 `0828173438-8e50bf4cc390`；`src/agent/agent.cpp:HarnessBatchState`；`coordinator_decide.py`；SessionLog Hook | 旧轨迹的第二批 Skill 停在 `llm.request`，没有 `llm.response/skill.end`；活跃 Session TTL、Bridge 等待超时和节点内长调用可先于业务终态切断执行，并非两个 Batch 共用状态或迟到 completion 覆盖 | 活跃分支没有框架 terminal slot 时 ALL-join 自然不能产生第二个 `batch.result`，Coordinator 也没有机会产生 `dialogue.end` | 旧 T03 第一批有 3 个 `skill.end` 和 `batch.result(status=partial)`；第二批 `outfit-stylist` 在第 3 轮 `llm.request` 后消失，实际不是“4 个 start/end 均完整”。当前每轮 UUID batch 独立，C++ 对乱序、部分失败、重复 completion 和中断均填满唯一 terminal slot；生命周期与 LLM/Tool 预算修复保证外部停止也能走结构化终态。 |
| 已修复（CHG-20260829-093） | 产物成功后模型仍进行无必要的 Shell/PIL 验证循环 | `packages/agentflow-runtime/src/agentflow_runtime/prompts/skill.py:SKILL_ARTIFACT_AUTO_COLLECT_PROMPT` | 旧提示只说明平台会自动发布产物，没有把结构化 `status=succeeded + files` 定义为该产物的权威完成条件 | 明显可避免的 Loop、延迟和 token 消耗；安全拒绝被误当业务失败 | `generate_image` 已生成图片后，旧模型又用 `ls/PIL/file` 验证。修复后同一产物不得再经 bash、Python/PIL、file、ls 或 read_file 复核；只有用户要求的其他产物未完成时才继续，全部完成立即返回。 |
| 已修复（CHG-20260829-094） | 已生成 Artifact 在后续失败轮次中被重复持久化 | `packages/agentflow-runtime/src/agentflow_runtime/skill_executor.py:execute/Tool artifact collection` | 每处理一个 Tool 结果都把跨轮累计的 `generated_artifacts` 全量重新送入 `_ensure_artifacts_in_package`；最终文本也在去重前先持久化 | 同一物理文件产生多次相同 `artifact.persist`，并被重复上传；回放可能误判为多次生成 | `generate_image` 首次上传图片，后续 bash 返回安全拒绝却再次上传旧图片。修复后每个 Tool 只持久化本次结果报告的路径；最终文本只补收从未观察过的路径。生成工具真正再次返回同一路径时仍保留覆盖持久化事实。 |
| 已修复（CHG-20260829-095） | Bridge 凭据脱敏日志处理破坏 Uvicorn access record 参数 | `frontend/bridge/services/secret_safety.py:CredentialRedactionFilter.filter` | 通用过滤逻辑先渲染 message 再清空 `record.args`，但 Uvicorn `AccessFormatter` 必须解包原 5 元组 | 每个 HTTP 请求都可能打印 logging traceback；真正请求日志被噪声淹没，轨迹诊断困难 | `/chat` 已执行，access formatter 却报 `expected 5, got 0`。修复后仅 `uvicorn.access` 保留脱敏后的 5 元参数；普通/异常日志逻辑不变。脱敏器不可用时用同形安全占位值 fail-closed。 |
| 已修复（CHG-20260829-096） | Bridge 脱敏后 Uvicorn 普通日志仍显示未展开占位符 | `frontend/bridge/services/secret_safety.py:CredentialRedactionFilter.filter` | 通用分支已渲染 message 并清空 `record.args`，但彩色 formatter 随后又使用仍含 `%s/%d` 的 `color_message` | 启动进程号、监听地址和端口丢失，真实环境难以从日志确认 | Bridge 启动显示 `Started server process [%d]`、`Uvicorn running on %s://%s:%d`。修复后仅移除非 access `uvicorn.*` 记录的失效彩色模板，formatter 使用已渲染且脱敏的正文。 |
| 已修复（CHG-20260829-097） | 最终 LLM 文本可把未观测的旧文件误记为本轮 Artifact | `packages/agentflow-runtime/src/agentflow_runtime/skill_executor.py:最终响应 Artifact 扫描` | 最终文本的正则通道只检查路径存在和后缀；对 Tool 从未报告的路径仍调用持久化管线 | 旧产物被重复上传并挂到错误 Skill/轮次；轨迹把失败或否定回答伪装成新生成事实 | 上一轮已有 `assets/look.png`，本轮生成失败后模型回答“没有生成 assets/look.png”。旧逻辑因文件仍存在而将它加入本轮 Artifact；修复后最终文本只审计未观测路径并记录计数，只有 Tool Result 或 Hook 能新增产物。 |
| 已修复（CHG-20260829-098） | 只读 Tool 正文中的路径可被误记为生成 Artifact | `packages/agentflow-runtime/src/agentflow_runtime/skill_executor.py:Tool Result Artifact 收集`；`artifact_pipeline.py:extract_all_artifacts` | Tool Result 同时使用结构化字段和无来源文本正则；`web_fetch/read_file` 正文里的图片 URL 或旧文件路径与生成工具输出无法区分 | 网页资源、参考图和历史文件进入本轮产物账本；Artifact 数量、归因和回放分支错误 | `web_fetch` 成功返回一篇包含 `https://site/logo.png` 的网页，或 `read_file` 回显旧 `assets/look.png`。旧逻辑把它们当生成产物；修复后 Harness 主链只采信 Tool 的结构化 `files/artifacts/outputs/result/path` 列表，公共 legacy 文本提取 API 保留但不写入账本。 |
| 已修复（CHG-20260829-099） | 空的高优先级 Artifact 字段会遮蔽后续真实产物字段 | `packages/agentflow-runtime/src/agentflow_runtime/artifact_pipeline.py:extract_structured_artifacts` | 实现遇到第一个 list 字段就停止，即使该列表为空；与注释声明的“第一个 truthy 字段”不一致 | Tool 已通过 `files` 返回真实产物，账本却得到空列表；缺少 `artifact.persist`，回放误判为未生成 | Tool 返回 `{"artifacts":[],"files":["outputs/report.html"]}`。旧逻辑在空 `artifacts` 处短路并丢掉 HTML；修复后只在非空 list 上短路，非空字段的既有优先级保持不变。 |
| 已修复（CHG-20260829-100） | `web_fetch` 把 gzip 响应字节当作正文 | `packages/agentflow-runtime/src/agentflow_runtime/executors/host.py:HostExecutor.fetch_url` | HTTP 响应带 `Content-Encoding: gzip`，实现读取后直接按字符集解码，没有先解压；Local Sandbox 的 `web_fetch` 最终复用该 Host 传输 | 模型收到二进制乱码，无法提取页面信息；无效 Loop、Prompt token 和日志体积增加 | 真实请求 `https://www.python.org/` 返回头 `gzip`、正文魔数 `1f8b`，旧链路输出替换字符和控制字节。修复后仅在 gzip 响应上先解压再走原有字符集与 HTML 清理；真实 hchat 返回可读的 `Welcome to Python.org...`。 |
| 已修复（CHG-20260829-101） | 用户输入中的裸凭据不会被脱敏 | `packages/agentflow-runtime/src/agentflow_runtime/security/secrets.py`；`docs/sessionlog/sessionlog_hook.py`；`frontend/bridge/services/secret_safety.py`；`src/daemon/rpc_handler.cpp` | 旧脱敏器主要匹配已注册配置值，未注册的裸 provider key、编码形态和分片流可绕过；原始用户正文还会先进入 C++ 日志 | 凭据可能进入 LLM 出站请求、SessionLog、Span、daemon/Bridge 日志和源码提交 | 修复后按来源名注册任意厂商凭据，并覆盖精确值、常见 provider 形状、URL/Base64/JSON 编码和 SSE 分片；所有持久化/出站边界复用同一策略，源码扫描器只报告元数据。追踪 1713 个文件和未追踪 59 个文件均为 0 findings。 |
| 已修复（CHG-20260829-102） | Sandbox GC 看不到通过 endpoint 发生的真实活动 | `src/daemon/sandbox_lifecycle.cpp:SandboxLifecycle::create`；`tests/integration/sandbox_lifecycle_test.cpp` | Workflow/Tool 直接请求 endpoint，不经过 `Sandbox::exec()`；旧复用分支也不刷新 `last_active_at` | 活跃多轮会话的 Sandbox 可能在 10 分钟处被回收，后续 Skill 继续使用失效 endpoint | 三会话实验曾在约 604 秒被同时 GC。修复后每个外部 user turn 经 `handle_agent_execute → create` 复用时调用 `mark_active()`；定向测试确认同一实例和活跃时间都被刷新，单轮长调用由更短的 Bridge/节点 deadline 闭合。 |
| 已修复（CHG-20260829-103） | Tool 业务失败不会上卷为 Skill 业务状态 | `packages/agentflow-runtime/src/agentflow_runtime/skill_executor.py:Tool result/final branch` | 旧最终分支只要拿到非空 LLM 总结就固定输出 `completed`，没有保存跨轮未恢复 Tool 失败 | `skill.end`、`batch.result` 与真实任务失败矛盾，失败样本可能进入成功训练集 | 修复后按 Tool 名+参数维护未恢复失败账本；最终非空总结仍输出 `tool_error`，同一调用后续成功才清除。Python 正常退出，因此业务失败不改变 Workflow completion，也不取消并发兄弟分支。 |
| 已修复（CHG-20260829-104） | 生成媒体默认写入 Agent 全局目录而非用户会话工作区 | `packages/agentflow-runtime/src/agentflow_runtime/security/trusted_media.py:publish_media`；`common/runtime_context.py:artifact_routing_id` | Host 模式相对媒体路径旧以 Agent 包根解析，没有 user/client 隔离前缀 | 不同用户可覆盖同名图片/视频，磁盘事实无法按 Conversation 独立回放 | 修复后 Host 媒体统一写入 `workspace/{artifact_routing_id}/...`，已有同前缀路径保持幂等，缺少任一身份明确失败；Sandbox 模式继续使用每用户独立实例。 |
| 已修复（CHG-20260829-105） | Artifact 下载路由没有用户级访问控制 | `frontend/bridge/routes/artifacts.py:get_artifact_legacy/get_artifact_file/sign_artifact_url` | 旧 GET 只校验路径和后缀，知道或猜到路径即可下载 | 可直接读取其他用户产物，媒体全局落盘风险升级为跨用户泄露 | 修复后新旧下载路由均要求完整 artifact id 的持久化 HMAC capability；无签名或错误签名返回 403。聊天交付与历史回放统一补签，签名查询串不进入普通日志。 |
| 已修复（CHG-20260829-106） | `{user_id}_{client_id}` 路由键存在身份碰撞 | `packages/agentflow-runtime/src/agentflow_runtime/common/runtime_context.py:artifact_routing_id` | user/client 都允许下划线，直接拼接不能唯一反解两部分 | 两组不同身份可共享同一物理 Artifact/媒体目录并串写 | `user=a_b, client=c` 与 `user=a, client=b_c` 旧值都为 `a_b_c`；修复后歧义身份使用长度前缀 v2 编码，无下划线组合保持兼容，最大合法输入仍不超过文件系统单段上限。 |
| 已修复（CHG-20260829-107） | `sandbox-mode=none` 启动时解引用空 SessionManager | `src/daemon/daemon.cpp:Daemon::initialize` | `session_manager_` 只在启用 Sandbox 时构造，RpcHandler 随后却无条件接收其引用 | 本地无 Sandbox 模式在 daemon 初始化阶段崩溃，无法聊天或采集轨迹 | 修复后只把 SessionManager 构造移到条件分支前；SandboxLifecycle/GC 仍按 mode 创建，未改变底座协议。Daemon 定向与完整 CTest 已覆盖。 |
| 已修复（CHG-20260829-109） | 相同 dialogue_id 的 `client.delivery` 可能写入另一用户文件 | `frontend/bridge/services/sessionlog_delivery.py:_sessionlog_path/append_bridge_delivery`；`routes/chat_rest.py` | Bridge 旧逻辑在整个输出根按 dialogue 文件名全局搜索，没有使用请求已有 user/client 身份 | 最终交付事实、响应哈希和父事件跨用户串线，轨迹树污染且构成隐私问题 | 两用户各有 `shared-dialogue` 时，旧逻辑取排序后的第一份文件；修复后正式调用精确定位 `by-user/{user}/{client}/{dialogue}.jsonl`，旧布局也只在同一 user 下回退，找不到即 fail-open 不写。 |
| 已修复（CHG-20260829-110） | 流式脱敏器会长时间吞住正常短文本 | `packages/agentflow-runtime/src/agentflow_runtime/security/secrets.py:SecretStreamRedactor` | 旧实现按最长已注册凭据长度固定保留流尾；只要配置中存在长 Key，普通中文 delta 也要等后续 chunk 或 flush 才能释放 | hchat 看起来长时间无响应；progress 与真实模型输出时间失真 | 配置一个长 provider key 后流式返回“你好”。旧实现首个 chunk 输出为空；修复后只保留“可能继续组成某个已注册凭据”的最长尾前缀，无关文本立即释放，跨 chunk 凭据仍完整脱敏。 |
| 已修复（CHG-20260829-111） | 开发环境与回归测试未声明 Runtime/uv 的真实依赖 | `environment.dev.yml`；`frontend/bridge/pyproject.toml`；`frontend/bridge/uv.lock`；相关旧测试 | Bridge 已复用 Runtime 的安全与路由契约，但独立环境没有安装本地 Runtime；旧测试仍使用被安全边界禁止的远程明文 HTTP 占位地址 | 新开发环境缺模块或测试假红，真正回归被噪声遮蔽 | 全新环境运行 Bridge 测试时导入 `agentflow_runtime` 失败，或测试在发出 mock 请求前因 `http://x/v1` 被拒绝。修复后 Bridge 显式 editable 依赖本地 Runtime，环境包含 uv，旧 mock 仅改为 loopback HTTP。 |
| 已修复（CHG-20260829-112） | Local Sandbox E2E 未显式声明危险凭据工具测试权限 | `tests/e2e/conftest.py:local_sandbox` | 安全默认值已禁止本地进程读取宿主凭据，旧 E2E 子进程仍假设该能力默认开启 | 正式默认值正确但 E2E 启动失败；若全局放宽又会污染真实运行安全边界 | 修复后只给测试创建的 Sandbox 子进程注入并启用 `AF_ALLOW_UNSAFE_LOCAL_SECRET_TOOLS`，父进程、daemon、Bridge 和生产配置均不改变。 |
| 已修复（CHG-20260829-113） | 用户输入在 Runtime 脱敏前已进入 daemon 日志边界 | `frontend/bridge/client.py:DaemonClient.agent_execute` | Runtime DLP 位于 daemon 之后；Bridge 旧实现把原始 `agent.execute` params 直接发送给 C++，daemon 生命周期日志可先观察到正文 | 即使 SessionLog 和 LLM 出站安全，用户粘贴的任意厂商 Key 仍可能出现在 daemon/skill stderr | 修复后 REST、WebSocket 和直接调用共用的 `agent_execute` 边界先对参数副本脱敏，再发送给 daemon；原调用对象不变。真实 canary 在 daemon、Bridge、Skill stderr、SessionLog 中均为 0 次精确命中。 |
| 已修复（CHG-20260829-114） | Bridge restart 可能保留旧 worker 并继续运行旧代码 | `scripts/start_bridge.sh:_is_bridge_pid/_resolve_pid/_wait_port_free` | PID 文件记录 wrapper 而非实际 Uvicorn worker；fallback 命令特征过窄；机器没有 lsof 时又总把占用端口判断为空闲 | “已重启”后请求仍进入旧进程，代码修复与日志结果矛盾；新进程因端口冲突启动失败 | 修复后优先用 fuser 找真实端口 owner，并以 cwd/app-dir 校验属于本项目；端口释放检测改用 Bash `/dev/tcp`。真实 restart 成功停止旧 worker 并释放 8199。 |
| 已修复（CHG-20260829-115） | Bridge SQLite 会保存原始用户凭据，且脱敏器故障时会丢整轮 | `frontend/bridge/providers/local_session_provider.py:save_dialogue` | SessionLog/daemon 已脱敏不代表持久会话库安全；旧 sink 直接写 `user_input`；首次修复的 fail-closed 分支又直接跳过该轮 | 凭据长期留在 `data/sessions.db`，或安全组件异常时多轮上下文与回放断层 | 修复后只在 SQLite sink 脱敏不可信 `user_input`，不改带签名 Artifact 的 assistant/artifact 字段；脱敏异常时保存固定安全占位符而非原文或丢轮。真实新 canary 在 DB 为 0 次精确命中。 |
| 已修复（CHG-20260829-116） | SessionSync 把 `agent.list` 对象响应当列表迭代 | `frontend/bridge/services/session_sync.py:AgentflowSessionSync.sync` | daemon 实际返回 `{"agents":[...]}`，旧代码直接 `for agent in agents`，得到字符串键 `"agents"` 后调用 `.get` | Bridge 每 30 秒打印一次异常，Session 状态长期不同步，长时运行日志被污染 | 修复后同时接受列表和 `agents` 包装对象并跳过非对象项；连续多个同步周期无新错误。 |
| 已修复（CHG-20260829-117、CHG-20260829-120） | `batch.result.status` 混用 `true/false/partial`，缺少可直接序列化的统一枚举 | `docs/sessionlog/sessionlog_hook.py:handle(coordinator.batch_result)` 及两份正式挂载副本 | 框架原始 `AF_SKILL_SUCCESS` 布尔字符串和多分支 `partial` 被原样复用为同一 status 字段；首次新增的 `workflow_status` 又错误沿用了这个业务聚合值 | Protobuf 必须把布尔字符串和业务状态混进同一个枚举；业务失败会被误写成 Workflow failed | 现在保留 raw `status` 兼容，`workflow_status` 只按 `results[].workflow_success` 聚合；图片成功、视频 `tool_error` 时结果为 `status=partial`、`workflow_status=completed`，业务失败不会污染进程闭合事实。 |
| 已修复（CHG-20260829-119） | 已脱敏的嵌套 JSON 再次脱敏时会破坏 JSONL 转义 | `packages/agentflow-runtime/src/agentflow_runtime/security/secrets.py:_redact_text`；SessionLog 最终写入边界 | Hook 先递归脱敏 payload，再对序列化后的整行二次脱敏；赋值正则把转义引号前的反斜杠吞进 value，替换后留下裸引号 | 第二轮 `dialogue.start` 无法解析，Reader 拒绝整条轨迹；多轮历史树从第二轮开始断裂 | 首轮响应含签名 Artifact URL，第二轮把历史嵌入 messages JSON；旧行转义损坏。修复后保留奇数尾反斜杠并让 placeholder 幂等；10 个新 dialogue、397 个事件全部直接解析，0 个 Reader 问题。 |

## 五、建树前基线问题最终核销

> 前三类表保留“发现问题时”的原始描述。本节按原表顺序逐项给出最终状态；原始基线 31/31、本轮全面验收新增问题 2/2 均已核销，合计 33/33，没有把“未被索引到”当作安全证明。

### 第一类：执行结果与流程终态（15/15）

1. Tool Call 语义恢复不重试——已修复（CHG-20260826-003）。
2. Skill 逻辑失败被上报成功——已修复（CHG-20260826-005、CHG-20260827-013、CHG-20260829-103）。
3. 并行 Skill 共用结果键——已修复（CHG-20260826-007、CHG-20260827-057）。
4. 处理一项 completion 清掉兄弟 completion——已修复（CHG-20260826-007）。
5. final 绕过 pending Skill 屏障——已修复（CHG-20260826-007、CHG-20260827-021）。
6. Dispatch 非事务且忽略写入结果——已修复（CHG-20260827-010、CHG-20260827-056）。
7. `run_skills` 缺少运行时校验——已修复（CHG-20260827-033、CHG-20260827-041）。
8. Hook 阻断项仍记为已调度——已修复（CHG-20260827-010）。
9. 重复调度只看最近完成项——已修复（CHG-20260827-034）。
10. Manifest 丢失后执行 `unknown-skill`——已修复（CHG-20260827-035）。
11. `after_tool` 改写不校验数量和身份——已修复（CHG-20260827-036）。
12. 空最终文本仍标 completed——已修复（CHG-20260827-037、CHG-20260827-049）。
13. Timeout/Max Rounds 返回不可靠末条消息——已修复（CHG-20260827-038、CHG-20260827-062）。
14. SpanWriter 写失败打断业务——已修复（CHG-20260827-039）。
15. Tool 实现覆盖后保留旧 schema——已修复（CHG-20260827-040）。

### 第二类：轨迹关联、并发与回放（10/10）

1. Skill completion 丢失 dispatch 身份——已修复（CHG-20260827-057）。
2. 同批 skill-runner 的 task_id 冲突——已修复（CHG-20260827-057）。
3. LLM trace index 并发读改写丢项——已修复（CHG-20260827-018）。
4. `emit_event` 未进入 NDJSON——已修复（CHG-20260827-023）。
5. Tool Call/Result 缺少持久终态——已修复（CHG-20260827-024）。
6. Recorder 值与 Hook 后执行值不同——已修复（CHG-20260827-025）。
7. 最终响应只聚合最近 Skill Artifact——已修复（CHG-20260827-026）。
8. Hook 原地 append Artifact 不回写——已修复（CHG-20260827-022）。
9. LLM Recorder 缺少执行身份——已修复（CHG-20260827-027、CHG-20260827-069）。
10. SpanWriter 进程内锁不能保护并行进程——已修复（CHG-20260827-028）。

### 第三类：消息协议与统计口径（6/6）

1. Skill completion 被注入为 `role=user`——已修复（CHG-20260827-029）。
2. `run_skills` Tool ACK 与真实执行结果混淆——已修复（CHG-20260827-029）；ACK 保留为异步 dispatch receipt，真实 completion 通过原 tool_call 与 Batch terminal 独立关联。
3. Dialogue Start/End 实为 Coordinator 激活边界——已修复（CHG-20260827-030）。
4. Coordinator round 混入 completion 次数——已修复（CHG-20260827-052）。
5. Workflow JSON 与 Python stdout ABI 不一致——已修复（CHG-20260827-031）。
6. LLM transport retry 数固定为 0——已修复（CHG-20260827-032）。

### 第四类：全面验收新增问题（2/2）

1. 嵌套 JSON 二次脱敏破坏 JSONL 转义——已修复（CHG-20260829-119）。
2. `workflow_status` 误用业务聚合状态——已修复（CHG-20260829-120）。

## 六、独立进化组件冷启动核对新增问题

以下属于 trajectory_evolution 组件，不改变前述 Harness 历史问题的核销结果。

### 2026-09-14：真实 L4 搜索核对

| 状态 | 问题 | 位置 | 诱发原因 | 可能导致的结果 | 具体场景 |
| --- | --- | --- | --- | --- | --- |
| 已加搜索防回归检查；真实收益待复测 | 答对率或有界 fitness 提高掩盖原始无效 loop 上升 | `tools/trajectory_evolution/src/gepa_search.cpp` 的 `scores/improves`；`gepa_strategy.cpp` 的 `no_worse` | 搜索优先通过率，loop 成本经过饱和归一化，而发布要求原始均值下降 | 花费大量试验后仍无法通过最终门槛 | L1 holdout：基线 13/15 通过、loop 均值 1.1333；候选 15/15 通过、loop 均值 1.4，未发布。现同样本比较增加原始均值不得上升的检查，holdout 门槛不变。 |
| 已收紧反思约束；语义效果待真实复测 | 经验正文漂移到与来源触发条件不一致的场景 | `config/prompts/gepa-reflection.txt`、`gepa_search.cpp::packet`、`config/gepa.json`（均在进化组件） | 强制轮换组件可能与当前训练题不相关；多代改写只见当前正文 | 错误停止、额外重试、经验与来源错配 | 视频 UnsupportedModel 的经验被改为图片超时重试，触发条件仍为视频错误。现按证据选择组件，必要时展示原文约束，允许无改动，不把可修正参数错误一律终止。 |
| 已区分固定约束与实际搜索维度（CHG-258） | 标为 L2 的搜索实际没有动作规则生成 | 服务配置 `control_learning`；`gepa_search.cpp::run` | max_level/宿主 actions 能力不等于开启控制学习；原实验缺少 control_learning | L2 仍只改经验文本，连续候选淘汰被误解为控制规则执行失败 | 原 L2 的种子及 5 个候选均无 action_rules。现 available_components 列可变维度，fixed_controls 列自动读取的必填参数检查；默认无需工具名单，显式扩展规则仍受授权约束，不将固定检查当成 GEPA 变异。 |
| 已修复合并前筛选 | 互补分支因平均成绩较低而没有融合机会 | `tools/trajectory_evolution/src/gepa_strategy.cpp::select_merge` | 原要求两个分支整体质量/fitness 均不低于共同祖先，与逐任务前沿保留局部专长矛盾 | merge_attempts 一直为空，无法结合不同分支优势 | L1 节点 3、4 位于不同分支，节点 4 平均低于种子而被挡住；现每个分支有逐任务改善即可尝试，融合子节点仍须真实评测不差于双亲。 |
| 已修复（CHG-20260914-259） | 离线冷启动生成私有经验但公共经验为空 | `integration_service.cpp::public_evidence`、`shared_memory.cpp` | 原先一律排除 training_source，缺少与真实用户分开的训练支持规则 | 公共提炼不执行，纯训练冷启动无法验证公共管理 | 现核验冻结 train 任务、L0 无注入轨迹和来源指纹，按不同任务组计票；真实 API 完成 28 次提炼/复核/关系调用，产生 2 条 published、2 条 pending_support，真实用户支持均为 0。候选策略仍需真实 Agent 评测。 |

### 2026-09-14：通用接入检查（CHG-20260914-258）

| 状态 | 问题 | 位置 | 诱发原因 | 可能导致的结果 | 具体场景 |
| --- | --- | --- | --- | --- | --- |
| 已修复 | 需要重复填写工具和必填参数名单 | `model_contract.cpp`、`boundary_runtime.cpp` | 仅接受显式动作规则，没有读取实际请求中的工具定义 | 换工具需同步维护配置，名单易过时 | 请求已声明工具 value 必填，过去仍要再配置；现在由模型/动作点读取并校验，可选字段不拦截，缺定义或冲突保持未知。 |
| 已修复 | L3 循环搜索依赖动作名单 | `gepa_search.cpp::components/compose` | 动作和循环维度放在同一名单条件下，组合代码也假定动作维度存在 | 只授权循环优化却无法生成循环候选 | 不配置任何 actions、有基础 loop 上限时，现仍能独立生成并评测 loop_rules。 |
| 已修复常见格式；保留显式覆盖 | 模型注入和用量依赖重复字段映射 | `model_contract.cpp`、`boundary_runtime.cpp` | 模型点不解析已有结构元数据 | 没填 binding 无法注入，没填 token 映射则计数缺失 | 普通 messages 请求与 usage 响应无需再填映射；不认识的格式可在原打点给 binding/metrics，非法计数保持未知。 |
| 已修复 | Python 配置入口不能直接包装异步调用 | `clients/python/evolution_configured.py` | 底层有异步方法，配置门面只暴露同步方法 | 用户需另外管理客户端或拆开 before/after | 原 async 模型调用现在可用 acall，异步循环可用 aallow_loop；未启用时原函数仍只执行一次。 |
| 已修正文档 | 推荐评测入口与旧 stdio 协议混写 | `docs/19-datasets-and-benchmarks.md`、`03-configuration.md` | 算法说明将旧 stdin/stdout 协议写成所有执行方式的共同要求 | 用户以为必须另写 agent.py 启动器 | entry 只绑定已有收消息函数和原启动命令；投递、等待 run 完成、汇总均由包处理。任务 ID、业务结果、权限和验收标准仍由原系统提供。 |

以下保留此前冷启动问题记录：

| 状态 | 问题 | 位置 | 诱发原因 | 可能导致的结果 | 具体场景 |
| --- | --- | --- | --- | --- | --- |
| 已修复（CHG-20260910-180） | 评测依赖用户自写宿主或 v1 专用启动器 | `tools/trajectory_evolution/src/task_evaluation.cpp`、`evaluation_entry.h`、各语言客户端 | 旧协议只接收能读取 stdin 任务包的程序，没有包内的原入口绑定与逐轮投递 | 换框架须重写驱动；异步返回可能被误当作完成 | 现将已有消息接收函数交给包内 evaluate；原函数只负责投递任务也可，组件等待真实 run.end 再发下一题。专用 v1 驱动已移除。 |
| 已修复（CHG-20260910-180） | 原生打点与新入口评测试验的采集服务争用目录锁 | `tools/trajectory_evolution/src/local_runtime.cpp` | 评测器已持有采集服务，原生客户端又按试验配置打开同一目录 | 原生客户端初始化失败、没有可评分轨迹 | 仅新入口评测模式转发到试验服务；普通原生模式仍本地采集，已添加原生打点评测回归。 |
| 已修复（CHG-20260910-176） | 无策略返回仍把整数改成带小数的 JSON 记号 | `src/integration_json.h`、`local_runtime.cpp`、`integration_service.cpp`、`local_c.cpp`（均在 tools/trajectory_evolution） | 内部 double 规范编码被直接用作宿主请求返回 | 下游严格整数参数校验可能拒绝请求 | 输入 max_tokens=4096，原返回为 4096.0；现只调整对外编码，旧落盘指纹不变。前次 HTTP 400 的正文未保留，未断言这是唯一原因。 |
| 已修复（CHG-20260910-176） | 评测子进程异常退出后直接丢弃已采集证据 | `tools/trajectory_evolution/src/task_evaluation.cpp` | 根据进程错误提前返回，尚未读取采集目录就清理工作区 | 报告仅有 runner_nonzero_exit，无法定位失败阶段或核对已发生调用 | Agent 完成模型打点后异常退出：现保留安全、版本一致的完整轨迹；不完整轨迹单列 failure_capture，不改变原错误判定。 |
| 已修复（CHG-20260910-176） | 模型/自定义活动混入业务工具指标 | `tools/trajectory_evolution/src/trace_analyzer.cpp` | 把全部 action.start 转成 ToolActionSummary，不区分 semantic 和明确未执行动作 | 工具调用次数、失败率失真，同名模型与工具可能混判为重试 | 实际只有 1 次失败 LLM、0 次工具，旧报告 tool_call_count=1；现按语义和执行标记计数，模型证据与重复调用诊断仍保留。 |
| 已修复（CHG-20260910-177） | 评测模型关闭 thinking 不等于被测 v1 也关闭 | `packages/agentflow-runtime/src/agentflow_runtime/common/clients/openai_compatible_client.py` | 两者使用独立模型请求配置，v1 流式请求沿用原默认值 | 真实 Agent 的时延和推理消耗与实验设置不一致 | 评测配置 disabled，但 Coordinator 仍启用；现在可用 AF_LLM_THINKING_MODE 显式设置，未设置保留原行为。 |
| 已修复（CHG-20260910-178） | 正常多轮任务被评测器拒绝 | `tools/trajectory_evolution/src/task_evaluation.cpp`、`agent_evaluation.cpp` | 一个 trial 被限定为一个 Run，而 v1 每次用户输入有独立 dialogue/Run | 多轮无法评分；若仅取一轮则漏计调用和策略应用 | 三轮修改图片任务完整产生三个 Run，旧逻辑拒绝；现在逐条校验和保存，并汇总完整指标、遍历各轮策略回执。 |
| 已修复（CHG-20260910-178） | 异步多轮在两轮之间提前收尾 | `tools/trajectory_evolution/src/task_evaluation.cpp`、`integration_service.cpp`、`integration_http.cpp` | 派发器已退出且当前所有 Run 已关闭，但后续轮次尚未创建 | 清理进程组会杀掉准备下一轮的 worker，偶发缺轮或不完整轨迹 | worker 完成第一轮后暂停 200ms 再开启第二轮；现在 per_input 配置预先声明整项任务轮数，收到完整数量再结束。 |
| 已修复（CHG-20260910-179） | 缺少终态产物声明时无法独立核验文件；产物失败丢失多轮分析 | `tools/trajectory_evolution/src/task_evaluation.cpp` | 文件声明仅依赖 Run 输出，且验证结果延后到文件通过后写入报告 | 回复称已保存但文件未经独立读取；缺文件时多轮轨迹详情被漏存 | 原 Agent 只返回路径、不填 artifacts：现可在评测配置声明 outputs/report.json；文件缺失会验收失败，已验证的完整轨迹仍保留。 |


### 2026-09-14：GEPA 评分续跑（CHG-20260914-260）

| 状态 | 问题 | 位置 | 诱发原因 | 可能导致的结果 | 具体场景 |
| --- | --- | --- | --- | --- | --- |
| 已修复并经真实续评验证 | 一批题目部分评分失败时，重试可能重新执行已完成的 Agent | tools/trajectory_evolution/src/gepa_search.cpp 的 Search::evaluate | 复用成功题目后，把评分失败题目拆成新配置，绕过原来的完整执行检查点 | 重复工具副作用、额外 API 消耗与等待，执行轨迹被不必要地重采 | 真实 v1 中 t-10、t-22 已执行完成，但评测请求超时。修复后仅重评两题；四题原始轨迹、耗时及调用指标完全不变。传输重试次数与判分规则不变。 |
| 诊断已补充，偶发缺轮根因待确认（CHG-20260914-261） | run 打点降级后缺少直接错误提示 | clients/python/evolution.py 的 Evolution.point（自进化包内） | 业务调用保持运行，调用处未检查 fail-open 返回值 | 评测等不到全部 Run，直至整题超时 | 一次 t-14 已执行第三轮，但只采集两轮；同题复现三个 Run 均完整闭合并通过续评。现仅记录安全固定错误码便于定位，不将未复现的根因标为已修复。 |
| 能力已补齐，真实收益复测中（CHG-20260914-262） | GEPA 无法尝试停用无收益或有干扰的经验 | src/gepa_search.cpp 的 components/compose/packet（自进化包内） | 经验组件只允许非空正文，搜索只能反复改写原文 | 明显无收益的经验仍进入每个候选，搜索空间受限 | 改写两版经验后验证表现变差，却无法比较不注入它们。现可用 content:null 停用种子批准的来源，后续同一 ID 可恢复；不删除记忆或放松基础规则。 |
| 已修复并通过定向回归（CHG-20260914-263） | 临时评分超时使有价值的 GEPA 候选跳过验证 | src/gepa_search.cpp 的 Search::evaluate（自进化包内） | 一次完整训练批次存在评分错误时直接返回 inconclusive，没有使用已保存执行检查点自动补评 | 正常候选因评分基础设施短暂不可用而失去验证机会 | 真实候选四题全部完成，三条评分超时；同轨迹重评四题通过、无效 loop 为 0。现仅针对临时评分错误最多补评一次，不重新执行 Agent；429、权限或业务错误不进入自动重评。 |
| 已修复并补导出真实版本（CHG-20260915-264） | 固定候选发布后没有自动生成离线策略包 | 自进化包 src/agent_evaluation.cpp 的发布分支及启动恢复 | 版本包导出只在 gepa=true 时执行，重启也未补齐缺失文件 | 已验收策略不能直接复用，用户需要手工拼装离线文件；发布后中断也可能留下同样缺口 | 最终真实 v1 的 L3 固定候选已通过 15 题验收并登记发布，但缺少 published-version.json。现固定候选自动导出，重启核验原发布记录后补齐；失败或未发布候选不导出，旧上下文不自动获新准入。 |
| 已修复并通过独立导入验证（CHG-20260915-265） | 单文件策略导入仍依赖旧配置和经验库，评测又绕过该依赖 | 自进化包 src/agent_evaluation_support.h、integration_service.cpp、local_runtime.cpp、task_evaluation.cpp | 离线加载先读取当前记忆和模板、编译策略，再比较整体选项；内联 artifact 评测直接替换策略 | 导出文件看似完整，换成干净部署却降级 L0，旧评测不能证明迁移有效 | 同一个真实 L3 包只因当前默认配置多出请求绑定选项即被拒绝。现离线直接使用包内快照/参数/模板，校验当前身份、能力和限制，不读取旧记忆；评测新增 policy_version_file 走相同离线路径。无旧配置和经验时实际包已成功注入，新任务收益另行验证。 |
| 已修复，97 项回归及真实评分通过（CHG-20260915-266） | 去掉 LLM 请求后仍因重复策略诊断超过评分输入上限 | 自进化包 src/model_input.h 的 task_evidence_view | 每轮 policy.query 重复携带检索查询、经验和注入上下文 | Agent 已正常结束，但无法完成模型评分 | 两轮 JSON 文件修改的 L3 轨迹完整，评分却报 input_limit；现只精简模型输入副本中的检索载荷，原轨迹保留，两次真实评分正常完成并保留业务失败。 |
| 已修复并通过 RPC 及真实多轮复测（CHG-20260915-267） | 历史文本里的不配对括号导致下一轮 RPC 参数被截断 | src/daemon/rpc_server.cpp 的 extract_json_params | 手工括号计数没有跳过 JSON 字符串 | 合法多轮请求被拒绝，下一轮没有进入 Agent，评测入口失败 | 第一轮解释去掉 layers:2,} 的尾逗号，第二轮带上该历史时多出的右括号被当成 params 结束。现复用 RapidJSON 正确解析；UDS 回归和真实 L0/L3 各两次复测通过，8 个 Run 全部闭合并评分，业务失败仍保留。 |
| 已修复，定向回归通过（CHG-20260915-268） | 评分输入去重未覆盖重复输出和注入回执，CLI 缺少续跑入口 | 自进化包 src/task_evaluation.cpp 的 compact_judge_evidence、task_evaluation_cli.cpp | 只处理事件 input，多轮结束事件重复携带大段结果；底层已有的 resume_report 未接到命令行 | 三轮采集完整仍无法评分；暂停后可能不必要地重跑已有 Agent | 真实三轮 L3 任务完成 14 次 LLM 调用却评分超限。现无损共享重复输出/回执，并通过 --resume-report 复用已完成通过/失败，评分失败只补评；源轨迹、ID、耗时和文件哈希保持不变。 |
| 已修复，容量及续跑回归通过（CHG-20260915-269） | 报告预算被提前减半，最终序列化未遵守较小配置 | 自进化包 src/task_evaluation.cpp 的 run_task_evaluation | 累计证据比较 max_report_bytes/2，最终 JSON 只检查固定 128 MiB | 大批评测可能在报告尚未达到配置上限时终止；小预算也不能严格约束最终文件 | 120 次试验若累计证据超过约 64 MiB，旧实现会在未达 128 MiB 配置值时拒绝；本轮在达到该阈值前修正。现使用完整预算，最终 JSON/Protobuf 均检查配置上限；已完成试验通过检查点复用，不重复 Agent 执行。 |
| 已补输入来源与验收规则，目标案例复核一致（CHG-20260915-270） | 模型裁判对同一题的文件核验标准不一致 | 自进化包 task judge 的模型输入与提示词 | 裁判将评测器事后检查或内部 Skill 说明视为 Agent 已核验，或以未配置 required_checks 免除文字标准 | 等价执行被判为一通过一失败，成功率差异不一定代表策略退化 | h-image-english 两版均只生成图片，没有实际文件核验；现输入明示 artifact_provenance，提示词明确内部规范不能减免必需动作，两条最终复核均判失败。原报告与复核均保留；模型语义仍需持续复核。 |
| 已补原始事实计数并真实复核（CHG-20260915-270） | 裁判理由中的工具次数与原始事件不一致 | 自进化包 src/task_evaluation.cpp 的 model_grade | 模型按 Skill 重派或共享引用概括，未准确统计实际调用 | 低估重试次数，错误反馈影响进化方向 | t-04 的 L0 实际 5 次 generate_video，旧理由写 3 次；现提供严格配对得到的 observed_action_counts，真实复核准确识别 5 次。失败仍计数，未执行不计数，缺少执行标记保持 null。 |
| 输入排序与评审顺序已修正，目标案例复核通过（CHG-20260915-270） | 业务 verdict 与逐轮违规理由互相矛盾 | 自进化包 task judge 与 task-loop-review 提示词 | 提前输出 verdict 后才核验逐轮贡献，清单又按哈希乱序；把重派发视为新目标首次探索 | 违反停止条件的反复执行仍判成功，loop 收益失真 | t-07 的 L0 失败后继续调用视频，共 8 次，曾被判通过。现清单按观测顺序、最终汇总 verdict；该轨迹复核判失败。并发仍用执行身份与 caused_by，不把排序当作依赖。 |
| 已修复并用于真实诊断（CHG-20260915-271） | 暂停大批测试后不能仅重评已完成题 | 自进化包 run_task_evaluation | 重评要求整个源批次完整 | 为诊断一题被迫先完成其他题或重跑 Agent，增加成本 | 120 次计划执行中暂停后，两版目标轨迹已完成；现显式 task_ids 可仅重评这两版，缺题/取消仍拒绝，原始轨迹及耗时不变。 |
| 已增加可选 Pareto 筛选，真实搜索中（CHG-20260915-272） | 中间整体门槛过严，局部优势分支难以保留或融合 | 自进化包 Search::run/merge、gepa::select_merge | 候选在进入完整验证前须整体质量、fitness、loop 全不退化 | 局部改进在融合前被剪掉，最后只能留下简单单方向策略 | A 改善网络题，B 改善循环耗时，但各自在其他题暂有退化；现 acceptance=pareto 可让局部优势继续验证和重组，最终三项发布门槛仍独立检查。 |
