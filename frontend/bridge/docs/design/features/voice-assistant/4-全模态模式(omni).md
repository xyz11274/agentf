# 全模态模式：Qwen3-Omni 语音直入直出 — 可行性分析

> **层级**: L3 · `docs/design/features/voice-assistant/4-全模态模式(omni).md`
> **状态**: 分析文档 · **暂不实现**
> **创建日期**: 2026-07-13

---

## 1. 概述

全模态模式（Omni Mode）是**端到端语音对话架构**：跳过显式 ASR/TTS 中间环节，使用 Qwen3-Omni-Realtime 统一模型直接处理语音输入并生成语音输出。

**核心特征**：
- 语音输入 → 统一模型推理 → 语音输出（跳过文本中间层）
- 内置 VAD + Turn Detection（server_vad），无需自建
- 保留语音中的语气、语调、情感等副语言信息
- 延迟 300-500ms，远低于级联管线

---

## 2. 目标

### 2.1 业务目标

为语音助手提供**第二个语音对话模式**，与管线模式互补：

| 维度 | 管线模式 | 全模态模式 |
|------|---------|-----------|
| 延迟 | 800ms-2s | 300-500ms |
| 情感保留 | 丢失 | 保留 |
| 打断 | 需自建 | 内置 |
| 成本 | $0.17-0.30/通 | $0.57-1.07/通 |
| **最佳场景** | 工具调用复杂、成本敏感 | 情感交互、自然对话 |

### 2.2 架构目标

将全模态模式**纳入 AgentFlow 框架**，让 AgentFlow 能够承载有状态、流式、双向的语音 Node：

```
目标状态:
  ┌────────────────────────────────────────────────────────────────┐
  │  AgentFlow 框架                                                 │
  │                                                                │
  │  ┌──────────────────────┐  ┌────────────────────────────────┐  │
  │  │ 管线模式 (现有 Node)   │  │ 全模态模式 (新增 Node 类型)     │  │
  │  │                      │  │                                │  │
  │  │ 无状态 → 离散输入输出  │  │ 有状态 → 流式双向              │  │
  │  │ ASR→LLM→TTS 串行     │  │ Omni Node 长连接              │  │
  │  └──────────────────────┘  └────────────────────────────────┘  │
  │                                                                │
  │  复用: Session/Task/DataBus/Context/MCP 工具                   │
  └────────────────────────────────────────────────────────────────┘
```

---

## 3. 现状

### 3.1 AgentFlow 当前 Node 执行模型

```
Node 执行模型:
  ┌──────────┐     ┌──────────┐     ┌──────────┐
  │ 输入数据  │────→│ Node 执行 │────→│ 输出数据  │
  └──────────┘     └──────────┘     └──────────┘
       ↑                ↑                ↑
   离散的           无状态的           离散的
   (完整传入)       (每次独立)         (完整传出)
```

**核心特征**：
| 特征 | 说明 |
|------|------|
| **无状态** | 每次 Node 调用独立，执行完即销毁 |
| **离散输入** | 传入完整输入数据（如一段文本、一个文件路径） |
| **离散输出** | 输出完整结果数据（如回复文本、工具调用结果） |
| **同步调用** | 调用方等待 Node 执行完毕，返回结果 |
| **串行编排** | Workflow 按 edges 定义顺序执行，前一个完成才启动后一个 |

### 3.2 现有 Node 类型

| Node 类型 | 执行方式 | 状态 | 流式 |
|-----------|---------|------|------|
| C++ Node | 进程内执行 | 无状态 | 否 |
| Python Node (UDS) | 外部 Python 进程，通过 UDS 调用 | 无状态 | 否 |
| LLM 流式调用 | 流式返回文本 chunks | 无状态 | **部分支持**（文本流） |

### 3.3 现有 LLM 流式调用的实现方式

AgentFlow 当前支持 LLM 流式输出（SSE 方式），但这与全模态模式需要的流式有本质区别：

```
当前 LLM 流式:
  调用方 → LLM Node → 流式文本 chunks → 调用方逐段接收
  ↑ 单向流，输出方向流式，输入仍是离散的

全模态模式需要的流式:
  音频帧 → Omni Node → Qwen3-Omni WS → 音频帧 → 调用方
  ↑ 双向流，输入和输出同时都是流式的
  ↑ 且 Node 本身要保持长连接，不能每次调用重建
```

---

## 4. 问题分析

### 4.1 核心问题

全模态模式与 AgentFlow 当前 Node 模型存在**三个根本性冲突**：

| 冲突 | 当前 Node 模型 | 全模态模式的要求 | 冲突等级 |
|------|---------------|-----------------|---------|
| **P1: 有状态长连接** | 无状态，每次调用独立 | 跨多轮对话保持 WebSocket 连接（10-30分钟） | ❌ 致命 |
| **P2: 双向流式数据** | 离散输入→离散输出 | 音频帧持续流入、流式音频帧持续流出，同时进行 | ❌ 致命 |
| **P3: 事件驱动节奏** | 调用方同步等待执行结果 | Qwen3-Omni 的 server_vad 控制节奏，Node 主动推送 | ❌ 致命 |

### 4.2 问题详解

#### P1: 有状态长连接

```
管线模式 (无状态, 符合现有模型):
  第1轮: 用户说话 → ASR Node(独立) → LLM Node(独立) → TTS Node(独立) → 回传
  第2轮: 用户说话 → ASR Node(独立) → LLM Node(独立) → TTS Node(独立) → 回传
  ↑ 每轮都是全新调用, Node 间无状态关联

全模态模式 (有状态, 不符合现有模型):
  通话开始: Omni Node 初始化 → 建立 Qwen3-Omni WebSocket 连接
  第1轮: 音频帧 → Omni Node(同一连接) → 音频帧 → 回传
  第2轮: 音频帧 → Omni Node(同一连接) → 音频帧 → 回传
  通话结束: Omni Node 清理 → 关闭 WebSocket
  ↑ Node 实例跨多轮对话存活, 不能每次重建
```

#### P2: 双向流式数据

```
管线模式 (离散, 符合现有模型):
  [输入: 完整音频 PCM] → ASR Node → [输出: 完整文本]
  [输入: 完整文本] → LLM Node → [输出: 流式文本 chunks]
  [输入: 完整文本] → TTS Node → [输出: 完整音频 PCM]
  ↑ 每个 Node 的输入是离散的(完整数据), 输出是离散的(完整数据)

全模态模式 (双向流式, 不符合现有模型):
  时间→  →  →  →  →  →  →  →  →  →  →  →  →
  [帧1][帧2][帧3]...[帧N] → Omni Node → [音频帧1][音频帧2]...[音频帧M]
  ↑ 输入是持续流式, 输出也是持续流式, 同时进行
  ↑ Node 不能在"接收完所有输入"之后才开始处理
```

#### P3: 事件驱动节奏

```
管线模式 (同步调用, 符合现有模型):
  Workflow 引擎: 执行 ASR Node → 等待结果 → 执行 LLM Node → 等待结果 → 执行 TTS Node
  ↑ 调用方主动控制节奏, Node 被动执行

全模态模式 (事件驱动, 不符合现有模型):
  Qwen3-Omni server_vad 检测到用户说话 → Omni Node 收到音频
  Qwen3-Omni server_vad 检测到静音 → Omni Node 开始推理
  Qwen3-Omni 生成音频 → Omni Node 主动推送音频
  Qwen3-Omni function_call → Omni Node 调用 MCP 工具
  Qwen3-Omni 响应完成 → Omni Node 等待下一轮
  ↑ 节奏完全由 Qwen3-Omni 控制, Workflow 引擎无法编排
```

### 4.3 证据：Qwen3-Omni-Realtime 官方 API 调用方式

以下信息来自阿里云百炼官方文档（2026年7月已联网校验），用于支撑上述问题分析：

#### 4.3.1 基本连接信息

| 项目 | 值 | 来源 |
|------|------|------|
| 端点 | `wss://dashscope.aliyuncs.com/api-ws/v1/realtime?model=<model_name>` | [官方文档](https://help.aliyun.com/zh/model-studio/realtime) |
| 协议 | WebSocket（原生）/ WebRTC | 同上 |
| 认证 | Header: `Authorization: Bearer <API_KEY>` | 同上 |
| 模型 | `qwen3-omni-flash-realtime` | 同上 |
| **单会话最大时长** | **120 分钟** | [官方文档](https://docs.qwencloud.com/developer-guides/speech/realtime-multimodal-speech) |
| 音频输入格式 | PCM 16kHz 16bit mono | [官方文档](https://help.aliyun.com/zh/model-studio/omni-realtime-python-sdk) |
| 音频输出格式 | PCM 24kHz 16bit mono | 同上 |

#### 4.3.2 关键证据：单会话 120 分钟

> 官方原文："A single WebSocket session for Qwen-Omni-Realtime can last up to **120 minutes**. After this time, the service automatically closes the connection."

**对架构的影响**：这意味着 Node 必须保持一个跨多轮对话的 WebSocket 长连接，持续 10-30 分钟甚至更久。当前 AgentFlow 的 Node 无状态模型无法承载。

#### 4.3.3 关键证据：事件驱动回调模式

Qwen3-Omni-Realtime 的交互完全基于**回调（Callback）**，客户端通过实现 `OmniRealtimeCallback` 接口来处理服务端事件：

```python
class OmniRealtimeCallback:
    def on_open(self):
        """连接建立时回调"""
        pass

    def on_event(self, message: str):
        """服务端事件回调 — 包含所有交互事件"""
        # 事件类型包括:
        #   session.created          — 会话已创建
        #   session.updated          — 会话配置已更新
        #   input_audio_buffer.committed  — 音频已提交
        #   conversation.item.created     — 对话项已创建
        #   response.created             — 开始生成响应
        #   response.audio.delta         — 流式音频输出
        #   response.audio.done          — 音频生成完成
        #   response.audio_transcript.delta — 流式转录文本
        #   response.audio_transcript.done  — 转录完成
        #   response.function_call       — 工具调用请求
        #   response.done                — 响应完成
        #   speech.started              — 用户开始说话
        #   speech.stopped              — 用户停止说话
        pass

    def on_close(self, close_status_code, close_msg):
        """连接关闭时回调"""
        pass
```

**对架构的影响**：这证实了全模态模型是**事件驱动**的，服务端主动推送事件，客户端被动响应。无法用"调用方发起请求 → Node 执行 → 返回结果"的同步模型来封装。

#### 4.3.4 关键证据：VAD 模式下的交互流程

官方文档描述 VAD 模式的交互流程：

```
VAD 模式（默认）:
  1. 客户端建立 WebSocket 连接
  2. 客户端发送 session.update 配置 VAD 参数
  3. 客户端持续发送音频帧 (input_audio_buffer.append)
  4. 服务端自动检测语音起点和终点
  5. 服务端检测到一句话结束时自动触发模型响应
  6. 服务端通过回调推送 response.audio.delta (流式音频)
  7. 如果配置了工具，服务端通过 response.function_call 请求工具调用
  8. 客户端通过 conversation.item.create 返回工具结果
  9. 客户端发送 response.create 触发模型继续生成
  10. 回到步骤 3，等待下一轮用户说话
```

**对架构的影响**：整个流程是**持续的、双向的、异步的**，没有明确的"开始执行"和"执行完成"的边界。Workflow 引擎无法用"Node A → Node B → Node C"的串行模型来编排。

#### 4.3.5 关键证据：输入输出格式差异

| 方向 | 格式 | 说明 |
|------|------|------|
| 输入音频 | PCM 16kHz 16bit mono | 与前端麦克风采集一致 |
| 输出音频 | PCM 24kHz 16bit mono | **与输入采样率不同**，需处理重采样 |
| 辅助文本 | 可通过 `enable_input_audio_transcription` 开启 | 可选的文本转录，用于日志/质检 |

**对架构的影响**：输入输出音频格式不同（16kHz vs 24kHz），这意味着 Node 需要在接收端和发送端做不同的音频处理，进一步增加了封装复杂度。

#### 4.3.6 证据总结

| 证据 | 冲突的问题 | 严重程度 |
|------|-----------|---------|
| 单会话最大 120 分钟 | P1: 有状态长连接 | ❌ 致命 |
| 事件驱动回调模式 | P3: 事件驱动节奏 | ❌ 致命 |
| VAD 自动检测 + 持续推送 | P2: 双向流式数据 | ❌ 致命 |
| 输入输出格式不同 | P2: 需要额外处理 | ⚠️ 加重 |

**结论**：以上证据来自阿里云百炼官方文档，确认了全模态模式与 AgentFlow 当前 Node 模型存在三个根本性冲突，不是小修小补能解决的。

---

## 5. 解决方案分析

### 5.1 方案对比

| 方案 | 说明 | 改动量 | 可行性 |
|------|------|--------|--------|
| **A: 独立通道** | Bridge 直接管理 Qwen3-Omni 连接，AgentFlow 只做 MCP 工具执行 | 小 | ✅ 立即可行 |
| **B: 扩展 Node 模型** | 在 AgentFlow 中增加"有状态流式 Node"类型 | 大 | ⚠️ 需框架改造 |
| **C: 混合方案** | 管线模式走 Workflow，全模态模式作为独立 Workflow 类型 | 中 | ⚠️ 需部分改造 |

### 5.2 方案A: 独立通道（推荐当前采用）

```
Bridge 直接管理 Omni 连接:
  ┌───────────┐
  │  Bridge    │
  │            │
  │  ┌──────────────────────┐
  │  │ Omni Connection Mgr  │────→ Qwen3-Omni WebSocket
  │  │  (直接管理 WS 连接)   │←────
  │  └──────────────────────┘
  │            │
  │            │ 只调用 MCP 工具
  │            ▼
  │  ┌──────────────────────┐
  │  │ AgentFlow (工具执行器)│
  │  │  MCP Call → DataBus  │
  │  └──────────────────────┘
  └───────────┘
```

**优点**：不改 AgentFlow，立即可用
**缺点**：Bridge 变厚，打破了"薄接口层"定位

### 5.3 方案C: 混合方案（折中方案）

**核心思路**：不改变现有 Node 执行模型，而是将整个全模态对话视作一个**特殊的 Workflow 类型**，由 Workflow 引擎提供特殊的执行环境。

```
┌─────────────────────────────────────────────────────────────────────────┐
│  AgentFlow 框架 — 双轨 Workflow 引擎                                    │
│                                                                         │
│  ┌────────────────────────────────────────────┐                         │
│  │  Workflow 引擎 (现有)                        │                         │
│  │  执行: 管线模式 Workflow                     │                         │
│  │  Node 模型: 无状态 + 离散 I/O               │                         │
│  └────────────────────────────────────────────┘                         │
│                                                                         │
│  ┌────────────────────────────────────────────┐                         │
│  │  Stream Workflow 引擎 (新增)                │                         │
│  │  执行: 全模态模式 Workflow                  │                         │
│  │  Node 模型: 有状态 + 流式 I/O              │                         │
│  │                                            │                         │
│  │  ┌──────────────────────────────────────┐  │                         │
│  │  │  Omni Node (流式 Node)                │  │                         │
│  │  │  ┌────────────────────────────────┐  │  │                         │
│  │  │  │  Input Stream: 音频帧流入       │  │  │                         │
│  │  │  │  Output Stream: 音频帧流出      │  │  │                         │
│  │  │  │  Event Channel: function_call   │  │  │                         │
│  │  │  │  Lifecycle: init → stream → done│  │  │                         │
│  │  │  └────────────────────────────────┘  │  │                         │
│  │  └──────────────────────────────────────┘  │                         │
│  └────────────────────────────────────────────┘                         │
│                                                                         │
│  共享: Session/Task/DataBus/Context/MCP 工具                            │
└─────────────────────────────────────────────────────────────────────────┘
```

**关键设计**：

| 维度 | 标准 Workflow | Stream Workflow |
|------|--------------|----------------|
| **Node 执行** | 串行，一个完成再启动下一个 | 单 Node 长运行，持续处理流 |
| **数据流** | 离散输入→离散输出 | 流式输入→流式输出 |
| **生命周期** | 由 Workflow 引擎控制 | Node 自身管理（init/cleanup） |
| **通信方式** | 同步 UDS 调用 | 流式 UDS 通道 + 事件通道 |
| **Workflow 定义** | edges 定义执行顺序 | 单个 Node 定义，无 edges |

**Stream Workflow 定义示例**：

```json
{
  "workflow": {
    "id": "voice-omni-stream",
    "type": "stream",             // 标记为流式 Workflow
    "nodes": [
      {
        "id": "omni_core",
        "type": "python",
        "entry": "voice_nodes/omni_node.py",
        "stream": true,            // 标记为流式 Node
        "lifecycle": {
          "init_on": "session_start",   // 通话开始时初始化
          "cleanup_on": "session_end"   // 通话结束时清理
        },
        "config": {
          "model": "qwen3-omni-flash-realtime",
          "voice": "alloy"
        }
      }
    ]
  }
}
```

**Stream Workflow 执行流程**：

```
Bridge 触发 Workflow:
  workflow_engine.start("voice-omni-stream", session_id)
  → 引擎创建 Stream Workflow 实例
  → 调用 omni_node.init() → 建立 Qwen3-Omni WS 连接
  → 返回 Stream 通道给调用方

Bridge 通过 Stream 通道交互:
  channel.send(audio_frame)    → omni_node.on_input(audio_frame)
  channel.receive()            → omni_node 流式输出音频
  channel.on("function_call")  → omni_node 触发 MCP 调用

Bridge 结束 Workflow:
  workflow_engine.stop("voice-omni-stream", session_id)
  → 调用 omni_node.cleanup() → 关闭 WS 连接
  → 引擎释放资源
```

**需要的 AgentFlow 改造**：

| 新增组件 | 说明 | 工作量 |
|----------|------|--------|
| **Stream Workflow 引擎** | 新 Workflow 类型，支持单 Node 长运行 | 中 |
| **流式 UDS 通道** | UDS 协议扩展，支持持续流式读写 | 中 |
| **事件通道** | Node 主动推送事件（function_call 等） | 中 |
| **Worker 资源管理** | 长连接资源生命周期管理 | 中 |

**方案C 与方案B 的对比**：

| 维度 | 方案B: 扩展 Node 模型 | 方案C: 混合方案 |
|------|---------------------|----------------|
| **改造范围** | 改 Node 执行引擎，影响所有 Workflow | 新增 Stream Workflow 类型，不影响现有管线 |
| **侵入性** | 高 — 修改核心模型 | 中 — 新增并行执行路径 |
| **复用度** | 所有 Node 可受益 | 仅全模态模式受益 |
| **改造成本** | 大 | 中 |
| **风险** | 可能影响现有 Workflow 稳定性 | 隔离性好，不影响现有逻辑 |

**方案C 的优缺点**：

**优点**：
- 不影响现有管线模式 Workflow，隔离性好
- Bridge 保持薄（只做 WebSocket 代理，不管理 Omni 连接）
- 全模态模式纳入 AgentFlow 管理，享受 Session/DataBus/MCP 复用
- 改造成本小于方案B

**缺点**：
- 需要新增 Workflow 类型和流式通道，改动量中等
- 两套 Workflow 引擎并存，维护成本增加
- 流式 Node 的调试和观测比普通 Node 复杂

### 5.4 方案B: 扩展 Node 模型（长期目标）

如果未来要在 AgentFlow 框架内支持全模态模式，需要扩展 Node 执行模型：

#### 5.3.1 需要新增的能力

| 能力 | 说明 | 影响范围 |
|------|------|---------|
| **有状态 Node** | Node 实例跨多轮 Task 存活，有 init/cleanup 生命周期 | Node 执行引擎、Worker 调度 |
| **流式 I/O 通道** | Node 支持持续流入/流出数据，不等输入完成就输出 | Workflow 数据流、UDS 协议 |
| **事件驱动回调** | Node 主动推送事件（如音频到达、工具调用请求） | Workflow 编排、事件总线 |
| **长连接资源管理** | 超时、断连、重连、资源释放 | Worker 资源管理 |

#### 5.3.2 扩展后的 Node 接口

```python
# 扩展后的 Node 接口 (概念设计)
class OmniNode:
    """有状态流式 Node 的接口定义"""

    def on_init(self, ctx):
        """通话开始时调用 (代替 __init__)"""
        self.ws = connect_qwen_omni(ctx.config.api_key)

    def on_input(self, ctx, data: bytes):
        """每帧数据到达时调用 (代替 run)"""
        # data: 用户音频帧 (PCM 16kHz)
        self.ws.send_audio(data)

    def on_output(self):
        """主动获取输出流 (Generator)"""
        while True:
            frame = self.ws.receive_audio()
            yield frame

    def on_event(self, ctx, event: dict):
        """事件到达时调用 (如 function_call)"""
        if event["type"] == "function_call":
            result = ctx.call_mcp(event["name"], event["arguments"])
            self.ws.send_tool_result(result)

    def on_cleanup(self, ctx):
        """通话结束时调用"""
        self.ws.close()
```

#### 5.3.3 扩展后的 Workflow 执行流程

```
当前 Workflow 执行:
  executor.execute(node, input) → 等待 → 获取 output → 传给下一个

扩展后的 Workflow 执行:
  executor.start(node, initial_input)
  → 持续 node.on_input(ctx, data)  (流式送数据)
  → 持续 node.on_output() 接收产出 (流式取数据)
  → 处理 node.on_event() 事件  (事件驱动)
  → executor.stop(node)  (显式停止)
```

#### 5.3.4 涉及的 AgentFlow 组件改动

| 组件 | 改动项 | 工作量评估 |
|------|--------|-----------|
| **Node 执行引擎** | 支持有状态生命周期、流式 I/O 通道 | 大（核心改造） |
| **Worker 调度** | 长连接资源管理、超时回收 | 中 |
| **UDS 协议** | 扩展为支持流式双向通信 | 中 |
| **Workflow 编排** | 支持事件驱动节点 | 大 |
| **DataBus** | 流式数据通道 | 中 |
| **Session 管理** | 关联长连接资源生命周期 | 小 |

---

## 6. 决策

### 6.1 当前决策：暂不实现全模态模式

```
Phase 1 (当前): 管线模式 MVP
  └── AgentFlow 框架能力完全匹配
  └── 快速上线，验证语音助手价值

Phase 2 (后续): 评估全模态模式
  └── 根据业务需求决定是否需要
  └── 如果需要，选择方案A(独立通道) 或 方案B(扩展框架)
```

### 6.2 方案选择决策树

```
需要全模态模式?
  │
  ├─ 否 → 维持管线模式，不扩展框架
  │
  └─ 是 → 需要评估:
       │
       ├─ 是否可以接受 Bridge 变厚？
       │  ├─ 是 → 方案A: 独立通道 (立即可用)
       │  └─ 否 → 方案B: 扩展 Node 模型 (需框架改造)
       │
       └─ 是否有足够资源改造框架？
          ├─ 是 → 方案B: 扩展 Node 模型 (长期)
          └─ 否 → 方案A: 独立通道 (折中)
```

### 6.3 如果选择方案B，建议的改造策略

```
改造策略 (建议分步):
  第1步: 在现有 Python Node UDS 机制上，增加流式 I/O 通道
         (不改变 Node 执行模型，只扩展数据传输方式)
  第2步: 引入有状态 Node 生命周期
         (init/cleanup 回调，跨 Task 复用)
  第3步: 引入事件驱动回调
         (Node 主动推送事件，Workflow 引擎响应)

每一步都是可独立上线的小版本，不需要一次性完成全模态模式。
```

---

## 7. 总结

| 维度 | 内容 |
|------|------|
| **目标** | 让 AgentFlow 支持全模态语音模式（Qwen3-Omni 直入直出） |
| **现状** | AgentFlow Node 是无状态、离散输入输出、同步调用的模型 |
| **问题** | 三个根本冲突：有状态长连接、双向流式数据、事件驱动节奏 |
| **方案A** | 独立通道 — Bridge 直接管理 Omni 连接，不改框架（立即可用） |
| **方案B** | 扩展 Node 模型 — 新增有状态流式 Node 类型（需框架改造） |
| **当前决策** | 暂不实现，先聚焦管线模式 MVP |
| **后续** | 如需要，按方案B分步改造 AgentFlow 框架 |