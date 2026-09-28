```mermaid
%%{init: {"layout": "elk", "theme": "base", "look": "classic", "themeVariables": {"fontFamily": "Noto Sans CJK SC, Microsoft YaHei, Arial, sans-serif", "fontSize": "16px", "lineColor": "#64748b", "primaryTextColor": "#0f172a"}, "elk": {"mergeEdges": false, "nodePlacementStrategy": "BRANDES_KOEPF", "nodePlacementAlignment": "LEFTUP", "considerModelOrder": "NODES_AND_EDGES", "forceNodeModelOrder": true, "cycleBreakingStrategy": "GREEDY_MODEL_ORDER"}, "flowchart": {"curve": "linear", "nodeSpacing": 38, "rankSpacing": 58, "padding": 18, "wrappingWidth": 260, "useMaxWidth": true}, "themeCSS": ".nodeLabel div {white-space:normal;overflow-wrap:anywhere;} .nodeLabel .body {width:270px;text-align:left;line-height:1.5;} .nodeLabel .store {width:220px;line-height:1.5;text-align:center;} .node b {font-weight:650;} .cluster-label {font-size:21px;font-weight:700;} .edgeLabel {font-size:14px;}"}}%%
flowchart TB
%% 实线：调用和数据流；无箭头虚线：补充字段/规则；回路表示后续执行或版本反馈。
subgraph PERIPHERY["01 · 外围接入｜同一 point 语义，两种传输方式"]
direction TB
subgraph M1_INPUT["边界请求：身份、因果关系与输入"]
direction LR
POINT_KIND["<div class='body'><b>① 确定边界与类型</b><br/>phase：before / after / abort / guard 等<br/>kind：run / model / action / loop 等<br/>before 请求策略；after / abort 回报实际执行事实</div>"]
POINT_REF["<div class='body'><b>② 使用宿主已有 ID 建立关系</b><br/>ref.run：任务；ref.key：本次活动或循环<br/>ref.parent：父活动；ref.after：已完成的直接依赖<br/>同一次调用前后共用 key；重试作为新调用使用新 key</div>"]
POINT_DATA["<div class='body'><b>③ 传入当前边界的数据</b><br/>input：待执行请求；state：当前可观察状态<br/>结束点：output / actual_input / executed / status<br/>任务开始可传 conversation_id；scope 标识策略作用域</div>"]
POINT["<div class='body'><b>④ 调用统一 point(fields)</b><br/>SDK 封装 operation=point 与 request_id<br/>call / acall 可包住原调用并自动上报前后边界<br/>原生或 HTTP 是两种传输方式，采用相同 point 语义</div>"]
POINT_KIND --> POINT_REF
POINT_REF --> POINT_DATA
POINT_DATA --> POINT
POINT_REF_PART_2["<div class='body'>组件据此关联并发分支、因果顺序与任务收尾</div>"]
POINT_REF -.- POINT_REF_PART_2
POINT_DATA_PART_2["<div class='body'>自定义模型结构可用 binding 指明注入字段</div>"]
POINT_DATA -.- POINT_DATA_PART_2
DEPLOY_CONFIG["<div class='body'><b>部署配置 boundary-service.json</b><br/>principals：user_id + token_env<br/>instrumentation：level + capabilities<br/>data_dir / 策略规则 / 后台学习参数</div>"]
POINT -.- DEPLOY_CONFIG
end
subgraph INTERFACES["两种并列接入方式"]
direction TB
TRANSPORT_CALL["<div class='body'><b>本次边界调用 point(fields)</b><br/>按部署选择原生或 HTTP；采用相同字段和语义</div>"]
subgraph NATIVE_INTERFACE["1.1 · 原生接口｜宿主进程内"]
direction TB
NATIVE_STEP_1["<div class='body'><b>每次边界调用</b><br/>LocalEvolution.point(fields)<br/>复用句柄，携带当前用户 token</div>"]
NATIVE_STEP_2["<div class='body'><b>LocalEvolution.invoke(point, fields)</b><br/>补 operation / request_id<br/>编码 UTF-8 JSON</div>"]
NATIVE_STEP_3["<div class='body'><b>进入 C ABI</b><br/>te_local_invoke(handle, request, length)</div>"]
NATIVE_STEP_4["<div class='body'><b>LocalRuntime::invoke(request)</b><br/>关联本实例的 run / activity / loop<br/>维护并发活动与因果关系</div>"]
NATIVE_STEP_5["<div class='body'><b>处理本次 point</b><br/>before / guard：准备 input / decision<br/>after / abort：记录实际结果</div>"]
NATIVE_STEP_6["<div class='body'><b>生成返回与采集状态</b><br/>内存中更新状态和事件<br/>SDK 读取响应 JSON</div>"]
NATIVE_STEP_7["<div class='body'><b>释放缓冲并检查结果</b><br/>te_local_free<br/>检查 ok，取出 result</div>"]
NATIVE_STEP_8["<div class='body'><b>同步返回宿主</b><br/>采用 input / decision<br/>继续原来的 Agent 执行流程</div>"]
NATIVE_STEP_1 --> NATIVE_STEP_2
NATIVE_STEP_2 --> NATIVE_STEP_3
NATIVE_STEP_3 --> NATIVE_STEP_4
NATIVE_STEP_4 --> NATIVE_STEP_5
NATIVE_STEP_5 --> NATIVE_STEP_6
NATIVE_STEP_6 --> NATIVE_STEP_7
NATIVE_STEP_7 --> NATIVE_STEP_8
subgraph NATIVE_OPERATIONS["初始化、独立后台与生命周期"]
direction TB
NATIVE_NOTE_1["<div class='body'><b>初始化一次 · Python SDK</b><br/>LocalEvolution(library, config, token)<br/>ctypes.CDLL 加载库</div>"]
NATIVE_NOTE_2["<div class='body'><b>创建原生运行时</b><br/>te_local_open 创建句柄与 LocalRuntime<br/>按 background 选项启动线程</div>"]
NATIVE_NOTE_3["<div class='body'><b>独立后台 · writer</b><br/>保存开始标记与结束状态<br/>不在同步返回链上</div>"]
NATIVE_NOTE_4["<div class='body'><b>独立后台 · learner</b><br/>按批次学习并刷新记忆<br/>run.end 可先返回 persisted=false</div>"]
NATIVE_NOTE_5["<div class='body'><b>按需维护</b><br/>metrics：读取计数<br/>manage：管理；drain：维护落盘</div>"]
NATIVE_NOTE_6["<div class='body'><b>应用关闭</b><br/>停止所有业务调用后 close<br/>调用 te_local_close</div>"]
NATIVE_NOTE_7["<div class='body'><b>析构与进程边界</b><br/>停后台线程并尽力排空状态<br/>活跃 run 归本实例；fork 子进程须重新初始化</div>"]
NATIVE_NOTE_8["<div class='body'><b>其他原生绑定 · JS</b><br/>point → Node addon<br/>再进入同一 C ABI</div>"]
NATIVE_NOTE_9["<div class='body'><b>C++ 直接调用</b><br/>构造 LocalRuntime 后 invoke(request)<br/>跳过 C ABI，不调用 te_local_free</div>"]
NATIVE_NOTE_10["<div class='body'><b>同步与后台的关系</b><br/>宿主先取得边界结果继续执行<br/>落盘、学习随后独立推进</div>"]
NATIVE_NOTE_1 -.- NATIVE_NOTE_2
NATIVE_NOTE_3 -.- NATIVE_NOTE_4
NATIVE_NOTE_4 -.- NATIVE_NOTE_5
NATIVE_NOTE_6 -.- NATIVE_NOTE_7
NATIVE_NOTE_7 -.- NATIVE_NOTE_8
NATIVE_NOTE_9 -.- NATIVE_NOTE_10
end
end
subgraph HTTP_INTERFACE["1.2 · HTTP 接口｜常驻组件服务"]
direction TB
HTTP_STEP_1["<div class='body'><b>每次边界调用</b><br/>Evolution.point(fields)<br/>复用客户端配置，携带 token</div>"]
HTTP_STEP_2["<div class='body'><b>进入 SDK 请求封装</b><br/>_invoke(point, fields,<br/>request_id, timeout, 0)</div>"]
HTTP_STEP_3["<div class='body'><b>JSON 编码并连接常驻服务</b><br/>POST /v1/invoke<br/>Authorization: Bearer</div>"]
HTTP_STEP_4["<div class='body'><b>进入 HTTP worker</b><br/>connection() 解析请求头与请求体</div>"]
HTTP_STEP_5["<div class='body'><b>IntegrationService::call(token, JSON)</b><br/>关联服务持有的 run<br/>执行本次 point 边界操作</div>"]
HTTP_STEP_6["<div class='body'><b>服务发送同步响应</b><br/>send_response<br/>边界响应不等待后台学习完成</div>"]
HTTP_STEP_7["<div class='body'><b>SDK 接收返回</b><br/>读取并检查 ok / result<br/>关闭本次连接，point 返回 result</div>"]
HTTP_STEP_8["<div class='body'><b>同步返回宿主</b><br/>采用 input / decision<br/>继续原来的 Agent 执行流程</div>"]
HTTP_STEP_1 --> HTTP_STEP_2
HTTP_STEP_2 --> HTTP_STEP_3
HTTP_STEP_3 --> HTTP_STEP_4
HTTP_STEP_4 --> HTTP_STEP_5
HTTP_STEP_5 --> HTTP_STEP_6
HTTP_STEP_6 --> HTTP_STEP_7
HTTP_STEP_7 --> HTTP_STEP_8
subgraph HTTP_OPERATIONS["初始化、独立后台与生命周期"]
direction TB
HTTP_NOTE_1["<div class='body'><b>服务初始化一次</b><br/>trajectory_evolution serve --config ...<br/>创建 IntegrationHttpServer，启动监听与线程</div>"]
HTTP_NOTE_2["<div class='body'><b>客户端初始化一次</b><br/>Evolution(URL, token)，创建后复用<br/>构造只保存配置，不负责启动服务</div>"]
HTTP_NOTE_3["<div class='body'><b>服务端校验</b><br/>鉴权、参数与幂等检查<br/>before / guard：准备 input / decision</div>"]
HTTP_NOTE_4["<div class='body'><b>记录执行结果</b><br/>after / abort：核验回执<br/>buffered 入缓冲；durable 同步持久化</div>"]
HTTP_NOTE_5["<div class='body'><b>独立后台 · capture_writer</b><br/>buffered 模式异步落盘<br/>不在同步返回链上</div>"]
HTTP_NOTE_6["<div class='body'><b>独立后台 · 学习调度</b><br/>process_evaluations 调度评测与学习<br/>宿主不等待反思或记忆更新完成</div>"]
HTTP_NOTE_7["<div class='body'><b>跨进程接续</b><br/>同用户可用同一 ref.run 接续<br/>请求连接关闭，服务仍持有 run 状态</div>"]
HTTP_NOTE_8["<div class='body'><b>任务与服务收尾</b><br/>任务显式结束；服务停机时停止并等待线程<br/>管理操作按需调用</div>"]
HTTP_NOTE_9["<div class='body'><b>当前 v1 客户端封装</b><br/>evolution_configured.points<br/>→ Evolution.point</div>"]
HTTP_NOTE_10["<div class='body'><b>当前 v1 配置绑定</b><br/>EVOLUTION_POINT_CONFIG<br/>→ point-client.json</div>"]
HTTP_NOTE_11["<div class='body'><b>当前 v1 请求设置</b><br/>按 user_id 选择 token_env<br/>本机 HTTP；默认 20 ms，无重试</div>"]
HTTP_NOTE_1 -.- HTTP_NOTE_2
HTTP_NOTE_3 -.- HTTP_NOTE_4
HTTP_NOTE_4 -.- HTTP_NOTE_5
HTTP_NOTE_6 -.- HTTP_NOTE_7
HTTP_NOTE_7 -.- HTTP_NOTE_8
HTTP_NOTE_9 -.- HTTP_NOTE_10
HTTP_NOTE_10 -.- HTTP_NOTE_11
end
end
TRANSPORT_RETURN["<div class='body'><b>本次 point 同步返回</b><br/>统一 result；宿主继续原来的业务调用<br/>独立后台无需在此等待</div>"]
TRANSPORT_CALL -->|原生| NATIVE_INTERFACE
TRANSPORT_CALL -->|HTTP| HTTP_INTERFACE
NATIVE_INTERFACE -->|result| TRANSPORT_RETURN
HTTP_INTERFACE -->|result| TRANSPORT_RETURN
end
subgraph M1_RESULT["统一结果与降级处理"]
direction LR
ENTRY["<div class='body'><b>两种接口返回到同一个宿主调用点</b><br/>point 已完成对应的 run / activity / loop 操作<br/>同步策略处理发生在接口内部，展开过程见 06<br/>宿主根据本次结果继续执行，后台学习独立推进</div>"]
POINT_RESULT["<div class='body'><b>统一返回结果 · 本次 point 的调用方接收</b><br/>协议外层 ok / result；Python、JS SDK 取出 result<br/>input：准备后的模型请求；decision：动作或循环决定<br/>degraded：是否降级；capture_complete：采集完整性</div>"]
ENTRY --> POINT_RESULT
POINT_RESULT_PART_2["<div class='body'>policy_applied 表示策略应用状态，须结合后续执行回执</div>"]
POINT_RESULT -.- POINT_RESULT_PART_2
POINT_FALLBACK["<div class='body'><b>降级时由宿主继续业务</b><br/>沿用原输入、原调度和原有权限、循环限制<br/>执行结果与采集完整性分开记录<br/>受影响轨迹进入隔离，不充当正常学习证据</div>"]
POINT_RESULT -.- POINT_FALLBACK
end
M1_INPUT -->|调用 point| INTERFACES
INTERFACES -->|接收本次结果| M1_RESULT
end
subgraph LANES["同步执行与后台演化"]
direction LR
subgraph FOREGROUND["前台｜通用 Agent 与同步策略"]
direction TB
subgraph ONLINE_ENTRY["宿主执行与边界策略处理"]
direction LR
subgraph M2["02 · ReAct 执行与 7 个插入点"]
direction TB
RUN_BEGIN["<div class='body'><b>插入点 ① · 任务开始</b><br/>point(before, run)：ref.run + 任务 input<br/>关联用户与会话，选择并固定本次策略版本<br/>后续模型、工具与循环都归属本 run</div>"]
LOOP_POINT["<div class='body'><b>插入点 ② · 每轮开始前</b><br/>point(guard, loop)：当前 state + 循环引用<br/>取得 continue / replan / stop<br/>与宿主原有循环条件共同决定下一步</div>"]
MODEL_BEFORE["<div class='body'><b>插入点 ③ · 模型调用前</b><br/>point(before, model)：真实请求 + ref.key<br/>取 result.input 作为本次实际发送内容<br/>经验上下文与本轮 replan 在此进入模型请求</div>"]
RUN_EXEC["<div class='body'><b>Reason · 调用原有模型</b><br/>传入准备好的消息、工具定义与任务上下文<br/>得到文本答复或待执行的工具调用<br/>模型调用仍由宿主自己的客户端执行</div>"]
MODEL_AFTER["<div class='body'><b>插入点 ④ · 模型调用后</b><br/>point(after, model)，异常用 abort<br/>沿用 ref.key，报告 actual_input / output / executed<br/>有真实 usage 时一并上报，供 token 成本统计</div>"]
NEED_ACTION["<div class='body'><b>模型是否请求工具？</b></div>"]
ACTION_BEFORE["<div class='body'><b>插入点 ⑤ · 工具执行前</b><br/>point(before, action)：工具名与真实参数<br/>检查 result.decision 与 reason<br/>allow 执行；reject 跳过本次工具调用</div>"]
ACTION_EXEC["<div class='body'><b>Act · 执行原有工具</b><br/>使用宿主已有执行器、权限与运行环境<br/>保留原来的串行或并行调度<br/>得到真实结果、错误或业务产物</div>"]
ACTION_AFTER["<div class='body'><b>插入点 ⑥ · 工具结果处</b><br/>point(after, action)，异常用 abort<br/>报告实际 output / error / executed<br/>策略拒绝时也收尾，并明确 executed=false</div>"]
OBSERVE["<div class='body'><b>Observe · 将工具结果写入上下文</b><br/>包括成功结果、失败原因及跳过说明<br/>更新下一轮可观察 state，再进行循环检查</div>"]
LOOP_BACK["<div class='body'><b>进入下一轮</b><br/>把更新后的 state 交回插入点 ②</div>"]
FINAL_ANSWER["<div class='body'><b>宿主汇总当前结果，形成任务答复</b><br/>结合已完成分支、失败或未完成事项<br/>答复、业务成功与采集成功分别记录</div>"]
RUN_END["<div class='body'><b>插入点 ⑦ · 任务真正收尾</b><br/>point(after, run)，异常终止用 abort<br/>报告最终 output 与业务状态<br/>异步派发返回不等于任务完成，待活动闭合后收尾<br/>收尾后把任务答复返回业务方</div>"]
RUN_BEGIN --> LOOP_POINT
LOOP_POINT --> MODEL_BEFORE
MODEL_BEFORE --> RUN_EXEC
RUN_EXEC --> MODEL_AFTER
MODEL_AFTER --> NEED_ACTION
LOOP_POINT -->|stop| FINAL_ANSWER
NEED_ACTION -->|答复| FINAL_ANSWER
NEED_ACTION -->|工具| ACTION_BEFORE
ACTION_BEFORE -->|allow| ACTION_EXEC
ACTION_BEFORE -->|reject| ACTION_AFTER
ACTION_EXEC --> ACTION_AFTER
ACTION_AFTER --> OBSERVE
OBSERVE --> LOOP_BACK
LOOP_BACK -->|下一轮| LOOP_POINT
FINAL_ANSWER --> RUN_END
end
subgraph M6_MATCH["6.1–6.2 · 选定快照、构造查询与匹配"]
direction TB
SNAPSHOT["<div class='body'><b>任务开始时选择并固定版本 · apply_agent_version</b><br/>无准入：在新 run 编译当前获准经验<br/>有准入：选择兼容的评测发布 / 指定版本<br/>绑定 agent / user / conversation / run 身份</div>"]
QUERY_PREP["<div class='body'><b>① 先准备本次边界</b><br/>模型：先确定 binding，再提取检索用状态<br/>工具：先检查本任务 / 分支的工具契约，失败可直接拒绝<br/>L0 或 apply_policy=false 时跳过策略应用</div>"]
QUERY_BUILD["<div class='body'><b>② 构造 RuntimeQuery</b><br/>沿用 run 身份；boundary = llm / actions / loop<br/>target_scope = agent:应用 / actor:scope_id<br/>task 取任务 input；state 优先取显式 state，否则取本次输入</div>"]
MATCH_SCOPE["<div class='body'><b>③ 校验身份并筛选作用域</b><br/>PolicyRuntime::decide → ShadowRuntime::query_advice<br/>agent / user / run / conversation 必须与快照一致<br/>经验 target_scope 与本次作用域精确一致</div>"]
MATCH_TERMS["<div class='body'><b>④ 计算触发词命中数</b><br/>经验词项在编译时从 trigger_pattern 提取<br/>与当前 task + state 的词项集合求交集<br/>命中数 ≥ min_matched_terms 才进入候选</div>"]
MATCH_RANK["<div class='body'><b>⑤ 排序、去重并限制返回量</b><br/>私有经验优先 → 命中词多者优先 → memory_id 排序<br/>已选私有经验的公共重复项跳过<br/>依次装入 max_advice_items / max_advice_bytes 预算</div>"]
SNAPSHOT --> QUERY_PREP
QUERY_PREP --> QUERY_BUILD
QUERY_BUILD --> MATCH_SCOPE
MATCH_SCOPE --> MATCH_TERMS
MATCH_TERMS --> MATCH_RANK
SNAPSHOT_PART_2["<div class='body'>同一 run 使用固定策略；更新留给后续新 run<br/>启用发布准入却无兼容版本时，以 L0 启动</div>"]
SNAPSHOT -.- SNAPSHOT_PART_2
QUERY_PREP_PART_2["<div class='body'>启用策略且通过前置检查后，构造查询</div>"]
QUERY_PREP -.- QUERY_PREP_PART_2
QUERY_BUILD_PART_2["<div class='body'>检索文本按预算截取，业务原输入保持完整</div>"]
QUERY_BUILD -.- QUERY_BUILD_PART_2
MATCH_SCOPE_PART_2["<div class='body'>只检索获准 memory_id + 对应内容版本</div>"]
MATCH_SCOPE -.- MATCH_SCOPE_PART_2
MATCH_TERMS_PART_2["<div class='body'>不使用 observed_problem 或建议正文作为触发词</div>"]
MATCH_TERMS -.- MATCH_TERMS_PART_2
MATCH_RANK_PART_2["<div class='body'>得到本次命中的经验清单，供下面三类边界使用</div>"]
MATCH_RANK -.- MATCH_RANK_PART_2
MATCH_LANGUAGE["<div class='body'><b>匹配方式与示例 · 词项相交</b><br/>英文：转小写，保留至少 2 字符的词；中文：相邻双字<br/>例：缺少素材 → 缺少 / 少素 / 素材<br/>经验侧去除 stop_terms，并受 max_match_terms 限制</div>"]
MATCH_LANGUAGE_PART_2["<div class='body'>仓库示例：至少命中 2 项，最多 3 条、4096 字节建议</div>"]
MATCH_OBSERVATION["<div class='body'><b>检索上下文说明</b><br/>未显式传 state，且 binding.mode=system_message 时<br/>从绑定消息列表提取状态，排除 system / developer<br/>避免把静态指令和此前注入的经验当作本次观察</div>"]
MATCH_OBSERVATION_PART_2["<div class='body'>经验清单为空时，基础动作 / 循环规则仍可独立生效</div>"]
MATCH_RANK -.- MATCH_LANGUAGE
MATCH_LANGUAGE -.- MATCH_LANGUAGE_PART_2
MATCH_LANGUAGE -.- MATCH_OBSERVATION
MATCH_OBSERVATION -.- MATCH_OBSERVATION_PART_2
end
M2 <-->|before / guard 请求；返回处理结果| M6_MATCH
end
subgraph HANDLERS["6.3 · 三类条件注入"]
direction TB
INJECTION_ROUTER["<div class='body'><b>按本次边界分派策略</b><br/>仅启用具备层级、能力和授权的处理器<br/>模型 before / 工具 before / loop guard</div>"]
subgraph M6_L1["L1 · 模型输入｜插入点 ③"]
direction TB
L1_CONTEXT["<div class='body'><b>模型调用前 · level ≥ L1 + context.request</b><br/>按匹配顺序读取获准经验<br/>每条转成 JSON：target_scope /<br/>trigger_pattern / proposed_change</div>"]
L1_RENDER["<div class='body'><b>生成本次注入上下文</b><br/>经验清单填入模板的 {{experience}}<br/>外层加 trajectory-experience 与 policy_id 标记<br/>最终文本受 max_context_bytes 限制</div>"]
L1_WRITE["<div class='body'><b>inject_request_context 写入请求副本</b><br/>system_message：定位消息数组<br/>追加到首个可用 system 文本末尾<br/>没有可用 system 文本时，在数组首部插入</div>"]
L1_RETURN["<div class='body'><b>返回 result.input</b><br/>宿主用它发起本次真实模型调用<br/>宿主继续报告实际发送的 actual_input<br/>无经验注入、也无重规划上下文时</div>"]
L1_CONTEXT --> L1_RENDER
L1_RENDER --> L1_WRITE
L1_WRITE --> L1_RETURN
L1_CONTEXT_PART_2["<div class='body'>有获准改写时使用 experience_overrides<br/>不包含整条历史轨迹</div>"]
L1_CONTEXT -.- L1_CONTEXT_PART_2
L1_RENDER_PART_2["<div class='body'>连同模板包装后超预算的经验项跳过<br/>保留适用条件，供模型结合本次任务判断</div>"]
L1_RENDER -.- L1_RENDER_PART_2
L1_WRITE_PART_2["<div class='body'>text：追加到 pointer 指向的文本末尾<br/>该轮有 replan 时，再追加重规划上下文</div>"]
L1_WRITE -.- L1_WRITE_PART_2
L1_RETURN_PART_2["<div class='body'>模型 input 保持原样</div>"]
L1_RETURN -.- L1_RETURN_PART_2
L1_NOTE["<div class='body'><b>绑定说明 · 在匹配前确定</b><br/>本次 point.binding → instrumentation.binding<br/>都未提供时，从请求结构自动推断<br/>pointer 指定字段，mode 指定写入方式</div>"]
L1_NOTE_PART_2["<div class='body'>例如 /messages + system_message<br/>或 /system、/instructions + text</div>"]
L1_RETURN -.- L1_NOTE
L1_NOTE -.- L1_NOTE_PART_2
end
subgraph M6_L2["L2 · 工具动作｜插入点 ⑤"]
direction TB
L2_INPUT["<div class='body'><b>工具执行前 · level ≥ L2 + action.filter</b><br/>读取本次 action_id / operation / arguments<br/>输入是宿主准备执行的工具及参数<br/>使用当前作用域与本次命中经验清单</div>"]
L2_RULE["<div class='body'><b>匹配 action_rules</b><br/>target_scope 与当前作用域精确一致<br/>operation 与本次工具名称精确一致<br/>基础规则：无需命中某条经验</div>"]
L2_DECIDE["<div class='body'><b>按规则顺序检查本次参数</b><br/>deny=true → reject<br/>required_arguments 有缺失 / 空值 → reject<br/>首条导致拒绝的规则决定 rule_id / reason</div>"]
L2_RETURN["<div class='body'><b>返回 result.decision 与 reason</b><br/>allow：宿主按原流程执行工具<br/>reject：宿主跳过工具，报告 executed=false<br/>工具结束后回传实际结果与执行状态</div>"]
L2_INPUT --> L2_RULE
L2_RULE --> L2_DECIDE
L2_DECIDE --> L2_RETURN
L2_INPUT_PART_2["<div class='body'>逐条检查 RuntimeOptions.action_rules<br/>先把本次决定初始化为 allow</div>"]
L2_INPUT -.- L2_INPUT_PART_2
L2_RULE_PART_2["<div class='body'>经验生成规则：source_experience<br/>必须出现在本次命中清单且版本一致</div>"]
L2_RULE -.- L2_RULE_PART_2
L2_DECIDE_PART_2["<div class='body'>没有规则要求拒绝 → allow<br/>输入参数本身不在此自动改写</div>"]
L2_DECIDE -.- L2_DECIDE_PART_2
L2_RETURN_PART_2["<div class='body'>对应插入点 ⑥</div>"]
L2_RETURN -.- L2_RETURN_PART_2
L2_NOTE["<div class='body'><b>前置检查说明 · 在匹配前执行</b><br/>begin_activity 可先做工具契约检查<br/>契约来自已实际发送的模型请求<br/>限定在当前任务 / 分支，L2 及以上可拦截缺参</div>"]
L2_NOTE_PART_2["<div class='body'>契约失败直接 reject，不再进入策略匹配<br/>宿主自己的权限检查仍由宿主负责</div>"]
L2_RETURN -.- L2_NOTE
L2_NOTE -.- L2_NOTE_PART_2
end
subgraph M6_L3["L3 · 循环控制｜插入点 ②"]
direction TB
L3_STATE["<div class='body'><b>循环检查前 · level ≥ L3 + loop.decide</b><br/>读取本循环 iteration、当前 state 指纹<br/>以及上一轮 progress / no_progress / replans<br/>仍有进行中的 activity 时返回 continue</div>"]
L3_RULE["<div class='body'><b>选择规则并更新进展计数</b><br/>同作用域下，命中的经验生成规则优先<br/>否则使用匹配的基础循环规则<br/>相邻轮次状态指纹相同：no_progress + 1</div>"]
L3_DECIDE["<div class='body'><b>根据阈值决定下一步</b><br/>启用 max_iterations 且超限 → stop<br/>启用 max_no_progress 且达到阈值时<br/>获准 replan 且次数未满 → replan</div>"]
L3_RETURN["<div class='body'><b>返回 continue / replan / stop</b><br/>宿主据此继续、调整计划或结束本循环<br/>replan 生成带 policy_id 的上下文<br/>关联该 control 的后续模型 before 点</div>"]
L3_STATE --> L3_RULE
L3_RULE --> L3_DECIDE
L3_DECIDE --> L3_RETURN
L3_STATE_PART_2["<div class='body'>否则匹配本作用域的 loop_rules<br/>经验生成规则还须命中来源经验及版本</div>"]
L3_STATE -.- L3_STATE_PART_2
L3_RULE_PART_2["<div class='body'>状态改变：no_progress 归零<br/>规则与进度均属于当前 run / loop</div>"]
L3_RULE -.- L3_RULE_PART_2
L3_DECIDE_PART_2["<div class='body'>否则 stop；未触发阈值 → continue<br/>已有 stop 决定不会恢复为 continue</div>"]
L3_DECIDE -.- L3_DECIDE_PART_2
L3_RETURN_PART_2["<div class='body'>通过 L1 列的写入步骤真正注入</div>"]
L3_RETURN -.- L3_RETURN_PART_2
L3_NOTE["<div class='body'><b>循环边界说明</b><br/>guard 封装 loop.check + loop.commit<br/>拆开调用时，由宿主提交实际 decision<br/>replan 还需后续模型请求包含重规划上下文</div>"]
L3_NOTE_PART_2["<div class='body'>该模型边界需支持上下文注入<br/>循环决定只作用于对应 loop</div>"]
L3_RETURN -.- L3_NOTE
L3_NOTE -.- L3_NOTE_PART_2
end
HANDLER_RETURN["<div class='body'><b>返回本次准备结果</b><br/>模型 input / 动作 allow 或 reject<br/>循环 continue、replan 或 stop</div>"]
INJECTION_ROUTER -->|model before| M6_L1
M6_L1 --> HANDLER_RETURN
INJECTION_ROUTER -->|action before| M6_L2
M6_L2 --> HANDLER_RETURN
INJECTION_ROUTER -->|loop guard| M6_L3
M6_L3 --> HANDLER_RETURN
end
subgraph M6_RECEIPTS["6.4 · 返回执行、事后核验与应用回执"]
direction LR
APPLY["<div class='body'><b>本次边界响应回到宿主</b><br/>模型：准备好的 input；动作 / 循环：decision + reason<br/>宿主在 02 对应插入点采用返回值<br/>L0 / apply_policy=false 时沿用原输入和默认决定</div>"]
POLICY_OBSERVE["<div class='body'><b>另一次回调：对应 02 的 after / abort 插入点</b><br/>模型：actual_input + executed；工具：实际执行或跳过<br/>循环：提交 decision；guard 会自动提交本次决定</div>"]
POLICY_VERIFY["<div class='body'><b>PolicyRuntime::verify 重算相同策略决定并比对证据</b><br/>L1 / replan：调用已执行，绑定字段中完整上下文恰好出现 1 次<br/>L2：被拒绝的 action 确实跳过，且未执行</div>"]
POLICY_RECEIPT["<div class='body'><b>记录 RuntimeReceipt → advice.applied 事件</b><br/>applied：执行证据符合策略要求<br/>failed：策略已 ready，但未观察到正确应用<br/>rejected：策略没有需要应用的改变</div>"]
APPLY --> POLICY_OBSERVE
POLICY_OBSERVE --> POLICY_VERIFY
POLICY_VERIFY --> POLICY_RECEIPT
POLICY_OBSERVE_PART_2["<div class='body'>边界运行时根据记录的 query 构造 RuntimeObservation</div>"]
POLICY_OBSERVE -.- POLICY_OBSERVE_PART_2
POLICY_VERIFY_PART_2["<div class='body'>L3 stop：提交的循环决定是 stop，且没有待执行 activity</div>"]
POLICY_VERIFY -.- POLICY_VERIFY_PART_2
POLICY_RECEIPT_PART_2["<div class='body'>记入 run 的轨迹与 receipt_counts，供 03 / 04 使用</div>"]
POLICY_RECEIPT -.- POLICY_RECEIPT_PART_2
RECEIPT_NOTE["<div class='body'><b>回执说明</b><br/>before 返回准备好的 input，不等于已经发送给模型<br/>重复注入、漏传 actual_input 或未执行，都不能确认应用成功<br/>applied 说明执行证据符合要求；任务效果仍由评测判断</div>"]
POLICY_RECEIPT -.- RECEIPT_NOTE
end
TRACE_EXPORT["<div class='body'><b>持续上报给后台</b><br/>每次 point 的边界事件<br/>实际结果、采集状态与策略回执</div>"]
VERSION_IMPORT["<div class='body'><b>后续新 run 读取新版本</b><br/>在 6.1 选择并固定快照<br/>当前 run 继续使用原策略</div>"]
end
subgraph BACKGROUND["后台｜从真实轨迹到新策略版本"]
direction TB
subgraph M3["03 · 轨迹采集、批次调度与因果分析"]
direction LR
TRACE["<div class='body'><b>边界事件汇入标准 TraceRun</b><br/>身份：user / conversation / run / actor<br/>事件：run、step、action 的开始与结束、状态观察<br/>关联：execution / loop / iteration / caused_by</div>"]
LEARN_QUEUE["<div class='body'><b>完整新轨迹按用户进入后台学习队列</b><br/>auto_evaluate 开启，达到最少新 run 数或等待期限<br/>按最老待办选择用户，受批次间隔与条数 / 字节预算约束<br/>冻结 job 成员、来源指纹与 batch_id</div>"]
ANALYZE["<div class='body'><b>validate_trace → analyze_trace</b><br/>event_id 去重；同 ID 不同内容视为冲突<br/>检查身份、开始结束配对、事件引用与唯一终态<br/>用因果边建立 DAG，拓扑排序；有环或缺口则隔离</div>"]
TRACE --> LEARN_QUEUE
LEARN_QUEUE --> ANALYZE
TRACE_PART_2["<div class='body'>保存实际输入输出、错误、策略版本与应用回执<br/>终态分别记录业务结果和 capture_complete</div>"]
TRACE -.- TRACE_PART_2
LEARN_QUEUE_PART_2["<div class='body'>新到轨迹留给后续批次；成功报告校验后可复用<br/>阶段持久化：分析 → 合成 → 入库 → 完成</div>"]
LEARN_QUEUE -.- LEARN_QUEUE_PART_2
ANALYZE_PART_2["<div class='body'>按 step 组织 Episode，归入动作、结果、诊断与指纹<br/>没有内部 step 时形成黑盒 Episode，不猜测工具与轮次</div>"]
ANALYZE -.- ANALYZE_PART_2
end
subgraph M4["04 · 诊断、反思、提炼与候选门槛"]
direction LR
EVALUATION["<div class='body'><b>对已有轨迹进行规则与可选模型评测</b><br/>规则识别有证据的无效执行，如不可重试后的重复失败<br/>模型读取脱敏、限长后的事件正文与 Episode 关联<br/>判断 USEFUL / NECESSARY_RETRY / INVALID / UNCERTAIN 等</div>"]
CANDIDATES["<div class='body'><b>synthesize_reports：从报告提取经验候选</b><br/>规则诊断生成 seed；有效模型反思提供带条件的建议<br/>经验字段：target_scope / trigger_pattern / proposed_change</div>"]
REFLECTION["<div class='body'><b>可选元反思 · enable_meta_reflection</b><br/>按 target_type + scope + 经验契约分组<br/>至少两条候选，且模型与调用预算可用时尝试合并<br/>模型选择组内 seed_ids，提炼条件、建议与必要例外</div>"]
CANDIDATE_GATE["<div class='body'><b>候选质量与证据门槛 · finalize_candidate</b><br/>支持按独立 run 和 Episode 去重，默认各至少 2 个<br/>F：两类支持数 / 各自门槛中的较小值，上限 1<br/>置信度 = 0.7 × 平均证据置信度 + 0.3 × F</div>"]
EVALUATION --> CANDIDATES
CANDIDATES --> REFLECTION
REFLECTION --> CANDIDATE_GATE
EVALUATION_PART_2["<div class='body'>校验输出格式、作用域、证据引用后融合规则与模型结论<br/>产出逐 Episode 判断、整段轨迹反思和评测报告</div>"]
EVALUATION -.- EVALUATION_PART_2
CANDIDATES_PART_2["<div class='body'>保留 observed_problem、预期收益及直接 evidence_event_ids<br/>candidate_id 标识建议；evidence_id 去重来源证据<br/>用本批新报告寻找同 scope / error_class 的反证</div>"]
CANDIDATES -.- CANDIDATES_PART_2
REFLECTION_PART_2["<div class='body'>程序继承所选候选的原始证据，重新计算支持与反证<br/>关键响应不合法时保留原 seeds，不接受半套合并结果</div>"]
REFLECTION -.- REFLECTION_PART_2
CANDIDATE_GATE_PART_2["<div class='body'>有反证时乘 0.6，状态为 CONFLICTED<br/>格式或证据不合格 → REJECTED_BY_GATE；支持不足则暂存<br/>满足支持门槛且无反证 → PROPOSED，再交给记忆管理</div>"]
CANDIDATE_GATE -.- CANDIDATE_GATE_PART_2
REFLECTION_NOTE["<div class='body'><b>本模块处理既有轨迹，不启动新的业务 Agent</b><br/>规则确认的无效执行在融合时保留；未知不补成有效<br/>评测 / 元反思 / 私库整理共享批次模型预算<br/>阶段失败保留诊断与检查点，按配置回退或等待显式重试</div>"]
CANDIDATE_GATE -.- REFLECTION_NOTE
end
subgraph M5["05 · 记忆管理与公共投影"]
direction LR
MEMORY_CURATE["<div class='body'><b>增量私库整理 · curate_memory_in_place</b><br/>batch_id 已处理则幂等返回，不重复累加支持<br/>候选 ID / 别名相同，或规范化策略身份相同：直接合并证据<br/>其余同类型、同 scope 候选按词面相似度选有界短名单</div>"]
PRIVATE_MEMORY[("<div class='store'><b>用户私有 EvolutionMemoryStore</b></div>")]
PUBLIC_REVIEW["<div class='body'><b>公共提炼前先核验独立来源</b><br/>从合格私有经验中选取无冲突、有证据的来源<br/>回查真实 run、来源指纹与完整评测，排除经验注入回声<br/>distill 提炼泛化后的 scope / 条件 / 建议</div>"]
PUBLIC_RELATIONS["<div class='body'><b>公共支持与公私关系管理</b><br/>真实用户支持按用户去重，训练支持按 dataset + group 去重<br/>默认至少 2 个独立用户，或启用训练来源且至少 2 个任务组</div>"]
PUBLIC_MEMORY[("<div class='store'><b>可用公共经验投影</b></div>")]
MEMORY_CURATE --> PRIVATE_MEMORY
PRIVATE_MEMORY --> PUBLIC_REVIEW
PUBLIC_REVIEW --> PUBLIC_RELATIONS
PUBLIC_RELATIONS --> PUBLIC_MEMORY
MEMORY_CURATE_PART_2["<div class='body'>可选模型判断 DUPLICATE / RELATED / CONTRADICTS / DISTINCT<br/>重复归并、相关或冲突建双向关系；不确定时独立保存</div>"]
MEMORY_CURATE -.- MEMORY_CURATE_PART_2
PRIVATE_MEMORY_PART_2["<div class='body'>保存 memory_id、候选正文、别名、证据与关系<br/>新条目为 CANDIDATE；支持不足的候选也可暂存<br/>跨 conversation / run 累积，但证据仍属于同一用户<br/>revision 记录库更新；content_revision 标识内容版本</div>"]
PRIVATE_MEMORY -.- PRIVATE_MEMORY_PART_2
PRIVATE_MEMORY_PART_3["<div class='body'>PB 保存权威数据，JSON 提供可读视图</div>"]
PRIVATE_MEMORY -.- PRIVATE_MEMORY_PART_3
PUBLIC_REVIEW_PART_2["<div class='body'>另一次 review 请求检查隐私、泛化、支持与不覆盖私有偏好<br/>通过硬校验与置信度门槛后，登记公共建议及来源支持</div>"]
PUBLIC_REVIEW -.- PUBLIC_REVIEW_PART_2
PUBLIC_RELATIONS_PART_2["<div class='body'>private / public 的 duplicate、related、conflict、independent 分别记录<br/>确认 conflict 后暂停对应公共建议；未审关系限制该用户使用<br/>支持过期、来源撤回与容量排名会重新计算公共可用状态</div>"]
PUBLIC_RELATIONS -.- PUBLIC_RELATIONS_PART_2
PUBLIC_MEMORY_PART_2["<div class='body'>只有 published 且在容量内的条目进入投影<br/>支持不足 pending_support；有争议 disputed；超容量 reserve<br/>用户查询时仍检查 scope、私有关系和版本授权<br/>公共投影包含泛化建议，私人原始证据留在所属私库</div>"]
PUBLIC_MEMORY -.- PUBLIC_MEMORY_PART_2
PUBLIC_MEMORY_PART_3["<div class='body'>公共内容变化后重新检查相关用户的跨库关系</div>"]
PUBLIC_MEMORY -.- PUBLIC_MEMORY_PART_3
MEMORY_MAINTENANCE["<div class='body'><b>独立维护 · 累计新建条目达到阈值后做全库整理</b><br/>先精确去重，再按预算做语义关系检查；完成后才清零计数<br/>按条数 / 字节预算淘汰，CONTRADICTS 连通组一起处理</div>"]
MEMORY_MAINTENANCE_PART_2["<div class='body'>公共管理按用户待办和到期时间推进，不在每次 point 内执行</div>"]
MEMORY_FEEDBACK["<div class='body'><b>独立反馈 · 普通任务对照评测可写回记忆实测状态</b><br/>报告按 job_id 去重，并绑定 memory_id + content_revision<br/>版本组合通过 / 失败先记录组合层面的观察</div>"]
MEMORY_FEEDBACK_PART_2["<div class='body'>只有单条经验差异、实际 applied 且配对证据充分，才自动验证或归档<br/>GEPA 搜索不会把多组件改写归因成单条经验的新支持</div>"]
PUBLIC_MEMORY -.- MEMORY_MAINTENANCE
MEMORY_MAINTENANCE -.- MEMORY_MAINTENANCE_PART_2
MEMORY_MAINTENANCE -.- MEMORY_FEEDBACK
MEMORY_FEEDBACK -.- MEMORY_FEEDBACK_PART_2
end
subgraph M6_PREP["06 · 筛选、授权与策略编译"]
direction LR
MEMORY_SELECT["<div class='body'><b>① 从记忆中挑选可用经验 · compile_shadow_policy</b><br/>私有经验属于当前用户，状态为 CANDIDATE / VALIDATED<br/>candidate.status=PROPOSED，且支持来源和作用域合法<br/>排除反证、私库冲突及不符合经验契约的内容</div>"]
POLICY_APPROVE["<div class='body'><b>② 确认经验授权与运行能力</b><br/>approved_experiences 精确绑定 memory_id + content_revision<br/>auto_approve_experiences 开启时授权当前快照内经验<br/>内容变化需要对应的新版本授权</div>"]
COMPILE["<div class='body'><b>③ compile_runtime_policy 编译完整候选策略</b><br/>经验快照 + RuntimeOptions → RuntimePolicy / policy_id<br/>冻结经验、模板、上下文预算、动作规则与循环规则<br/>绑定用户、应用、会话 / run 的使用身份和作用域</div>"]
MEMORY_SELECT --> POLICY_APPROVE
POLICY_APPROVE --> COMPILE
MEMORY_SELECT_PART_2["<div class='body'>公共经验来自已审核投影，排除跨库冲突或未审关系<br/>按经验条数与快照字节预算生成候选工作集</div>"]
MEMORY_SELECT -.- MEMORY_SELECT_PART_2
POLICY_APPROVE_PART_2["<div class='body'>requested_level、宿主 capabilities 与部署上限共同约束能力<br/>保存进记忆库、能被检索、获准实际应用是不同条件</div>"]
POLICY_APPROVE -.- POLICY_APPROVE_PART_2
COMPILE_PART_2["<div class='body'>无评测准入时按授权使用；有准入时交给 07 / 08 验证<br/>候选生成不改写正在运行的 Agent 策略</div>"]
COMPILE -.- COMPILE_PART_2
POLICY_CONFIG["<div class='body'><b>配置说明 · 仓库 boundary-service.json 示例</b><br/>默认 L0；approved_experiences 为空<br/>auto_approve_experiences=false<br/>实际开启注入需配置层级、边界能力与经验授权</div>"]
COMPILE -.- POLICY_CONFIG
end
subgraph ASSESSMENT["验证候选版本"]
direction LR
subgraph M7["07 · 数据集、真实 Agent 评测与准入"]
direction TB
DATASET[("<div class='store'><b>冻结评测集 · EvaluationDataset / EvaluationTask</b></div>")]
EVAL_JOB["<div class='body'><b>AgentEvaluationService 调度任务评测</b><br/>固定数据集、Agent revision、配置与候选 / 基线版本<br/>计划 = 选中任务 × repetitions × 策略版本<br/>每次试验分配 trial_id，关联任务、版本、轨迹与成绩</div>"]
EVAL_AGENT["<div class='body'><b>每次试验启动一个真实 Agent 进程</b><br/>posix_spawn 执行配置的 command 参数数组<br/>使用独立工作目录、进程组和允许传入的环境变量<br/>原应用初始化后，由 evaluate / aevaluate 绑定已有收消息函数</div>"]
EVAL_EVIDENCE["<div class='body'><b>采集实际结果与产物证据</b><br/>等待闭合 run，核对 trial / 用户 / 版本与最终 output 一致<br/>从工作目录独立读取声明产物，检查路径、大小和格式<br/>标准轨迹重算可观察的模型、工具、步骤、loop 与 token 指标</div>"]
TASK_SCORE["<div class='body'><b>逐题验收：规则检查 + 可选模型评审</b><br/>JSON Pointer 读取事实，检查 exists / equals / contains / 数值界限<br/>规则明确失败后锁定失败；模型按验收标准核对输出和证据</div>"]
SCORE_AGGREGATE["<div class='body'><b>汇总成绩，并按 task_id + repetition 配对比较</b><br/>S = passed + failed；N = 全部计划试验数<br/>coverage = S / N；pass_rate = passed / S<br/>pass_rate_all_trials = passed / N；S=0 时 pass_rate 为 null</div>"]
GATE_CHECK["<div class='body'><b>发布准入条件 · gate_result</b><br/>覆盖率、确定评分数与配对样本数达标<br/>候选通过率达标，且相对基线无超限退化<br/>配置了成本 / 无效循环要求时也必须满足</div>"]
DATASET --> EVAL_JOB
EVAL_JOB --> EVAL_AGENT
EVAL_AGENT --> EVAL_EVIDENCE
EVAL_EVIDENCE --> TASK_SCORE
TASK_SCORE --> SCORE_AGGREGATE
SCORE_AGGREGATE --> GATE_CHECK
DATASET_PART_2["<div class='body'>身份：task_id / group_id / task_version<br/>题面：task_input / followup_inputs；分类：category / difficulty<br/>验收：expected_behavior / constraints</div>"]
DATASET -.- DATASET_PART_2
DATASET_PART_3["<div class='body'>环境：initial_state；限制：budget 的 steps / tools / timeout<br/>来源：provenance；扩展：attributes<br/>审核与划分：review_status / split；全局 dataset_version</div>"]
DATASET -.- DATASET_PART_3
EVAL_JOB_PART_2["<div class='body'>按队列、并发、最小间隔和每小时启动预算运行<br/>普通准入与 GEPA 均复用 run_task_evaluation</div>"]
EVAL_JOB -.- EVAL_JOB_PART_2
EVAL_AGENT_PART_2["<div class='body'>组件经 entry 投题；多轮输入等待前一 run 真正结束并持久化<br/>最终答复取 run 结束 output，依旧经过 02 的原有插入点</div>"]
EVAL_AGENT -.- EVAL_AGENT_PART_2
EVAL_EVIDENCE_PART_2["<div class='body'>可选逐轮模型评审记录浪费轮次及真实事件引用<br/>没有 usage 或内部打点的指标保持未知，不按零成本计</div>"]
EVAL_EVIDENCE -.- EVAL_EVIDENCE_PART_2
TASK_SCORE_PART_2["<div class='body'>校验模型 verdict、理由与证据路径；必要时在预算内修复格式<br/>结果：passed / failed / uncertain / error<br/>独立保存 execution_status；程序执行成功不代表任务通过</div>"]
TASK_SCORE -.- TASK_SCORE_PART_2
SCORE_AGGREGATE_PART_2["<div class='body'>汇总胜 / 负 / 平、Agent 时延、调用、token 和无效循环<br/>规则下界与模型逐轮判断分开标明方法，评分模型调用单列</div>"]
SCORE_AGGREGATE -.- SCORE_AGGREGATE_PART_2
GATE_CHECK_PART_2["<div class='body'>缺少所需指标时无法通过，质量失败不能由成本收益抵消</div>"]
GATE_CHECK -.- GATE_CHECK_PART_2
DATASET_NOTE["<div class='body'><b>数据边界与报告 · 独立说明</b><br/>同 group / 会话固定在同一 split；已学习暴露的数据只进 train<br/>train 用于反思 / 补充经验，val 选择候选，holdout 最终验收</div>"]
DATASET_NOTE_PART_2["<div class='body'>initial_state 是声明式初始条件，按宿主入口约定提供<br/>保存 trials、report.json / pb / md、results.jsonl 与完成 manifest<br/>complete 表示计划均有终态，不表示全部题目通过</div>"]
BOOTSTRAP["<div class='body'><b>可用经验不足时补充训练 · 可选</b><br/>统计已过滤且获准的经验数，低于 min_experiences 才启动<br/>从冻结且 reviewed 的 train 中选未用任务，以 L0 空策略执行<br/>成功或业务失败的完整可信轨迹均可送 03 学习</div>"]
BOOTSTRAP_PART_2["<div class='body'>校验任务 / 数据集版本、trial_id、用户、策略及来源指纹<br/>等本批入库后再决定下一批；受 batch_size / max_batches 限制</div>"]
GATE_CHECK -.- DATASET_NOTE
DATASET_NOTE -.- DATASET_NOTE_PART_2
DATASET_NOTE -.- BOOTSTRAP
BOOTSTRAP -.- BOOTSTRAP_PART_2
end
subgraph M8["08 · GEPA 搜索与留出集验证"]
direction TB
SEED["<div class='body'><b>冻结 L1–L3 种子策略与实验上下文</b><br/>来自获准记忆的 seed，另固定当前生产 baseline<br/>绑定数据集、宿主版本、评测 / 反思配置和控制授权<br/>调用 07 在完整 val 上评测种子，建立逐题质量与成本记录</div>"]
PARENT["<div class='body'><b>选父节点与训练小批次</b><br/>按逐题前沿占据情况抽样父版本，内部节点与叶节点规则相同<br/>轮换可改组件，按 epoch 抽取 train 小批次<br/>可配置同父多提案、P×N 采样或 ComBEE 分组反思</div>"]
PROPOSE["<div class='body'><b>生成候选：反思改写或双亲重组</b><br/>反思读取父组件、训练题、真实执行结果、评分与失败证据<br/>只修改已授权的经验、生成动作规则或循环规则<br/>双亲重组记录共同祖先、parents 与各组件来源</div>"]
FRONTIER["<div class='body'><b>调用 07 的真实试验，筛选并更新候选前沿</b><br/>反思候选先与父版本在同一 train 小批次对照<br/>默认先保质量再看 fitness，严格改善后执行完整 val<br/>可配置 Pareto 接纳；重组先做覆盖双亲优势的验证子集筛选</div>"]
WINNER["<div class='body'><b>结束搜索，先固定最终 winner</b><br/>达到轮数、调用数、试验数、耐心或时间预算即停止<br/>预留最终 baseline + winner 的 holdout 试验预算<br/>选定后才打开 holdout，交给 07 做最终配对准入</div>"]
SEED --> PARENT
PARENT --> PROPOSE
PROPOSE --> FRONTIER
FRONTIER --> WINNER
SEED_PART_2["<div class='body'>没有有效评分或所需成本、循环证据时停止</div>"]
SEED -.- SEED_PART_2
PARENT_PART_2["<div class='body'>有合适不同分支及共同祖先时，可优先尝试双亲重组</div>"]
PARENT -.- PARENT_PART_2
PROPOSE_PART_2["<div class='body'>检查隐私、范围、引用、长度和控制边界，编译完整子策略<br/>相同行为版本去重；父策略与原记忆正文保持原版本</div>"]
PROPOSE -.- PROPOSE_PART_2
FRONTIER_PART_2["<div class='body'>保存逐题 / 多目标成绩，更新前沿及多父谱系 DAG<br/>有效同上下文缓存可复用，只补齐缺失试验</div>"]
FRONTIER -.- FRONTIER_PART_2
WINNER_PART_2["<div class='body'>留出失败保留生产旧版本，不把留出题反馈给反思继续搜索</div>"]
WINNER -.- WINNER_PART_2
SEARCH_NOTE["<div class='body'><b>搜索约束 · 独立说明</b><br/>L4 是后台搜索能力，候选实际仍为 L1 / L2 / L3<br/>val / holdout 题面不进入反思模型，普通试验轨迹不进入生产学习<br/>中间 accepted / winner 不等于已发布，最终看 published</div>"]
SEARCH_NOTE_PART_2["<div class='body'>种子验证、候选筛选、最终留出均调用 07；复用关系用文字标明<br/>谱系、候选、检查点与每次真实试验结果均保存用于恢复与追溯</div>"]
SEARCH_EVAL["<div class='body'><b>GEPA 复用 07</b><br/>提交 train / val / holdout 试验<br/>接收逐题成绩、执行证据与成本</div>"]
SEARCH_AGAIN["<div class='body'><b>继续搜索时回到 PARENT</b><br/>预算耗尽或停止条件满足后固定 winner<br/>最终留出失败保留现有生产版本</div>"]
TRAINING_BACK["<div class='body'><b>训练补充轨迹回到 03</b><br/>仅经过任务、版本与来源校验的训练证据<br/>普通评测试验不进入生产学习</div>"]
WINNER -.- SEARCH_NOTE
SEARCH_NOTE -.- SEARCH_NOTE_PART_2
SEARCH_NOTE -.- SEARCH_EVAL
SEARCH_EVAL -.- SEARCH_AGAIN
SEARCH_AGAIN -.- TRAINING_BACK
end
end
subgraph M6_PUBLISH["策略版本发布"]
direction LR
PUBLISH_MODE["<div class='body'><b>本次发布是否需要准入？</b><br/>以当前部署配置和候选类型为准</div>"]
GATE_RESULT["<div class='body'><b>准入判断</b><br/>覆盖率、确定评分、配对样本与通过率均达标<br/>配置的成本和无效循环指标也必须满足</div>"]
GATE_FAILURE["<div class='body'><b>准入未通过或指标缺失</b><br/>保留现有策略版本<br/>GEPA 不用 holdout 结果继续反思搜索</div>"]
READY["<div class='body'><b>登记可供新 run 使用的策略版本</b><br/>无准入：按当前获准记忆和配置编译<br/>启用准入：成绩通过，且仍是该用户 / 层级的最新候选<br/>还须确认生产父版本未变，随后持久化 publication 并刷新视图</div>"]
READY_PART_2["<div class='body'>GEPA 发布的是完整策略版本，不把搜索改写当作新记忆证据<br/>未通过保留现有版本；已开始的 run 继续使用固定快照</div>"]
VERSION_EXPORT["<div class='body'><b>新策略版本可用</b><br/>持久化 publication 并刷新视图<br/>仅影响后续新 run</div>"]
PUBLISH_MODE -->|需要：校验已有评测结果| GATE_RESULT
PUBLISH_MODE -->|无需准入| READY
GATE_RESULT -->|未通过 / 指标缺失| GATE_FAILURE
GATE_RESULT -->|通过| READY
READY -.- READY_PART_2
READY --> VERSION_EXPORT
end
end
end
PERIPHERY -->|同一接入协议；下方展开同步执行与后台演化| LANES
FOREGROUND <-->|轨迹与回执 →；← 后续新 run 的策略版本| BACKGROUND
VERSION_IMPORT -.->|新 run 选定快照；当前 run 不变| ONLINE_ENTRY
ONLINE_ENTRY -->|按当前边界和有效层级分派| HANDLERS
HANDLERS -->|宿主采用 input / decision| M6_RECEIPTS
M6_RECEIPTS -->|真实执行核验后上报| TRACE_EXPORT
M3 -->|Episode 与诊断| M4
M4 -->|满足门槛的经验候选| M5
M5 -->|合格私有记忆 / 获准公共投影| M6_PREP
M6_PREP -->|启用普通准入或 GEPA| ASSESSMENT
M6_PREP -->|无准入：按当前授权编译| M6_PUBLISH
M8 <-->|调用真实试验；读取逐题成绩| M7
ASSESSMENT -->|固定最终候选后取得准入结果| M6_PUBLISH
classDef step fill:#fff,stroke:#94a3b8,color:#0f172a
classDef input fill:#f8fafc,stroke:#94a3b8,color:#0f172a
classDef hook fill:#fffbeb,stroke:#d97706,color:#0f172a
classDef native fill:#ecfeff,stroke:#0891b2,color:#0f172a
classDef http fill:#eef2ff,stroke:#6366f1,color:#0f172a
classDef note fill:#f8fafc,stroke:#cbd5e1,color:#0f172a,stroke-dasharray:4 3
classDef capture fill:#f5f3ff,stroke:#a78bfa,color:#0f172a
classDef reflect fill:#faf5ff,stroke:#c084fc,color:#0f172a
classDef memory fill:#ecfdf5,stroke:#34d399,color:#0f172a
classDef database fill:#d1fae5,stroke:#059669,color:#0f172a
classDef policy fill:#f0fdf4,stroke:#86efac,color:#0f172a
classDef l1 fill:#ecfeff,stroke:#22d3ee,color:#0f172a
classDef l2 fill:#fff7ed,stroke:#fdba74,color:#0f172a
classDef l3 fill:#eef2ff,stroke:#a5b4fc,color:#0f172a
classDef evaluation fill:#fff7ed,stroke:#fbbf24,color:#0f172a
classDef search fill:#fffbeb,stroke:#f59e0b,color:#0f172a
classDef bridge fill:#f0f9ff,stroke:#0284c7,color:#0f172a
classDef section fill:#e2e8f0,stroke:#64748b,color:#0f172a
classDef role fill:transparent,stroke:transparent,color:#0f172a
classDef nativeTitle fill:#cffafe,stroke:#0891b2,color:#0f172a
classDef httpTitle fill:#e0e7ff,stroke:#6366f1,color:#0f172a
class NATIVE_STEP_1,NATIVE_STEP_2,NATIVE_STEP_3,NATIVE_STEP_4,NATIVE_STEP_5,NATIVE_STEP_6,NATIVE_STEP_7,NATIVE_STEP_8 native
class NATIVE_NOTE_1,NATIVE_NOTE_2,NATIVE_NOTE_3,NATIVE_NOTE_4,NATIVE_NOTE_5,NATIVE_NOTE_6,NATIVE_NOTE_7,NATIVE_NOTE_8,NATIVE_NOTE_9,NATIVE_NOTE_10,HTTP_NOTE_1,HTTP_NOTE_2,HTTP_NOTE_3,HTTP_NOTE_4,HTTP_NOTE_5,HTTP_NOTE_6,HTTP_NOTE_7,HTTP_NOTE_8,HTTP_NOTE_9,HTTP_NOTE_10,HTTP_NOTE_11,DEPLOY_CONFIG,POINT_FALLBACK,MATCH_LANGUAGE,MATCH_LANGUAGE_PART_2,MATCH_OBSERVATION,MATCH_OBSERVATION_PART_2,L1_NOTE,L1_NOTE_PART_2,L2_NOTE,L2_NOTE_PART_2,L3_NOTE,L3_NOTE_PART_2,RECEIPT_NOTE,REFLECTION_NOTE,MEMORY_MAINTENANCE,MEMORY_MAINTENANCE_PART_2,MEMORY_FEEDBACK,MEMORY_FEEDBACK_PART_2,POLICY_CONFIG,DATASET_NOTE,DATASET_NOTE_PART_2,BOOTSTRAP,BOOTSTRAP_PART_2,SEARCH_NOTE,SEARCH_NOTE_PART_2,SEARCH_EVAL,SEARCH_AGAIN,TRAINING_BACK note
class HTTP_STEP_1,HTTP_STEP_2,HTTP_STEP_3,HTTP_STEP_4,HTTP_STEP_5,HTTP_STEP_6,HTTP_STEP_7,HTTP_STEP_8 http
class POINT_KIND,POINT_REF,POINT_REF_PART_2,POINT_DATA,POINT_DATA_PART_2,POINT,ENTRY,POINT_RESULT,POINT_RESULT_PART_2 input
class TRACE_EXPORT,VERSION_IMPORT,VERSION_EXPORT bridge
class RUN_BEGIN,LOOP_POINT,MODEL_BEFORE,RUN_EXEC,MODEL_AFTER,NEED_ACTION,ACTION_BEFORE,ACTION_EXEC,ACTION_AFTER,OBSERVE,FINAL_ANSWER,RUN_END hook
class LOOP_BACK step
class SNAPSHOT,SNAPSHOT_PART_2,QUERY_PREP,QUERY_PREP_PART_2,QUERY_BUILD,QUERY_BUILD_PART_2,MATCH_SCOPE,MATCH_SCOPE_PART_2,MATCH_TERMS,MATCH_TERMS_PART_2,MATCH_RANK,MATCH_RANK_PART_2,APPLY,POLICY_OBSERVE,POLICY_OBSERVE_PART_2,POLICY_VERIFY,POLICY_VERIFY_PART_2,POLICY_RECEIPT,POLICY_RECEIPT_PART_2,MEMORY_SELECT,MEMORY_SELECT_PART_2,POLICY_APPROVE,POLICY_APPROVE_PART_2,COMPILE,COMPILE_PART_2,READY,READY_PART_2 policy
class L1_CONTEXT,L1_CONTEXT_PART_2,L1_RENDER,L1_RENDER_PART_2,L1_WRITE,L1_WRITE_PART_2,L1_RETURN,L1_RETURN_PART_2 l1
class L2_INPUT,L2_INPUT_PART_2,L2_RULE,L2_RULE_PART_2,L2_DECIDE,L2_DECIDE_PART_2,L2_RETURN,L2_RETURN_PART_2 l2
class L3_STATE,L3_STATE_PART_2,L3_RULE,L3_RULE_PART_2,L3_DECIDE,L3_DECIDE_PART_2,L3_RETURN,L3_RETURN_PART_2 l3
class TRACE,TRACE_PART_2,LEARN_QUEUE,LEARN_QUEUE_PART_2,ANALYZE,ANALYZE_PART_2 capture
class EVALUATION,EVALUATION_PART_2,CANDIDATES,CANDIDATES_PART_2,REFLECTION,REFLECTION_PART_2,CANDIDATE_GATE,CANDIDATE_GATE_PART_2 reflect
class MEMORY_CURATE,MEMORY_CURATE_PART_2,PRIVATE_MEMORY_PART_2,PRIVATE_MEMORY_PART_3,PUBLIC_REVIEW,PUBLIC_REVIEW_PART_2,PUBLIC_RELATIONS,PUBLIC_RELATIONS_PART_2,PUBLIC_MEMORY_PART_2,PUBLIC_MEMORY_PART_3 memory
class PRIVATE_MEMORY,PUBLIC_MEMORY,DATASET database
class DATASET_PART_2,DATASET_PART_3,EVAL_JOB,EVAL_JOB_PART_2,EVAL_AGENT,EVAL_AGENT_PART_2,EVAL_EVIDENCE,EVAL_EVIDENCE_PART_2,TASK_SCORE,TASK_SCORE_PART_2,SCORE_AGGREGATE,SCORE_AGGREGATE_PART_2,GATE_CHECK,GATE_CHECK_PART_2 evaluation
class SEED,SEED_PART_2,PARENT,PARENT_PART_2,PROPOSE,PROPOSE_PART_2,FRONTIER,FRONTIER_PART_2,WINNER,WINNER_PART_2 search
style PERIPHERY fill:#f8fafc,stroke:#cbd5e1,stroke-width:1.5px,color:#0f172a
style M1_INPUT fill:#f8fafc,stroke:#cbd5e1,stroke-width:1.5px,color:#0f172a
class POINT_REF_PART_2 note
class POINT_DATA_PART_2 note
style INTERFACES fill:#f8fafc,stroke:#cbd5e1,stroke-width:1.5px,color:#0f172a
class TRANSPORT_CALL bridge
style NATIVE_INTERFACE fill:#ecfeff,stroke:#67e8f9,stroke-width:1.5px,color:#0f172a
style NATIVE_OPERATIONS fill:#f8fafc,stroke:#cbd5e1,stroke-width:1.5px,color:#0f172a
style HTTP_INTERFACE fill:#eef2ff,stroke:#a5b4fc,stroke-width:1.5px,color:#0f172a
style HTTP_OPERATIONS fill:#f8fafc,stroke:#cbd5e1,stroke-width:1.5px,color:#0f172a
class TRANSPORT_RETURN bridge
style M1_RESULT fill:#f8fafc,stroke:#cbd5e1,stroke-width:1.5px,color:#0f172a
class POINT_RESULT_PART_2 note
style LANES fill:#f8fafc,stroke:#cbd5e1,stroke-width:1.5px,color:#0f172a
style FOREGROUND fill:#eff6ff,stroke:#93c5fd,stroke-width:1.5px,color:#0f172a
style ONLINE_ENTRY fill:transparent,stroke:transparent,stroke-width:1.5px,color:#0f172a
style M2 fill:#eff6ff,stroke:#93c5fd,stroke-width:1.5px,color:#0f172a
style M6_MATCH fill:#eff6ff,stroke:#93c5fd,stroke-width:1.5px,color:#0f172a
class SNAPSHOT_PART_2 note
class QUERY_PREP_PART_2 note
class QUERY_BUILD_PART_2 note
class MATCH_SCOPE_PART_2 note
class MATCH_TERMS_PART_2 note
class MATCH_RANK_PART_2 note
class MATCH_LANGUAGE_PART_2 note
class MATCH_OBSERVATION_PART_2 note
style HANDLERS fill:#eff6ff,stroke:#93c5fd,stroke-width:1.5px,color:#0f172a
class INJECTION_ROUTER bridge
style M6_L1 fill:#eff6ff,stroke:#93c5fd,stroke-width:1.5px,color:#0f172a
class L1_CONTEXT_PART_2 note
class L1_RENDER_PART_2 note
class L1_WRITE_PART_2 note
class L1_RETURN_PART_2 note
class L1_NOTE_PART_2 note
style M6_L2 fill:#eff6ff,stroke:#93c5fd,stroke-width:1.5px,color:#0f172a
class L2_INPUT_PART_2 note
class L2_RULE_PART_2 note
class L2_DECIDE_PART_2 note
class L2_RETURN_PART_2 note
class L2_NOTE_PART_2 note
style M6_L3 fill:#eff6ff,stroke:#93c5fd,stroke-width:1.5px,color:#0f172a
class L3_STATE_PART_2 note
class L3_RULE_PART_2 note
class L3_DECIDE_PART_2 note
class L3_RETURN_PART_2 note
class L3_NOTE_PART_2 note
class HANDLER_RETURN bridge
style M6_RECEIPTS fill:#eff6ff,stroke:#93c5fd,stroke-width:1.5px,color:#0f172a
class POLICY_OBSERVE_PART_2 note
class POLICY_VERIFY_PART_2 note
class POLICY_RECEIPT_PART_2 note
style BACKGROUND fill:#faf5ff,stroke:#c4b5fd,stroke-width:1.5px,color:#0f172a
style M3 fill:#faf5ff,stroke:#c4b5fd,stroke-width:1.5px,color:#0f172a
class TRACE_PART_2 note
class LEARN_QUEUE_PART_2 note
class ANALYZE_PART_2 note
style M4 fill:#faf5ff,stroke:#c4b5fd,stroke-width:1.5px,color:#0f172a
class EVALUATION_PART_2 note
class CANDIDATES_PART_2 note
class REFLECTION_PART_2 note
class CANDIDATE_GATE_PART_2 note
style M5 fill:#ecfdf5,stroke:#6ee7b7,stroke-width:1.5px,color:#0f172a
class MEMORY_CURATE_PART_2 note
class PRIVATE_MEMORY_PART_2 note
class PRIVATE_MEMORY_PART_3 note
class PUBLIC_REVIEW_PART_2 note
class PUBLIC_RELATIONS_PART_2 note
class PUBLIC_MEMORY_PART_2 note
class PUBLIC_MEMORY_PART_3 note
class MEMORY_MAINTENANCE_PART_2 note
class MEMORY_FEEDBACK_PART_2 note
style M6_PREP fill:#faf5ff,stroke:#c4b5fd,stroke-width:1.5px,color:#0f172a
class MEMORY_SELECT_PART_2 note
class POLICY_APPROVE_PART_2 note
class COMPILE_PART_2 note
style ASSESSMENT fill:transparent,stroke:transparent,stroke-width:1.5px,color:#0f172a
style M7 fill:#faf5ff,stroke:#c4b5fd,stroke-width:1.5px,color:#0f172a
class DATASET_PART_2 note
class DATASET_PART_3 note
class EVAL_JOB_PART_2 note
class EVAL_AGENT_PART_2 note
class EVAL_EVIDENCE_PART_2 note
class TASK_SCORE_PART_2 note
class SCORE_AGGREGATE_PART_2 note
class GATE_CHECK_PART_2 note
class DATASET_NOTE_PART_2 note
class BOOTSTRAP_PART_2 note
style M8 fill:#faf5ff,stroke:#c4b5fd,stroke-width:1.5px,color:#0f172a
class SEED_PART_2 note
class PARENT_PART_2 note
class PROPOSE_PART_2 note
class FRONTIER_PART_2 note
class WINNER_PART_2 note
class SEARCH_NOTE_PART_2 note
style M6_PUBLISH fill:#faf5ff,stroke:#c4b5fd,stroke-width:1.5px,color:#0f172a
class PUBLISH_MODE bridge
class READY_PART_2 note
```
