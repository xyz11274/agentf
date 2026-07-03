# Bridge E2E 测试设计

> 状态：部分自动化 · 已落地并验证 22 / 25 TC
> 更新日期：2026-07-03
> 测试目录：[test-catalog.md](test-catalog.md)

## 1. 测试目标与范围

Bridge E2E 测试看护以下真实进程链路：

```text
External Client / UI
        ↓ CoreMind callback / HTTP or WebSocket
 C++ SideCar（生产外部入口）
        ↓ HTTP（任务回调转发）
      Bridge
        ↓ daemon protocol
    agentflowd
        ↓ workflow / coordinator / skill
 Scripted LLM + tools
        ↓
 response + artifact files
        ↓ HTTP GET
External Client / UI
```

E2E 必须通过真实网络端口启动 Bridge 和 daemon，不得把 `DaemonClient`、消息队列、文件路由或进程生命周期替换成 mock。模型服务使用本地脚本化的 OpenAI 兼容服务，以确定性响应触发正常、截断、坏工具参数、超时等分支。

本设计不评测穿搭建议的审美质量，也不把真实外部模型的稳定性作为 PR 合并条件。Agent 问答质量应进入独立 Agent 能力评测；真实模型网络只进入非阻塞 canary。

## 2. 当前测试基线与缺口

| 现有资产 | 能看护什么 | 关键缺口 |
|----------|------------|----------|
| `test_chat.py` | WS 编排、错误帧、stop 的 mock 测试 | mock 了 daemon，抓不到协议、进程和消息路由问题 |
| `test_client_frame_reassembly.py` | Bridge 客户端消息分发 | 没有真实大响应跨进程验证 |
| `e2e_outfit_multiturn.py` | 真实 WS 多轮对话 | 独立脚本，未形成稳定 pytest fixture，依赖真实模型 |
| `test_outfit_e2e.py` | daemon 工作流是否执行完成 | 明确绕过 Bridge，且只读日志，不能证明客户端收到结果 |
| `test_concurrency_guard.py` | skill 提前结束竞态线索 | 主请求绕过 Bridge，不能看护 Bridge 端到端返回 |
| `e2e_test.py` | 旧 WS 流程 | 仍使用已废弃的 `session_id` 客户端协议 |
| Agent 层 artifact/recovery 单测 | dispatch key、坏参数恢复、路径转换 | 没有验证产物跨 daemon、Bridge 到外部客户端的完整传输 |

结论：当前测试存在“节点执行成功，但外部客户端仍拿不到响应或图片”的盲区。新的 E2E 门禁必须以客户端实际收到并可消费最终结果为成功条件，daemon 日志只作为失败诊断证据。

## 3. 业务约束表

| 约束 ID | 业务规则 | 违反后的业务影响 |
|---------|----------|------------------|
| B1 | Bridge 接受有效任务后必须在有限时间内返回最终结果或明确错误 | 外部服务永久等待，表现为“发送消息不返回” |
| B2 | REST 与 WS 必须保持各自公开契约，REST 返回 envelope，WS 按 chunk → done 收口 | 对接方解析失败或无法判断任务结束 |
| B3 | 同一 `user_id + client_id` 的多轮历史连续，不同身份历史隔离 | 丢失上下文或发生跨用户数据泄漏 |
| B4 | 每次 skill 委派具有独立执行上下文；同一轮重复委派不得复用上次 message list | 后一次 skill 被前一次历史污染 |
| B5 | 工具参数截断或 JSON 非法时不得执行残缺调用，也不得静默中断整轮 | 执行错误操作或用户得不到最终解释 |
| B6 | 已成功生成的 Artifact 不因后续 LLM/工具失败而丢失 | 图片存在于服务器，但客户端不可见 |
| B6a | Artifact 为可选输出；仅当请求及执行计划明确承诺生成时，缺失才构成未满足需求 | 普通文本请求被误判失败，或图片请求在无产物时被误报成功 |
| B7 | 图片与 HTML 必须通过客户端可访问的 URL 交付 | 返回本地绝对路径，外部服务无法访问 |
| B8 | HTML 目录层级必须保留，相对图片、CSS、JS 可继续加载 | HTML 能打开但内容残缺 |
| B9 | Artifact 只能读取包根目录内的白名单文件，并返回正确 MIME | 目录穿越、敏感文件泄漏或图片无法显示 |
| B10 | 公网基地址配置后，REST 与 WS 都返回绝对 Artifact URL；未配置时保留相对 URL | 内置 UI 或外部服务其中一方不可用 |
| B11 | daemon 正常退出、重启及连续多轮执行不得留下导致 mutex 错误的损坏状态 | 服务无法重启或运行若干轮后退化 |
| B12 | 并发或连续任务不得串线、丢消息、提前 final 或重复 final | 返回错误用户/错误 skill 的结果 |
| B13 | 外部模型网络失败必须与 Bridge/daemon 内部失败可区分 | 把内部缺陷误判为网络波动，无法定位 |
| B14 | C++ SideCar 必须与启动脚本、Python 客户端和 Adapter 保持同一端口/路由契约，并原样传递任务语义 | 生产唯一入口无法启动、被误判为不可用，或追踪信息丢失 |

## 4. 系统不变量

- I1：每个被接受的 turn 最终恰好出现一个终态：成功结果或明确错误。
- I2：WS 最多一个 `final=true` chunk，随后恰好一个 `done`；`done` 后不得继续输出本轮内容。
- I3：REST 成功响应始终为 `success=true`、`data.task_id` 非空、`data.content` 可消费、`error=null`。
- I4：同身份上下文连续，异身份上下文不可见。
- I5：一个 dispatch 的消息历史、重试计数和工具调用状态不能泄漏到另一个 dispatch。
- I6：残缺工具参数永不进入工具执行器；恢复成功或耗尽重试后都必须形成最终用户可见结果。
- I7：Artifact 文件字节与生成文件一致；图片 MIME 依据真实内容识别。
- I8：聊天响应不得暴露服务端本地绝对路径。
- I9：HTML 与其相对资源共享同一可解析 URL 目录树。
- I10：路径规范化后位于包根目录之外的请求始终被拒绝。
- I11：SIGTERM 后 daemon 在规定等待窗口内退出，退出日志无 `mutex lock failed`，同端口可重新启动并完成新 turn。
- I12：响应内容、task、dialogue、用户和客户端路由保持一致，不跨任务串线。
- I13：SideCar 三个任务入口保留请求体与追踪头，保留 Adapter 状态码与响应类型；依赖不可达时有限失败。

## 5. 错误模型

| 错误模型 | 典型错误实现 | 对应 TC |
|----------|--------------|---------|
| 终态丢失 | daemon 已完成，但 Bridge 未消费 notification | TC-brg-001、002、004 |
| 契约漂移 | REST 返回旧的裸对象或 WS 帧顺序变化 | TC-brg-002、003 |
| 帧截断/拼接错误 | 大 JSON 响应被误判为 malformed frame | TC-brg-002 |
| 状态未隔离 | session 或 dispatch 复用 message list | TC-brg-005、006、007 |
| 错误失败升级 | 坏工具 JSON 直接终止整轮 | TC-brg-008、009 |
| 危险容错 | 将残缺参数强制转换为空对象并执行工具 | TC-brg-008、009 |
| 中间结果丢失 | 图片生成成功，后续失败覆盖 Artifact | TC-brg-010 |
| 路径语义错误 | 返回本地路径或扁平化 HTML 目录 | TC-brg-011、012、013 |
| 安全边界遗漏 | `../`、绝对路径或 symlink 逃逸 | TC-brg-014 |
| 配置分支漂移 | 只在 REST 或只在 WS 补公网 URL | TC-brg-013 |
| 生命周期竞态 | 全局对象先析构、mutex 后使用 | TC-brg-015、016 |
| 并发竞态 | skill 未完成 coordinator 就发送 final | TC-brg-017 |
| 依赖归因错误 | 外部连接错误被报告为内部解析错误 | TC-brg-018 |
| SideCar 契约漂移 | CMake 不可构建、健康路由/端口不一致、别名入口丢失追踪头 | TC-brg-019、020、021 |

## 6. 测试架构

### 6.1 确定性门禁环境

每个测试会话创建独立临时目录和动态端口，启动以下真实组件：

1. 本地脚本化 LLM HTTP 服务；
2. 使用专用 E2E Agent Package 的 `agentflowd` 子进程；
3. 指向该 daemon 的 Bridge 子进程；
4. HTTP、WebSocket 和 Artifact 客户端。

脚本化 LLM 根据测试场景返回固定响应序列：普通文本、合法工具调用、`finish=length` 的残缺调用、非法 arguments JSON、工具成功后模型失败、延迟和连接中断。这样既走真实模型 HTTP 协议，又不依赖公网和模型随机性。

专用 Agent Package 应包含确定性工具：

- `echo_context`：返回当前 dispatch 标识和输入摘要；
- `record_tool_call`：记录是否收到残缺参数；
- `generate_fixture_artifacts`：生成固定字节图片、HTML 和相对资源树；
- `slow_skill`：通过受控事件制造并发顺序。

### 6.2 进程隔离与诊断

- 每个测试使用独立 DataBus/临时目录，不执行全局 `/tmp/agentflow_*` 通配删除。
- 测试启动前探测端口，启动后通过健康接口等待就绪，禁止固定 sleep 代替 readiness。
- 结束时先发 SIGTERM 并等待；超时才 SIGKILL，同时把退出码和日志作为失败附件。
- 每个 turn 使用唯一 `user_id`、`client_id` 和可追踪输入 token。
- 失败报告必须附 Bridge 日志、daemon 日志、脚本化 LLM 请求记录和最后收到的客户端帧。

### 6.3 真实模型 canary

真实 MiniMax/OpenAI 测试只验证“当前部署是否能连通并完成一次普通 turn”，结果单独分类：

- `provider_network_failure`：DNS、TLS、连接重置、连接或读取超时；
- `provider_protocol_failure`：HTTP 状态、响应 Schema、finish reason 异常；
- `agent_runtime_failure`：工具解析、workflow、daemon 或 Bridge 错误；
- `success`。

该 canary 默认不阻塞 PR；连续失败告警不能替代确定性 E2E 门禁。

## 7. 高价值场景组合

| 场景 | 约束 | 关键变量组合 | 目标错误模型 | 保护语义 | 优先级 |
|------|------|--------------|--------------|----------|--------|
| S1 | B1,B2 | REST/WS × 普通/大响应 | 终态丢失、帧截断 | 两种入口都能完成并返回完整内容 | P0 |
| S2 | B3 | 相同/不同 user-client × 多轮 | 状态未隔离 | 连续且不串线 | P0 |
| S3 | B4 | 同轮重复同 skill × 不同 dispatch | message list 复用 | 每次委派历史独立 | P0 |
| S4 | B5 | `finish=length`/坏 JSON × 恢复成功/重试耗尽 | 危险容错、错误中断 | 不执行残缺工具且有终态 | P0 |
| S5 | B6-B10 | 图片/HTML × 相对/公网 URL × 后续失败 | 中间结果丢失、路径错误 | 外部客户端实际 GET 成功 | P0 |
| S6 | B9 | 正常/`../`/绝对/symlink 路径 | 安全边界遗漏 | 根目录外文件不可读取 | P0 |
| S7 | B11 | 正常退出/连续多轮后退出 × 重启 | 生命周期竞态 | 无 mutex 错误且可重启 | P0 |
| S8 | B12 | 串行/并发 × 快慢 skill | 提前 final、串线 | skill 完成后唯一 final | P0 |
| S9 | B13 | 本地成功/公网网络失败/协议失败 | 归因错误 | 故障分类准确 | P1 |
| S10 | B14 | 本地端点/三个任务入口/依赖不可达 | 契约漂移、头部丢失、伪成功 | 真实 C++ 进程可构建并正确对接 Adapter | P0 |

不改变执行分支的城市名、衣服颜色等输入差异不单独建立工程 TC；它们属于 Agent 能力评测样本。

## 8. 正式测试用例

### TC-brg-001：REST 主链路返回可消费结果

- 用户输入：“请回复本次请求中的确认码。”
- 覆盖：B1、B2。
- 断言：真实 Bridge 接受请求；daemon 完成；REST envelope、task_id、确认码和终态完整。
- 失败意味着：Bridge → daemon 主链路仍存在“发送消息不返回”或消息丢失。

### TC-brg-002：WS 大响应完整收口

- 用户输入：“请完整返回这段较长内容。”
- 覆盖：B1、B2。
- 断言：拼接后的字节与脚本化模型预期完全一致；唯一 final chunk 后紧跟 done；无 malformed frame。
- 失败意味着：真实协议在大响应下发生截断、错误拼接或终态丢失。

### TC-brg-003：REST 与 WS 对外契约一致

- 用户输入：“请返回一个简短确认。”
- 覆盖：B2。
- 断言：REST 使用 `success/data/error`；WS 使用 chunk/done；两者最终文本一致且都不暴露 session_id。
- 失败意味着：外部对接 Schema 发生漂移。

### TC-brg-004：daemon 无响应时有限失败

- 用户输入：“请执行当前任务。”
- 覆盖：B1。
- 断言：受控 daemon 停止响应后，REST 返回明确超时错误，WS 返回 error 并 done；连接和后台任务被清理。
- 失败意味着：调用方可能永久等待或 Bridge 泄漏 in-flight 状态。

### TC-brg-005：同身份多轮上下文连续

- 用户输入：先说“请记住代号青竹”，再问“我的代号是什么？”
- 覆盖：B3。
- 断言：第二轮返回“青竹”，无需客户端传 session_id。
- 失败意味着：`user_id + client_id` 会话路由失效。

### TC-brg-006：不同身份上下文隔离

- 用户输入：用户 A 保存代号，用户 B 询问自己的代号。
- 覆盖：B3、B12。
- 断言：B 的响应和 trace 均不包含 A 的代号。
- 失败意味着：发生跨用户历史泄漏。

### TC-brg-007：同轮重复 skill 委派历史隔离

- 用户输入：“分别完成两次独立检查，再汇总结果。”
- 覆盖：B4。
- 断言：脚本化 LLM 收到两个不同 dispatch 上下文；第二次 message list 不包含第一次私有工具消息；两次结果均进入汇总。
- 失败意味着：dispatch key 或执行实例复用导致 skill 历史污染。

### TC-brg-008：截断工具调用恢复且不误执行

- 用户输入：“生成一份测试结果。”
- 覆盖：B5。
- 断言：首个模型响应为 `finish=length` 且 arguments 残缺；工具执行记录为零；后续恢复产生合法调用并完成 turn。
- 失败意味着：残缺参数被执行，或解析失败直接中断。

### TC-brg-009：非法工具 JSON 重试耗尽后仍有终态

- 用户输入：“生成一份测试结果。”
- 覆盖：B1、B5。
- 断言：所有非法参数均未执行；达到重试上限后客户端收到可理解的失败说明，且仅一个终态。
- 失败意味着：错误被吞掉、无限重试或执行中断无返回。

### TC-brg-010：产物生成后模型失败仍交付 Artifact

- 用户输入：“生成图片，并给我结果。”
- 覆盖：B6。
- 断言：工具先生成固定图片，随后模型返回受控错误；最终响应仍包含 Artifact URL，GET 后字节与 fixture 一致。
- 失败意味着：已生成图片在后续失败路径中丢失。

### TC-brg-011：图片跨完整链路可访问且 MIME 正确

- 用户输入：“生成一张测试图片。”
- 覆盖：B7、B9。
- 断言：客户端从聊天内容取得 URL；URL 不含本地绝对路径；GET 200；字节一致；扩展名与内容不一致时仍返回真实 MIME。
- 失败意味着：前端看见链接却无法显示图片。

### TC-brg-012：HTML 相对资源树可访问

- 用户输入：“生成一份带图片的 HTML 报告。”
- 覆盖：B7、B8。
- 断言：GET HTML 成功；解析其中相对图片/CSS URL 后逐一 GET 成功；目录层级未扁平化。
- 失败意味着：HTML 文件存在，但外部客户端拿不到相对资源。

### TC-brg-013：公网与相对 Artifact URL 配置分支

- 用户输入：“生成一张测试图片。”
- 覆盖：B10。
- 断言：未配置时 REST/WS 均返回 `/api/artifacts/...`；配置公网基地址时两者均返回同一绝对前缀，且无双斜杠。
- 失败意味着：外部服务与内置 UI 只能有一方正常工作。

### TC-brg-014：Artifact 路径逃逸被拒绝

- 覆盖：B9。
- 断言：`../`、编码后的路径逃逸、绝对路径和指向根目录外的 symlink 均不可读取；合法包内文件仍可访问。
- 失败意味着：Bridge 可能泄漏宿主机文件。

### TC-brg-015：daemon 优雅退出并可重启

- 覆盖：B11。
- 断言：完成一个真实 turn 后发送 SIGTERM；进程正常退出；日志无 `mutex lock failed`；同端口重启后新 turn 成功。
- 失败意味着：全局析构或共享状态清理仍不安全。

### TC-brg-016：连续多轮后 daemon 不退化

- 用户输入：连续发送带唯一确认码的普通请求。
- 覆盖：B1、B11、B12。
- 断言：每轮都返回对应确认码；无跨轮串线；完成后仍可健康检查和优雅退出。
- 说明：轮数作为测试配置，不在设计中承诺生产容量；CI 使用能覆盖历史“若干轮后退化”故障的最小值。
- 失败意味着：存在累计资源泄漏或共享内存状态损坏。

### TC-brg-017：并发快慢 skill 不提前 final、不串线

- 用户输入：两个不同客户端分别请求快任务和受控慢任务。
- 覆盖：B12。
- 断言：慢任务的 final 晚于其 skill 完成事件；每个客户端只收到自己的确认码；每轮唯一 final。
- 失败意味着：coordinator 竞态、notification 队列串线或提前完成。

### TC-brg-018：真实模型 canary 故障分类

- 用户输入：“请回复连接测试成功。”
- 覆盖：B13。
- 断言：成功、网络错误、协议错误和内部运行时错误进入不同分类并保留原始证据。
- 状态：夜间非阻塞；不得替代 TC-brg-001～017。
- 失败意味着：故障归因不可用，但不直接证明 Bridge 主链路回归。

### TC-brg-019：SideCar 本地端点契约

- 覆盖：B14。
- 断言：真实 C++ 进程同时支持 `/health` 与 `/healthcheck`；能力宣告和 Agent 管理路由可用。
- 失败意味着：启动脚本或 Python 客户端会把健康 SideCar 判为不可用。

### TC-brg-020：SideCar 任务转发保留完整契约

- 覆盖：B14。
- 断言：三个公开任务入口均经真实 C++ 进程转发；请求体、`x-request-id`、Adapter 状态码和 `Content-Type` 不丢失。
- 失败意味着：部分入口丢失任务或追踪上下文，对接方可能收到伪成功。

### TC-brg-021：SideCar 依赖不可达时有限失败

- 覆盖：B14。
- 断言：Adapter 不可达返回 502；本地无 CoreMind 构建的服务发现返回 503/批量 partial，不伪造成功。
- 失败意味着：上游无法区分任务已接受与内部依赖中断。

### TC-brg-022：伪造内部上下文被拒绝

- 覆盖：B14、T1、T2。
- 断言：缺少或错误内部凭据时 Bridge 返回 401，且不调用 `agent.execute`。
- 失败意味着：外部调用者可以伪造服务地址并影响 Node 出站连接。

### TC-brg-023：Bridge 将可信上下文提交到 AgentFlow

- 覆盖：B14、T4。
- 断言：Bridge 保持 `platform_context` 结构，并与 `request_id` 一起提交到 daemon。
- 失败意味着：SideCar 已发现服务，但跨 Bridge 边界后静默丢失。

### TC-brg-024：SideCar → Bridge → AgentFlow → Node 完整链路

- 覆盖：B14、I13、T1～T4。
- 断言：真实 C++ SideCar、Python Bridge、agentflowd 和确定性 Node 全部启动；Node 返回本次请求的 `request_id` 及已解析实例。
- 失败意味着：组件契约分别正确，但生产完整链路仍无法向 Node 交付可信上下文。

### TC-brg-025：不一致的服务快照在 Bridge 边界拒绝

- 覆盖：B14、I13。
- 断言：`resolved` 服务没有实例，或 `unavailable` 服务没有错误码时返回 422，且不调用 daemon。
- 失败意味着：语义自相矛盾的上下文进入 Node，只能在运行期以模糊连接错误失败。

## 9. 测试分层与门禁

| 套件 | 内容 | 外部网络 | 建议门禁 |
|------|------|----------|----------|
| Contract/Unit | 路由 Schema、URL 改写、路径与 MIME 边界 | 否 | 每次 PR |
| Deterministic E2E P0 | TC-brg-001～017 真实 Bridge + daemon；TC-brg-019～024 覆盖 C++ SideCar 与可信上下文 | 否 | 每次 PR；较慢用例可拆为合并前必跑组 |
| Live Provider Canary | TC-brg-018 | 是 | Nightly/手工，不阻塞 PR |

P0 E2E 的成功定义是“外部客户端实际收到并消费结果”，不能用日志中的 `Response ready` 或节点 `Exit code=0` 替代。

## 10. 自动化落地约束

建议代码结构：

```text
frontend/bridge/tests/e2e/
├── conftest.py                 # 动态端口、临时目录、真实进程生命周期
├── scripted_llm_server.py      # 确定性模型场景
├── fixture_agent/              # 专用 Agent Package
├── test_chat_transport.py      # TC-brg-001～004
├── test_session_isolation.py   # TC-brg-005～007
├── test_tool_recovery.py       # TC-brg-008～009
├── test_artifact_delivery.py   # TC-brg-010～014
├── test_daemon_lifecycle.py    # TC-brg-015～016
├── test_concurrency.py         # TC-brg-017
└── test_live_provider.py       # TC-brg-018
└── test_coremind_sidecar_e2e.py # TC-brg-019～024
```

每个 pytest 函数 docstring 首行必须包含 TC ID，并写明业务规则、错误模型和失败含义。实现状态以 [test-catalog.md](test-catalog.md) 为准。

当前已落地：TC-brg-001～003、005、006、008～010、016 使用真实 Bridge + daemon + 脚本化 LLM；TC-brg-011～014 已落地 Artifact 路由与聊天 URL 的集成/契约自动化，其中图片跨完整聊天链路由 TC-brg-010 覆盖；TC-brg-015 使用真实 daemon 子进程验证退出和同端口重启；TC-brg-017 已覆盖并发响应隔离，skill 完成前不得 final 的分支仍待补齐；TC-brg-019～021 使用本地关闭 CoreMind 发现的同一 C++ SideCar 二进制验证生产入口契约。

## 11. 可拦截的回归

本设计能直接拦截：消息无返回、大响应 malformed frame、旧 session_id 契约回潮、多轮串线、重复 skill_runner 历史污染、`finish=length` 残缺工具误执行、坏 JSON 导致整轮中断、图片已生成但未返回、HTML 相对资源失效、外部服务收到本地路径、错误 MIME、路径穿越、daemon 退出 mutex 错误、连续执行退化、coordinator 提前 final，以及把公网模型波动误归因为内部解析故障。

不能由本套工程 E2E 稳定判断的内容：穿搭质量、模型审美、自然语言覆盖率和真实 provider 的长期 SLA。这些应分别由 Agent 能力评测和独立可用性监控承担。
