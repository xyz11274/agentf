<script setup lang="ts">
/**
 * ChatPage.vue — 集成聊天页面
 *
 * 左侧：AgentList（可折叠）+ SessionList
 * 右侧：ChatView
 * 流程：选择 Agent → 选择/新建 Session → WebSocket 连接 → 发送消息
 *
 * 核心设计：
 * - user_id 内置固定值（"web-user"），不对外暴露 session_id
 * - session 由 daemon 内部按 user_id:client_id 自动管理
 * - 前端通过 client_id 标识"会话"，必须先选择/创建会话才能聊天
 */
import { computed, onBeforeUnmount, onMounted, ref, watch } from 'vue'
import { useRouter } from 'vue-router'
import { ElMessage } from 'element-plus'
import { useAgentStore } from '@/stores/agentStore'
import { useSessionStore } from '@/stores/sessionStore'
import { useChatStore } from '@/stores/chatStore'
import { useDaemonStore } from '@/stores/daemonStore'
import { useChatWebSocket } from '@/composables/useChatWebSocket'
import AgentList from '@/components/AgentList.vue'
import SessionList from '@/components/SessionList.vue'
import ChatView from '@/components/ChatView.vue'

/** 内置 user_id，所有前端聊天使用此值 */
const BUILTIN_USER_ID = 'web-user'

const agentStore = useAgentStore()
const sessionStore = useSessionStore()
const chatStore = useChatStore()
const daemonStore = useDaemonStore()
const ws = useChatWebSocket()
const router = useRouter()

const currentSessionId = ref<string | null>(null)
const currentClientId = ref<string | null>(null)

/** 是否已选择 session（控制聊天输入框是否可用） */
const hasActiveSession = computed(() => currentClientId.value !== null)

// ── Agent selection ───────────────────────────────────────

const selectedAgentId = computed(() => agentStore.currentAgentId)

function selectAgent(agentId: string) {
  agentStore.setCurrentAgent(agentId)
  currentSessionId.value = null
  currentClientId.value = null
  if (ws.isConnected.value) {
    ws.disconnect()
  }
}

// ── Session selection ─────────────────────────────────────

/**
 * 选择已有会话。参数是 daemon 内部的 session_id，
 * 我们从 sessionStore 中查找对应的 client_id。
 */
function selectSession(sessionId: string) {
  const session = sessionStore.sessions.find((s) => s.session_id === sessionId)
  if (session) {
    currentSessionId.value = session.session_id
    currentClientId.value = session.client_id
    chatStore.currentUserId = session.user_id
    chatStore.currentClientId = session.client_id
    // 切换会话时清空当前消息列表
    chatStore.clearMessages()
    connectChat()
  }
}

/**
 * 新建会话：生成一个新的 client_id（UUID），
 * 用 user_id + 新 client_id 连接 WebSocket，
 * 并在会话列表中立即显示该条目。
 */
function createSession() {
  const newClientId = 'client_' + generateShortId()
  if (selectedAgentId.value) {
    sessionStore.addPendingSession(selectedAgentId.value, BUILTIN_USER_ID, newClientId)
  }
  currentSessionId.value = null
  currentClientId.value = newClientId
  connectChat()
}

function generateShortId(): string {
  return Date.now().toString(36) + Math.random().toString(36).slice(2, 8)
}

// ── WebSocket connection ──────────────────────────────────

async function connectChat() {
  if (!selectedAgentId.value || !currentClientId.value) return
  try {
    await ws.connect(selectedAgentId.value, BUILTIN_USER_ID, currentClientId.value)
  } catch (e) {
    ElMessage.error(`连接聊天失败: ${e instanceof Error ? e.message : e}`)
  }
}

async function sendMessage(content: string, images?: string[]) {
  if (!ws.isConnected.value) {
    ElMessage.error('发送失败: WebSocket 未连接')
    return
  }
  try {
    ws.send(content, images)
  } catch (e) {
    ElMessage.error(`发送失败: ${e instanceof Error ? e.message : e}`)
  }
}

// ── Auto-connect on mount (if navigated from AgentList) ───

onMounted(() => {
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
  if (ws.isConnected.value) {
    ws.disconnect()
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
        :user-id="BUILTIN_USER_ID"
        :current-session-id="currentSessionId"
        @session-selected="selectSession"
        @new-session="createSession"
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
        v-else-if="ws.error.value"
        class="chat-page__banner chat-page__banner--error"
        data-testid="chat-ws-error"
      >
        WebSocket 错误: {{ ws.error.value }}
      </div>
      <div
        v-else-if="ws.isConnected.value"
        class="chat-page__banner chat-page__banner--success"
      >
        聊天已连接
        <span v-if="selectedAgentId" class="chat-page__banner-agent">
          · {{ selectedAgentId }}
        </span>
        <span v-if="currentClientId" class="chat-page__banner-session">
          · 会话: {{ currentClientId.slice(0, 8) }}…
        </span>
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
          <el-button type="primary" @click="createSession">
            新建会话
          </el-button>
        </el-empty>
      </div>

      <!-- Chat area -->
      <ChatView
        v-else
        :agent-id="selectedAgentId"
        :session-id="currentSessionId ?? undefined"
        :connected="ws.isConnected.value"
        :streaming="ws.isStreaming.value"
        @send="sendMessage"
        @stop="ws.stop()"
      />
    </main>
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
.chat-page__empty {
  flex: 1;
  display: flex;
  align-items: center;
  justify-content: center;
}
</style>
