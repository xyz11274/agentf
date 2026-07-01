<script setup lang="ts">
/**
 * AssistantWidget.vue — 浮动气泡式智能助手
 *
 * 全局悬浮在页面右下角的气泡按钮，点击展开聊天面板。
 * 自动识别当前页面上下文，提供场景化辅助。
 *
 * 核心能力：
 *  - 连接 agentflow_assistant agent 进行问答
 *  - 识别当前路由页面，发送上下文信息
 *  - 提供场景化快捷建议
 *  - 消息持久化到 sessionStorage
 */
import { getApiClient } from '@/api/context'
import MarkdownContent from '@/components/MarkdownContent.vue'
import { useChatWebSocket } from '@/composables/useChatWebSocket'
import { ChatDotSquare, Close, Promotion } from '@element-plus/icons-vue'
import { ElMessage } from 'element-plus'
import {
  computed, nextTick, onBeforeUnmount, onMounted, ref, watch,
} from 'vue'
import { useRoute } from 'vue-router'

// ── 固定的助手 Agent ID ─────────────────────────────────────
const ASSISTANT_AGENT_ID = 'agentflow-assistant'
const STORAGE_KEY = 'assistant-widget-messages'

// ── 状态 ────────────────────────────────────────────────────
const isOpen = ref(false)        // 气泡面板展开/收起
const inputText = ref('')
const messages = ref<Array<{ id: string; role: 'user' | 'assistant'; content: string; timestamp: number }>>([])
const isStreaming = ref(false)
const streamingMsgId = ref<string | null>(null)
const error = ref<string | null>(null)
const unreadCount = ref(0)
const messageListRef = ref<HTMLElement | null>(null)
const contextSent = ref(false)   // 当前页面上下文是否已发送

// ── 拖拽定位 ──────────────────────────────────────────────
interface Position { x: number; y: number }
const POSITION_KEY = 'assistant-widget-pos'
function loadPosition(): Position {
  try {
    const raw = localStorage.getItem(POSITION_KEY)
    if (raw) return JSON.parse(raw) as Position
  } catch { /* ignore */ }
  return {
    x: window.innerWidth - 404,
    y: window.innerHeight - 624,
  }
}
const position = ref<Position>(loadPosition())
const isDragging = ref(false)
const dragOffset = ref({ x: 0, y: 0 })
const dragPerformed = ref(false)

function getClientPos(e: MouseEvent | TouchEvent): { x: number; y: number } {
  if ('touches' in e && e.touches.length > 0) {
    return { x: e.touches[0].clientX, y: e.touches[0].clientY }
  }
  const me = e as MouseEvent
  return { x: me.clientX, y: me.clientY }
}

function clamp(val: number, min: number, max: number): number {
  return Math.max(min, Math.min(val, max))
}

function startDrag(e: MouseEvent | TouchEvent) {
  dragPerformed.value = false
  const client = getClientPos(e)
  dragOffset.value = { x: client.x - position.value.x, y: client.y - position.value.y }
  isDragging.value = true
  document.addEventListener('mousemove', onDragMove)
  document.addEventListener('mouseup', onDragEnd)
  document.addEventListener('touchmove', onDragMove, { passive: false })
  document.addEventListener('touchend', onDragEnd)
}

function onDragMove(e: MouseEvent | TouchEvent) {
  if (!isDragging.value) return
  e.preventDefault()
  const client = getClientPos(e)
  // 移动超过 5px 才算真正的拖拽
  if (!dragPerformed.value) {
    const dx = Math.abs(client.x - (dragOffset.value.x + position.value.x))
    const dy = Math.abs(client.y - (dragOffset.value.y + position.value.y))
    if (dx > 5 || dy > 5) dragPerformed.value = true
  }
  const pw = isOpen.value ? PANEL_WIDTH : 56
  const ph = isOpen.value ? PANEL_HEIGHT : 56
  const ox = isOpen.value ? panelOffset.value.left : 0
  const oy = isOpen.value ? panelOffset.value.top : 0
  position.value = {
    x: clamp(client.x - dragOffset.value.x, -ox, window.innerWidth - pw - ox),
    y: clamp(client.y - dragOffset.value.y, -oy, window.innerHeight - ph - oy),
  }
}

function onDragEnd() {
  if (!isDragging.value) return
  isDragging.value = false
  savePosition(position.value)
  document.removeEventListener('mousemove', onDragMove)
  document.removeEventListener('mouseup', onDragEnd)
  document.removeEventListener('touchmove', onDragMove)
  document.removeEventListener('touchend', onDragEnd)
}

function savePosition(pos: Position) {
  try {
    localStorage.setItem(POSITION_KEY, JSON.stringify(pos))
  } catch { /* ignore */ }
}

// ── 面板展开方向（基于气泡位置智能偏移，避免溢出屏幕）────
const PANEL_WIDTH = 380
const PANEL_HEIGHT = 560
const BUBBLE_SIZE = 56
const GAP = 10
const panelOffset = ref({ top: 0, left: 0 })

function computePanelOffset() {
  const cx = position.value.x
  const cy = position.value.y
  const vw = window.innerWidth
  const vh = window.innerHeight

  // 默认：面板在气泡的右下方
  let top = BUBBLE_SIZE + GAP
  let left = BUBBLE_SIZE + GAP

  // 右侧空间不足 → 面板向左展开
  if (cx + BUBBLE_SIZE + GAP + PANEL_WIDTH > vw) {
    left = -(PANEL_WIDTH + GAP)
  }
  // 下方空间不足 → 面板向上展开
  if (cy + BUBBLE_SIZE + GAP + PANEL_HEIGHT > vh) {
    top = -(PANEL_HEIGHT + GAP)
  }
  panelOffset.value = { top, left }
}

function onWindowResize() {
  if (isOpen.value) computePanelOffset()
}

const route = useRoute()
const ws = useChatWebSocket()

// ── 页面上下文识别 ──────────────────────────────────────────
interface PageContext {
  name: string
  title: string
  description: string
  suggestions: string[]
}

function getPageContext(): PageContext {
  const path = route.path
  const name = String(route.name ?? '')

  if (path.startsWith('/packages/create')) {
    return {
      name: 'package-create',
      title: '创建智能体包',
      description: '正在创建新的 Agent 智能体包',
      suggestions: [
        '帮我创建一个 harness 类型的 agent',
        '展示可用的模板类型',
        '我想创建一个基础的 DAG agent',
      ],
    }
  }
  if (path.startsWith('/packages') && route.params.name) {
    return {
      name: 'package-detail',
      title: '包编辑器 / DAG 可视化编排',
      description: `正在编辑 Agent 包: ${route.params.name}`,
      suggestions: [
        '帮我分析当前 workflow 结构',
        '如何添加一个 Python 节点？',
        '解释 coordinator 和 skill 的区别',
      ],
    }
  }
  if (path === '/chat') {
    return {
      name: 'chat',
      title: 'Agent 问答页',
      description: '正在使用 Agent 问答功能',
      suggestions: [
        '我该如何使用这个聊天页面？',
        '如何创建新的会话？',
        'Agent 问答支持哪些功能？',
      ],
    }
  }
  // 兜底
  return {
    name: name || 'unknown',
    title: 'AgentFlow 平台',
    description: '浏览 AgentFlow 工作流平台',
    suggestions: [
      'AgentFlow 有哪些核心概念？',
      '如何开始创建一个 agent？',
      '展示可用的 agent 模板',
    ],
  }
}

const pageCtx = computed<PageContext>(() => getPageContext())

// ── 连接管理 ────────────────────────────────────────────────
let isConnecting = ref(false)

/**
 * 确保 agentflow-assistant agent 已加载到 daemon 中，然后连接 WebSocket。
 * 如果 agent 未加载，先通过 bridge API 加载它（POST /api/agents/{id}/load）。
 */
async function ensureAgentLoaded(): Promise<boolean> {
  try {
    // 1. 检查 agent 是否已加载
    const listRes = await getApiClient().get<unknown>('/api/agents')
    const rawList: Record<string, unknown>[] = Array.isArray(listRes.data)
      ? (listRes.data as Record<string, unknown>[])
      : ((listRes.data as Record<string, unknown>)?.agents ?? []) as Record<string, unknown>[]
    const loaded = rawList.some(
      (a) => (a.agent_id ?? a.id ?? a.name) === ASSISTANT_AGENT_ID
        || a.name === 'AgentFlowAssistant',
    )
    if (loaded) return true

    // 2. Agent 未加载 → 通过 bridge 加载它
    //    使用 package_path 指向 agentflow_assistant 目录
    await getApiClient().post<unknown>(
      `/api/agents/${encodeURIComponent(ASSISTANT_AGENT_ID)}/load`,
      { package_path: 'agentflow_assistant' },
    )
    // 等一小段时间让 daemon 完成加载
    await new Promise((r) => setTimeout(r, 500))
    return true
  } catch (e) {
    error.value = `加载助手失败: ${e instanceof Error ? e.message : e}`
    return false
  }
}

async function ensureConnected(): Promise<void> {
  if (ws.isConnected.value) return
  if (isConnecting.value) return
  isConnecting.value = true
  try {
    // 先确保 agent 已加载
    const loaded = await ensureAgentLoaded()
    if (!loaded) return
    // 再连接 WebSocket
    await ws.connect(ASSISTANT_AGENT_ID)
  } catch (e) {
    error.value = `连接失败: ${e instanceof Error ? e.message : e}`
    ElMessage.error('助手连接失败，请检查 Daemon 状态')
  } finally {
    isConnecting.value = false
  }
}

// ── 发送页面上下文 ──────────────────────────────────────────
function sendPageContext() {
  if (!ws.isConnected.value || contextSent.value) return
  const ctx = pageCtx.value
  const contextMsg = `【当前页面】${ctx.title}\n【描述】${ctx.description}\n【路径】${route.path}\n---\n请根据我当前所在的页面提供帮助。`
  contextSent.value = true
  // 通过 sendMessage 发送，确保走完整的 WS 发送 + 流式接收流程
  void sendMessage(contextMsg)
}

// ── 发送/接收消息 ────────────────────────────────────────────
let msgSeq = 0
function nextId(): string {
  msgSeq += 1
  return `aw-${Date.now()}-${msgSeq}`
}

async function sendMessage(text: string) {
  const content = text.trim()
  if (!content) return

  // 确保已连接
  if (!ws.isConnected.value) {
    await ensureConnected().catch(() => {})
    if (!ws.isConnected.value) return
  }

  // 添加用户消息
  messages.value.push({
    id: nextId(),
    role: 'user',
    content,
    timestamp: Date.now(),
  })

  // 准备 assistant 占位
  const asstMsg = { id: nextId(), role: 'assistant' as const, content: '', timestamp: Date.now() }
  messages.value.push(asstMsg)
  streamingMsgId.value = asstMsg.id
  isStreaming.value = true

  inputText.value = ''
  await scrollToBottom()

  // 发送
  ws.send(content)
}

function sendSuggestion(text: string) {
  void sendMessage(text)
}

async function scrollToBottom(): Promise<void> {
  await nextTick()
  const el = messageListRef.value
  if (el) el.scrollTop = el.scrollHeight
}

// ── 气泡打开/关闭 ────────────────────────────────────────────
async function toggleOpen() {
  // 拖拽操作不触发打开/关闭
  if (dragPerformed.value) {
    dragPerformed.value = false
    return
  }
  if (!isOpen.value) {
    computePanelOffset()
    isOpen.value = true
    unreadCount.value = 0
    await nextTick()
    // 确保连接
    await ensureConnected()
    // 如果还没发送上下文，发送
    if (ws.isConnected.value && !contextSent.value) {
      sendPageContext()
    }
    await scrollToBottom()
  } else {
    isOpen.value = false
  }
}

function handleClose() {
  isOpen.value = false
}

// ── 监听 WS 消息流 ──────────────────────────────────────────
// useChatWebSocket 通过 chatStore 管理消息，但我们使用独立的消息列表
// 我们需要劫持消息流。由于 useChatWebSocket 会写 chatStore，
// 我们直接读取 chatStore 的 streamingMessageId 变化来驱动 UI。
import { useChatStore } from '@/stores/chatStore'

const chatStore = useChatStore()

// 监听 stream 更新 -> 复制到本地消息
watch(() => chatStore.streamingMessageId, (newId) => {
  if (newId && isStreaming.value) {
    // 找到 chatStore 中正在流式输出的消息
    const streaming = chatStore.messages.find(m => m.id === newId)
    if (streaming && streamingMsgId.value) {
      const local = messages.value.find(m => m.id === streamingMsgId.value)
      if (local) {
        local.content = streaming.content
      }
    }
    void scrollToBottom()
  }
})

// 监听 stream 结束
watch(() => chatStore.isStreaming, (val) => {
  if (!val && isStreaming.value) {
    isStreaming.value = false
    // 先保存当前 streamingMsgId，再置空
    const lastStreamingId = streamingMsgId.value
    streamingMsgId.value = null
    // 确保完整内容已同步
    if (lastStreamingId) {
      const lastAsst = [...chatStore.messages].reverse().find(m => m.role === 'assistant')
      if (lastAsst) {
        const local = messages.value.find(m => m.id === lastStreamingId)
        if (local && !local.content) {
          local.content = lastAsst.content
        }
      }
    }
    saveToStorage()
  }
})

// 监听 WS 连接状态
watch(() => ws.isConnected.value, (connected) => {
  if (connected && isOpen.value && !contextSent.value) {
    sendPageContext()
  }
})

// 监听 ws 错误
watch(() => ws.error.value, (err) => {
  if (err) {
    error.value = err
  }
})

// ── 页面切换时重置上下文 ────────────────────────────────────
watch(() => route.path, () => {
  contextSent.value = false
  // 不清除消息，保留对话历史
})

// ── 持久化 ──────────────────────────────────────────────────
function saveToStorage() {
  try {
    sessionStorage.setItem(STORAGE_KEY, JSON.stringify(messages.value))
  } catch { /* ignore */ }
}

function loadFromStorage() {
  try {
    const raw = sessionStorage.getItem(STORAGE_KEY)
    if (raw) {
      const saved = JSON.parse(raw) as typeof messages.value
      if (Array.isArray(saved) && saved.length > 0) {
        messages.value = saved
      }
    }
  } catch { /* ignore */ }
}

// ── 生命周期 ─────────────────────────────────────────────────
onMounted(() => {
  loadFromStorage()
  window.addEventListener('resize', onWindowResize)
})

onBeforeUnmount(() => {
  window.removeEventListener('resize', onWindowResize)
  // 清理拖拽事件
  document.removeEventListener('mousemove', onDragMove)
  document.removeEventListener('mouseup', onDragEnd)
  document.removeEventListener('touchmove', onDragMove)
  document.removeEventListener('touchend', onDragEnd)
  if (ws.isConnected.value) {
    ws.disconnect()
  }
})

// ── 键盘事件 ────────────────────────────────────────────────
function handleKeydown(event: KeyboardEvent) {
  if (event.key === 'Enter' && !event.shiftKey) {
    event.preventDefault()
    void sendMessage(inputText.value)
  }
}

function canSend(): boolean {
  return ws.isConnected.value && inputText.value.trim().length > 0 && !isStreaming.value
}

// ── 清除消息 ────────────────────────────────────────────────
function clearMessages() {
  messages.value = []
  contextSent.value = false
  try {
    sessionStorage.removeItem(STORAGE_KEY)
  } catch { /* ignore */ }
}

function formatTime(ts: number): string {
  const d = new Date(ts)
  return `${String(d.getHours()).padStart(2, '0')}:${String(d.getMinutes()).padStart(2, '0')}`
}
</script>

<template>
  <div
    class="assistant-widget"
    :class="{ 'assistant-widget--dragging': isDragging }"
    :style="{ top: position.y + 'px', left: position.x + 'px' }"
    data-testid="assistant-widget"
  >
    <!-- ── 气泡按钮 ── -->
    <Transition name="bubble-fade">
      <button
        v-if="!isOpen"
        class="assistant-widget__bubble"
        :class="{ 'assistant-widget__bubble--pulse': !isOpen }"
        @mousedown="startDrag"
        @touchstart.prevent="startDrag"
        @click="toggleOpen"
        :title="'智能助手'"
        data-testid="assistant-bubble-btn"
      >
        <span class="assistant-widget__bubble-icon">
          <el-icon :size="22"><ChatDotSquare /></el-icon>
        </span>
        <Transition name="badge-pop">
          <span v-if="unreadCount > 0" class="assistant-widget__badge">
            {{ unreadCount > 99 ? '99+' : unreadCount }}
          </span>
        </Transition>
      </button>
    </Transition>

    <!-- ── 聊天面板 ── -->
    <Transition name="panel-slide">
      <div v-if="isOpen" class="assistant-widget__panel" :style="{ top: panelOffset.top + 'px', left: panelOffset.left + 'px' }" data-testid="assistant-panel">
        <!-- Header -->
        <header class="assistant-widget__header" @mousedown="startDrag" @touchstart.prevent="startDrag">
          <div class="assistant-widget__header-left">
            <span class="assistant-widget__avatar">
              <el-icon :size="16"><ChatDotSquare /></el-icon>
            </span>
            <div>
              <div class="assistant-widget__title">智能助手</div>
              <div class="assistant-widget__subtitle">
                <span class="assistant-widget__status-dot" :class="{ 'assistant-widget__status-dot--on': ws.isConnected.value }" />
                {{ ws.isConnected.value ? '在线' : '连接中…' }}
                · {{ pageCtx.title }}
              </div>
            </div>
          </div>
          <div class="assistant-widget__header-actions">
            <div class="assistant-widget__drag-grip" title="拖拽移动">
              <span class="assistant-widget__drag-grip-dot" />
              <span class="assistant-widget__drag-grip-dot" />
              <span class="assistant-widget__drag-grip-dot" />
            </div>
            <el-button
              link
              size="small"
              :icon="Close"
              @click="handleClose"
              title="关闭"
            />
          </div>
        </header>

        <!-- Messages -->
        <div
          ref="messageListRef"
          class="assistant-widget__messages"
          data-testid="assistant-messages"
        >
          <!-- Empty state -->
          <div v-if="messages.length === 0" class="assistant-widget__empty">
            <div class="assistant-widget__empty-icon">
              <el-icon :size="36"><ChatDotSquare /></el-icon>
            </div>
            <p class="assistant-widget__empty-title">你好！我是 AgentFlow 智能助手</p>
            <p class="assistant-widget__empty-hint">
              我当前在 <strong>{{ pageCtx.title }}</strong> 页面，可以帮你：
            </p>
            <div class="assistant-widget__suggestions">
              <button
                v-for="(sug, i) in pageCtx.suggestions"
                :key="i"
                class="assistant-widget__suggestion-btn"
                @click="sendSuggestion(sug)"
              >
                {{ sug }}
              </button>
            </div>
          </div>

          <!-- Message list -->
          <div v-else class="assistant-widget__list">
            <div
              v-for="msg in messages"
              :key="msg.id"
              class="assistant-widget__msg"
              :class="`assistant-widget__msg--${msg.role}`"
            >
              <!-- Assistant message -->
              <template v-if="msg.role === 'assistant'">
                <div class="assistant-widget__msg-meta">
                  <span class="assistant-widget__msg-avatar">A</span>
                  <span class="assistant-widget__msg-author">助手</span>
                  <span class="assistant-widget__msg-time">{{ formatTime(msg.timestamp) }}</span>
                </div>
                <div class="assistant-widget__bubble assistant-widget__bubble--assistant">
                  <template v-if="!msg.content && isStreaming && msg.id === streamingMsgId">
                    <span class="assistant-widget__typing">
                      <span /><span /><span />
                    </span>
                  </template>
                  <template v-else>
                    <MarkdownContent :content="msg.content" />
                  </template>
                </div>
              </template>

              <!-- User message -->
              <template v-else>
                <div class="assistant-widget__bubble assistant-widget__bubble--user">
                  {{ msg.content }}
                </div>
                <div class="assistant-widget__msg-meta assistant-widget__msg-meta--user">
                  <span class="assistant-widget__msg-time">{{ formatTime(msg.timestamp) }}</span>
                </div>
              </template>
            </div>

            <!-- Streaming indicator -->
            <div v-if="isStreaming" class="assistant-widget__streaming-hint">
              助手正在输入…
            </div>
          </div>
        </div>

        <!-- Error -->
        <div v-if="error" class="assistant-widget__error">
          <el-alert type="error" :title="error" show-icon closable @close="error = null" />
        </div>

        <!-- Context-aware suggestions (after first message) -->
        <div v-if="messages.length > 0 && !isStreaming" class="assistant-widget__quick-actions">
          <button
            v-for="(sug, i) in pageCtx.suggestions.slice(0, 2)"
            :key="i"
            class="assistant-widget__quick-btn"
            @click="sendSuggestion(sug)"
          >
            {{ sug }}
          </button>
          <button
            class="assistant-widget__quick-btn assistant-widget__quick-btn--clear"
            @click="clearMessages"
            title="清除对话"
          >
            清除
          </button>
        </div>

        <!-- Input -->
        <footer class="assistant-widget__composer">
          <div class="assistant-widget__input-row">
            <input
              v-model="inputText"
              class="assistant-widget__input"
              type="text"
              placeholder="输入问题… (Enter 发送)"
              :disabled="isStreaming"
              @keydown="handleKeydown"
              data-testid="assistant-input"
            />
            <button
              class="assistant-widget__send-btn"
              :disabled="!canSend()"
              @click="sendMessage(inputText)"
              data-testid="assistant-send"
            >
              <el-icon :size="18"><Promotion /></el-icon>
            </button>
          </div>
        </footer>
      </div>
    </Transition>
  </div>
</template>

<style scoped>
/* ==================================================================
   容器 — fixed 定位，通过 inline style 控制 top/left
   ================================================================== */
.assistant-widget {
  position: fixed;
  z-index: 9999;
  font-family: 'PingFang SC', 'Hiragino Sans GB', 'Microsoft YaHei', system-ui, -apple-system, sans-serif;
}
.assistant-widget--dragging {
  user-select: none;
  cursor: grabbing;
}

/* ==================================================================
   气泡按钮
   ================================================================== */
.assistant-widget__bubble {
  border-radius: 50%;
  border: none;
  background: linear-gradient(135deg, #409eff, #337ecc);
  color: #fff;
  cursor: pointer;
  display: flex;
  align-items: center;
  justify-content: center;
  box-shadow: 0 4px 16px rgba(64, 158, 255, 0.4);
  transition: transform 0.2s ease, box-shadow 0.2s ease;
  position: relative;
}
.assistant-widget__bubble:hover {
  box-shadow: 0 6px 24px rgba(64, 158, 255, 0.5);
}
.assistant-widget__bubble:active {
  transform: scale(0.95);
}
.assistant-widget__bubble {
  cursor: grab;
}
.assistant-widget__bubble--pulse {
  animation: bubble-pulse 3s infinite;
}
@keyframes bubble-pulse {
  0%, 100% { box-shadow: 0 4px 16px rgba(64, 158, 255, 0.4); }
  50% { box-shadow: 0 4px 24px rgba(64, 158, 255, 0.6); }
}
.assistant-widget__bubble-icon {
  display: flex;
  align-items: center;
  justify-content: center;
}

.assistant-widget__badge {
  position: absolute;
  top: -4px;
  right: -4px;
  background: #f56c6c;
  color: #fff;
  font-size: 10px;
  font-weight: 600;
  min-width: 20px;
  height: 20px;
  border-radius: 10px;
  display: flex;
  align-items: center;
  justify-content: center;
  padding: 0 5px;
  box-shadow: 0 2px 6px rgba(245, 108, 108, 0.4);
}

/* ==================================================================
   聊天面板
   ================================================================== */
.assistant-widget__panel {
  position: absolute;
  width: 380px;
  max-height: min(600px, calc(100vh - 48px));
  height: 560px;
  background: #ffffff;
  border-radius: 16px;
  box-shadow: 0 8px 40px rgba(0, 0, 0, 0.15);
  display: flex;
  flex-direction: column;
  overflow: hidden;
  border: 1px solid rgba(0, 0, 0, 0.06);
}

/* ── Header ── */
.assistant-widget__header {
  display: flex;
  align-items: center;
  justify-content: space-between;
  padding: 14px 16px;
  background: linear-gradient(135deg, #409eff, #337ecc);
  color: #fff;
  flex-shrink: 0;
  cursor: grab;
}
.assistant-widget__header:active {
  cursor: grabbing;
}
.assistant-widget__header-left {
  display: flex;
  align-items: center;
  gap: 10px;
  min-width: 0;
}
.assistant-widget__avatar {
  width: 32px;
  height: 32px;
  border-radius: 50%;
  background: rgba(255, 255, 255, 0.2);
  display: flex;
  align-items: center;
  justify-content: center;
  flex-shrink: 0;
}
.assistant-widget__title {
  font-size: 13px;
  font-weight: 600;
  line-height: 1.3;
}
.assistant-widget__subtitle {
  font-size: 10px;
  opacity: 0.85;
  display: flex;
  align-items: center;
  gap: 4px;
  margin-top: 2px;
  white-space: nowrap;
  overflow: hidden;
  text-overflow: ellipsis;
}
.assistant-widget__status-dot {
  width: 6px;
  height: 6px;
  border-radius: 50%;
  background: #e6a23c;
  flex-shrink: 0;
}
.assistant-widget__status-dot--on {
  background: #67c23a;
}
.assistant-widget__header-actions {
  display: flex;
  gap: 4px;
  flex-shrink: 0;
  align-items: center;
}

/* ── Drag grip indicator ── */
.assistant-widget__drag-grip {
  display: flex;
  flex-direction: column;
  gap: 3px;
  padding: 4px 6px;
  cursor: grab;
  opacity: 0.5;
  transition: opacity 0.2s ease;
  border-radius: 4px;
}
.assistant-widget__drag-grip:hover {
  opacity: 1;
  background: rgba(255, 255, 255, 0.12);
}
.assistant-widget__drag-grip:active {
  cursor: grabbing;
}
.assistant-widget__drag-grip-dot {
  display: block;
  width: 4px;
  height: 4px;
  border-radius: 50%;
  background: rgba(255, 255, 255, 0.8);
}

/* ── Messages 区域 ── */
.assistant-widget__messages {
  flex: 1 1 auto;
  min-height: 0;
  overflow-y: auto;
  overflow-x: hidden;
  padding: 16px 16px 8px;
  background: #f5f7fa;
}

/* 空状态 */
.assistant-widget__empty {
  display: flex;
  flex-direction: column;
  align-items: center;
  justify-content: center;
  height: 100%;
  text-align: center;
  padding: 8px;
}
.assistant-widget__empty-icon {
  color: #409eff;
  opacity: 0.5;
  margin-bottom: 12px;
}
.assistant-widget__empty-title {
  font-size: 13px;
  font-weight: 600;
  color: #303133;
  margin: 0 0 8px;
}
.assistant-widget__empty-hint {
  font-size: 11px;
  color: #606266;
  margin: 0 0 14px;
  line-height: 1.6;
}
.assistant-widget__suggestions {
  display: flex;
  flex-direction: column;
  gap: 8px;
  width: 100%;
  max-width: 320px;
}
.assistant-widget__suggestion-btn {
  padding: 8px 12px;
  border-radius: 8px;
  border: 1px solid #d9ecff;
  background: #fff;
  color: #409eff;
  font-size: 11px;
  cursor: pointer;
  transition: all 0.15s ease;
  text-align: left;
  font-family: inherit;
  line-height: 1.4;
  box-shadow: 0 1px 3px rgba(0,0,0,0.04);
}
.assistant-widget__suggestion-btn:hover {
  background: #409eff;
  color: #fff;
  border-color: #409eff;
  box-shadow: 0 2px 8px rgba(64,158,255,0.25);
}

/* ── 消息列表 ── */
.assistant-widget__list {
  display: flex;
  flex-direction: column;
  gap: 16px;
}
.assistant-widget__msg {
  display: flex;
  flex-direction: column;
  max-width: 88%;
}
.assistant-widget__msg--user {
  align-self: flex-end;
  align-items: flex-end;
}
.assistant-widget__msg--assistant {
  align-self: flex-start;
  align-items: flex-start;
}

.assistant-widget__msg-meta {
  display: flex;
  align-items: center;
  gap: 6px;
  margin-bottom: 4px;
  font-size: 10px;
  color: #909399;
  padding: 0 4px;
}
.assistant-widget__msg-meta--user {
  margin-top: 3px;
  margin-bottom: 0;
}
.assistant-widget__msg-avatar {
  width: 20px;
  height: 20px;
  border-radius: 50%;
  background: linear-gradient(135deg, #409eff, #67c23a);
  color: #fff;
  font-size: 9px;
  font-weight: 600;
  display: inline-flex;
  align-items: center;
  justify-content: center;
  flex-shrink: 0;
}
.assistant-widget__msg-author {
  font-weight: 600;
  color: #303133;
  font-size: 11px;
}
.assistant-widget__msg-time {
  font-variant-numeric: tabular-nums;
  font-size: 10px;
}

/* ── 气泡 ── */
.assistant-widget__bubble {
  padding: 10px 14px;
  border-radius: 12px;
  overflow-wrap: break-word;
  word-wrap: break-word;
  word-break: break-word;
  font-size: 12px;
  line-height: 1.6;
}
.assistant-widget__bubble--assistant {
  background: #ffffff;
  border: 1px solid #e8eaed;
  color: #1f2937;
  box-shadow: 0 1px 4px rgba(0,0,0,0.04);
  border-bottom-left-radius: 4px;
}
.assistant-widget__bubble--assistant:deep(.markdown-content) {
  font-size: 12px;
  line-height: 1.6;
  color: #1f2937;
}
/* 让 MarkdownContent 渲染的段落/代码等在气泡内正确显示 */
.assistant-widget__bubble--assistant:deep(.markdown-content p) {
  margin: 0.3em 0;
  font-size: 12px;
  line-height: 1.6;
}
.assistant-widget__bubble--assistant:deep(.markdown-content p:first-child) {
  margin-top: 0;
}
.assistant-widget__bubble--assistant:deep(.markdown-content p:last-child) {
  margin-bottom: 0;
}
.assistant-widget__bubble--assistant:deep(.markdown-content pre) {
  margin: 0.4em 0;
  border-radius: 8px;
}
.assistant-widget__bubble--assistant:deep(.markdown-content code:not(.hljs code)) {
  font-size: 11px;
  padding: 1px 5px;
}
.assistant-widget__bubble--assistant:deep(.markdown-content ul),
.assistant-widget__bubble--assistant:deep(.markdown-content ol) {
  margin: 0.3em 0;
  padding-left: 1.3em;
}
.assistant-widget__bubble--assistant:deep(.markdown-content li) {
  margin: 0.1em 0;
}
.assistant-widget__bubble--assistant:deep(.markdown-content h1),
.assistant-widget__bubble--assistant:deep(.markdown-content h2),
.assistant-widget__bubble--assistant:deep(.markdown-content h3),
.assistant-widget__bubble--assistant:deep(.markdown-content h4) {
  margin: 0.5em 0 0.2em;
}
.assistant-widget__bubble--assistant:deep(.markdown-content table) {
  font-size: 11px;
}
.assistant-widget__bubble--assistant:deep(.markdown-content th),
.assistant-widget__bubble--assistant:deep(.markdown-content td) {
  padding: 4px 8px;
}

.assistant-widget__bubble--user {
  background: linear-gradient(135deg, #409eff, #3b8ce8);
  border: none;
  color: #ffffff;
  border-bottom-right-radius: 4px;
  box-shadow: 0 2px 8px rgba(64,158,255,0.18);
}

/* ── Typing dots ── */
.assistant-widget__typing {
  display: inline-flex;
  align-items: center;
  gap: 4px;
  height: 20px;
  padding: 2px 0;
}
.assistant-widget__typing > span {
  width: 6px;
  height: 6px;
  border-radius: 50%;
  background: #909399;
  display: inline-block;
  animation: typing-bounce 1.2s infinite ease-in-out;
}
.assistant-widget__typing > span:nth-child(2) {
  animation-delay: 0.15s;
}
.assistant-widget__typing > span:nth-child(3) {
  animation-delay: 0.3s;
}
@keyframes typing-bounce {
  0%, 60%, 100% { transform: translateY(0); opacity: 0.4; }
  30% { transform: translateY(-5px); opacity: 1; }
}

.assistant-widget__streaming-hint {
  font-size: 10px;
  color: #909399;
  text-align: center;
  padding: 2px 0 0;
}

/* ── Error ── */
.assistant-widget__error {
  padding: 0 12px 8px;
}

/* ── Quick actions (对话中底部的快捷按钮) ── */
.assistant-widget__quick-actions {
  display: flex;
  gap: 6px;
  padding: 8px 12px;
  border-top: 1px solid #ebeef5;
  background: #fafafa;
  flex-wrap: wrap;
  flex-shrink: 0;
}
.assistant-widget__quick-btn {
  padding: 4px 10px;
  border-radius: 12px;
  border: 1px solid #d9ecff;
  background: transparent;
  color: #409eff;
  font-size: 11px;
  cursor: pointer;
  transition: all 0.15s ease;
  white-space: nowrap;
  font-family: inherit;
  line-height: 1.3;
}
.assistant-widget__quick-btn:hover {
  background: #ecf5ff;
  border-color: #409eff;
}
.assistant-widget__quick-btn--clear {
  color: #909399;
  border-color: #e4e7ed;
  margin-left: auto;
}
.assistant-widget__quick-btn--clear:hover {
  color: #f56c6c;
  border-color: #f56c6c;
  background: #fef0f0;
}

/* ── 输入区 (Composer) ── */
.assistant-widget__composer {
  padding: 8px 14px 14px;
  border-top: 1px solid #ebeef5;
  background: #ffffff;
  flex-shrink: 0;
}
.assistant-widget__input-row {
  display: flex;
  align-items: center;
  gap: 8px;
  background: #f0f2f5;
  border-radius: 22px;
  padding: 4px 4px 4px 16px;
  transition: box-shadow 0.2s ease, background 0.2s ease;
  border: 1px solid transparent;
}
.assistant-widget__input-row:focus-within {
  box-shadow: 0 0 0 2px rgba(64, 158, 255, 0.15);
  background: #ffffff;
  border-color: #409eff;
}
.assistant-widget__input {
  flex: 1;
  border: none;
  outline: none;
  background: transparent;
  font-size: 12px;
  color: #1f2937;
  font-family: inherit;
  line-height: 1.5;
  padding: 7px 0;
  min-width: 0;
}
.assistant-widget__input::placeholder {
  color: #c0c4cc;
}
.assistant-widget__input:disabled {
  opacity: 0.6;
  cursor: not-allowed;
}
.assistant-widget__send-btn {
  width: 34px;
  height: 34px;
  border-radius: 50%;
  border: none;
  background: #409eff;
  color: #fff;
  cursor: pointer;
  display: flex;
  align-items: center;
  justify-content: center;
  transition: background 0.15s ease, transform 0.15s ease;
  flex-shrink: 0;
}
.assistant-widget__send-btn:hover:not(:disabled) {
  background: #337ecc;
  transform: scale(1.06);
}
.assistant-widget__send-btn:active:not(:disabled) {
  transform: scale(0.94);
}
.assistant-widget__send-btn:disabled {
  background: #c0c4cc;
  cursor: not-allowed;
}

/* ==================================================================
   过渡动画
   ================================================================== */
.bubble-fade-enter-active,
.bubble-fade-leave-active {
  transition: opacity 0.2s ease, transform 0.2s ease;
}
.bubble-fade-enter-from,
.bubble-fade-leave-to {
  opacity: 0;
  transform: scale(0.8);
}

.panel-slide-enter-active {
  transition: all 0.25s cubic-bezier(0.4, 0, 0.2, 1);
}
.panel-slide-leave-active {
  transition: all 0.18s cubic-bezier(0.4, 0, 0.2, 1);
}
.panel-slide-enter-from {
  opacity: 0;
  transform: translateY(20px) scale(0.95);
  transform-origin: bottom right;
}
.panel-slide-leave-to {
  opacity: 0;
  transform: translateY(10px) scale(0.97);
  transform-origin: bottom right;
}

.badge-pop-enter-active {
  transition: all 0.2s cubic-bezier(0.34, 1.56, 0.64, 1);
}
.badge-pop-leave-active {
  transition: all 0.15s ease;
}
.badge-pop-enter-from,
.badge-pop-leave-to {
  opacity: 0;
  transform: scale(0);
}
</style>
