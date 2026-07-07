# 多模态输入设计 — 前端 UI

> **文档层级**: L2 领域设计  
> **状态**: 初稿（待审阅）  
> **作者**: AgentFlow Bridge  
> **日期**: 2026-07-03  
> **关联**: `docs/design/features/multimodal-chat-input.md`（后端设计）  
> **传输策略**: 方案 C 混合 — 小图内联 base64，大图先上传再引用 file_url  
> **实施分期**: Phase A 内联传输（本期实现）→ Phase B 文件上传（future）

---

## 1. 问题陈述

当前前端 ChatView 只支持纯文本输入：

- `ChatView.vue:L372-381` — 单个 `el-input type="textarea"`，无附件能力
- `chatStore.ts:L49` — `send(content: string)` 只收字符串
- `agentflow.ts:L172,203` — `ChatMessage.content` 与 `ChatRequest.content` 均为 string

用户需要类似 ChatGPT 的体验：在对话框中粘贴/拖入图片，显示缩略图，可点击放大、可取消上传，与文本一起发送。

---

## 2. 设计目标

| # | 目标 | 优先级 | 验收标准 |
|---|------|--------|---------|
| F1 | 支持图片选择（文件选择器 + 粘贴 + 拖拽） | P0 | 三种方式都能添加图片 |
| F2 | 输入框上方显示已附图片缩略图 | P0 | 缩略图 64×64，点击放大 |
| F3 | 缩略图右上角"×"按钮取消上传 | P0 | 点击 × 移除该图 |
| F4 | 点击缩略图弹出全屏 lightbox 放大查看 | P1 | Esc/点击遮罩关闭 |
| F5 | 发送时把图片转 base64 + content array 发给后端 | P0 | 兼容后端 multimodal-chat-input 设计 |
| F6 | 限制图片类型与大小，超限给提示 | P0 | jpg/png/webp/gif，单图 ≤10MB |
| F7 | 历史消息中 user 消息渲染已附图片 | P0 | 历史回放能看到图 |
| F8 | 流式回复中 assistant 消息暂不支持图片 | P2 | 仅文本/markdown |

---

## 3. 现状调研（Pre-Scan）

### 3.1 当前组件结构

```
ChatPage.vue
  └── ChatView.vue
        ├── 消息列表（ul > li）
        │     ├── user 气泡：MarkdownContent
        │     └── assistant 气泡：MarkdownContent + LlmTracePanel
        └── 输入区（footer.chat-view__composer）
              ├── el-input textarea（v-model="inputText"）
              └── 发送/停止 按钮
```

### 3.2 当前数据流

```
ChatView.handleSend()
  → emit('send', inputText)             // 字符串
    → ChatPage 调 useChatWebSocket.send(content: string)
      → chatStore.send(content)         // 存 ChatMessage{content: string}
      → socket.send({content: string})  // 发给 bridge
```

### 3.3 已有依赖

- `element-plus` — `el-input`, `el-button`, `el-card` 等
- `@element-plus/icons-vue` — `Promotion`, `Refresh`, `Download`
- 无现成的图片上传/lightbox 组件

---

## 4. 设计方案

### 4.1 UI 交互设计（ChatGPT 风格）

```
┌─────────────────────────────────────────────────────┐
│  消息列表区                                          │
│  ┌───────────────────────────────────────────────┐  │
│  │ [user] 你看看这张图                            │  │
│  │        ┌──────────┐                           │  │
│  │        │ [缩略图] │  ← 已发送的图也能看       │  │
│  │        └──────────┘                           │  │
│  └───────────────────────────────────────────────┘  │
└─────────────────────────────────────────────────────┘
┌─────────────────────────────────────────────────────┐
│ 输入区 footer                                        │
│  ┌───┐ ┌───┐                                        │
│  │ × │ │ × │  ← 待发送缩略图（hover 显示 ×）       │
│  └───┘ └───┘                                        │
│  ┌─────────────────────────────────────────────┐ 📎 │
│  │ 输入消息… (Enter 发送, Shift+Enter 换行)    │ + │ ← 附件按钮 + 拖拽区
│  └─────────────────────────────────────────────┘   │
│                              [发送] / [停止]        │
└─────────────────────────────────────────────────────┘
```

**交互细节**：

| 动作 | 行为 |
|------|------|
| 点击 📎 / 拖拽图到输入框 / Ctrl+V 粘贴 | 添加图片到待发送列表 |
| hover 缩略图 | 右上角显示 × 按钮 |
| 点击 × | 移除该图（不发请求） |
| 点击缩略图本体 | 弹出 lightbox 全屏放大 |
| Esc / 点击遮罩 / 点击图 | 关闭 lightbox |
| 拖拽图进入页面 | 输入框边框高亮提示 |
| 超过 10MB / 类型不支持 | el-message.error 提示，不加入列表 |
| 发送 | 文本 + 所有缩略图一起发，发送后清空文本与图片 |

### 4.2 组件拆分

新增 3 个组件，ChatView 编排：

```
ChatView.vue (编排)
  ├── MessageList (现有逻辑抽出，可选)
  ├── MessageBubble (现有逻辑抽出，可选)
  │     └── MessageAttachments.vue   ★新增 — 渲染消息内已附图片
  ├── ChatComposer.vue               ★新增 — 输入区（文本+附件+发送按钮）
  │     ├── AttachmentPreview.vue    ★新增 — 待发送缩略图条
  │     └── ImageLightbox.vue        ★新增 — 全屏放大
  └── (其余保持不变)
```

**为何拆分**：ChatView 当前 658 行，再加多模态逻辑会超 1000 行，违反单一职责（F6）。拆出 ChatComposer 后 ChatView 只做编排。

### 4.3 数据模型（types/agentflow.ts）

```typescript
// ── Content array（与后端设计对齐）─────────────────
export interface TextPart {
  type: 'text'
  text: string
}

export interface ImagePart {
  type: 'image_url'
  image_url: { url: string }  // data:image/...;base64,... 或 file://...
}

export type ContentPart = TextPart | ImagePart

// ── ChatMessage 升级 ──────────────────────────────
export interface ChatMessage {
  id: string
  role: 'user' | 'assistant'
  content: string | ContentPart[]   // ← 联合类型
  timestamp: number
}

// ── ChatRequest 升级 ──────────────────────────────
export interface ChatRequest {
  user_id: string
  client_id: string
  content: string | ContentPart[]   // ← 联合类型
}

// ── 前端独有：待发送附件的本地状态 ─────────────────
export interface PendingAttachment {
  id: string                        // 前端生成的 uuid，用于 v-for key
  file: File                        // 原始 File 对象
  dataUrl: string                   // base64 data URL，用于预览与发送
  name: string
  size: number
  mime: string
}
```

**不变量**：

- I-F1: `PendingAttachment.dataUrl` 永远是 `data:image/...;base64,...` 形式（前端不做 file:// 转换，由 bridge 落盘）
- I-F2: `ChatMessage.content` 为 array 时，至少含一个 part
- I-F3: assistant 消息的 `content` 永远是 string（流式 chunk 拼接），前端渲染时不强求 array

### 4.4 chatStore 升级

```typescript
// chatStore.ts
function send(content: string | ContentPart[]): ChatMessage {
  // ...
  const msg: ChatMessage = {
    id: nextId(),
    role: 'user',
    content,                       // ← 直接存 array 或 string
    timestamp: Date.now(),
  }
  messages.value.push(msg)
  return msg
}
```

`useChatWebSocket.send` 签名同步升级：

```typescript
function send(content: string | ContentPart[]): void {
  // ...
  const frame: ChatClientFrame = {
    user_id: chatStore.currentUserId ?? '',
    client_id: chatStore.currentClientId ?? '',
    content: userMessage.content,  // ← array 或 string 都 JSON.stringify
  }
  socket!.send(JSON.stringify(frame))
}
```

**向后兼容**：旧的纯字符串调用方（如现有测试 `send('hello world')`）继续工作，无需改动。

### 4.5 ChatComposer.vue 关键逻辑

```typescript
// 待发送附件
const attachments = ref<PendingAttachment[]>([])

// ── 添加图片 ──
async function addFiles(files: File[]) {
  for (const file of files) {
    if (!ALLOWED_MIME.has(file.type)) {
      ElMessage.error(`不支持的图片类型：${file.type}`)
      continue
    }
    if (file.size > MAX_IMAGE_SIZE) {
      ElMessage.error(`图片过大：${(file.size / 1024 / 1024).toFixed(1)}MB > 10MB`)
      continue
    }
    const dataUrl = await readAsDataUrl(file)
    attachments.value.push({
      id: crypto.randomUUID(),
      file, dataUrl,
      name: file.name, size: file.size, mime: file.type,
    })
  }
}

// ── 移除 ──
function removeAttachment(id: string) {
  attachments.value = attachments.value.filter(a => a.id !== id)
}

// ── 发送 ──
function handleSend() {
  const text = inputText.value.trim()
  if (!text && attachments.value.length === 0) return
  
  // 构造 content array（与后端 multimodal-chat-input.md §4.2 对齐）
  const parts: ContentPart[] = []
  if (text) parts.push({ type: 'text', text })
  for (const att of attachments.value) {
    parts.push({ type: 'image_url', image_url: { url: att.dataUrl } })
  }
  
  // 纯文本时仍发字符串（保持向后兼容，减少后端无图场景开销）
  const content = attachments.value.length === 0
    ? text
    : parts
  
  emit('send', content)
  inputText.value = ''
  attachments.value = []
}

// ── 拖拽 ──
function onDrop(e: DragEvent) {
  e.preventDefault()
  isDragOver.value = false
  const files = Array.from(e.dataTransfer?.files ?? [])
  addFiles(files.filter(f => f.type.startsWith('image/')))
}

// ── 粘贴 ──
function onPaste(e: ClipboardEvent) {
  const items = Array.from(e.clipboardData?.items ?? [])
  const imageFiles = items
    .filter(i => i.type.startsWith('image/'))
    .map(i => i.getAsFile())
    .filter((f): f is File => f !== null)
  if (imageFiles.length > 0) {
    e.preventDefault()  // 阻止图片 URL 粘贴为文本
    addFiles(imageFiles)
  }
}

// ── 文件选择 ──
const fileInputRef = ref<HTMLInputElement | null>(null)
function triggerFilePicker() { fileInputRef.value?.click() }
function onFileChange(e: Event) {
  const input = e.target as HTMLInputElement
  addFiles(Array.from(input.files ?? []))
  input.value = ''  // 重置以便相同文件可再次选择
}
```

**常量**：

```typescript
const ALLOWED_MIME = new Set([
  'image/jpeg', 'image/png', 'image/webp', 'image/gif',
])
const MAX_IMAGE_SIZE = 10 * 1024 * 1024  // 10MB，与后端 max_image_size_mb 一致
```

### 4.6 AttachmentPreview.vue（缩略图条）

```vue
<template>
  <div v-if="attachments.length > 0" class="attachment-preview">
    <div
      v-for="att in attachments"
      :key="att.id"
      class="attachment-preview__item"
      @click="$emit('preview', att)"
    >
      <img :src="att.dataUrl" :alt="att.name" />
      <button
        class="attachment-preview__remove"
        @click.stop="$emit('remove', att.id)"
        aria-label="取消上传"
      >
        <el-icon><Close /></el-icon>
      </button>
      <span class="attachment-preview__size">{{ formatSize(att.size) }}</span>
    </div>
  </div>
</template>
```

样式要点：

- 缩略图 64×64，圆角 6px
- hover 时 × 按钮淡入（opacity 0 → 1）
- size 标签底部半透明遮罩

### 4.7 ImageLightbox.vue（全屏放大）

```vue
<template>
  <Teleport to="body">
    <div v-if="attachment" class="image-lightbox" @click="close">
      <img
        :src="attachment.dataUrl"
        :alt="attachment.name"
        class="image-lightbox__img"
        @click.stop
      />
      <button class="image-lightbox__close" @click="close">
        <el-icon><Close /></el-icon>
      </button>
      <div class="image-lightbox__caption">
        {{ attachment.name }} · {{ formatSize(attachment.size) }}
      </div>
    </div>
  </Teleport>
</template>
```

- Esc 键关闭（`@keydown.esc`）
- 点击遮罩关闭，点击图片本身不关闭（`@click.stop`）
- 最大 90vw × 90vh，保持比例

### 4.8 MessageAttachments.vue（历史消息内图片）

渲染已发送 user 消息中的图片：

```vue
<template>
  <div v-if="images.length > 0" class="message-attachments">
    <img
      v-for="(img, i) in images"
      :key="i"
      :src="img"
      class="message-attachments__thumb"
      @click="$emit('preview', { dataUrl: img, name: 'image', size: 0 })"
    />
  </div>
</template>

<script setup lang="ts">
const props = defineProps<{ content: string | ContentPart[] }>()
const emit = defineEmits<{ (e: 'preview', att: PendingAttachment): void }>()

const images = computed(() => {
  if (typeof props.content === 'string') return []
  return props.content
    .filter(p => p.type === 'image_url')
    .map(p => (p as ImagePart).image_url.url)
})
</script>
```

在 ChatView 的 user 气泡中：

```vue
<el-card class="chat-view__bubble chat-view__bubble--user">
  <MarkdownContent
    v-if="typeof msg.content === 'string'"
    :content="msg.content"
  />
  <template v-else>
    <MarkdownContent
      v-for="(part, i) in textParts(msg.content)"
      :key="i"
      :content="part.text"
    />
    <MessageAttachments
      :content="msg.content"
      @preview="openLightbox"
    />
  </template>
</el-card>
```

### 4.9 ChatView.vue 编排改动

ChatView 把输入区替换为 ChatComposer：

```vue
<footer class="chat-view__composer">
  <ChatComposer
    :disabled="chatStore.isStreaming"
    @send="handleSend"
    @stop="handleStop"
  />
</footer>

<!-- 全局 lightbox（由 ChatView 持有，避免多实例） -->
<ImageLightbox
  :attachment="lightboxAttachment"
  @close="lightboxAttachment = null"
/>
```

```typescript
const lightboxAttachment = ref<PendingAttachment | null>(null)

function handleSend(content: string | ContentPart[]) {
  emit('send', content)
}
```

---

## 5. 状态管理

### 5.1 组件状态归属

| 状态 | 归属 | 理由 |
|------|------|------|
| `inputText` | ChatComposer | 输入文本是 composer 局部状态 |
| `attachments` | ChatComposer | 待发送附件只属于 composer，发送后清空 |
| `lightboxAttachment` | ChatView | lightbox 可由历史消息或 composer 触发，单一实例 |
| `messages` | chatStore | 不变 |
| `isStreaming` | chatStore | 不变 |

### 5.2 生命周期

```
用户操作                ChatComposer 状态        chatStore 状态
──────────              ──────────────────       ─────────────
添加图片                attachments.push         —
点击 ×                  attachments.filter       —
点击发送                emit('send', content)    send(content) → messages.push
                         attachments=[]          —
                         inputText=''            —
收到 chunk              —                        appendChunk
收到 done               —                        endAssistantTurn
```

---

## 6. 与后端协议对齐

### 6.1 发送格式

**纯文本**（无附件）— 保持字符串：

```json
{ "user_id": "u1", "client_id": "c1", "content": "你好" }
```

**带图片** — content array：

```json
{
  "user_id": "u1",
  "client_id": "c1",
  "content": [
    { "type": "text", "text": "这张图里的衣服怎么搭？" },
    { "type": "image_url", "image_url": { "url": "data:image/png;base64,iVBOR..." } }
  ]
}
```

**关键决策**：前端始终发 `data:` base64（不发 `file://`），由 bridge 落盘替换。理由：

1. 前端不知道 bridge 的落盘目录
2. base64 是浏览器唯一原生可得的图片序列化形式
3. 与后端设计 §4.4 完全一致

### 6.1 Phase A / Phase B 边界（方案 C 混合）

本期实现 **Phase A：内联传输**。前端对所有图片统一走 base64 内联，bridge 端按 256KB 阈值决定是否落盘替换。前端不感知 bridge 的落盘行为，只发 `data:` 即可。

**Phase B（future）** 上线后，前端改为按 512KB 阈值分叉：

| 图片大小 | Phase A（本期） | Phase B（future） |
|----------|----------------|------------------|
| < 512KB | 内联 base64 | 内联 base64 |
| ≥ 512KB | 内联 base64（bridge 落盘） | **先 `POST /api/chat/uploads` 上传拿 file_url，再发 chat** |

Phase A 的前端代码在 Phase B 升级时只需在 `addFiles` 中加一个大小判断分支，不影响其他组件。AttachmentPreview 在 Phase B 会新增上传进度状态，但组件接口（`PendingAttachment`）已预留 `status` / `progress` 字段位（本期不实现，留空即可）。

### 6.2 接收格式

assistant 回复仍然是字符串 chunk（`ChatChunk.content: string`），前端不改动流式渲染逻辑。

未来若 assistant 需要回图（如生成穿搭效果图），由后端 Markdown 中的 `![](file://...)` 或 bridge artifact URL 承载，`MarkdownContent` 已能渲染——不在本期范围。

---

## 7. 不变量

| ID | 描述 | 守护点 |
|----|------|--------|
| I-F1 | `PendingAttachment.dataUrl` 永远是 `data:image/...;base64,...` | `addFiles` 中 `readAsDataUrl` 单一出口 |
| I-F2 | `ChatMessage.content` 为 array 时至少含一个 part | `handleSend` 校验 |
| I-F3 | assistant 消息 `content` 永远是 string | chatStore `appendChunk` 只对 string 拼接 |
| I-F4 | 单图 ≤ 10MB，类型 ∈ {jpeg,png,webp,gif} | `addFiles` 校验 + 测试 |
| I-F5 | 发送后 `attachments` 与 `inputText` 同时清空 | `handleSend` 末尾 |
| I-F6 | 纯文本场景发字符串，有图才发 array | `handleSend` 中 `attachments.length === 0` 判断 |
| I-F7 | 拖拽/粘贴不触发默认浏览器行为（打开图片/粘贴 URL） | `e.preventDefault()` + 测试 |

---

## 8. 测试策略

### 8.1 单元测试（Vitest）

| 测试目标 | 关键断言 |
|----------|---------|
| `addFiles` 拒绝错误类型 | attachments 长度不变，ElMessage.error 被调 |
| `addFiles` 拒绝超限 | 同上 |
| `handleSend` 纯文本 | emit 的 content 是 string |
| `handleSend` 带图 | emit 的 content 是 array，含 text + image_url |
| `handleSend` 发后清空 | attachments=[]，inputText='' |
| `removeAttachment` | 指定 id 被移除 |
| `MessageAttachments` 解析 | content array 中 image_url 被提取 |
| `ChatMessage` 联合类型 | string 与 array 都能存入 chatStore |

### 8.2 集成测试

| 场景 | 验证 |
|------|------|
| 粘贴图片 → 发送 → WS 帧检查 | WS 发出 content array JSON |
| 拖拽图片 → 缩略图出现 | DOM 中有 .attachment-preview__item |
| 点击 × → 缩略图消失 | DOM 中无该 item |
| 点击缩略图 → lightbox 出现 | body 下有 .image-lightbox |
| 历史消息渲染图 | user 气泡内有 .message-attachments__thumb |

### 8.3 e2e（Playwright，可选）

完整流程：选图 → 缩略图 → 发送 → 看 daemon 日志收到 content array → assistant 回复。

---

## 9. 实施分阶段

| 阶段 | 内容 | 验收 |
|------|------|------|
| **P1** | types 升级 + chatStore.send 联合类型 + useChatWebSocket.send 联合类型 | 现有测试全绿（向后兼容） |
| **P2** | ChatComposer.vue 拆出（纯文本先迁移，行为不变） | 现有 e2e 全绿 |
| **P3** | AttachmentPreview + 文件选择按钮 + addFiles 校验 | 选图出现缩略图 |
| **P4** | 拖拽 + 粘贴 + × 移除 | 三种添加方式可用 |
| **P5** | ImageLightbox + 点击放大 | lightbox 可开关 |
| **P6** | MessageAttachments 历史消息渲染图 | 发送后历史能看到图 |
| **P7** | handleSend content array 发送 + WS 帧 | bridge 收到 array，daemon 收到 file_url |
| **P8** | 单测 + 集成测试补齐 | 覆盖率达标 |

---

## 10. ADR

| 编号 | 决策 | 理由 |
|------|------|------|
| ADR-F1 | content array 与后端 OpenAI 格式对齐 | 生态兼容，后端设计已定 |
| ADR-F2 | 前端始终发 base64，不发 file:// | 前端不知 bridge 落盘目录；base64 是浏览器唯一原生形式 |
| ADR-F3 | 纯文本场景仍发字符串 | 减少无图场景的 JSON 序列化开销，保持与旧客户端 100% 兼容 |
| ADR-F4 | 拆出 ChatComposer 而非在 ChatView 内加逻辑 | ChatView 已 658 行，再加多模态超 1000 行，违反 F6 单一职责 |
| ADR-F5 | lightbox 用 Teleport 到 body | 避免 z-index 层叠上下文问题 |
| ADR-F6 | 单图 10MB 上限与后端 max_image_size_mb 一致 | 单真相来源，前后端任一改动需同步 |
| ADR-F7 | assistant 回复本期不支持图片 | 流式 chunk 是 string，回图由 Markdown artifact URL 承载（已有） |

---

## 11. 风险与未决问题

| 风险 | 影响 | 缓解 |
|------|------|------|
| 大图 base64 占用内存（10MB 图 → ~13MB base64） | 中 | 前端只持有 dataUrl 字符串，发送后立即清空引用 |
| 粘贴冲突（用户想粘贴文本却截获了图） | 低 | 只在 `clipboardData.items` 含 image 时截获并 preventDefault |
| 历史消息回放图片 base64 体积大 | 中 | 后端落盘后 content 中是 file_url，前端 MessageAttachments 需识别 file:// 并请求 bridge 转代理 URL（future） |
| 移动端拖拽支持差 | 低 | 移动端用文件选择器为主，拖拽为桌面增强 |
| Element Plus 无原生 lightbox | 低 | 自建 ImageLightbox，逻辑简单 |

**未决**：

- 历史消息中 `file://` 图片如何渲染？→ 后端 artifact URL 已有 `/api/artifacts/...`，本期 MessageAttachments 只处理 `data:` 与 `http(s):`，`file://` 留待 P2 阶段
- 多图排序与拖拽重排？→ future，本期按添加顺序
- 图片压缩（客户端 resize）？→ future，10MB 上限内不压缩

---

## 12. 参考

- 后端设计: `docs/design/features/multimodal-chat-input.md`
- 现有组件: `frontend/web/src/components/ChatView.vue`
- Store: `frontend/web/src/stores/chatStore.ts`
- 类型: `frontend/web/src/types/agentflow.ts`
- WS composable: `frontend/web/src/composables/useChatWebSocket.ts`
- OpenAI Chat Completions vision: https://platform.openai.com/docs/guides/vision
