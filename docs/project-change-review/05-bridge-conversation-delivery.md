# Bridge 会话与最终交付模块改动

## 1. 模块职责

Bridge 将前端 REST/SSE 请求转换为 Daemon Dialogue，保存多轮会话，向前端交付流式或非流式结果，并把实际交付事实写回轨迹。

主文件：

- frontend/bridge/routes/chat_rest.py
- frontend/bridge/client.py
- frontend/bridge/models.py
- frontend/bridge/providers/local_session_provider.py
- frontend/bridge/providers/session_provider.py
- frontend/bridge/services/sessionlog_delivery.py
- frontend/bridge/services/session_sync.py
- frontend/bridge/services/trace_puller.py
- frontend/bridge/routes/artifacts.py
- frontend/bridge/content_serializer.py

## 2. 用户、会话和轮次身份

前端字段的当前含义是：

- user_id：用户。
- client_id：该用户的逻辑会话，也就是轨迹中的 conversation_id。
- dialogue_id：一次用户输入到最终交付的单轮。
- Daemon session：底座内部路由和串行化槽位，不作为轨迹业务层级。

Bridge 按 user_id + client_id 获取或创建会话，并将历史轮次注入新的 Dialogue。相同用户的不同 client_id 不共享历史；不同用户即使 client_id 或 dialogue_id 相同也不能串线。

## 3. 活动 Dialogue 管理

Bridge 为 user_id + client_id 注册活动 Dialogue：

- 同一逻辑会话不会同时启动两个相互覆盖的 Dialogue。
- 记录精确 dialogue_id、task_id 和 Daemon session 路由。
- 完成、错误、超时和客户端断开都会注销活动项。
- 取消按精确路由找到原 Daemon Session，而不是猜“最近 Session”。

## 4. 多轮持久化

用户消息、最终助手内容和 Artifact 保存到 SessionProvider。当前修正包括：

- 流式 llm_chunk 只提取真正的 delta.content，不把整块 Provider JSON 存入助手历史。
- progress、思考过程和错误进度不会混入最终助手历史。
- 用户多模态输入保存可读文本和输入文件事实。
- 持久化失败时尽量保住该轮用户输入，不因为脱敏或序列化异常整轮丢失。
- SessionSync 兼容 Daemon agent.list 的包装对象，避免重启后会话同步为空。

## 5. SSE 业务终态与传输终态分离

原来 SSE 的 done 容易被理解为业务成功。当前分开处理：

- final：业务层候选结果及 Artifact。
- error：业务或执行错误。
- done：SSE 传输结束。
- disconnect：客户端中途断开。

只有真正 final 的内容进入最终助手历史。错误 progress 不再被拼成答案。客户端断开会触发底层 Dialogue 取消，避免前端离开后后台继续长期占用资源。

## 6. Coordinator 候选与实际交付

Coordinator 生成 dialogue.end 时记录的是候选答案。Bridge 实际可能还会：

- 转换 Artifact 链接。
- 根据输出模式替换内容。
- 发生传输错误或断开。
- 只交付候选内容的一部分。

因此新增 client.delivery，记录 raw_content、delivered_content、Artifact、传输状态和来源终态。轨迹树应把 client.delivery 当成“用户实际看到什么”的最终事实，而不是仅看 Coordinator 输出。

## 7. Artifact 对外访问

Bridge 不再直接暴露可预测的本地文件路径：

- Artifact 路由使用无拼接碰撞的身份键。
- 下载地址使用不可猜测的能力 token。
- URL 对外展示与内部持久化内容分开，数据库可保存真实关联，前端只拿公开链接。
- Artifact 提取器消费结构化元数据，不从任意文本猜文件。

## 8. 轨迹投递

Bridge 在同步和 SSE 路径都能把 client.delivery 追加到正确 SessionLog：

- 优先使用显式 AF_SESSIONLOG_DIR。
- 否则定位 Agent package 或项目 trace_output。
- 按 user_id、client_id 和 dialogue_id 精确匹配。
- 对旧目录布局保留兼容，但身份齐全时不跨用户回退。

这补上了“框架完成”到“前端实际交付”之间原本缺失的一段。

## 9. 形成的不变量

- client_id 是 Conversation 身份，不再额外派生第二个业务 Session。
- 每个 Dialogue 必须能精确取消和注销。
- transport done 不等于业务成功。
- progress 和 error 不能进入助手历史。
- Coordinator 候选答案不能冒充实际交付事实。
- Artifact 下载能力不能由用户、会话和文件名直接猜出。

对应变更主要见 CHG-066、070、073、074、075、076、084、085、087、088、089、095、096、105、106、109、113、114、115、116。

