<script setup lang="ts">
/**
 * ChatPage.vue — 集成聊天页面
 *
 * 左侧：AgentList（可折叠）+ SessionList
 * 右侧：ChatView
 * 流程：选择 Agent → 选择/新建 Session → 加载历史消息 → SSE 连接 → 发送消息
 *
 * 核心设计（Phase 1）：
 * - 会话列表从 Bridge 持久化 API（GET /api/sessions/persistent）读取
 * - 选择会话后加载历史消息（GET /api/sessions/persistent/{id}/messages）
 * - user_id 内置固定值（"web-user"）
 * - 前端控制 SSE 连接生命周期
 */
import { computed, onBeforeUnmount, onMounted, ref, watch } from 'vue'
import { ElMessage } from 'element-plus'
import { useAgentStore } from '@/stores/agentStore'
import { useSessionStore } from '@/stores/sessionStore'
import { useChatStore } from '@/stores/chatStore'
import { useDaemonStore } from '@/stores/daemonStore'
import { useChatSse } from '@/composables/useChatSse'
import AgentList from '@/components/AgentList.vue'
import SessionList from '@/components/SessionList.vue'
import ChatView from '@/components/ChatView.vue'
import NewSessionDialog from '@/components/NewSessionDialog.vue'

/** 内置 user_id，所有前端聊天使用此值 */
const BUILTIN_USER_ID = 'web-user'

const agentStore = useAgentStore()
const sessionStore = useSessionStore()
const chatStore = useChatStore()
const daemonStore = useDaemonStore()
const sse = useChatSse()

const currentSessionId = ref<string | null>(null)
const currentClientId = ref<string | null>(null)

/** 新建会话对话框可见性 */
const showNewSessionDialog = ref(false)

/** 是否已选择 session（控制聊天输入框是否可用） */
const hasActiveSession = computed(() => currentClientId.value !== null)

// ── Agent selection ───────────────────────────────────────

const selectedAgentId = computed(() => agentStore.currentAgentId)

function selectAgent(agentId: string) {
  agentStore.setCurrentAgent(agentId)
  currentSessionId.value = null
  currentClientId.value = null
  if (sse.isConnected.value) {
    sse.disconnect()
  }
}

// ── Session selection ─────────────────────────────────────

/**
 * 选择已有持久化会话。参数是 Bridge session ID（UUID hex）。
 * 加载历史消息后 WebSocket 连接。
 */
async function selectSession(sessionId: string) {
  // 在 sessions 中查找
  const session = sessionStore.sessions.find((s) => s.id === sessionId)
  if (!session) return

  currentSessionId.value = session.id
  currentClientId.value = session.client_id
  chatStore.currentUserId = session.user_id
  chatStore.currentClientId = session.client_id
  chatStore.currentSessionId = session.id

  // 切换会话时清空当前消息列表
  chatStore.clearMessages()

  // 连接聊天（chatStore.connect 在切换 agent 时会清 messages）
  connectChat()

  // 加载历史消息（在 connect 之后，避免被 connect 的清空逻辑冲掉）
  await chatStore.loadHistoryMessages(session.id).catch(() => {
    // loadHistoryMessages 内部已静默
  })

  // 加载历史消息的 trace（从 Bridge 持久化 trace API）
  await chatStore.loadHistoryTraces(session.id).catch(() => {
    // loadHistoryTraces 内部已静默
  })
}

/**
 * 打开新建会话对话框，让用户输入 client_id 和 user_id。
 */
function openNewSessionDialog() {
  showNewSessionDialog.value = true
}

/**
 * 根据用户输入的 client_id 和 user_id 创建会话。
 */
function createSessionWithParams(clientId: string, userId: string) {
  showNewSessionDialog.value = false
  if (selectedAgentId.value) {
    sessionStore.addPendingSession(userId, clientId)
  }
  currentSessionId.value = null
  currentClientId.value = clientId
  chatStore.currentUserId = userId
  chatStore.currentClientId = clientId
  chatStore.clearMessages()
  connectChat()
}

// ── WebSocket connection ──────────────────────────────────

async function connectChat() {
  if (!selectedAgentId.value || !currentClientId.value) return
  try {
    const uid = chatStore.currentUserId || BUILTIN_USER_ID
    await sse.connect(selectedAgentId.value, uid, currentClientId.value)
  } catch (e) {
    ElMessage.error(`连接聊天失败: ${e instanceof Error ? e.message : e}`)
  }
}

async function sendMessage(content: string, images?: string[]) {
  if (!sse.isConnected.value) {
    ElMessage.error('发送失败: 未连接')
    return
  }
  try {
    sse.send(content, images)
  } catch (e) {
    ElMessage.error(`发送失败: ${e instanceof Error ? e.message : e}`)
  }
}

// ── Auto-restore from persistence + connect on mount ─────

onMounted(() => {
  // 从 Bridge 持久化 API 加载会话列表
  if (selectedAgentId.value) {
    sessionStore.loadSessions().catch(() => {
      // loadSessions 内部已记录 error
    })
  }
  if (selectedAgentId.value && currentClientId.value) {
    connectChat()
  }
})

// ── Watch agent changes ───────────────────────────────────

watch(selectedAgentId, (newId, oldId) => {
  if (newId !== oldId) {
    currentSessionId.value = null
    currentClientId.value = null
    chatStore.clearMessages()
    if (ws.isConnected.value) {
      ws.disconnect()
    }
  }
})

// ── Cleanup ───────────────────────────────────────────────

onBeforeUnmount(() => {
  if (sse.isConnected.value) {
    sse.disconnect()
  }
})
</script>

<template>
  <div class="chat-page" data-testid="chat-page">
    <!-- Left sidebar -->
    <aside class="chat-page__sidebar">
      <AgentList @agent-click="selectAgent" />

      <el-divider style="margin: 8px 0" />

      <div v-if="!selectedAgentId" class="chat-page__session-placeholder">
        请先选择一个 Agent 以查看会话
      </div>
      <SessionList
        v-else
        :key="selectedAgentId"
        :agent-id="selectedAgentId"
        :user-id="''"
        :current-session-id="currentSessionId"
        @session-selected="selectSession"
        @new-session="openNewSessionDialog"
        @session-destroyed="() => { currentSessionId = null; currentClientId = null }"
      />
    </aside>

    <!-- Right main area -->
    <main class="chat-page__main">
      <!-- Connection status banner -->
      <div
        v-if="!daemonStore.isConnected"
        class="chat-page__banner chat-page__banner--error"
        data-testid="daemon-status"
      >
        Daemon 未连接 — 聊天不可用
      </div>
      <div
        v-else-if="sse.error.value"
        class="chat-page__banner chat-page__banner--error"
        data-testid="chat-sse-error"
      >
        SSE 错误: {{ sse.error.value }}
      </div>
      <div
        v-else-if="sse.isConnected.value"
        class="chat-page__banner chat-page__banner--success"
      >
        聊天已连接 ({{ sse.streamingEnabled.value ? '流式' : '同步' }})
        <span v-if="selectedAgentId" class="chat-page__banner-agent">
          · {{ selectedAgentId }}
        </span>
        <span v-if="currentClientId" class="chat-page__banner-session">
          · 会话: {{ currentClientId.slice(0, 8) }}…
        </span>
        <el-switch
          v-model="sse.streamingEnabled.value"
          class="chat-page__streaming-toggle"
          size="small"
          active-text="流式"
          inactive-text="同步"
          inline-prompt
        />
      </div>

      <!-- Empty state: no agent selected -->
      <div
        v-if="!selectedAgentId"
        class="chat-page__empty"
      >
        <el-empty description="从左侧面板选择一个 Agent 开始聊天" :image-size="100" />
      </div>

      <!-- Empty state: agent selected but no session -->
      <div
        v-else-if="!hasActiveSession"
        class="chat-page__empty"
      >
        <el-empty description="请先选择或新建一个会话" :image-size="80">
          <el-button type="primary" @click="openNewSessionDialog">
            新建会话
          </el-button>
        </el-empty>
      </div>

      <!-- Chat area -->
      <ChatView
        v-else
        :agent-id="selectedAgentId"
        :session-id="currentSessionId ?? undefined"
        :connected="sse.isConnected.value"
        :streaming="sse.isStreaming.value"
        @send="sendMessage"
        @stop="sse.stop()"
      />
    </main>

    <!-- 新建会话对话框 -->
    <NewSessionDialog
      :visible="showNewSessionDialog"
      :default-user-id="BUILTIN_USER_ID"
      @create="createSessionWithParams"
      @close="showNewSessionDialog = false"
    />
  </div>
</template>

<style scoped>
.chat-page {
  display: flex;
  height: 100%;
}
.chat-page__sidebar {
  width: 300px;
  min-width: 300px;
  border-right: 1px solid #e4e7ed;
  padding: 12px;
  overflow-y: auto;
  background: #fafafa;
  display: flex;
  flex-direction: column;
  gap: 0;
}
.chat-page__session-placeholder {
  font-size: 12px;
  color: #909399;
  padding: 8px 0;
  text-align: center;
}
.chat-page__main {
  flex: 1;
  display: flex;
  flex-direction: column;
  overflow: hidden;
  padding: 12px;
  background: #f5f7fa;
}
.chat-page__banner {
  padding: 6px 16px;
  font-size: 13px;
  text-align: center;
  border-radius: 8px;
  margin-bottom: 8px;
}
.chat-page__banner--error {
  background: #fef0f0;
  color: #f56c6c;
}
.chat-page__banner--success {
  background: #f0f9eb;
  color: #67c23a;
}
.chat-page__banner-agent,
.chat-page__banner-session {
  font-weight: 600;
}
.chat-page__streaming-toggle {
  margin-left: 12px;
}
.chat-page__empty {
  flex: 1;
  display: flex;
  align-items: center;
  justify-content: center;
}
</style>
