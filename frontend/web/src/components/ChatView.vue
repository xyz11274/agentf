<script setup lang="ts">
/**
 * ChatView
 * ------------------------------------------------------------------
 * ChatGPT-style conversation surface for AgentFlow.
 *
 * Responsibilities (kept narrow on purpose):
 *   - Render the message timeline (user / assistant bubbles)
 *   - Capture multi-line user input and dispatch via chatStore.send()
 *   - Surface connection state and streaming indicator
 *   - Show empty / error states
 *
 * NOT responsible for (per task scope):
 *   - WebSocket lifecycle (T18 composable)
 *   - localStorage persistence
 *   - Search / export
 */
import { computed, nextTick, onBeforeUnmount, ref, watch } from 'vue'
import { Download, PictureFilled, Promotion, Refresh } from '@element-plus/icons-vue'
import { ElMessage } from 'element-plus'
import { useChatStore } from '../stores/chatStore'
import type { ChatMessage } from '../types/agentflow'
import MarkdownContent from './MarkdownContent.vue'
import LlmTracePanel from './LlmTracePanel.vue'
import { getLlmTraceDetail } from '../api/llmTraceClient'
import * as yaml from 'js-yaml'

const emit = defineEmits<{
  (e: 'send', content: string, images?: string[]): void
  (e: 'stop'): void
}>()

const chatStore = useChatStore()

// ── Local UI state ----------------------------------------------------
const inputText = ref('')
const messageListRef = ref<HTMLElement | null>(null)
const fileInputRef = ref<HTMLInputElement | null>(null)
const selectedImages = ref<string[]>([])
const MAX_IMAGES = 10
const MAX_IMAGE_SIZE_MB = 20

// ── Derived state -----------------------------------------------------
/** "Connecting" only makes sense before the first connect() call. */
const connectionLabel = computed<string>(() => {
  if (chatStore.isConnected) return '已连接'
  // Distinguish "never connected" from "dropped" via currentAgentId.
  return chatStore.currentAgentId ? '已断开' : '连接中…'
})

const connectionTone = computed<'on' | 'off' | 'pending'>(() => {
  if (chatStore.isConnected) return 'on'
  if (chatStore.currentAgentId) return 'off'
  return 'pending'
})

const canSend = computed<boolean>(
  () => chatStore.isConnected
    && (inputText.value.trim().length > 0 || selectedImages.value.length > 0)
    && !chatStore.isStreaming,
)

const showEmptyState = computed<boolean>(
  () => chatStore.messages.length === 0 && !chatStore.error,
)

/** True when the assistant placeholder is empty (typing dots) vs has content. */
function isPendingAssistant(msg: ChatMessage): boolean {
  return msg.role === 'assistant' && msg.content.length === 0
}

/** True when the assistant placeholder is both empty and being replaced by progress info. */
function shouldHideAssistantBubble(msg: ChatMessage): boolean {
  return isPendingAssistant(msg) && !!chatStore.progressMessage
}

/** Check whether a message has associated LLM traces. */
function hasTraces(msg: ChatMessage): boolean {
  return msg.role === 'assistant' && !!chatStore.llmTraces[msg.id]
}

// ── Auto-scroll -------------------------------------------------------
/** Scroll the message list to the bottom on any message change. */
async function scrollToBottom(): Promise<void> {
  await nextTick()
  const el = messageListRef.value
  if (el) el.scrollTop = el.scrollHeight
}

watch(
  () => chatStore.messages.length,
  () => {
    void scrollToBottom()
  },
)

watch(
  () => chatStore.streamingMessageId,
  () => {
    void scrollToBottom()
  },
)

// ── Fetch LLM traces when a turn completes ---------------------------
watch(
  () => chatStore.isStreaming,
  (streaming) => {
    // Streaming just ended → turn completed → fetch traces.
    if (!streaming &&
        chatStore.currentAgentId &&
        chatStore.currentUserId &&
        chatStore.currentClientId) {
      void chatStore.fetchLlmTraces(
        chatStore.currentAgentId,
        chatStore.currentUserId,
        chatStore.currentClientId,
      )
    }
  },
)

// ── 图片上传 ──────────────────────────────────────────────
function handleImageSelect(event: Event): void {
  const input = event.target as HTMLInputElement
  if (!input.files || input.files.length === 0) return

  for (const file of input.files) {
    if (selectedImages.value.length >= MAX_IMAGES) {
      ElMessage.warning(`最多上传 ${MAX_IMAGES} 张图片`)
      break
    }
    if (file.size > MAX_IMAGE_SIZE_MB * 1024 * 1024) {
      ElMessage.warning(`图片 ${file.name} 超过 ${MAX_IMAGE_SIZE_MB}MB 限制`)
      continue
    }
    if (!file.type.startsWith('image/')) {
      ElMessage.warning(`文件 ${file.name} 不是图片格式`)
      continue
    }
    const reader = new FileReader()
    reader.onload = () => {
      if (typeof reader.result === 'string') {
        selectedImages.value.push(reader.result)
      }
    }
    reader.readAsDataURL(file)
  }
  // Reset input so same file can be re-selected
  input.value = ''
}

function removeImage(index: number): void {
  selectedImages.value.splice(index, 1)
}

function triggerFileInput(): void {
  fileInputRef.value?.click()
}

// --- Actions -----------------------------------------------------------
function handleSend(): void {
  const text = inputText.value.trim()
  if (text.length === 0 && selectedImages.value.length === 0) return
  if (!chatStore.isConnected) {
    pushSystemError('无法发送: 未连接到 Agent。')
    return
  }
  try {
    const images = selectedImages.value.length > 0 ? [...selectedImages.value] : undefined
    emit('send', text, images)
    inputText.value = ''
    selectedImages.value = []
    void scrollToBottom()
  } catch (e) {
    pushSystemError(e instanceof Error ? e.message : String(e))
  }
}

function handleStop(): void {
  emit('stop')
}

function handleKeydown(event: KeyboardEvent): void {
  // Shift+Enter inserts a newline; plain Enter sends.
  if (event.key === 'Enter' && !event.shiftKey) {
    event.preventDefault()
    handleSend()
  }
}

/** Append a non-store, in-view system error message so the user sees it inline. */
const systemMessages = ref<Array<{ id: string; content: string }>>([])
function pushSystemError(message: string): void {
  systemMessages.value.push({
    id: `sys-${Date.now()}-${systemMessages.value.length}`,
    content: message,
  })
  void scrollToBottom()
}

function dismissSystemMessage(id: string): void {
  systemMessages.value = systemMessages.value.filter((m) => m.id !== id)
}

// Surface chatStore.error reactively as a system message.
watch(
  () => chatStore.error,
  (err) => {
    if (err) pushSystemError(err)
  },
)

function formatTime(ts: number): string {
  const d = new Date(ts)
  const hh = String(d.getHours()).padStart(2, '0')
  const mm = String(d.getMinutes()).padStart(2, '0')
  return `${hh}:${mm}`
}

// ── 加载耗时计时器 ───────────────────────────────────
const streamStartTime = ref(0)           // ms timestamp when streaming started
const elapsedMs = ref(0)                 // ms elapsed since start
let elapsedTimer: ReturnType<typeof setInterval> | null = null

function formatElapsed(ms: number): string {
  const totalSec = Math.floor(ms / 1000)
  const min = Math.floor(totalSec / 60)
  const sec = totalSec % 60
  return `${String(min).padStart(2, '0')}:${String(sec).padStart(2, '0')}`
}

function startElapsedTimer(): void {
  stopElapsedTimer()
  streamStartTime.value = Date.now()
  elapsedMs.value = 0
  elapsedTimer = setInterval(() => {
    elapsedMs.value = Date.now() - streamStartTime.value
  }, 1000)
}

function stopElapsedTimer(): void {
  if (elapsedTimer !== null) {
    clearInterval(elapsedTimer)
    elapsedTimer = null
  }
}

onBeforeUnmount(stopElapsedTimer)

watch(
  () => chatStore.isStreaming,
  (streaming) => {
    if (streaming) {
      startElapsedTimer()
    } else {
      stopElapsedTimer()
    }
  },
)

// ── 导出对话 ──────────────────────────────────────────────
const exporting = ref(false)

async function exportConversation(format: 'json' | 'yaml'): Promise<void> {
  if (chatStore.messages.length === 0) return
  exporting.value = true

  try {
    // 收集完整的 LLM trace 详情（含 request/response 等 system 级别内容）
    const tracesMap: Record<string, unknown[]> = {}
    for (const msg of chatStore.messages) {
      if (msg.role === 'assistant') {
        const summaries = chatStore.llmTraces[msg.id]
        if (!summaries?.length) continue

        // 并行拉取每条 trace 的完整详情
        const details = await Promise.allSettled(
          summaries.map((t) =>
            getLlmTraceDetail(
              chatStore.currentAgentId ?? '',
              chatStore.currentUserId ?? '',
              chatStore.currentClientId ?? '',
              t.call_id,
            ).then((detail) => ({
              call_id: t.call_id,
              model: t.model,
              latency_ms: t.latency_ms,
              timestamp: t.timestamp,
              token_usage: t.token_usage,
              request_summary: t.request_summary,
              response_summary: t.response_summary,
              // 完整请求/响应数据（含 system prompt 等）
              request: detail.request ?? null,
              response: detail.response ?? null,
              meta: detail.meta ?? null,
            })),
          ),
        )

        tracesMap[msg.id] = details
          .filter((r) => r.status === 'fulfilled')
          .map((r) => (r as PromiseFulfilledResult<unknown>).value)
      }
    }

    const exportData = {
      exported_at: new Date().toISOString(),
      agent_id: chatStore.currentAgentId,
      user_id: chatStore.currentUserId,
      client_id: chatStore.currentClientId,
      total_messages: chatStore.messages.length,
      messages: chatStore.messages.map((msg) => ({
        role: msg.role,
        content: msg.content,
        timestamp: msg.timestamp,
        ...(msg.role === 'assistant' && tracesMap[msg.id]?.length
          ? { llm_traces: tracesMap[msg.id] }
          : {}),
      })),
    }

    const filename = `agentflow-conversation-${Date.now()}`
    const blob = format === 'json'
      ? new Blob([JSON.stringify(exportData, null, 2)], { type: 'application/json' })
      : new Blob([yaml.dump(exportData, { indent: 2 })], { type: 'text/yaml' })

    const url = URL.createObjectURL(blob)
    const a = document.createElement('a')
    a.href = url
    a.download = `${filename}.${format}`
    document.body.appendChild(a)
    a.click()
    document.body.removeChild(a)
    URL.revokeObjectURL(url)
  } catch (e) {
    pushSystemError(`导出失败: ${e instanceof Error ? e.message : e}`)
  } finally {
    exporting.value = false
  }
}

const showExportMenu = ref(false)

function toggleExportMenu(): void {
  showExportMenu.value = !showExportMenu.value
}
</script>

<template>
  <section class="chat-view" data-testid="chat-view">
    <!-- Connection status bar -->
    <header class="chat-view__status" :data-tone="connectionTone">
      <span class="chat-view__dot" :class="`chat-view__dot--${connectionTone}`" aria-hidden="true" />
      <span class="chat-view__status-label">{{ connectionLabel }}</span>
      <span v-if="chatStore.isStreaming" class="chat-view__streaming-tag">
        <span class="chat-view__typing" aria-label="Assistant is typing">
          <span /><span /><span />
        </span>
        <span class="chat-view__streaming-text">生成中…</span>
        <span v-if="elapsedMs > 0" class="chat-view__elapsed">{{ formatElapsed(elapsedMs) }}</span>
      </span>
      <span class="chat-view__export">
        <el-dropdown
          trigger="click"
          :hide-on-click="true"
          @visible-change="(v: boolean) => showExportMenu = v"
        >
          <el-button
            link
            size="small"
            :icon="Download"
            :disabled="chatStore.messages.length === 0 || exporting"
            :loading="exporting"
            title="导出对话"
            @click.stop
          />
          <template #dropdown>
            <el-dropdown-menu>
              <el-dropdown-item @click="exportConversation('json')">
                JSON 格式
              </el-dropdown-item>
              <el-dropdown-item @click="exportConversation('yaml')">
                YAML 格式
              </el-dropdown-item>
            </el-dropdown-menu>
          </template>
        </el-dropdown>
      </span>
    </header>

    <!-- Message list -->
    <div
      ref="messageListRef"
      class="chat-view__messages"
      data-testid="chat-messages"
    >
      <div v-if="showEmptyState" class="chat-view__empty">
        <p class="chat-view__empty-title">暂无消息</p>
        <p class="chat-view__empty-hint">
          选择一个 Agent 和会话开始聊天。
        </p>
      </div>

      <ul v-else class="chat-view__list">
        <li
          v-for="msg in chatStore.messages"
          :key="msg.id"
          class="chat-view__item"
          :class="`chat-view__item--${msg.role}`"
          :data-testid="`chat-message-${msg.role}`"
        >
          <!-- Assistant: avatar + name on top -->
          <div v-if="msg.role === 'assistant'" class="chat-view__meta">
            <span class="chat-view__avatar" aria-hidden="true">A</span>
            <span class="chat-view__author">AI 助手</span>
            <span class="chat-view__time">{{ formatTime(msg.timestamp) }}</span>
          </div>

          <!-- Assistant pending bubble: completely hidden when progress is active -->
          <el-card
            v-if="!shouldHideAssistantBubble(msg)"
            class="chat-view__bubble"
            :class="`chat-view__bubble--${msg.role}`"
            shadow="never"
            :body-style="{ padding: '10px 14px' }"
          >
            <template v-if="isPendingAssistant(msg)">
              <span class="chat-view__typing" aria-label="Assistant is typing">
                <span /><span /><span />
              </span>
            </template>
            <template v-else>
              <!-- Image previews for user messages -->
              <div v-if="msg.images && msg.images.length > 0" class="chat-view__msg-images">
                <img
                  v-for="(img, i) in msg.images"
                  :key="i"
                  :src="img"
                  class="chat-view__msg-image"
                  alt="用户上传的图片"
                  loading="lazy"
                />
              </div>
              <MarkdownContent :content="msg.content" />
              <!-- LLM trace panel (inline below assistant response) -->
              <LlmTracePanel
                v-if="hasTraces(msg)"
                :traces="chatStore.llmTraces[msg.id]"
              />
            </template>
          </el-card>

          <!-- User: timestamp on the right -->
          <div v-if="msg.role === 'user'" class="chat-view__meta chat-view__meta--user">
            <span class="chat-view__time">{{ formatTime(msg.timestamp) }}</span>
          </div>
        </li>

        <!-- System / error messages -->
        <li
          v-for="sys in systemMessages"
          :key="sys.id"
          class="chat-view__item chat-view__item--system"
          data-testid="chat-system-message"
        >
          <el-alert
            type="error"
            :title="sys.content"
            show-icon
            :closable="true"
            @close="dismissSystemMessage(sys.id)"
          />
        </li>

        <!-- Progress indicator (while skills are executing) -->
        <li
          v-if="chatStore.progressMessage"
          class="chat-view__item chat-view__item--progress"
          data-testid="chat-progress"
        >
          <div class="chat-view__progress">
            <span class="chat-view__progress-spinner" />
            <span class="chat-view__progress-text">{{ chatStore.progressMessage }}</span>
            <span v-if="elapsedMs > 0" class="chat-view__elapsed chat-view__elapsed--progress">{{ formatElapsed(elapsedMs) }}</span>
          </div>
        </li>
      </ul>
    </div>

    <!-- Input area -->
    <footer class="chat-view__composer">
      <!-- Image preview chips -->
      <div v-if="selectedImages.length > 0" class="chat-view__image-previews">
        <div
          v-for="(img, i) in selectedImages"
          :key="i"
          class="chat-view__image-chip"
          @click="removeImage(i)"
        >
          <img :src="img" class="chat-view__image-chip-img" alt="预览" />
          <span class="chat-view__image-chip-remove">&times;</span>
        </div>
      </div>

      <el-input
        v-model="inputText"
        type="textarea"
        :rows="2"
        :autosize="{ minRows: 2, maxRows: 6 }"
        placeholder="输入消息… (Enter 发送, Shift+Enter 换行)"
        resize="none"
        data-testid="chat-input"
        @keydown="handleKeydown"
      />
      <div class="chat-view__actions">
        <div class="chat-view__actions-left">
          <!-- Hidden file input -->
          <input
            ref="fileInputRef"
            type="file"
            accept="image/*"
            multiple
            class="chat-view__file-input"
            data-testid="chat-image-input"
            @change="handleImageSelect"
          />
          <el-button
            :icon="PictureFilled"
            :disabled="chatStore.isStreaming"
            data-testid="chat-image-btn"
            @click="triggerFileInput"
            title="上传图片"
          />
        </div>
        <div class="chat-view__actions-right">
          <el-button
            v-if="!chatStore.isStreaming"
            type="primary"
            :icon="Promotion"
            :disabled="!canSend"
            data-testid="chat-send"
            @click="handleSend"
          >
            发送
          </el-button>
          <el-button
            v-else
            type="danger"
            :icon="Refresh"
            data-testid="chat-stop"
            @click="handleStop"
          >
            停止
          </el-button>
        </div>
      </div>
    </footer>
  </section>
</template>

<style scoped>
/* ------------------------------------------------------------------
   Layout shell
   ------------------------------------------------------------------ */
.chat-view {
  display: flex;
  flex-direction: column;
  height: 100%;
  min-height: 0;
  background: #ffffff;
  border: 1px solid #e4e7ed;
  border-radius: 12px;
  overflow: hidden;
  box-shadow: 0 2px 8px rgba(0, 0, 0, 0.04);
}

/* ------------------------------------------------------------------
   Status bar
   ------------------------------------------------------------------ */
.chat-view__status {
  display: flex;
  align-items: center;
  gap: 8px;
  padding: 8px 16px;
  border-bottom: 1px solid #ebeef5;
  background: #f8f9fb;
  font-size: 13px;
  color: #606266;
  flex-shrink: 0;
}
.chat-view__dot {
  width: 8px;
  height: 8px;
  border-radius: 50%;
  background: #c0c4cc;
  flex-shrink: 0;
  transition: background-color 0.2s ease, box-shadow 0.2s ease;
}
.chat-view__dot--on {
  background: #67c23a;
  box-shadow: 0 0 0 2px rgba(103, 194, 58, 0.22);
}
.chat-view__dot--off {
  background: #f56c6c;
  box-shadow: 0 0 0 2px rgba(245, 108, 108, 0.18);
}
.chat-view__dot--pending {
  background: #e6a23c;
  box-shadow: 0 0 0 2px rgba(230, 162, 60, 0.18);
}
.chat-view__status-label {
  font-weight: 500;
}
.chat-view__streaming-tag {
  margin-left: auto;
  display: inline-flex;
  align-items: center;
  gap: 8px;
  color: #409eff;
  font-size: 12px;
}
.chat-view__elapsed {
  font-variant-numeric: tabular-nums;
  letter-spacing: 0.2px;
  color: #909399;
  font-size: 11px;
}
.chat-view__elapsed--progress {
  margin-left: auto;
  font-size: 12px;
  color: #606266;
  background: #e6f7ff;
  padding: 1px 6px;
  border-radius: 4px;
}
.chat-view__streaming-text {
  letter-spacing: 0.2px;
}
.chat-view__export {
  margin-left: auto;
  display: flex;
  align-items: center;
}
.chat-view__export .el-button {
  color: #909399;
  font-size: 14px;
}
.chat-view__export .el-button:hover {
  color: #409eff;
}

/* ------------------------------------------------------------------
   Message list
   ------------------------------------------------------------------ */
.chat-view__messages {
  flex: 1 1 auto;
  min-height: 0;
  overflow-y: auto;
  padding: 20px 24px;
  background: #f8f9fb;
}
.chat-view__list {
  list-style: none;
  margin: 0;
  padding: 0;
  display: flex;
  flex-direction: column;
  gap: 16px;
}
.chat-view__item {
  display: flex;
  flex-direction: column;
  max-width: 80%;
}
.chat-view__item--user {
  align-self: flex-end;
  align-items: flex-end;
}
.chat-view__item--assistant {
  align-self: flex-start;
  align-items: flex-start;
}
.chat-view__item--system {
  max-width: 100%;
  align-self: stretch;
}

/* Meta line (avatar + name + time) */
.chat-view__meta {
  display: flex;
  align-items: center;
  gap: 8px;
  margin-bottom: 6px;
  font-size: 12px;
  color: #909399;
}
.chat-view__meta--user {
  margin-top: 4px;
  margin-bottom: 0;
}
.chat-view__avatar {
  width: 24px;
  height: 24px;
  border-radius: 50%;
  background: linear-gradient(135deg, #409eff 0%, #67c23a 100%);
  color: #ffffff;
  font-size: 12px;
  font-weight: 600;
  display: inline-flex;
  align-items: center;
  justify-content: center;
  flex-shrink: 0;
}
.chat-view__author {
  font-weight: 600;
  color: #303133;
}
.chat-view__time {
  font-variant-numeric: tabular-nums;
  letter-spacing: 0.2px;
}

/* ------------------------------------------------------------------
   Bubbles
   ------------------------------------------------------------------ */
.chat-view__bubble {
  border: 1px solid transparent;
  border-radius: 10px;
  overflow-wrap: break-word;
}
.chat-view__bubble :deep(.el-card__body) {
  padding: 10px 14px;
}
.chat-view__bubble--user {
  background: #ecf5ff;
  border-color: #d9ecff;
}
.chat-view__bubble--assistant {
  background: #ffffff;
  border-color: #ebeef5;
}
.chat-view__bubble-text {
  margin: 0;
  white-space: pre-wrap;
  line-height: 1.55;
  color: #303133;
  font-size: 14px;
}

/* ------------------------------------------------------------------
   Typing animation (three blinking dots)
   ------------------------------------------------------------------ */
.chat-view__typing {
  display: inline-flex;
  align-items: center;
  gap: 4px;
  height: 18px;
}
.chat-view__typing > span {
  width: 6px;
  height: 6px;
  border-radius: 50%;
  background: #909399;
  display: inline-block;
  animation: chat-view-typing-bounce 1.1s infinite ease-in-out;
}
.chat-view__typing > span:nth-child(2) {
  animation-delay: 0.15s;
}
.chat-view__typing > span:nth-child(3) {
  animation-delay: 0.3s;
}
@keyframes chat-view-typing-bounce {
  0%, 60%, 100% {
    transform: translateY(0);
    opacity: 0.5;
  }
  30% {
    transform: translateY(-4px);
    opacity: 1;
  }
}

/* ------------------------------------------------------------------
   Empty state
   ------------------------------------------------------------------ */
.chat-view__empty {
  display: flex;
  flex-direction: column;
  align-items: center;
  justify-content: center;
  height: 100%;
  text-align: center;
  color: #909399;
  padding: 32px;
}
.chat-view__empty-title {
  margin: 0 0 6px;
  font-size: 15px;
  font-weight: 600;
  color: #606266;
}
.chat-view__empty-hint {
  margin: 0;
  font-size: 13px;
  line-height: 1.6;
}

/* ------------------------------------------------------------------
   Composer (input + send/stop)
   ------------------------------------------------------------------ */
.chat-view__composer {
  display: flex;
  flex-direction: column;
  gap: 8px;
  padding: 12px 16px 14px;
  border-top: 1px solid #ebeef5;
  background: #ffffff;
  flex-shrink: 0;
}

/* ── Image preview chips ── */
.chat-view__image-previews {
  display: flex;
  flex-wrap: wrap;
  gap: 8px;
}
.chat-view__image-chip {
  position: relative;
  width: 72px;
  height: 72px;
  border-radius: 8px;
  overflow: hidden;
  cursor: pointer;
  border: 1px solid #e4e7ed;
  flex-shrink: 0;
}
.chat-view__image-chip-img {
  width: 100%;
  height: 100%;
  object-fit: cover;
  display: block;
}
.chat-view__image-chip-remove {
  position: absolute;
  top: 2px;
  right: 2px;
  width: 18px;
  height: 18px;
  border-radius: 50%;
  background: rgba(0, 0, 0, 0.5);
  color: #fff;
  font-size: 14px;
  line-height: 18px;
  text-align: center;
  opacity: 0.7;
  transition: opacity 0.15s;
}
.chat-view__image-chip:hover .chat-view__image-chip-remove {
  opacity: 1;
}

/* ── Hidden file input ── */
.chat-view__file-input {
  display: none;
}

/* ── Actions row ── */
.chat-view__actions {
  display: flex;
  justify-content: space-between;
  align-items: center;
  gap: 8px;
}
.chat-view__actions-left {
  display: flex;
  gap: 4px;
}
.chat-view__actions-right {
  display: flex;
  gap: 8px;
}

/* ── Images in message bubbles ── */
.chat-view__msg-images {
  display: flex;
  flex-wrap: wrap;
  gap: 6px;
  margin-bottom: 8px;
}
.chat-view__msg-image {
  max-width: 240px;
  max-height: 240px;
  border-radius: 8px;
  object-fit: contain;
  border: 1px solid #e4e7ed;
  background: #f5f7fa;
}

/* ── Progress indicator ── */
.chat-view__item--progress {
  align-self: flex-start;
  align-items: flex-start;
  max-width: 100%;
}
.chat-view__progress {
  display: inline-flex;
  align-items: flex-start;
  gap: 8px;
  padding: 8px 14px;
  background: #f0f9ff;
  border: 1px solid #d9ecff;
  border-radius: 8px;
  font-size: 13px;
  color: #409eff;
}
.chat-view__progress-spinner {
  width: 14px;
  height: 14px;
  border: 2px solid #d9ecff;
  border-top-color: #409eff;
  border-radius: 50%;
  animation: chat-view-spin 0.8s linear infinite;
  flex-shrink: 0;
}
.chat-view__progress-text {
  white-space: pre-wrap;
  overflow: hidden;
  max-height: 3.6em;
  line-height: 1.4;
  word-break: break-word;
}
@keyframes chat-view-spin {
  to { transform: rotate(360deg); }
}
</style>
