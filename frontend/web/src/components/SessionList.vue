<script setup lang="ts">
/**
 * Session list and management component.
 *
 * Renders the active sessions for the currently-selected agent and exposes
 * destroy / clear-dialogue actions. Auto-refreshes the list every 30s while
 * mounted.
 *
 * Sessions are identified by ``client_id`` (for display) and internally by
 * ``session_id`` (for API calls). The ``user_id`` is built-in.
 */
import { useSessionStore } from '@/stores/sessionStore'
import type { SessionInfo } from '@/types/agentflow'
import {
  ChatLineSquare,
  Clock,
  Delete,
  Loading as ElIconLoading,
  Plus,
  User,
} from '@element-plus/icons-vue'
import { ElMessage } from 'element-plus'
import { computed, onBeforeUnmount, onMounted, ref, watch } from 'vue'

interface Props {
  /** ID of the agent whose sessions should be displayed. */
  agentId: string | null
  /** Built-in user_id to filter sessions for this user. */
  userId: string
  /** ID of the currently-bound session (drives highlight + emit filtering). */
  currentSessionId?: string | null
}

interface Emits {
  /** Fired when the user clicks a session row. Payload: the internal session_id. */
  (e: 'session-selected', sessionId: string): void
  /** Fired when the user clicks "New Session". The parent creates one. */
  (e: 'new-session'): void
  /** Fired after a session is destroyed. */
  (e: 'session-destroyed', sessionId: string): void
}

const props = withDefaults(defineProps<Props>(), {
  currentSessionId: null,
})
const emit = defineEmits<Emits>()

const sessionStore = useSessionStore()

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

/** Auto-refresh interval in milliseconds. */
const REFRESH_INTERVAL_MS = 30_000

/** Client ID truncation length for display. */
const CLIENT_ID_DISPLAY_LEN = 12

/** Visual hint for status colors. */
const STATUS_TAG_TYPE: Record<string, 'success' | 'warning' | 'info' | 'danger' | 'primary'> = {
  active: 'success',
  idle: 'info',
  paused: 'warning',
  error: 'danger',
}

// ---------------------------------------------------------------------------
// Reactive state
// ---------------------------------------------------------------------------

/** Local ticker used to re-format "age" strings every second. */
const tick = ref(0)

/** Whether the auto-refresh interval is currently armed. */
const refreshArmed = ref(false)

/** Per-session loading flags for destructive actions; key is session_id. */
const busyIds = ref<Set<string>>(new Set())

let refreshTimer: ReturnType<typeof setInterval> | null = null

// ---------------------------------------------------------------------------
// Computed
// ---------------------------------------------------------------------------

const sessions = computed<SessionInfo[]>(() => sessionStore.sessions)

const isLoading = computed(() => sessionStore.isLoading)

const errorMessage = computed(() => sessionStore.error)

const hasSessions = computed(() => sessions.value.length > 0)

const canRender = computed(() => props.agentId !== null && props.agentId.length > 0)

/** Sort by age_ms descending — most recently active at the top. */
const sortedSessions = computed(() =>
  [...sessions.value].sort((a, b) => b.age_ms - a.age_ms),
)

/** Tick-driven reactive age strings; recomputed each second. */
const aliveLabel = (ageMs: number): string => {
  void tick.value
  return formatAge(ageMs)
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

onMounted(() => {
  if (canRender.value) {
    void refresh()
    startAutoRefresh()
  }
})

onBeforeUnmount(() => {
  stopAutoRefresh()
})

// Re-arm refresh when the bound agent changes.
watch(
  () => props.agentId,
  (next, prev) => {
    if (next === prev) return
    stopAutoRefresh()
    if (canRender.value) {
      void refresh()
      startAutoRefresh()
    }
  },
)

// ---------------------------------------------------------------------------
// Actions
// ---------------------------------------------------------------------------

async function refresh(): Promise<void> {
  if (!canRender.value) return
  try {
    await sessionStore.loadSessions(props.agentId as string, props.userId)
  } catch {
    // Store records the error; we surface it via errorMessage below.
  }
}

function startAutoRefresh(): void {
  if (refreshArmed.value) return
  refreshArmed.value = true
  refreshTimer = setInterval(() => {
    void refresh()
  }, REFRESH_INTERVAL_MS)
}

function stopAutoRefresh(): void {
  refreshArmed.value = false
  if (refreshTimer !== null) {
    clearInterval(refreshTimer)
    refreshTimer = null
  }
}

/** Format age_ms into a compact human-readable string. */
function formatAge(ageMs: number): string {
  if (ageMs < 0 || !Number.isFinite(ageMs)) return '—'
  const totalSeconds = Math.floor(ageMs / 1000)
  const hours = Math.floor(totalSeconds / 3600)
  const minutes = Math.floor((totalSeconds % 3600) / 60)
  const seconds = totalSeconds % 60
  if (hours > 0) return `${hours}h ${minutes}m`
  if (minutes > 0) return `${minutes}m ${seconds}s`
  return `${seconds}s`
}

function truncateClientId(id: string): string {
  if (id.length <= CLIENT_ID_DISPLAY_LEN + 3) return id
  return `${id.slice(0, CLIENT_ID_DISPLAY_LEN)}…`
}

function statusType(status: string): 'success' | 'warning' | 'info' | 'danger' | 'primary' {
  return STATUS_TAG_TYPE[status.toLowerCase()] ?? 'primary'
}

function isBusy(sessionId: string): boolean {
  return busyIds.value.has(sessionId)
}

function withBusy(sessionId: string, fn: () => Promise<void>): Promise<void> {
  busyIds.value.add(sessionId)
  busyIds.value = new Set(busyIds.value)
  return fn().finally(() => {
    busyIds.value.delete(sessionId)
    busyIds.value = new Set(busyIds.value)
  })
}

function onRowClick(session: SessionInfo): void {
  emit('session-selected', session.session_id)
}

function onNewSession(): void {
  emit('new-session')
}

async function onDestroy(session: SessionInfo): Promise<void> {
  if (!canRender.value) return
  const sessionId = session.session_id
  try {
    await withBusy(sessionId, () =>
      sessionStore.destroySession(sessionId, props.agentId as string),
    )
    ElMessage.success(`会话 ${truncateClientId(session.client_id)} 已销毁`)
    if (props.currentSessionId === sessionId) {
      emit('session-destroyed', sessionId)
    }
  } catch (e) {
    ElMessage.error(`销毁会话失败: ${errorMessage.value ?? 'unknown error'}`)
  }
}

async function onClearDialogue(session: SessionInfo): Promise<void> {
  if (!canRender.value) return
  const sessionId = session.session_id
  const dialogueId = session.session_id
  try {
    await withBusy(sessionId, () =>
      sessionStore.clearDialogue(sessionId, dialogueId, props.agentId as string),
    )
    ElMessage.success(`对话已清除 ${truncateClientId(session.client_id)}`)
  } catch (e) {
    ElMessage.error(`清除对话失败: ${errorMessage.value ?? 'unknown error'}`)
  }
}
</script>

<template>
  <section class="session-list" data-testid="session-list">
    <header class="session-list__header">
      <div class="session-list__title-row">
        <h3 class="session-list__title">
          <el-icon class="session-list__title-icon"><ChatLineSquare /></el-icon>
          会话列表
        </h3>
        <el-button
          class="session-list__new-btn"
          type="primary"
          size="small"
          :icon="Plus"
          data-testid="new-session"
          :disabled="!canRender"
          @click="onNewSession"
        >
          新建会话
        </el-button>
      </div>

      <div v-if="canRender" class="session-list__meta">
        <el-tag
          :type="isLoading ? 'warning' : 'success'"
          size="small"
          effect="plain"
          round
          data-testid="session-list-status"
        >
          <el-icon v-if="isLoading" class="session-list__loading"><ElIconLoading /></el-icon>
          <span>{{ isLoading ? '加载中…' : `${sessions.length} 个活跃` }}</span>
        </el-tag>
        <el-tooltip
          content="每 30 秒自动刷新"
          placement="top"
          :show-after="200"
        >
          <span class="session-list__refresh-hint">
            <el-icon><Clock /></el-icon>
            <span>30秒</span>
          </span>
        </el-tooltip>
      </div>
    </header>

    <el-alert
      v-if="!canRender"
      type="info"
      :closable="false"
      show-icon
      title="请先选择一个 Agent"
      description="先在包列表中选一个 Agent 来查看其活跃会话。"
    />

    <el-alert
      v-else-if="errorMessage"
      type="error"
      :closable="false"
      show-icon
      title="加载会话失败"
      :description="errorMessage"
    />

    <el-empty
      v-else-if="!hasSessions && !isLoading"
      class="session-list__empty"
      description="暂无活跃会话"
      :image-size="80"
      data-testid="session-list-empty"
    >
      <el-button type="primary" :icon="Plus" @click="onNewSession">
        创建第一个会话
      </el-button>
    </el-empty>

    <ul v-else class="session-list__items" role="listbox" aria-label="活跃会话">
      <li
        v-for="session in sortedSessions"
        :key="session.session_id"
        class="session-item"
        :class="{ 'session-item--selected': session.session_id === props.currentSessionId }"
        :data-testid="`session-${session.session_id}`"
        role="option"
        :aria-selected="session.session_id === props.currentSessionId"
        @click="onRowClick(session)"
        @keydown.enter="onRowClick(session)"
        @keydown.space.prevent="onRowClick(session)"
        tabindex="0"
      >
        <div class="session-item__head">
          <el-tooltip
            :content="session.client_id"
            placement="top"
            :show-after="300"
            :disabled="session.client_id.length <= CLIENT_ID_DISPLAY_LEN + 3"
          >
            <span class="session-item__id" data-testid="session-item-id">
              {{ truncateClientId(session.client_id) }}
            </span>
          </el-tooltip>
          <el-tag
            :type="statusType(session.status)"
            size="small"
            effect="light"
            round
            class="session-item__status"
          >
            {{ session.status }}
          </el-tag>
        </div>

        <dl class="session-item__meta">
          <div class="session-item__row">
            <el-icon class="session-item__icon"><User /></el-icon>
            <dt class="session-item__label">用户</dt>
            <dd class="session-item__value session-item__value--mono">
              {{ session.user_id }}
            </dd>
          </div>
          <div class="session-item__row">
            <el-icon class="session-item__icon"><ChatLineSquare /></el-icon>
            <dt class="session-item__label">会话</dt>
            <dd class="session-item__value session-item__value--mono">
              {{ session.client_id }}
            </dd>
          </div>
        </dl>

        <footer class="session-item__foot">
          <el-tooltip
            :content="`Alive for ${aliveLabel(session.age_ms)}`"
            placement="top"
            :show-after="200"
          >
            <span class="session-item__age" data-testid="session-item-age">
              <el-icon><Clock /></el-icon>
              <span>{{ aliveLabel(session.age_ms) }}</span>
            </span>
          </el-tooltip>

          <div class="session-item__actions" @click.stop>
            <el-popconfirm
              :title="`Clear dialogue history for ${truncateClientId(session.client_id)}?`"
              confirm-button-text="清除"
              cancel-button-text="取消"
              width="240"
              @confirm="onClearDialogue(session)"
            >
              <template #reference>
                <el-button
                  size="small"
                  text
                  bg
                  :icon="ChatLineSquare"
                  :loading="isBusy(session.session_id)"
                  :disabled="isBusy(session.session_id)"
                  :data-testid="`clear-dialogue-${session.session_id}`"
                >
                  Clear
                </el-button>
              </template>
            </el-popconfirm>

            <el-popconfirm
              :title="`Destroy session ${truncateClientId(session.client_id)}? This cannot be undone.`"
              confirm-button-text="销毁"
              cancel-button-text="取消"
              confirm-button-type="danger"
              width="260"
              @confirm="onDestroy(session)"
            >
              <template #reference>
                <el-button
                  size="small"
                  type="danger"
                  text
                  bg
                  :icon="Delete"
                  :loading="isBusy(session.session_id)"
                  :disabled="isBusy(session.session_id)"
                  :data-testid="`destroy-${session.session_id}`"
                >
                  Destroy
                </el-button>
              </template>
            </el-popconfirm>
          </div>
        </footer>
      </li>
    </ul>
  </section>
</template>

<style scoped>
.session-list {
  display: flex;
  flex-direction: column;
  gap: 12px;
  padding: 16px;
  background: #ffffff;
  border-radius: 12px;
  border: 1px solid #ebeef5;
  min-height: 200px;
  box-shadow: 0 1px 4px rgba(0, 0, 0, 0.03);
}

.session-list__header {
  display: flex;
  flex-direction: column;
  gap: 10px;
  padding-bottom: 12px;
  border-bottom: 1px solid #ebeef5;
}

.session-list__title-row {
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: 12px;
}

.session-list__title {
  display: flex;
  align-items: center;
  gap: 8px;
  margin: 0;
  font-size: 15px;
  font-weight: 600;
  color: #303133;
  letter-spacing: 0.2px;
}

.session-list__title-icon {
  font-size: 18px;
  color: #409eff;
}

.session-list__new-btn {
  flex-shrink: 0;
}

.session-list__meta {
  display: flex;
  align-items: center;
  gap: 10px;
}

.session-list__loading {
  animation: rotating 1.4s linear infinite;
  margin-right: 4px;
}

.session-list__refresh-hint {
  display: inline-flex;
  align-items: center;
  gap: 4px;
  font-size: 11px;
  color: #909399;
  padding: 2px 6px;
  border-radius: 4px;
  background: #f5f7fa;
  cursor: default;
}

.session-list__items {
  list-style: none;
  margin: 0;
  padding: 0;
  display: flex;
  flex-direction: column;
  gap: 8px;
  max-height: 60vh;
  overflow-y: auto;
}

.session-item {
  display: flex;
  flex-direction: column;
  gap: 8px;
  padding: 12px 14px;
  border: 1px solid #ebeef5;
  border-radius: 8px;
  background: #fafbfc;
  cursor: pointer;
  transition: border-color 0.15s ease, background-color 0.15s ease, transform 0.15s ease;
  outline: none;
}

.session-item:hover {
  border-color: #c6e2ff;
  background: #ecf5ff;
}

.session-item:focus-visible {
  border-color: #409eff;
  box-shadow: 0 0 0 2px rgba(64, 158, 255, 0.25);
}

.session-item--selected {
  border-color: #409eff;
  background: linear-gradient(180deg, #ecf5ff 0%, #f5f9ff 100%);
  box-shadow: 0 2px 8px rgba(64, 158, 255, 0.12);
}

.session-item--selected:hover {
  border-color: #409eff;
}

.session-item__head {
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: 8px;
}

.session-item__id {
  font-family:
    'JetBrains Mono', 'SF Mono', Menlo, Consolas, 'Liberation Mono', monospace;
  font-size: 12px;
  color: #303133;
  font-weight: 600;
  letter-spacing: 0.3px;
}

.session-item__status {
  text-transform: capitalize;
  flex-shrink: 0;
}

.session-item__meta {
  margin: 0;
  display: grid;
  grid-template-columns: 1fr 1fr;
  gap: 4px 12px;
}

.session-item__row {
  display: flex;
  align-items: center;
  gap: 6px;
  min-width: 0;
}

.session-item__icon {
  font-size: 13px;
  color: #909399;
  flex-shrink: 0;
}

.session-item__label {
  margin: 0;
  font-size: 11px;
  color: #909399;
  text-transform: uppercase;
  letter-spacing: 0.5px;
  flex-shrink: 0;
}

.session-item__value {
  margin: 0;
  font-size: 12px;
  color: #606266;
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
}

.session-item__value--mono {
  font-family:
    'JetBrains Mono', 'SF Mono', Menlo, Consolas, 'Liberation Mono', monospace;
}

.session-item__foot {
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: 8px;
  padding-top: 8px;
  border-top: 1px dashed #ebeef5;
}

.session-item__age {
  display: inline-flex;
  align-items: center;
  gap: 4px;
  font-size: 11px;
  color: #606266;
  padding: 2px 8px;
  border-radius: 999px;
  background: #f0f2f5;
  font-variant-numeric: tabular-nums;
}

.session-item__actions {
  display: inline-flex;
  align-items: center;
  gap: 4px;
}

.session-list__empty {
  padding: 16px 0;
}

@keyframes rotating {
  from {
    transform: rotate(0deg);
  }
  to {
    transform: rotate(360deg);
  }
}
</style>
