# Artifact 输出管道设计

> **文档层级**: L2 领域设计
> **状态**: 初稿
> **日期**: 2026-07-05
> **关联**: `multimodal-chat-input.md`（输入方向）、`bridge-session-persistence.md`（会话持久化）、`streaming-response.md`（SSE 格式）、`config-system.md`（配置）
> **代码入口**: `routes/artifact_extractor.py`（提取逻辑）、`routes/artifacts.py`（HTTP serve）、`routes/chat_rest.py`（SSE/同步传递）、`../../web/src/components/ArtifactList.vue`（前端渲染）

## 1. 概述

### 1.1 要解决的问题

Agent（如 outfit-assistant）在执行技能后生成各种类型的产物：图片（穿搭效果图）、HTML 报告、可下载文件（PPT、PDF 等）。这些产物需要从 agent 子进程一路传递到前端用户可见的交互界面。

### 1.2 业务场景

| 场景 | 产物类型 | 示例 |
|------|---------|------|
| 穿搭顾问生成效果图 | `image` | `![生成图片 1](/api/artifacts/outfit-assistant/files/.../look.png)` |
| 报告生成 | `html` | `[查看报告 1](/api/artifacts/.../report.html)` |
| 文档生成 | `download` | `[📥 下载文件 1](/api/artifacts/.../deck.pptx)` |

### 1.3 设计目标

| ID | 目标 | 优先级 |
|----|------|--------|
| G1 | agent 生成的图片/HTML/文件能通过统一 URL 路径被前端访问 | P0 |
| G2 | 前端能获取结构化 artifact 元数据（类型、URL、文件名、MIME），无需解析 Markdown | P0 |
| G3 | artifact 路径受目录白名单保护，防止路径穿越攻击 | P0 |
| G4 | 历史对话回放时，结构化 artifact 元数据应能从 Bridge 持久化层恢复 | P1 |
| G5 | 产物可配置公网 CDN 或相对路径（通过 `artifact_public_base_url`） | P1 |
| G6 | 产物收集守"本次新生成"语义边界，不收 skill 消费过的用户上传输入（`workspace/upload/` 是输入目录，见 `content_serializer._compute_upload_dir`） | P0 |

## 2. 数据流全景

```
┌─────────────────────────────────────────────────────────────────────────┐
│ Agent 子进程 (coordinator_decide.py / skill_executor.py)                │
│  append_artifacts_markdown(text, artifacts_json)                        │
│    → 输出 stdout JSON: {"response": "... ![图片](url) ..."}             │
└────────────────────┬────────────────────────────────────────────────────┘
                     │ 子进程 stdout → CodeLoader → DataBus _e2e_response
                     ▼
┌─────────────────────────────────────────────────────────────────────────┐
│ daemon RESPONSE_POLLER                                                  │
│   读取 DataBus _e2e_response key → 发送到 Bridge 的 agent.response 通知   │
└────────────────────┬────────────────────────────────────────────────────┘
                     │ WebSocket JSON-RPC notification
                     ▼
┌─────────────────────────────────────────────────────────────────────────┐
│ Bridge _chat_turn_events (routes/chat_rest.py)                          │
│   1. _extract_content(notification) → 提取文本内容                        │
│   2. publicize_artifact_urls(content) → 替换相对 URL 为公网 URL          │
│   3. extract_artifact_infos(content) → 提取结构化 ArtifactInfo[]        │
│      (仅在 is_final 时执行)                                              │
│   4. yield event { content, artifacts, final, ... }                     │
└─────────┬───────────────┬─────────────────────────────┬─────────────────┘
          │ SSE 流         │ 同步模式                     │ 持久化
          ▼                ▼                             ▼
┌─────────────────┐ ┌─────────────────┐ ┌──────────────────────────────┐
│ _sse_stream     │ │ 累积 content    │ │ serialize_artifacts() → JSON  │
│ payload +=      │ │ + artifacts     │ │ → save_dialogue(artifacts=)   │
│ "_artifacts"    │ │ → ChatRestResp. │ │ → SQLite dialogue_rounds      │
│  → data: {...}  │ │   .artifacts    │ │    .artifacts TEXT            │
└────────┬────────┘ └────────┬────────┘ └──────────────┬───────────────┘
         │ SSE               │ HTTP JSON               │ 历史回放
         ▼                   ▼                         ▼
┌─────────────────────────────────────────────────────────────────────────┐
│ 前端 (useChatSse.ts)                                                     │
│   parseSseStream: 解析 data: { _artifacts, choices[0].delta.content }   │
│   executeSseRequest: appendChunk(msgId, content, artifacts)             │
└────────────────────┬────────────────────────────────────────────────────┘
                     │
                     ▼
┌─────────────────────────────────────────────────────────────────────────┐
│ chatStore.appendChunk(messageId, content, artifacts?)                    │
│   → target.content += chunk                                             │
│   → target.artifacts += artifacts (dedup by url)                        │
└────────────────────┬────────────────────────────────────────────────────┘
                     │
                     ▼
┌─────────────────────────────────────────────────────────────────────────┐
│ ChatView.vue                                                            │
│   MarkdownContent(msg.content)    ← Markdown 渲染图片链接 ![](url)       │
│   ArtifactList(msg.artifacts)     ← 结构化缩略图 / 文件列表              │
└─────────────────────────────────────────────────────────────────────────┘
```

## 3. 分层设计

### 3.1 Agent 层 — Markdown 标记生成

Agent 子进程（coordinator 或 skill）在最终响应文本中嵌入 artifact 标记。

**入口函数**: `coordinator_decide.py:append_artifacts_markdown()`

**产物收集边界（I-6）**: 产物清单由 `skill_executor.extract_artifact_paths` 从 skill 执行过程的文本（task_instruction / tool_result）里扫路径得出。**输入路径不得混入**：API 传入图片经 `content_serializer._compute_upload_dir` 落到 `{agent_packages}/{agent_id}/workspace/upload/`，并以文本形式注入 user message 透传到 skill prompt — 因此 `workspace/upload/`（兼容旧 `uploads/`）是输入目录，扫到时必须跳过。详见 §5 I-6。

**标记格式**:

| 类型 | 标记模式 | 常量 |
|------|---------|------|
| image | `![生成图片 {index}]({url})` | `ARTIFACT_IMAGE_TEMPLATE` |
| html | `[查看报告 {index}]({url})` | `ARTIFACT_HTML_TEMPLATE` |
| download | `[📥 下载文件 {index}]({url})` | `ARTIFACT_DOWNLOAD_TEMPLATE` |

**URL 生成**: `local_artifact_url(path)` — 将本地绝对路径转换为 `/api/artifacts/{agent_id}/files/{relative_path}` 格式。

**输出协议**: agent 子进程通过 stdout 输出 `{"response": "...", ...}` JSON，由 daemon 的 `CodeLoader` 捕获后写入 DataBus 的 `_e2e_response` key。

### 3.2 Daemon 层 — 响应通知

Daemon 的 `RESPONSE_POLLER` 监控 DataBus 中 `_e2e_response` key 的变化。当 agent 写入最终响应后，RESPONSE_POLLER 将其推送为 `agent.response` 通知，通过 WebSocket JSON-RPC 发送给 Bridge。

通知结构（字段含义）:

```
{
  "type": "text",          // 通知类型: text | llm_chunk | progress
  "content": "...",        // 响应文本（含 Markdown artifact 标记）
  "final": true,           // 是否为最终响应
  "metadata": {            // 可选元数据
    "skill": "...",
    "round": "...",
    "status": "..."
  }
}
```

### 3.3 Bridge 层

#### 3.3.1 Artifact 提取 — `routes/artifact_extractor.py`

```python
extract_artifact_infos(content: str) -> list[ArtifactInfo]
```

正则匹配 Markdown 标记模式 → 返回结构化 `ArtifactInfo` 列表。

**ArtifactInfo 结构** (models.py):

| 字段 | 类型 | 说明 |
|------|------|------|
| `type` | `"image" \| "html" \| "download"` | 语义类型 |
| `url` | `str` | 可访问 URL（相对或绝对）|
| `name` | `str` | 文件名（从 URL 最后一段提取）|
| `mime_type` | `str` | MIME 类型（根据扩展名推断或默认）|

**URL 公网化 — `publicize_artifact_urls(content)`**:

当 `settings.artifact_public_base_url` 配置时（如 `https://cdn.example.com`），将 `(/api/artifacts/` 替换为 `(https://cdn.example.com/api/artifacts/`。未配置时直接透传相对路径。

#### 3.3.2 Artifact HTTP Serve — `routes/artifacts.py`

| 端点 | 用途 | 安全机制 |
|------|------|---------|
| `GET /api/artifacts/{agent_id}/files/{path}` | 通用 artifact serve，支持 HTML 相对资源引用 | 路径须解析在 `agent_packages_path` 内；后缀白名单 `{png,jpg,jpeg,webp,gif,html,htm,css,js,json,svg,pptx,ppt,docx,doc,xlsx,xls,pdf,zip}` |
| `GET /api/artifacts/{agent_id}?path=...` | 兼容旧格式，仅限图片 | 同上白名单限制为图片后缀；`FileResponse` 带正确 `Content-Type` |

**路径解析策略** (`_resolve_artifact`):

1. 主路径: `{agent_packages_path}/{relative_path}` — 大多数 artifact 在此
2. 回退路径: 当 `agent_packages_dir` 是相对路径且主路径不存在时，strip agent 包名前缀后在项目根查找

#### 3.3.3 SSE 流传递 — `routes/chat_rest.py:_sse_stream()`

`_chat_turn_events` yield 的 event 包含 `artifacts` 字段后，`_sse_stream` 将其序列化为 SSE payload 的非标准字段 `_artifacts`:

```
data: {"choices":[{"delta":{"content":"..."},"index":0}],"_artifacts":[...],"_dialogue_id":"..."}
```

**同步模式** (非 SSE): 累积 `accumulated_artifacts` 后放入 `ChatRestResponse.artifacts` 字段，通过标准 JSON 响应返回。

#### 3.3.4 持久化 — `providers/session_provider.py`

`serialize_artifacts(content)` 在保存 dialogue 时被调用：

```python
def serialize_artifacts(content: str) -> str:
    artifacts = extract_artifact_infos(content)
    return json.dumps([a.model_dump() for a in artifacts], ensure_ascii=False)
# → save_dialogue(..., artifacts=serialize_artifacts(full_content))
```

SQLite schema 中 `dialogue_rounds.artifacts` 为 `TEXT DEFAULT '[]'`，存储 ArtifactInfo JSON 数组。

**历史回放**: `loadHistoryMessages` 从 DB 读取 `artifacts` 字段 → 注入 `ChatMessage.artifacts` → `ArtifactList` 渲染。

### 3.4 前端层

#### 3.4.1 SSE 事件流处理 — `composables/useChatSse.ts`

```
parseSseStream: data: {...} → yield { type:'content', content, artifacts, ... }

executeSseRequest:
  SSE path:   appendChunk(msgId, event.content, event.artifacts)
  Sync path:  appendChunk(msgId, data.content, data.artifacts)
```

#### 3.4.2 Store 层 — `stores/chatStore.ts:appendChunk()`

```typescript
function appendChunk(messageId, chunk, artifacts?) {
  target.content += chunk
  if (artifacts) {
    target.artifacts = dedup(target.artifacts || [], artifacts)  // by url
  }
}
```

#### 3.4.3 展示层

| 组件 | 职责 | 数据来源 |
|------|------|---------|
| `ChatView.vue` | 消息时间线 | `chatStore.messages` |
| `MarkdownContent.vue` | Markdown 渲染（含 `![](url)` 图片） | `msg.content` |
| `ArtifactList.vue` | 结构化产物卡片（缩略图网格 + 文件按钮） | `msg.artifacts` |

**`ArtifactList` 渲染策略**:

- `type === "image"`: 内联缩略图网格，点击在新标签页打开原图
- `type === "html"`: "查看报告"按钮，点击在新标签页打开 HTML
- `type === "download"`: "下载文件"按钮 + 文件名

## 4. 配置项

| 配置 key | 环境变量 | 默认值 | 说明 |
|---------|---------|--------|------|
| `artifact_public_base_url` | `ARTIFACT_PUBLIC_BASE_URL` | `""` | 公网 CDN 前缀。空 = 用相对 `/api/artifacts/` 路径 |
| `content_image_inline_threshold_kb` | — | `256` | 图片 base64 ≥ 此阈值时落地磁盘（关注入方向，影响出方向的是 agent 产物路径）|

## 5. 不变量

| ID | 不变量 | 守护点 |
|----|--------|--------|
| I-1 | artifact URL 必须解析在 `agent_packages_path` 或项目根目录内 | `_resolve_artifact` 的 `relative_to` 检测 + 遍历防护 |
| I-2 | 前端展示的 artifact URL 必须可通过 HTTP GET 访问 | artifact 路由不依赖 daemon 在线 |
| I-3 | SSE 流、同步模式、历史回放三条路径的 artifact 数据格式一致 | 共用 `extract_artifact_infos` + `serialize_artifacts` |
| I-4 | 同一对话内重复 artifact URL 只在前端显示一次 | `appendChunk` 的 `some(existing => existing.url === a.url)` 去重 |
| I-5 | 持久化失败不应阻塞对话主流程 | `save_dialogue` 有 `try/except` 包裹（I-5 模式） |
| I-6 | **artifacts 必须是 skill 本次新生成的文件，不得是 skill 消费过的输入** | 数据流真相源：`content_serializer._compute_upload_dir` 把 API 传入图片统一落到 `{agent_packages_path}/{agent_id}/workspace/upload/{sha}.{ext}`，并把路径以文本形式注入 user message（`"用户上传了图片，路径: {file_path} …"`）一路透传到 skill prompt。因此 `workspace/upload/` 是**输入目录**，绝非产物目录。`extract_artifact_paths` 的产物边界守卫：扫到 `workspace/upload/`（以及兼容旧约定的 `uploads/`）路径时必须跳过，无论文件是否存在。产物语义 = 本次新生成，仅凭"路径出现在文本里 + 文件存在"不足够 |

## 6. 错误模型

| 错误场景 | 表现 | 防护措施 |
|---------|------|---------|
| artifact 文件不存在 | HTTP 404 + `"artifact not found"` | `is_file()` 检查 |
| 路径穿越尝试 | HTTP 403 + `"artifact path blocked"` | `relative_to` 白名单校验 |
| 不支持的 artifact 类型 | HTTP 415 + `"unsupported artifact type"` | 后缀白名单 |
| SSE 流中 artifacts 丢失 | 前端只渲染 Markdown 图片链接，不显示 ArtifactList | 降级到 Markdown 图片渲染 |
| 历史回放 artifacts 为空（旧数据）| 前端只渲染 Markdown 图片链接 | 降级路径 |

## 7. 未来扩展

| 阶段 | 计划 |
|------|------|
| Phase B | 新增 artifact 类型（`video`、`audio`），扩展 `_IMAGE_SUFFIXES` 等集合 |
| Phase C | artifact CDN 推送：Bridge 自动将文件同步到外部对象存储 |
| Phase D | artifact TTL 管理：daemon 清理 + Bridge serve 404 时自动回退 |

## 8. 参考实现

- 提取逻辑: `frontend/bridge/routes/artifact_extractor.py`
- HTTP serve: `frontend/bridge/routes/artifacts.py`
- SSE 传递: `frontend/bridge/routes/chat_rest.py:_sse_stream()` + `_chat_turn_events()`
- 持久化: `frontend/bridge/providers/session_provider.py:save_dialogue()`
- 前端 SSE 解析: `frontend/web/src/composables/useChatSse.ts:parseSseStream()`
- 前端 store: `frontend/web/src/stores/chatStore.ts:appendChunk()`
- 前端展示: `frontend/web/src/components/ArtifactList.vue`
- Agent 端标记生成: `examples/agent/outfit-assistant/nodes/coordinator_decide.py:append_artifacts_markdown()`
