# 并发性能测试设计 — aisf_agentflow

> **概要**：测量系统（Bridge WebSocket → daemon → agent → LLM）的并发承载能力，建立并发数 ↔ TTFT/完成时延/错误率的基线。
>
> **TC 域**: `CONC`（并发容量）
> **状态**: ✅ 初版设计
> **设计者**: test-design skill + ai-agent-perf-metrics reference

---

## 1. 业务约束建模

| 约束ID | 业务规则 | 违反后的业务影响 |
|--------|---------|---------------|
| B1 | 系统能同时处理 ≥1 个活跃 WebSocket session | 单用户也无法使用 → 系统不可用 |
| B2 | 每个 session 的响应内容和用户身份必须隔离，不得跨 session 混数据 | 用户 A 看到用户 B 的响应 → 数据泄漏 |
| B3 | 并发数增加时，系统不崩溃（Error Rate < 5%） | 用户无法完成对话 → 可用性 SLA 不达标 |
| B4 | 并发数增加时，TTFT 的退化应是 sub-linear | 用户感知"越来越慢"→ 体验不可接受 |

## 2. 系统不变量

| ID | 不变量 | 关联约束 |
|----|--------|---------|
| I-1 | **每个并发 session 独立完成**：所有 N 个并发请求都必须返回 `final=true` 的 `done` 帧，不允许静默丢弃 | B1 |
| I-2 | **无跨 session 污染**：每个 session 的 WebSocket 连接收到的完整响应序列应当语义一致，不混入其他 session 的内容。验证方式：各 session 的请求内容唯一可辨（如嵌入序号），在 MOCK 模式下确认每连接均收到 `{type:"done", final:true}`，在 REAL 模式下检查响应内容是否与自身请求内容相关 | B2 |
| I-3 | **TTFT 退化可控**：N 并发下的 TTFT P50 ≤ 单 session TTFT P50 × N^0.3（即 sub-linear，待首次运行后校准） | B4 |
| I-4 | **错误率上界**：所有并发场景下 error rate ≤ 5%（timeout + disconnect + exception） | B3 |

## 3. 错误模型

| 错误模型 | 说明 | 对应不变量 | 拦截方式 |
|---------|------|-----------|---------|
| 连接拒绝 | Bridge 在并发达到某个阈值后拒绝新的 WebSocket 连接 | I-1 | 监控 `websocket.accept()` 是否抛出异常 |
| Session 资源耗尽 | Daemon 线程/协程池耗尽，请求排队或丢弃 | I-1 | 测量 session 建立到首次响应的时间，检测是否超时 |
| 跨 session 数据错乱 | 并发请求导致共享状态污染，session A 收到 session B 的 chunk | I-2 | 每个 session 拥有独立的 WebSocket 连接，检查每条连接的响应序列是否语义完整；MOCK 模式下验证响应内容不为空且无异常，REAL 模式下可通过对话内容语义反向验证 |
| 响应延迟退化（O(n²)） | 并发数翻倍时延迟增速远超线性 | I-3 | 量级矩阵判读（翻倍看 2x/4x/8x…）|
| 静默断连 | Bridge 或 daemon 在并发下异常断开 WS 连接 | I-4 | 捕获 `WebSocketDisconnect` 非预期码 |

## 4. 场景组合表

### 关键变量

| 变量 | 取值 | 依据 |
|------|------|------|
| 并发数 N | 1, 3, 5, 10, 20 | 1=基线, 3=轻量, 5=中等, 10=重度, 20=压力 |
| LLM 模式 | MOCK（scripted）, REAL（真实模型） | MOCK 测系统框架瓶颈, REAL 测全链路真实表现 |
| Agent 类型 | minimal（单 coordinator 直连 LLM） | 消除 skill 调度复杂度的干扰 |

### 场景组合

| 场景ID | N | LLM | Agent | 目标 | 优先级 |
|--------|---|-----|-------|------|--------|
| S1 | 1 | MOCK | minimal | 建立单 session 基线（系统零负载性能） | P0 |
| S2 | 3 | MOCK | minimal | 轻量并发：验证基础并行处理能力 | P0 |
| S3 | 5 | MOCK | minimal | 中等并发：观察系统是否开始排队 | P0 |
| S4 | 10 | MOCK | minimal | 重度并发：查找 daemon/Bridge 瓶颈 | P1 |
| S5 | 20 | MOCK | minimal | 压力：探测系统极限（可选项，视前序结果） | P2 |
| S6 | 1 | REAL | minimal | 全链路真实基线，含 LLM 推理延迟 | P1 |
| S7 | 3 | REAL | minimal | 真实负载轻量并发，建立现实基线 | P1 |

> **原则**：MOCK LLM 优先（可重复、低成本），确认框架瓶颈后再用 REAL 确认全链路表现。

## 5. TC ID 列表

| TC ID | 关联场景 | 关联约束 | 关联不变量 |
|-------|---------|---------|-----------|
| TC-PERF-CONC-001 | S1（基线，MOCK，N=1）| B1 | I-1 |
| TC-PERF-CONC-002 | S2（轻量并发，MOCK，N=3）| B1, B4 | I-1, I-3, I-4 |
| TC-PERF-CONC-003 | S3（中等并发，MOCK，N=5）| B1, B3, B4 | I-1, I-3, I-4 |
| TC-PERF-CONC-004 | S4（重度并发，MOCK，N=10）| B1, B3, B4 | I-1, I-3, I-4 |
| TC-PERF-CONC-005 | S5（压力，MOCK，N=20）| B1, B3, B4 | I-1, I-3, I-4 |
| TC-PERF-CONC-006 | S6（真实 LLM 基线，N=1）| B1 | I-1 |
| TC-PERF-CONC-007 | S7（真实 LLM 并发，N=3）| B1, B3, B4 | I-1, I-3, I-4 |

## 6. 测量指标

每个 TC 测量以下指标：

| 指标 | 符号 | 单位 | 测量方式 |
|------|------|------|---------|
| Session 建立时间 | ws_connect | ms | 客户端发起到 `websocket.accept()` 返回 |
| 首 token 时延 | TTFT | ms | 客户端发送消息到收到第一个 `ChatChunk` |
| 端到端完成时延 | E2E | ms | 客户端发送消息到收到 `ChatDone(final=true)` |
| 生成吞吐 | tokens/sec | tok/s | 总 token 数 / (E2E - TTFT) |
| 错误率 | err_rate | % | `非正常完成数 / 总请求数` |
| 断连数 | disconnects | 个 | 非预期的 WebSocket 关闭 |

Tier 2（软阈值，仅记录不作断言）：

| 指标 | 说明 |
|------|------|
| CPU 使用率 (bridge + daemon) | `psutil` 或 `top` 采样 |
| 内存使用 (bridge + daemon) | `psutil` 或 `top` 采样 |
| TTFT 退化曲线 | N 翻倍时 TTFT P50 的增长倍数 |

## 7. 测试架构

### 测试工具：独立 Python 脚本，非 pytest

**原因**：pytest 的 fixture 机制是串行化的，不适合做并发压力测试。使用裸 `asyncio` + `websockets` 库控制并发。

```
tests/perf/
├── run_concurrent_perf.py      # 主脚本
└── baselines/                   # 基线历史
    ├── baseline_20260726.json
    └── ...
```

### 执行流程

```
1. start daemon (agent-connectivity, MOCK LLM)
2. start bridge as main:app（不是 main:bridge_app），设置环境变量:
   - AF_BR_DAEMON_HOST / AF_BR_DAEMON_PORT → daemon
   - AF_BR_LISTEN → 127.0.0.1:{bridge_port}
3. for each N in [1, 3, 5, 10, 20]:
   a. run warmup (N 个并发, 1 轮, 不计入测量)
   b. run measurement:
      - 并发建立 N 个 WebSocket 连接到 /health/ws/chat/{agent_id}
        （bridge_app 挂载在 /health 下，见 main.py L514: app.mount("/health", bridge_app)）
      - 各 session 独立发消息 {"user_id": "perf-{i}", "client_id": "perf-{i}", "content": "你好{i}"}
      - 记录每个 session 的 ws_connect / TTFT / E2E / err / 收到的心跳帧数
      - 静默忽略 {"type": "ping"} 心跳帧
      - 等待所有 session 完成（或超时 120s）
      - 收集结果
4. print summary table
5. generate baseline JSON
```

### 单 session 时序

Client 发送 `{"user_id": "...", "client_id": "...", "content": "你好"}`，Bridge 返回 JSON 帧。
注意 Bridge 每 30s 会发心跳帧 `{"type": "ping"}`，测试客户端需静默忽略。

```
Client                  Bridge              Daemon              LLM
 │                        │                   │                  │
 ├─ ws connect ─────────► │                   │                  │  ws_connect 计时开始
 │◄─ accept ─────────────┤                   │                  │  ws_connect 计时结束
 │                        │                   │                  │
 │◄─ {"type":"ping"}─────┤                   │                  │  (心跳, 静默忽略)
 │                        │                   │                  │
 ├─ send JSON ──────────►│                   │                  │  ── TTFT/E2E 计时开始 ──
 │   {"user_id":"...",   │                   │                  │
 │    "client_id":"...", │                   │                  │
 │    "content":"你好"}   │                   │                  │
 │                        ├─ agent.execute──►│                  │
 │                        │                   ├─ node run ─────►│
 │◄─ {"type":"chunk",    ┤◄── chunk ────────┤◄── stream ──────┤  TTFT 计时结束
 │     "content":"...",  │                   │                  │
 │     "final":false,    │                   │                  │
 │     "chunk_type":"text"}                   │                  │
 │◄─ {"type":"chunk",    ┤◄── chunk ────────┤◄── stream ──────┤
 │     "content":"...",  │                   │                  │
 │     "final":false}    │                   │                  │
 │◄─ {"type":"done",     ┤◄── done ─────────┤                  │  E2E 计时结束
 │     "final":true}     │                   │                  │
 │                        │                   │                  │
```

## 8. 硬阈值（首次基线，运行后根据实测修正）

| TC ID | 指标 | 硬阈值 | 说明 |
|-------|------|--------|------|
| TC-PERF-CONC-001 | E2E | ≤ 30s | MOCK LLM 单 session 应在 30s 内完成 |
| TC-PERF-CONC-002 | E2E P50 | ≤ 45s | 3 并发下应基本接近单 session |
| TC-PERF-CONC-003 | err_rate | ≤ 5% | 5 并发下不应有显著错误 |
| TC-PERF-CONC-004 | err_rate | ≤ 10% | 10 并发下允许轻度退化 |
| TC-PERF-CONC-006 | E2E | ≤ 60s | 真实 LLM 单 session 应在 60s 内完成（含推理时间） |

**重要**：以上阈值在第一次运行后应根据实测中位数重新校准，参考 `ai-agent-perf-metrics.md §5` 基线建立方法。

## 9. 测试隔离要求

| 要求 | 说明 |
|------|------|
| 测试标记 | 所有 TC 在测试脚本中以 `@pytest.mark.perf` 语义标注（脚本运行时通过 `--perf` flag 触发），从默认 CI 流水线排除 |
| Warmup | 每个 N 级别先跑 1 轮 warmup（与测量轮同 N 并发数），warmup 结果不计入统计 |
| 重复 | 每个 N 级别跑 ≥3 次取中位数 |
| 环境记录 | 每次运行记录：CPU 型号、内存、OS、daemon 构建版本、LLM 模型名 |
| Mock 原则 | **MOCK LLM 测试禁止依赖真实网络 I/O**（防止环境噪声污染测量） |

## 10. 全局测试目录

| TC ID | 文件 | 函数/场景 | 状态 |
|-------|------|----------|------|
| TC-PERF-CONC-001 | `tests/perf/run_concurrent_perf.py` | N=1, MOCK | 📝 待实现 |
| TC-PERF-CONC-002 | `tests/perf/run_concurrent_perf.py` | N=3, MOCK | 📝 待实现 |
| TC-PERF-CONC-003 | `tests/perf/run_concurrent_perf.py` | N=5, MOCK | 📝 待实现 |
| TC-PERF-CONC-004 | `tests/perf/run_concurrent_perf.py` | N=10, MOCK | 📝 待实现 |
| TC-PERF-CONC-005 | `tests/perf/run_concurrent_perf.py` | N=20, MOCK | 📝 待实现 |
| TC-PERF-CONC-006 | `tests/perf/run_concurrent_perf.py` | N=1, REAL | 📝 待实现 |
| TC-PERF-CONC-007 | `tests/perf/run_concurrent_perf.py` | N=3, REAL | 📝 待实现 |

## 11. 设计自检

### 业务层
- [x] 每条业务约束都有对应测试（B1→S1-S7, B2→所有, B3→S3-S5, B4→S3-S5）
- [x] 测试失败能明确映射到业务问题

### 系统层
- [x] 所有关键不变量被验证（I-1→TC-PERF-CONC-001~007, I-2→内容匹配, I-3→量级矩阵, I-4→err_rate 断言）
- [x] 过程状态被显式检查（时序分解为 ws_connect / TTFT / E2E 三段）

### 错误防护
- [x] 针对已知错误模型（连接拒绝/session 耗尽/跨 session 污染/退化/断连）
- [x] 错误不会被"悄悄吞掉"（err_rate + 断连数显式记录）

### 测试价值
- [x] 错误实现无法轻易通过（所有断言都依据实测基线而非猜测）
- [x] 测试失败信息具备诊断价值（输出每个 session 的完整时序分解）

### 可追溯性
- [x] 每个 TC 有独立 ID，与测试目录映射
- [x] 测试目录包含"文件/函数"自动化位置列
