# 管线模式：ASR → LLM → TTS

> **层级**: L3 · `docs/design/features/voice-assistant/3-管线模式(pipeline).md`
> **状态**: 初稿
> **创建日期**: 2026-07-13

---

## 1. 概述

管线模式（Pipeline Mode）是**传统语音对话架构**：将语音处理拆分为 ASR（语音转文本）、LLM（文本推理）、TTS（文本转语音）三个独立组件，串行构成一条处理管线。

**核心特征**：
- 语音/文本之间有明确的转换边界，每个组件可独立替换、独立优化
- **所有语音处理组件以 AgentFlow Python Node 形式运行**，Bridge 仅做透传
- 现有 AgentFlow 的 LLM、MCP 工具、记忆系统**完全复用**
- VAD 检测由前端/Bridge 完成（通过用户松开按钮或静音检测）

---

## 2. 架构

### 2.1 整体架构

```
┌─────────────────────────────────────────────────────────────────────────┐
│  AgentFlow Voice Workflow (pipeline 模式)                                │
│                                                                         │
│  ┌──────────────────────────────────────────────────────────────────┐   │
│  │  Bridge 层 (薄 — 仅透传+缓冲)                                      │   │
│  │                                                                   │   │
│  │  前端 WebSocket 音频流 → 缓冲完整一句话 → UDS 触发 Workflow        │   │
│  │  Workflow 返回音频/文本 → 透传回前端 WebSocket                     │   │
│  └──────────────────────────────────────────────────────────────────┘   │
│                              │ UDS                                       │
│                              ▼                                           │
│  ┌──────────────────────────────────────────────────────────────────┐   │
│  │  AgentFlow 执行器                                                  │   │
│  │                                                                   │   │
│  │  ┌──────────┐    ┌──────────┐    ┌──────────┐    ┌────────────┐  │   │
│  │  │ ASR Node  │───→│ LLM Node │───→│ TTS Node │───→│ 输出回传   │  │   │
│  │  │ (Python)  │    │ (复用现有)│    │ (Python)  │    │ (UDS)     │  │   │
│  │  │ 音频→文本  │    │ 文本→文本 │    │ 文本→音频 │    │            │  │   │
│  │  └──────────┘    └────┬─────┘    └──────────┘    └────────────┘  │   │
│  │                       │                                            │   │
│  │              ┌────────┴────────┐                                   │   │
│  │              │  MCP 工具 (复用)  │                                   │   │
│  │              │  DataBus (复用)  │                                   │   │
│  │              │  Context (复用)  │                                   │   │
│  │              └─────────────────┘                                   │   │
│  └──────────────────────────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────────────────────────┘
```

### 2.2 一次语音问答的执行流程

```
用户: 按住🎤说话 "今天上海穿什么？" (连续音频帧, 约2秒)
  │
  ▼
前端: getUserMedia 采集 → WebSocket 发送音频帧 (每20ms一帧)
  │
  ▼
Bridge: 缓冲音频帧到完整一句话 (用户松开按钮 = 一句话说完)
  │
  ▼
Bridge: 通过 UDS 调用 AgentFlow Voice Workflow
  │  传入: 完整音频 PCM + session_id + agent_id
  │
  ▼
AgentFlow: Voice Workflow 开始执行
  │
  ├── Step 1: ASR Node (Python UDS Node)
  │   输入: 完整音频 PCM (16kHz, 16bit, mono)
  │   处理: 调用 FunASR API
  │   输出: "今天上海穿什么？"
  │
  ├── Step 2: LLM Node (复用现有)
  │   输入: "今天上海穿什么？"
  │   处理: AgentFlow 现有 LLM 推理 + 天气工具调用
  │   输出: "上海今天28°C多云，建议穿白色短袖配浅蓝色牛仔裤..."
  │
  ├── Step 3: TTS Node (Python UDS Node)
  │   输入: "上海今天28°C多云，建议穿白色短袖配浅蓝色牛仔裤..."
  │   处理: 调用 CosyVoice API
  │   输出: 完整音频 PCM (16kHz, 16bit, mono)
  │
  └── 输出回传: 音频 + 文本 → UDS → Bridge → WebSocket → 前端
```

### 2.3 Workflow DSL 定义

```jsonc
// voice-pipeline.workflow.jsonc — 管线模式 Workflow DSL
// 格式遵循 AgentFlow Workflow DSL 规范 (publish/workflow/workflow_sample.jsonc)
{
  "version": "1.0",
  "id": "voice-pipeline",
  "name": "Voice Pipeline (ASR → LLM → TTS)",
  "description": "管线模式语音对话工作流: ASR 转文本 → LLM 推理 → TTS 合成语音",

  "nodes": [
    {
      "id": "asr_transcribe",
      "name": "ASR Transcription",
      "type": "transform",
      "execution_mode": "shell_script",
      "command": "bash voice_nodes/asr_node.sh",
      "inputs": [{"name": "audio_pcm", "type": "string", "required": true}],
      "outputs": [{"name": "text", "type": "string"}]
      "timeout_ms": 30000
    },
    {
      "id": "llm_reason",
      "name": "LLM Reasoning",
      "type": "transform",
      "execution_mode": "shell_script",
      "command": "bash voice_nodes/llm_reason.sh",
      "inputs": [{"name": "user_text", "type": "string", "required": true}],
      "outputs": [{"name": "reply_text", "type": "string"}],
      "timeout_ms": 60000
    },
    {
      "id": "tts_synthesize",
      "name": "TTS Synthesis",
      "type": "transform",
      "execution_mode": "shell_script",
      "command": "bash voice_nodes/tts_node.sh",
      "inputs": [{"name": "text", "type": "string", "required": true}],
      "outputs": [{"name": "audio_pcm", "type": "string"},
                  {"name": "text", "type": "string"}],
      "timeout_ms": 30000
    }
  ],

  "edges": [
    {
      "id": "e_asr_to_llm",
      "source": "asr_transcribe",
      "target": "llm_reason",
      "source_outputs": ["text"],
      "target_inputs": ["user_text"]
    },
    {
      "id": "e_llm_to_tts",
      "source": "llm_reason",
      "target": "tts_synthesize",
      "source_outputs": ["reply_text"],
      "target_inputs": ["text"]
    }
  ]
}
```

**调用方式**：Bridge 通过 UDS 调用 AgentFlow 的 Workflow 执行接口，传入 `audio_pcm` 作为初始输入，从 `tts_synthesize` 的输出中获取 `audio_pcm` 和 `text`。

---

## 3. 组件选型

### 3.1 ASR（自动语音识别）

**MVP 选型**：**FunASR-Realtime**（`funasr-realtime-v2`）— 阿里云百炼 DashScope API

| 维度 | 选型值 |
|------|--------|
| 模型 ID | `funasr-realtime-v2` |
| 接入方式 | HTTP API（`POST /api/v1/services/asr`） |
| 输入格式 | 音频 PCM 16kHz 16bit mono |
| 返回格式 | JSON `{"text": "..."}` |
| 中文质量 | 最佳（含 16 种方言） |
| 延迟 | 首字百毫秒级 |
| 成本 | ¥0.001/min |
| 配置方式 | `.env` → `AF_BR_ASR_API_KEY` |

**代码实现**：
```python
# voice_nodes/asr_node.py
import os, requests

def run(ctx):
    api_key = os.environ["AF_BR_ASR_API_KEY"]
    resp = requests.post(
        "https://dashscope.aliyuncs.com/api/v1/services/asr",
        headers={"Authorization": f"Bearer {api_key}"},
        files={"audio": ctx.input.audio_pcm},
        data={"model": "funasr-realtime-v2"}
    )
    ctx.output.text = resp.json()["text"]
    return ctx.output
```

### 3.2 LLM（大语言模型）

**MVP 选型**：**复用系统现有 LLM** — 即 AgentFlow 当前配置的 LLM 模型

| 维度 | 选型值 |
|------|--------|
| 模型 ID | `AF_LLM_MODEL` 环境变量指定（当前为 `aisf/deepseek-v4-flash`） |
| 接入方式 | 通过现有 LLM 调用链路（`AF_LLM_BASE_URL` + `AF_LLM_API_KEY`） |
| 推理方式 | DataBus 传递文本 → Coordinator + next_skills 编排 |
| 工具调用 | 复用现有 MCP 工具链 |
| 上下文 | 复用现有 Context 管理 |

**核心策略**：Voice Pipeline 不引入新的 LLM 模型，完全复用 AgentFlow 现有配置。ASR 输出的文本通过 DataBus 喂给现有 LLM 链路，LLM 回复文本通过 DataBus 传给 TTS。

**数据流**：
```
ASR Node → DataBus(sys/text) → Coordinator → next_skills → LLM Node
  → DataBus(sys/reply) → Coordinator → next_skills → TTS Node
```

**代码实现**：
```python
# voice_nodes/llm_reason.py
import os, json

def run(ctx):
    user_text = ctx.databus.get("sys", "text")
    messages = ctx.databus.get_history() + [{"role": "user", "content": user_text}]
    result = ctx.call_llm(messages, tools=ctx.mcp_tools)
    ctx.databus.put("sys", "reply", result["content"])
    ctx.output.text = result["content"]
    return ctx.output
```

### 3.3 TTS（文本转语音）

**MVP 选型**：**CosyVoice**（`cosyvoice-v1`）— 阿里云百炼 DashScope API

| 维度 | 选型值 |
|------|--------|
| 模型 ID | `cosyvoice-v1` |
| 接入方式 | HTTP API（`POST /api/v1/services/tts`） |
| 输入格式 | 文本（UTF-8） |
| 输出格式 | 音频 PCM 16kHz 16bit mono |
| 音色 | `longxiaochun`（默认女声，可配置） |
| 延迟 | 首帧 200-500ms |
| 成本 | ¥2/千字符 |
| 配置方式 | `.env` → `AF_BR_TTS_API_KEY` |

**代码实现**：
```python
# voice_nodes/tts_node.py
import os, requests

def run(ctx):
    api_key = os.environ["AF_BR_TTS_API_KEY"]
    resp = requests.post(
        "https://dashscope.aliyuncs.com/api/v1/services/tts",
        headers={"Authorization": f"Bearer {api_key}"},
        json={
            "model": "cosyvoice-v1",
            "input": {"text": ctx.input.text},
            "voice": "longxiaochun",
            "parameters": {"sample_rate": 16000}
        }
    )
    ctx.output.audio = resp.content
    ctx.output.text = ctx.input.text  # 返回文本供前端显示
    return ctx.output
```

### 3.4 多 Provider 抽象（ASR / TTS）

ASR 和 TTS 可能使用不同的服务商——阿里云百炼、MiniMax、自建模型等。Node 代码不应硬编码具体 API，而是通过 **Provider 抽象层**解耦。

#### 3.4.1 与 LLM Provider 的差异

LLM 有一个**事实上的 API 标准**——OpenAI Chat Completions 格式（`POST /v1/chat/completions`）。几乎所有 LLM 提供商（DeepSeek、Qwen、MiniMax 等）都兼容这个格式，更换只需改 `base_url` + `api_key`。

ASR 和 TTS 的标准化程度则不同：

| 维度 | LLM | ASR | TTS |
|------|-----|-----|-----|
| **事实标准** | ✅ OpenAI `/v1/chat/completions` | ⚠️ 正在形成（OpenAI `/v1/audio/transcriptions`） | ⚠️ 正在形成（OpenAI `/v1/audio/speech`） |
| **提供商兼容度** | 几乎所有厂商兼容 | FunASR/CrispASR 等自建方案兼容；MiniMax 等商业 API **不兼容** | OpenRouter/TTS.ai 兼容；MiniMax/自建 **不兼容** |
| **参数差异** | 基本一致（model/messages/temperature） | 差异大（音频格式、语言指定、VAD 参数） | 差异大（音色名、语速控制、情感参数、输出格式） |
| **标准化成熟度** | 成熟（多年演进） | 早期（2025-2026 年才开始对齐） | 早期（2025-2026 年才开始对齐） |

**结论**：ASR/TTS 的 Provider 抽象层是**有价值的**，但不像 LLM 那样可以做到"统一格式 + 仅换 base_url"。抽象层主要解决：

1. **核心接口统一**：`audio → text`（ASR）、`text → audio`（TTS）的统一调用
2. **Provider 特定参数透传**：音色、语速、情感等差异通过 `provider_params` 字典透传
3. **配置切换**：切换 Provider 时只改 `.env`，不改 Node 代码

但不能做到像 LLM 那样"一个 API 调用格式通吃所有提供商"。

#### 3.4.2 抽象接口

```python
# voice_nodes/provider_base.py — ASR/TTS Provider 抽象接口
from abc import ABC, abstractmethod

class ASRProvider(ABC):
    @abstractmethod
    def transcribe(self, audio_pcm: bytes) -> str:
        """音频 → 文本"""
        pass

class TTSProvider(ABC):
    @abstractmethod
    def synthesize(self, text: str, voice: str = "") -> bytes:
        """文本 → 音频 PCM"""
        pass
```

#### 3.4.3 各 Provider 实现

```python
# voice_nodes/providers/dashscope_asr.py — 阿里云百炼 FunASR
class DashScopeASR(ASRProvider):
    def __init__(self):
        self.api_key = os.environ["AF_BR_ASR_API_KEY"]
        self.url = "https://dashscope.aliyuncs.com/api/v1/services/asr"

    def transcribe(self, audio_pcm: bytes) -> str:
        resp = requests.post(
            self.url,
            headers={"Authorization": f"Bearer {self.api_key}"},
            files={"audio": audio_pcm},
            data={"model": "funasr-realtime-v2"}
        )
        return resp.json()["text"]

# voice_nodes/providers/dashscope_tts.py — 阿里云百炼 CosyVoice
class DashScopeTTS(TTSProvider):
    def __init__(self):
        self.api_key = os.environ["AF_BR_TTS_API_KEY"]
        self.url = "https://dashscope.aliyuncs.com/api/v1/services/tts"

    def synthesize(self, text: str, voice: str = "longxiaochun") -> bytes:
        resp = requests.post(
            self.url,
            headers={"Authorization": f"Bearer {self.api_key}"},
            json={
                "model": "cosyvoice-v1",
                "input": {"text": text},
                "voice": voice,
                "parameters": {"sample_rate": 16000}
            }
        )
        return resp.content

# voice_nodes/providers/minimax_asr.py — MiniMax ASR (示例)
class MiniMaxASR(ASRProvider):
    def __init__(self):
        self.api_key = os.environ["AF_BR_ASR_API_KEY"]
        self.url = "https://api.minimax.chat/v1/audio/asr"

    def transcribe(self, audio_pcm: bytes) -> str:
        # 调用 MiniMax ASR API
        ...

# voice_nodes/providers/self_hosted_asr.py — 自建 Whisper 服务 (示例)
class SelfHostedASR(ASRProvider):
    def __init__(self):
        self.url = os.environ.get("AF_BR_ASR_BASE_URL", "http://localhost:8080/transcribe")

    def transcribe(self, audio_pcm: bytes) -> str:
        resp = requests.post(self.url, files={"audio": audio_pcm})
        return resp.json()["text"]
```

#### 3.4.3 Provider 选择（通过环境变量配置）

```bash
# .env — 通过环境变量选择 ASR/TTS Provider
# ASR 提供商: dashscope | minimax | self-hosted
AF_BR_ASR_PROVIDER=dashscope

# TTS 提供商: dashscope | minimax | self-hosted
AF_BR_TTS_PROVIDER=dashscope

# 各 Provider 的 API Key (ASR 和 TTS 可共用或独立)
AF_BR_ASR_API_KEY=sk-xxx
AF_BR_TTS_API_KEY=sk-xxx

# 自建服务的地址 (self-hosted 模式时使用)
# AF_BR_ASR_BASE_URL=http://localhost:8080/v1/audio/transcriptions
# AF_BR_TTS_BASE_URL=http://localhost:8080/v1/audio/speech
```

#### 3.4.4 Provider 注册与使用

```python
# voice_nodes/provider_registry.py — Provider 注册中心
from providers.dashscope_asr import DashScopeASR
from providers.dashscope_tts import DashScopeTTS
# from providers.minimax_asr import MiniMaxASR
# from providers.self_hosted_asr import SelfHostedASR

ASR_PROVIDERS = {
    "dashscope": DashScopeASR,
    # "minimax": MiniMaxASR,
    # "self-hosted": SelfHostedASR,
}

TTS_PROVIDERS = {
    "dashscope": DashScopeTTS,
    # "minimax": MiniMaxTTS,
    # "self-hosted": SelfHostedTTS,
}

def get_asr_provider() -> ASRProvider:
    name = os.environ.get("AF_BR_ASR_PROVIDER", "dashscope")
    cls = ASR_PROVIDERS.get(name)
    if not cls:
        raise ValueError(f"Unknown ASR provider: {name}")
    return cls()

def get_tts_provider() -> TTSProvider:
    name = os.environ.get("AF_BR_TTS_PROVIDER", "dashscope")
    cls = TTS_PROVIDERS.get(name)
    if not cls:
        raise ValueError(f"Unknown TTS provider: {name}")
    return cls()
```

#### 3.4.5 改造后的 ASR Node / TTS Node

```python
# voice_nodes/asr_node.py — 改造后: 通过 Provider 抽象调用
from provider_registry import get_asr_provider

def run(ctx):
    if ctx.input.metadata.get("input_mode") != "voice":
        ctx.output.text = ctx.input.text  # 透传
        return ctx.output

    provider = get_asr_provider()
    ctx.output.text = provider.transcribe(ctx.input.audio_pcm)
    ctx.output.metadata = ctx.input.metadata
    return ctx.output

# voice_nodes/tts_node.py — 改造后: 通过 Provider 抽象调用
from provider_registry import get_tts_provider

def run(ctx):
    ctx.output.text = ctx.input.reply_text
    if not ctx.input.metadata.get("needs_tts"):
        return ctx.output

    provider = get_tts_provider()
    ctx.output.audio_pcm = provider.synthesize(ctx.input.reply_text)
    return ctx.output
```

#### 3.4.6 新增 Provider 的步骤

```
1. 新建 providers/xxx_asr.py, 实现 ASRProvider 接口
2. 在 provider_registry.py 中注册
3. 在 .env 中设置 AF_BR_ASR_PROVIDER=xxx
4. 在 .env 中配置对应 API Key
5. 在 requirements.txt 中添加对应 SDK 依赖
```

#### 3.4.7 架构总结

```
┌─────────────────────────────────────────────────────────────────────┐
│  ASR Node / TTS Node (条件执行)                                      │
│                                                                     │
│  run(ctx):                                                          │
│    provider = get_asr_provider()  ← 读环境变量 ASR_PROVIDER         │
│    result = provider.transcribe() ← 调用抽象接口                    │
│                                                                     │
│  Provider 层:                                                       │
│  ┌──────────┐  ┌──────────┐  ┌──────────┐  ┌────────────────────┐  │
│  │ DashScope │  │ MiniMax  │  │ 自建部署  │  │ 未来其他...        │  │
│  │ (FunASR)  │  │ (ASR)    │  │ (Whisper)│  │                    │  │
│  └──────────┘  └──────────┘  └──────────┘  └────────────────────┘  │
│                                                                     │
│ 环境变量:  AF_BR_ASR_PROVIDER=dashscope  (切换只需改 .env, 不改代码)      │
└─────────────────────────────────────────────────────────────────────┘
```

---

## 4. 关键工程问题

### 4.1 Turn-taking（轮次检测）

```
用户说话     │██████████████████████████████████│
            │  VAD 检测到说话                     │
            │                                    │
静音检测     │                       │████████│
            │                       │ 500ms   │
            │                       │ 静音阈值  │
            │                                    │
ASR 触发     │                       │          │
            │                       │ 开始识别   │
            │                                    │
LLM 推理     │                       │  ██████  │
            │                       │  推理中   │
            │                                    │
TTS 播放     │                       │    ████████
            │                       │    AI 说话
```

**关键参数**：
- 静音阈值：500ms（用户停止说话后等待 500ms 确认说完）
- 最小说话片段：250ms（过滤噪声短暂触发）
- 最大单次说话：30s（超过后强制结束轮次）

### 4.2 Barge-in（打断）

```
打断场景:
  AI 正在说话: "您的订单目前处于████████████████"
  用户打断:                       "等等，我要查另一个"
  检测到用户声音:
    1. 立即停止 TTS 播放
    2. 清空音频输出缓冲区
    3. 切换到 ASR 模式
    4. 播放简短提示音 (可选)
  处理用户新输入: "等等，我要查另一个订单"
```

**实现要点**：

Barge-in 由前端/Bridge 控制，不在 AgentFlow Node 内部实现：

```
前端/Bridge 检测逻辑:
  1. 前端正在播放 TTS 音频时, 检测到用户麦克风声音
  2. 前端立即停止 TTS 播放
  3. 前端通知 Bridge "用户正在说话, 取消当前回复"
  4. Bridge 重新采集用户音频, 发送新一轮 ASR → LLM → TTS
```

```python
# Bridge 层 Barge-in 逻辑 (概念示意)
class BargeInController:
    def __init__(self):
        self.is_playing = False
        self.current_stream = None

    def on_user_speech_detected(self):
        """前端检测到用户说话时调用"""
        if self.is_playing:
            # 1. 停止当前 TTS 播放
            self.stop_current_tts()
            # 2. 清空音频缓冲区
            self.clear_audio_buffer()
            # 3. 通知前端停止播放
            self.notify_frontend("cancel_playback")
            # 4. 开始采集用户新输入
            self.start_capturing()

    def stop_current_tts(self):
        if self.current_stream:
            self.current_stream.close()
            self.current_stream = None
        self.is_playing = False
```

### 4.3 Filler Audio（填充音）

```
时间线:
  ASR 完成 → LLM 开始推理 (可能需要 1-3s 查询数据库)
            │
            ├─ 立即播放: "让我查一下..." (500ms)
            │
            ├─ LLM 仍在推理, 播放: "稍等一下..." (500ms)
            │
            └─ LLM 完成 → 播放 LLM 回复
```

**策略**：
- 调用 LLM 时立即启动 Filler Audio
- 如果 LLM 在 2s 内返回，不播放第二次填充音
- 超过 2s 每 2s 播放一次"稍等一下"
- 超过 10s 自动兜底回复"抱歉，我需要更多时间，请稍后重试"

### 4.4 回声消除（AEC）

**必须配置**，否则 AI 会把自己的回声当成用户输入，导致死循环。AEC 在前端（浏览器 `getUserMedia` 的 `echoCancellation` 约束）和 Bridge 音频处理层两级配置。

```python
# 前端 getUserMedia 配置
constraints = {
    "audio": {
        "echoCancellation": True,
        "noiseSuppression": True,
        "sampleRate": 16000,
        "channelCount": 1
    }
}

# Bridge 音频处理层 AEC 配置 (如适用)
aec_config = {
    "enabled": True,
    "delay_ms": 50,          # 回声延迟估计
    "tail_length_ms": 200,   # 回声尾部长度
}
```

---

## 5. 成本估算

| 组件 | 模型 | 单次调用成本 | 一次完整对话 (约 10 轮交互) |
|------|------|-------------|------------------------------|
| VAD | Silero (本地) | $0 | $0 |
| ASR | FunASR-Realtime API | ¥0.001/min | ~$0.007 |
| LLM | Qwen (本地/API) | $0.01/轮 | $0.10 |
| TTS | CosyVoice API (MVP) | ¥2/千字符 | ~$0.03 |
| **语音处理小计** | | | **$0.10-0.17/通** |
| 电话接入 | Twilio / Asterisk | 按需另计 | 见电话接入方案文档 |

> 注：电话接入费用（Twilio ~$0.08/通、国内 SIP 中继 ~¥0.2/通）未计入上表，当前 MVP 阶段不涉及。

---

## 6. 与 Harness Agent 模板的集成

### 6.1 核心思路：ASR/TTS 作为条件 Node 注入现有 Workflow

ASR 和 TTS 是 AgentFlow 执行层的**条件 Node**，根据请求中的元数据决定是否激活。现有 dispatcher → coordinator → skill-runner 流程**完全不变**。

```
输入格式扩展:
  {                       ← 统一的请求格式，不再只是文本
    "text": "..."          ← 文本输入 (可选)
    "audio_pcm": "..."     ← 音频输入 (可选, base64 PCM 16kHz)
    "metadata": {
      "input_mode": "text|voice",   ← 输入模式
      "needs_tts": true|false       ← 是否需要对输出做 TTS
    }
  }

执行流程:
  Bridge → 统一的请求 → AgentFlow
    │
    ├── ASR Node (条件): 如果 input_mode=voice, 则 audio_pcm → text
    │                    如果 input_mode=text,  则透传, 不处理
    │
    ├── dispatcher (不变)
    ├── coordinator (不变)
    ├── skill-runner (不变)
    │
    └── TTS Node (条件): 如果 needs_tts=true, 则 reply_text → audio_pcm
                         如果 needs_tts=false, 则透传, 不处理
    │
    → 响应回 Bridge → 前端
```

### 6.2 输入格式扩展

```jsonc
// 统一的请求格式 (扩展后)
{
  "text": "今天上海穿什么？",         // 文本输入 (文本模式时)
  "audio_pcm": "<base64...>",        // 音频输入 (语音模式时, PCM 16kHz 16bit mono)
  "metadata": {
    "input_mode": "voice",           // "text" | "voice"
    "needs_tts": true,               // 是否需要 TTS 输出
    "session_id": "sess-xxx",
    "agent_id": "outfit-stylist"
  }
}
```

### 6.3 ASR Node（条件处理）

ASR Node 放在 dispatcher 之前，作为**输入预处理层**：

```python
# voice_nodes/asr_node.py — 条件 ASR Node
# 如果 input_mode=voice, 将 audio_pcm 转成 text
# 如果 input_mode=text,  透传, 不处理
import os, base64, requests

def run(ctx):
    input_mode = ctx.input.metadata.get("input_mode", "text")

    if input_mode == "voice":
        # 语音输入 → ASR 转文本
        audio_data = base64.b64decode(ctx.input.audio_pcm)
        api_key = os.environ["AF_BR_ASR_API_KEY"]
        resp = requests.post(
            "https://dashscope.aliyuncs.com/api/v1/services/asr",
            headers={"Authorization": f"Bearer {api_key}"},
            files={"audio": audio_data},
            data={"model": "funasr-realtime-v2"}
        )
        ctx.output.text = resp.json()["text"]
    else:
        # 文本输入 → 直接透传
        ctx.output.text = ctx.input.text

    # 透传 metadata
    ctx.output.metadata = ctx.input.metadata
    return ctx.output
```

### 6.4 TTS Node（条件处理）

TTS Node 放在 skill-runner 之后，作为**输出后处理层**：

```python
# voice_nodes/tts_node.py — 条件 TTS Node
# 如果 needs_tts=true, 将 reply_text 转成 audio_pcm
# 如果 needs_tts=false, 透传, 不处理
import os, base64, requests

def run(ctx):
    reply_text = ctx.input.reply_text
    needs_tts = ctx.input.metadata.get("needs_tts", False)

    ctx.output.text = reply_text  # 始终返回文本

    if needs_tts:
        # 需要语音输出 → TTS 合成
        api_key = os.environ["AF_BR_TTS_API_KEY"]
        resp = requests.post(
            "https://dashscope.aliyuncs.com/api/v1/services/tts",
            headers={"Authorization": f"Bearer {api_key}"},
            json={
                "model": "cosyvoice-v1",
                "input": {"text": reply_text},
                "voice": "longxiaochun",
                "parameters": {"sample_rate": 16000}
            }
        )
        ctx.output.audio_pcm = base64.b64encode(resp.content).decode()
    else:
        ctx.output.audio_pcm = ""

    return ctx.output
```

### 6.5 Workflow 改造

**dispatcher workflow (现有, 改造后)**：

实际模板中 dispatcher 是 `agent.json` 的 task 0（type: `dispatcher`），不作为独立 workflow JSON 存在。ASR Node 插入到现有的 `coordinator_workflow.json` 中，作为 coordinator 的前置预处理：

```jsonc
{
  "id": "coordinator-workflow",
  "version": "3.0",
  "nodes": [
    {
      "id": "asr_preprocess",           // ← 新增: ASR 条件处理
      "execution_mode": "shell_script",
      "command": "bash nodes/asr_node.sh",
      "inputs": [{"name": "input", "type": "string", "required": true}],
      "outputs": [{"name": "text", "type": "string"}]
    },
    {
      "id": "coordinator_decide",       // ← 现有, 不变
      "execution_mode": "shell_script",
      "command": "bash nodes/coordinator_decide.sh",
      "inputs": [{"name": "input", "type": "string", "required": true},
                  {"name": "session_id", "type": "string", "required": true},
                  {"name": "dialogue_id", "type": "string", "required": false}],
      "outputs": [{"name": "response", "type": "string"},
                  {"name": "next_skills", "type": "string"},
                  {"name": "coordinator_status", "type": "string"}]
    }
  ],
  "edges": [
    {"source": "asr_preprocess", "target": "coordinator_decide",
     "source_outputs": ["text"], "target_inputs": ["input"]}
  ]
}
```

**skill-runner workflow (改造后)**：

```jsonc
{
  "id": "skill-workflow",
  "version": "3.0",
  "nodes": [
    {
      "id": "skill_executor",           // ← 现有, 不变
      "execution_mode": "shell_script",
      "command": "bash nodes/skill_executor.sh",
      "inputs": [{"name": "input", "type": "string", "required": true}],
      "outputs": [{"name": "response", "type": "string"}]
    },
    {
      "id": "tts_postprocess",          // ← 新增: TTS 条件处理
      "execution_mode": "shell_script",
      "command": "bash nodes/tts_node.sh",
      "inputs": [{"name": "reply_text", "type": "string", "required": true}],
      "outputs": [{"name": "text", "type": "string"},
                  {"name": "audio_pcm", "type": "string"}]
    }
  ],
  "edges": [
    {"source": "skill_executor", "target": "tts_postprocess",
     "source_outputs": ["response"], "target_inputs": ["reply_text"]}
  ]
}
```

### 6.6 数据流总览

```
文本模式:
  Bridge → 请求 {text: "今天穿什么", metadata: {input_mode: "text", needs_tts: false}}
    → ASR Node(透传) → dispatcher → coordinator → skill-runner
    → TTS Node(透传, 只返回 text) → Bridge → 前端显示文字

语音模式:
  Bridge → 请求 {audio_pcm: "<base64>", metadata: {input_mode: "voice", needs_tts: true}}
    → ASR Node(转文本) → dispatcher → coordinator → skill-runner
    → TTS Node(合成语音, 返回 text+audio_pcm) → Bridge → 前端播放+显示

混合模式 (双栏):
  Bridge → 请求 {text: "今天穿什么", metadata: {input_mode: "text", needs_tts: true}}
    → ASR Node(透传) → dispatcher → coordinator → skill-runner
    → TTS Node(合成语音, 返回 text+audio_pcm) → Bridge → 前端播放+显示
```

### 6.7 对现有组件的影响

| 组件 | 改动 | 说明 |
|------|------|------|
| **dispatcher workflow** | ⚠️ 新增 ASR Node | 输入预处理层，条件转文本 |
| **skill-runner workflow** | ⚠️ 新增 TTS Node | 输出后处理层，条件转语音 |
| **coordinator workflow** | ❌ 不改 | 不知道语音存在 |
| **coordinator_decide.py** | ❌ 不改 | 收到的是文本，输出的是文本 |
| **skill_executor.py** | ❌ 不改 | 同上 |
| **agent.json** | ❌ 不改 | 任务定义不变 |
| **setup.sh** | ❌ 不改 | 自动检测 requirements.txt |
| **requirements.txt** | ⚠️ +1 行 | `dashscope>=1.23.9` |
| **.env** | ⚠️ +2 行 | `AF_BR_ASR_API_KEY` + `AF_BR_TTS_API_KEY` |
| **nodes/** | ⚠️ +2 对文件 | `asr_node.sh/.py` + `tts_node.sh/.py` |
| **Bridge 层** | ❌ 不改 | 仅透传格式化的请求，不做 ASR/TTS |

### 6.8 关键设计决策

| 决策 | 选择 | 理由 |
|------|------|------|
| **ASR/TTS 放在哪一层？** | AgentFlow 执行层 (Node) | 用户明确要求，与现有 Node 执行模型一致 |
| **条件判断逻辑在哪？** | Node 内部根据 metadata 决定 | 统一请求格式，不新增 Workflow 分支 |
| **Bridge 做什么？** | 格式化请求，透传音频数据 | 保持薄接口层定位 |
| **现有 Node 需不需要改？** | 不需要 | dispatcher/coordinator/skill-runner 不知道语音存在 |

| 场景 | 推荐度 | 原因 |
|------|--------|------|
| 订单通知/提醒 | ⭐⭐⭐⭐⭐ | 文本模板固定，无需复杂 LLM |
| 简单问答/FAQ | ⭐⭐⭐⭐⭐ | 知识库检索即可，LLM 负担小 |
| 问卷调查 | ⭐⭐⭐⭐⭐ | 流程固定，TTS 播放预设问题 |
| 复杂业务办理 | ⭐⭐⭐⭐ | 复用现有 LLM 工具链，MCP 集成方便 |
| 情感交互/销售 | ⭐⭐⭐ | 情感丢失，体验不如全模态 |
| 强合规/数据不出域 | ⭐⭐⭐⭐⭐ | 全组件可本地部署 |