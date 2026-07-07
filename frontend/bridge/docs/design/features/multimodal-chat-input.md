# 多模态输入设计 — Chat API Content Array 升级

> **文档层级**: L2 领域设计  
> **状态**: 初稿（待审阅）  
> **作者**: AgentFlow Bridge  
> **日期**: 2026-07-03  
> **关联**: `docs/design/features/agent-template-merge.md`、`docs/contracts/2.AISF-interface.md`  
> **传输策略**: 方案 C 混合 — 小图内联 base64，大图先上传再引用 file_url  
> **实施分期**: Phase A 内联传输（本期实现）→ Phase B 文件上传端点（future）

---

## 1. 问题陈述

当前 Chat API（`POST /api/chat/{agent_id}` 与 `WS /ws/chat/{agent_id}`）只接受字符串 `content`：

```json
{ "user_id": "u1", "client_id": "c1", "content": "你好" }
```

业务场景（穿搭顾问、文档问答、视觉检测等）需要上传图片让 agent 看。当前限制：

1. **只能纯文本** — 用户无法把"这张图里的衣服怎么搭"的图片传给 agent。
2. **业界不兼容** — OpenAI / Anthropic / Gemini 均已采用 `content array` 形式，我们与生态脱节。
3. **传输瓶颈** — bridge→daemon 的 WebSocket 帧 **硬限制 1MB**（`client.py:159 max_size=2**20`），一张手机照片 base64 后轻易超过 4MB，**不可能直接传**。

---

## 2. 传输策略 — 方案 C 混合

经方案 A（纯 base64）、方案 B（纯文件上传）、方案 C（混合）权衡，**采用方案 C**：

| 图片大小（base64 后） | 传输方式 | 路径 |
|----------------------|---------|------|
| < 512KB | **内联 base64** | content array 中直接 `data:image/...;base64,...` |
| ≥ 512KB | **文件上传** | 先 `POST /api/chat/uploads` 拿 `file_url`，再发 chat |

**理由**：

- 小图（截图、头像、emoji-like）内联可避免两步交互的复杂度，单次请求完成
- 大图走 multipart 流式上传，浏览器原生进度条，零内存膨胀
- 512KB 阈值确保 WS 1MB 帧限制下仍能容纳若干小图 + 文本（留 50% 安全余量）

**实施分期**：

| 阶段 | 内容 | 状态 |
|------|------|------|
| **Phase A**（本期） | 内联传输：content array schema、bridge 落盘替换、read_image 工具、node 解析 | 进行中 |
| **Phase B**（future） | 文件上传端点 `POST /api/chat/uploads`、前端大图走上传、上传进度 UI | 待启动 |

**本期边界**：Phase A 只实现内联传输。前端发 base64，bridge 对 ≥256KB 的图仍落盘替换为 file_url（保护 WS 帧），<256KB 的图直接透传。Phase B 上线后前端改为：≥512KB 走上传，<512KB 走内联，bridge 端落盘阈值同步调整。

---

## 2. 设计目标

| # | 目标 | 优先级 | 验收标准 |
|---|------|--------|---------|
| G1 | `content` 升级为业界通用的 content array | P0 | 支持 text/image_url 两种 type，符合 OpenAI Chat Completions 规范 |
| G2 | 向后兼容旧字符串形式 | P0 | 旧客户端发 `content: "你好"` 仍正常工作 |
| G3 | 支持图片 base64 内联（小图，<1MB） | P1 | 在 WS 帧限制内的图直接走内联 |
| G4 | 支持大图本地暂存 + file_url 引用 | P0 | 超过阈值（默认 256KB）的图自动落盘并替换为 file_url |
| G5 | bridge↔daemon 传递统一的 JSON content 结构 | P0 | daemon 接收的 content 永远是 JSON string，由节点解析 |
| G6 | 提供系统工具 `read_image`，按需 file_url→base64 | P0 | 业务 skill 通过工具调用按需取图，不强制 LLM 端预转换 |
| G7 | 旧字符串 content 自动包装为 `[{type:text}]` | P0 | 转换在 bridge 入口完成，daemon 只见 array |

---

## 3. 现状调研（Pre-Scan）

### 3.1 数据通路瓶颈

| 链路段 | 实现 | 限制 | 实测 |
|--------|------|------|------|
| HTTP 客户端 → bridge | FastAPI + uvicorn | `max_request_body_size=100MB`（已配置） | ✅ 足够 |
| bridge → daemon（JSON-RPC） | WebSocket 文本帧 | **`max_size=2**20` = 1MB**（`client.py:159`） | ❌ 不够 |
| daemon → workflow node | 子进程 env `AF_INPUT` | OS 限制 ~128KB（`ARG_MAX`） | ❌ 不够 |
| node → LLM | HTTPS（节点脚本内） | 无 bridge 限制 | ✅ 足够 |

**结论**：bridge→daemon 与 daemon→node 两段都装不下 base64 图片，**必须落盘 + 传路径**。

### 3.2 当前 content 流转

```
client → bridge:
    POST /api/chat/{agent_id} { content: "你好" }

bridge → daemon (agent.execute RPC):
    { "content": "你好", ... }   ← 纯字符串

daemon → workflow node (env):
    AF_INPUT="你好"              ← 纯字符串

node (coordinator_decide.py) → LLM:
    messages = [{role:user, content: "你好"}]   ← 纯字符串
```

### 3.3 现有图片处理参考

`outfit-assistant` 已有图片处理 skill（`outfit_vision.py`），但通过 `bash` 工具以 `python3 script.py --image /path/to/file.png` 调用 —— 即**业务 skill 自己处理 file path**。这与本设计目标 G6 一致：业务 skill 自己决定怎么用图。

---

## 4. 设计方案

### 4.1 总体架构

```
┌────────┐  HTTP/WS (≤100MB)   ┌───────┐  WS JSON-RPC (≤1MB)  ┌────────┐
│ Client │ ───────────────────▶│ Bridge│ ────────────────────▶│ Daemon │
└────────┘  content array      └───────┘  content JSON str    └────────┘
            (含 base64 或                  (只有 text parts,
             file_url)                     图片已展平为文本)
                                                  │
                                                  │ env AF_INPUT
                                                  ▼
                                           ┌─────────────┐
                                           │ Coordinator  │ ← 读 AF_INPUT，只看到 text
                                           │ (零改造)     │
                                           └─────────────┘
                                                  │
                                                  │ dispatch manifest
                                                  ▼
                                           ┌─────────────┐
                                           │ Skill       │
                                           │ Executor    │
                                           └─────────────┘
                                                  │
                                                  │ tool call: read_image(file_url) → base64
                                                  ▼
                                           ┌─────────────┐
                                           │ LLM Call    │ ← 按需传 base64 给多模态模型
                                           └─────────────┘
```

**关键决策**：

1. **bridge 入口**接收用户原始 content array（base64 内联或 file_url 都可）。
2. **bridge 展平并落盘**：`image_url` part 的 base64 解码保存到 `{agent_packages_dir}/{agent_id}/workspace/upload/{sha256}.{ext}`，替换为 `text` 类型 part 描述文件路径。
3. **bridge 出口**发给 daemon 的 content 永远是 JSON string，且**只有 `text` 类型的 parts**——不再包含 `image_url`。
4. **coordinator 零改造**：AF_INPUT 仍是 content array 但只有 text，coordinator 按原样拼接即可。
5. **skill 层**通过调用系统工具 `read_image` 把 `file://` 转成 base64，再拼到 LLM 调用里——业务自己决定"这张图要不要看、怎么用"。

### 4.2 客户端 → bridge：Content Array 格式

完全对齐 OpenAI Chat Completions：

```json
{
  "user_id": "u1",
  "client_id": "c1",
  "content": [
    { "type": "text", "text": "这张图里的衣服怎么搭？" },
    {
      "type": "image_url",
      "image_url": {
        "url": "data:image/png;base64,iVBORw0KGgo..."
      }
    }
  ]
}
```

**支持的 type**：

| type | 字段 | 来源 | 说明 |
|------|------|------|------|
| `text` | `text: str` | 用户 | 纯文本片段 |
| `image_url` | `image_url.url: str` | 用户 | 形式二选一：`data:image/...;base64,...` 或 `file://...` 绝对路径 |

**不支持**（KISS，YAGNI）：

- ❌ 外部 http(s) URL（用户场景无此需求；若需要，业务 skill 自己 `web_fetch`）
- ❌ `image_url.detail`（OpenAI 特有，我们 LLM 后端各异）
- ❌ `input_audio` / `video_url`（future）

### 4.3 向后兼容：字符串自动包装

bridge 入口在 Pydantic 校验层做联合类型：

```python
Content = Union[str, List[ContentPart]]

# 字符串自动包装为单元素 array
def _normalize_content(c: Content) -> list[ContentPart]:
    if isinstance(c, str):
        return [ContentPart(type="text", text=c)]
    return c
```

旧客户端 `content: "你好"` 在 bridge 入口立即变成 `[{type:text, text:"你好"}]`，下游统一处理。

### 4.4 bridge 落盘：base64 → file_url

bridge 在转发给 daemon 前，遍历 content array：

```
for each part:
  if part.type == "image_url" and url starts with "data:":
    decode base64 → 写到 /var/lib/agentflow/uploads/{sha256}.{ext}
    replace url with "file:///var/lib/agentflow/uploads/{sha256}.{ext}"
```

**阈值策略**：

| 图片大小 | 处理方式 |
|----------|---------|
| < 256KB（base64 后） | 允许直接透传（不落盘） |
| ≥ 256KB | 强制落盘 + 替换为 file_url |

阈值可配置（`content_image_inline_threshold_kb: 256`），默认 256KB 是为了保证 WS 1MB 帧限制下还能容纳若干张小图 + 文本。

**file_url 格式**：`file://` + 绝对路径。daemon 与 node 同机运行，可直接 `open(path)`。

### 4.5 bridge → daemon：统一 JSON string 协议

daemon 接收的 `content` 字段**永远是 JSON string**（序列化后的 content array）。这是为了让 daemon 不需要改造 JSON-RPC schema——它仍然看到 `params.content: str`。

```
bridge → daemon:
{
  "jsonrpc": "2.0",
  "method": "agent.execute",
  "params": {
    "agent_id": "...",
    "content": "[{\"type\":\"text\",\"text\":\"...\"},{\"type\":\"image_url\",\"image_url\":{\"url\":\"file:///var/lib/agentflow/uploads/abc.png\"}}]",
    ...
  }
}
```

**注意**：daemon 仍把 `content` 作为字符串塞进 `AF_INPUT` env。workflow node 拿到 `AF_INPUT` 后，**先尝试 JSON parse**：

```python
# node 端统一入口
def parse_af_input(raw: str) -> list[dict]:
    try:
        parts = json.loads(raw)
        if isinstance(parts, list):
            return parts
    except json.JSONDecodeError:
        pass
    # 旧字符串回退
    return [{"type": "text", "text": raw}]
```

### 4.6 daemon → node：env 传递

env `AF_INPUT` 的 OS 上限约 128KB。落盘后的 content JSON 通常 < 1KB（图片已变 file_url），**不会触顶**。

**不变量**：

- I-1: env 中永不含 base64 数据（图片永远是 file_url）
- I-2: env JSON 长度 < 32KB（daemon 启动时校验，超限直接拒收并报错）

### 4.7 系统工具 `read_image`：file_url → base64

新增模板内置工具 `harness/tools/read_image.py`：

```python
TOOL_NAME = "read_image"
TOOL_DESCRIPTION = (
    "Read an image from a file:// URL and return its base64 data URL. "
    "Use this when you need to send an image to a vision-capable LLM."
)
TOOL_PARAMS = {
    "type": "object",
    "properties": {
        "file_url": {
            "type": "string",
            "description": "file:// URL pointing to the image"
        },
        "max_size_kb": {
            "type": "integer",
            "description": "Max image size in KB; reject if larger (default 10240 = 10MB)",
            "default": 10240
        }
    },
    "required": ["file_url"]
}

def execute(tool_input: dict) -> str:
    file_url = tool_input["file_url"]
    max_kb = tool_input.get("max_size_kb", 10240)
    
    # 校验 file:// scheme
    if not file_url.startswith("file://"):
        return json.dumps({"error": "Only file:// URLs are supported"})
    
    path = Path(file_url[7:])  # strip "file://"
    # 路径白名单校验（防穿越）
    if not _is_within_allowed_root(path):
        return json.dumps({"error": "Path outside allowed roots"})
    
    if not path.exists():
        return json.dumps({"error": f"File not found: {path}"})
    
    size_kb = path.stat().st_size // 1024
    if size_kb > max_kb:
        return json.dumps({"error": f"Image too large: {size_kb}KB > {max_kb}KB"})
    
    # 探测 mime
    mime = _guess_mime(path)
    b64 = base64.b64encode(path.read_bytes()).decode("ascii")
    data_url = f"data:{mime};base64,{b64}"
    
    return json.dumps({
        "data_url": data_url,
        "mime": mime,
        "size_bytes": path.stat().st_size,
    })
```

**业务 skill 调用示例**：

```python
# skill 想让 LLM 看图
result = tools.execute({"name": "read_image", "input": {"file_url": img_url}})
data = json.loads(result)
if "data_url" in data:
    # 业务自己拼 OpenAI vision 格式发给 LLM
    messages = [{
        "role": "user",
        "content": [
            {"type": "text", "text": "识别这张图里的衣物"},
            {"type": "image_url", "image_url": {"url": data["data_url"]}},
        ]
    }]
    call_llm(messages, ...)
```

**为什么不自动注入 LLM 调用？**

业务 skill 才知道：

- 这张图要不要给 LLM 看？（可能只是元数据归档）
- 给哪个 LLM 看？（多模型场景）
- 用什么 prompt 配图？

强制 bridge 端转换会让 skill 失去控制力。**提供工具，不替业务做决策**——这是 S3 原则（核心逻辑不下沉 Adapter 层）的应用。

### 4.8 路径安全：白名单 + 校验

`read_image` 必须防穿越：

```python
ALLOWED_ROOTS = [
    Path("/var/lib/agentflow/uploads"),
    Path(settings.agent_packages_path) / ".uploads",
    Path("/tmp/agentflow/uploads"),
]

def _is_within_allowed_root(path: Path) -> bool:
    resolved = path.resolve()
    return any(resolved.is_relative_to(root.resolve()) for root in ALLOWED_ROOTS)
```

bridge 落盘目录默认在 `ALLOWED_ROOTS[0]`，可配置。

### 4.9 图片存储架构

#### 4.9.1 存储目录

bridge 统一将图片落盘到每个 agent 的 workspace 目录：

```
{agent_packages_dir}/{agent_id}/workspace/upload/
├── a1b2c3d4e5f67890.png    ← 内容寻址，sha256[:16]
└── fedcba0987654321.jpg
```

例如 `examples/agent/outfit-assistant/workspace/upload/abc123.png`。

**关键约束**：

| 约束 | 说明 |
|------|------|
| **同机可读** | daemon worker 与 bridge 在同一主机，node 脚本可直接 `open(file:///tmp/agentflow/.../xxx.png)` |
| **跨 agent 共享** | 文件不归属特定 agent，任何 agent 都可 `read_image`（只读操作，安全） |
| **内容寻址** | `sha256[:16].{ext}` 天然去重，同一图片多次上传只落盘一次 |
| **TTL 清理** | bridge 启动时 + 每 6 小时执行 `find ${uploads_dir} -mmin +$((24*60)) -delete` |
| **只写一次** | bridge 写入前检查 `if not exists`，不可变文件 |

#### 4.9.2 路径白名单

`read_image` 工具在 node 端做路径穿越防护，**仅允许读取 `uploads_dir` 内的文件**（I-3，见 §4.7）。若部署时修改 `uploads_dir`，必须同步更新 `read_image.py` 的白名单。

#### 4.9.3 Daemon worker 读取

daemon worker 通过环境变量或硬编码白名单获得可读路径集合。当前假设同机部署，worker 子进程与 bridge 共享文件系统。

### 4.10 数据完整流（bridge → daemon → coordinator → skill → LLM）

本节描述 content array 在 agentflow daemon 中的完整穿透路径。

#### 4.10.1 整体数据流

```
bridge (收到 content array)
  │  text parts → 透传
  │  image_url parts (data: URL) → 解码 → 落盘到 workspace/upload/
  │                              → 替换为 text part 描述图片路径
  │
  ▼ agent.execute(params.content = JSON string, 只有 text parts)
daemon
  │ AF_INPUT = JSON string
  ▼
coordinator node (coordinator_decide.py, 零改造)
  ├── 读 AF_INPUT → 纯字符串或 content array
  ├── 拼接 LLM messages（原样，图已是文字描述）
  ├── 调 LLM
  │     LLM 看到："用户上传了图片，路径: /.../workspace/upload/abc.png"
  │     如需看图，LLM 调用 run_skills → skill 调用 read_image
  ├── LLM 决定 dispatch skill → 写入 dispatch manifest 到 DataBus
  │
  ▼
skill runner node (skill_executor.py)
  ├── 从 DataBus 读 dispatch manifest
  ├── skill prompt 中包含图片路径描述
  └── skill 可按需调用 read_image(file_url) → 获取 base64 后传给 LLM
```

#### 4.10.2 转换点：bridge 展平 image_url → text

bridge `content_serializer.py` 的 `flatten_to_text` 函数处理每个 `image_url` part：

```python
def flatten_to_text(parts, upload_dir):
    result = []
    for part in parts:
        if part["type"] != "image_url":
            result.append(part)
            continue
        url = part["image_url"]["url"]
        file_path = _land_image(url, upload_dir) if url.startswith("data:") else url
        if not file_path:
            continue
        result.append({
            "type": "text",
            "text": f"用户上传了图片，路径: {file_path} 需要查看图片内容时请使用 read_image 工具",
        })
    return result
```

最终发给 daemon 的 content JSON 示例（只有 text parts）：

```json
[
  {"type": "text", "text": "识别这张图里的衣服"},
  {"type": "text", "text": "用户上传了图片，路径: /path/to/workspace/upload/abc.png 需要查看图片内容时请使用 read_image 工具"}
]
```

#### 4.10.3 coordinator 零改造

AF_INPUT 只含 text parts，现有 coordinator 代码不需要任何改动：

```python
user_input = get_env("AF_INPUT", "")
messages.append({"role": "user", "content": user_input})
```

LLM 收到的是包含图片路径描述的文字，它可以：
- 忽略图片（纯文本对话）
- 在 dispatch skill 时把路径放在 prompt 中
- skill 通过 `read_image` 工具按需读取

#### 4.10.4 skill 层处理图片

skill 收到包含图片路径的任务后：

```python
# skill 内调用 read_image
result = call_tool("read_image", {"file_url": "file:///path/to/workspace/upload/abc.png"})
data = json.loads(result)
if "data_url" in data:
    # 传给多模态 LLM
    messages = [{"role": "user", "content": [
        {"type": "text", "text": "识别这张图"},
        {"type": "image_url", "image_url": {"url": data["data_url"]}},
    ]}]
    call_llm(messages, model="gpt-4o", ...)
```

#### 4.10.5 对话历史中的图片

AF_INPUT 只包含**用户最新一条消息**（展平后的 text parts）。历史消息中已无图片引用，图片在文件 TTL 内存在于 workspace/upload/，超出后自动清理 —— 图片只在当前对话窗口有效。

### 4.11 agent.md 不需要声明多模态能力

多模态图片由 bridge 在入口处自动展平为路径文本，对 agent 透明。任何 agent 都能接收带图片路径的文本消息。

---

## 5. 数据模型

### 5.1 Pydantic v2 Schema（bridge 入口）

```python
from typing import Literal, Union, List
from pydantic import BaseModel, Field

class TextPart(BaseModel):
    type: Literal["text"]
    text: str

class ImageURL(BaseModel):
    url: str  # data:... 或 file://...
    # 不支持 detail 字段（OpenAI 特有）

class ImagePart(BaseModel):
    type: Literal["image_url"]
    image_url: ImageURL

ContentPart = Union[TextPart, ImagePart]
Content = Union[str, List[ContentPart]]

class ChatRequest(BaseModel):
    user_id: str
    client_id: str
    content: Content
```

### 5.2 序列化协议（bridge → daemon）

```python
def _serialize_content_for_daemon(parts: list[ContentPart]) -> str:
    """序列化 content array 为 JSON string，所有 base64 已替换为 file_url。"""
    return json.dumps(
        [p.model_dump() for p in parts],
        ensure_ascii=False,
        separators=(",", ":"),
    )
```

### 5.3 daemon 端 content 解析（node 通用入口）

每个 node 在读 `AF_INPUT` 时调用统一 helper：

```python
def parse_af_input(raw: str) -> list[dict]:
    """Parse AF_INPUT env. Returns content array.
    
    Backward compatible: raw string → [{type:text, text:raw}]
    """
    try:
        parts = json.loads(raw)
        if isinstance(parts, list) and all(isinstance(p, dict) for p in parts):
            return parts
    except json.JSONDecodeError:
        pass
    return [{"type": "text", "text": raw}]
```

---

## 6. 落盘目录与生命周期

### 6.1 目录结构

```
/var/lib/agentflow/uploads/           ← bridge 写入，daemon / node 读取
  ├── 2026/07/03/                     ← 按日期分桶
  │   ├── {sha256}.png
  │   └── {sha256}.jpg
  └── 2026/07/04/
      └── ...
```

### 6.2 文件命名

`{sha256_of_decoded_bytes}.{ext}`

- sha256 内容寻址，天然去重
- ext 从 `data:image/png` 的 mime 推断

### 6.3 清理策略

- **TTL**: 默认 24 小时（可配置 `uploads_ttl_hours: 24`）
- **清理任务**: bridge 启动时 + 每 6 小时执行一次 `find /var/lib/agentflow/uploads -mmin +$((24*60)) -delete`
- **不入 git**: 路径在 `.gitignore`

### 6.4 配置项

```yaml
# config.yaml 新增
uploads:
  dir: "/var/lib/agentflow/uploads"
  inline_threshold_kb: 256       # < 此值的 base64 内联透传，≥ 此值落盘
  ttl_hours: 24                  # 落盘文件保留时长
  max_image_size_mb: 10          # 单图上限（防恶意上传）
```

---

## 7. API 变更

### 7.1 REST `POST /api/chat/{agent_id}`

**请求 body（新格式）**：

```json
{
  "user_id": "u1",
  "client_id": "c1",
  "content": [
    { "type": "text", "text": "这张图里的衣服怎么搭？" },
    {
      "type": "image_url",
      "image_url": { "url": "data:image/png;base64,iVBORw0KGgo..." }
    }
  ]
}
```

**请求 body（旧格式，仍支持）**：

```json
{
  "user_id": "u1",
  "client_id": "c1",
  "content": "你好"
}
```

**响应 body（不变）**：

```json
{
  "success": true,
  "data": { "task_id": "...", "content": "..." },
  "error": null
}
```

### 7.2 WebSocket `/ws/chat/{agent_id}`

同一 schema，仅入站消息体升级为 content array。

### 7.3 错误码新增

| HTTP/WS | 条件 |
|---------|------|
| 400 | content array 为空 |
| 400 | content part 缺少必填字段 |
| 400 | image_url.url 既不是 `data:` 也不是 `file://` |
| 413 | 单张图片解码后超过 `max_image_size_mb` |

---

## 8. 不变量

| ID | 描述 | 守护点 |
|----|------|--------|
| I-1 | bridge 发给 daemon 的 content 中**永不含 base64 数据**，图片永远是 file_url | bridge 落盘 + 替换步骤 + 单元测试 |
| I-2 | AF_INPUT env JSON 长度 < 32KB | bridge 序列化后校验，daemon 启动时校验 |
| I-3 | read_image 只能读 ALLOWED_ROOTS 内的文件 | 路径白名单 + 单元测试 |
| I-4 | content array 至少包含一个 `text` 或 `image_url` part | Pydantic 校验 |
| I-5 | 同一 sha256 的图片只落盘一次 | bridge 写入前 `if not exists` 检查 |
| I-6 | 旧字符串 content 与新 array content 在 daemon 端等价（都解析成 list[dict]） | `parse_af_input` 双路径 + 测试 |

---

## 9. 实施分阶段

| 阶段 | 内容 | 验收 |
|------|------|------|
| **Phase 1** | Pydantic schema + 字符串自动包装 + bridge 内部统一为 array | 旧字符串请求 100% 兼容，单测覆盖 |
| **Phase 2** | bridge 落盘逻辑（base64→file_url）+ 配置项 + 清理任务 | 大图落盘，daemon 收到的 content 无 base64 |
| **Phase 3** | `read_image` 系统工具 + 加入模板 `_TEMPLATE_ONLY` | skill 可调用，路径穿越被拒 |
| **Phase 4** | node 端 `parse_af_input` helper + 模板节点接入 | coordinator / skill_executor 解析新格式 |
| **Phase 5** | e2e 测试：发图 → 落盘 → daemon → node → read_image → LLM | 全链路通过 |

---

## 10. ADR

| 编号 | 决策 | 理由 |
|------|------|------|
| ADR-001 | content array 对齐 OpenAI 格式而非自定义 | 生态兼容、前端 SDK 可复用、文档完善 |
| ADR-002 | bridge 落盘而非扩大 WS 帧限制 | 扩大到 100MB 帧会撑爆 daemon 内存；落盘是流式系统的标准做法 |
| ADR-003 | daemon content 永远是 JSON string 而非原生 array | 不改 JSON-RPC schema，daemon 零改造；node 端 `parse_af_input` 统一入口 |
| ADR-004 | 提供 `read_image` 工具而非 bridge 自动注入 LLM 调用 | 业务 skill 才知道图怎么用；自动注入违反 S3（核心逻辑不下沉 Adapter） |
| ADR-005 | 256KB 阈值内联 vs 落盘 | 平衡小图性能（emoji、头像）与大图稳定性 |
| ADR-006 | file:// 协议而非自定义 scheme | POSIX 标准，工具链友好；http(s) 不支持以避免 bridge 变代理 |

---

## 11. 风险与未决问题

| 风险 | 影响 | 缓解 |
|------|------|------|
| 多节点并行访问同一 file_url | 低（只读） | 文件不可变，sha256 寻址 |
| 落盘目录磁盘爆满 | 中 | TTL 清理 + 磁盘水位告警（future） |
| file_url 跨机部署失效 | 高 | 当前假设同机部署；分布式部署需改为对象存储（future ADR） |
| base64 解码 CPU 开销 | 低 | 在 bridge 异步线程池执行 |
| 旧 node 不识别新 JSON content | 中 | `parse_af_input` 双路径回退到字符串 |

**未决**：

- 是否支持 `http(s)://` 外部 URL？→ 暂不支持，业务 skill 自己 `web_fetch`
- 是否支持音频/视频？→ future，schema 已预留 `type` 字段扩展

---

## 12. 参考

- OpenAI Chat Completions API: https://platform.openai.com/docs/api-reference/chat
- Anthropic Messages API: https://docs.anthropic.com/en/api/messages
- 现有图片处理 skill: `examples/agent/outfit-assistant/harness/skills/outfit-stylist/scripts/outfit_vision.py`
- bridge WS 限制: `frontend/bridge/client.py:159`
- daemon worker env: `src/runtime/worker/worker.cpp`
