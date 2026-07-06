<script setup lang="ts">
/**
 * Session list and management component.
 *
 * Renders Bridge persistent sessions for the currently-selected agent and
 * exposes destroy actions. Reads from ``GET /api/sessions/persistent``.
 *
 * Supports single-destroy (existing), batch-destroy, and destroy-all via
 * a toggleable selection mode.
 *
 * No auto-refresh: the list only changes on user action (create / delete).
 */
import { useSessionStore } from '@/stores/sessionStore'
import type { PersistentSession } from '@/types/agentflow'
import {
  ChatLineSquare,
  Clock,
  Delete,
  Loading as ElIconLoading,
  Plus,
  Select,
  CircleCheck,
} from '@element-plus/icons-vue'
import { ElMessage, ElMessageBox } from 'element-plus'
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
  (e: 'session-selected', sessionId: string): void
  (e: 'new-session'): void
  /** Emitted when one or more sessions are destroyed (current session may have been affected). */
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

/** Per-session loading flags for destructive actions; key is session_id. */
const busyIds = ref<Set<string>>(new Set())

/** Whether multi-select mode is active. */
const selectMode = ref(false)

/** Set of session IDs currently checked in select mode. */
const selectedIds = ref<Set<string>>(new Set())

// ---------------------------------------------------------------------------
// Computed
// ---------------------------------------------------------------------------

const sessions = computed<PersistentSession[]>(() => sessionStore.sessions)
const isLoading = computed(() => sessionStore.isLoading)
const errorMessage = computed(() => sessionStore.error)
const hasSessions = computed(() => sessions.value.length > 0)
const canRender = computed(() => props.agentId !== null && props.agentId.length > 0)

/** Number of sessions currently checked. */
const selectedCount = computed(() => selectedIds.value.size)

/** Whether all sessions are checked. */
const allSelected = computed(
  () => hasSessions.value && selectedIds.value.size === sessions.value.length,
)

/** True when a bulk delete operation is in progress. */
const isBulkBusy = computed(() => busyIds.value.size > 0)

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

onMounted(() => {
  if (canRender.value) {
    void refresh()
  }
})

// Re-load when the bound agent changes.
watch(
  () => props.agentId,
  (next, prev) => {
    if (next === prev) return
    if (canRender.value) {
      void refresh()
    }
  },
)

// ---------------------------------------------------------------------------
// Actions
// ---------------------------------------------------------------------------

async function refresh(): Promise<void> {
  if (!canRender.value) return
  try {
    await sessionStore.loadSessions(props.userId || undefined)
  } catch {
    // Store records the error; surface via errorMessage below.
  }
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

function setBusyFlag(flag: boolean): void {
  if (flag) {
    busyIds.value = new Set(['__bulk__'])
  } else {
    busyIds.value = new Set()
  }
}

function onRowClick(session: PersistentSession): void {
  if (selectMode.value) {
    toggleSelect(session.id)
    return
  }
  emit('session-selected', session.id)
}

function onNewSession(): void {
  emit('new-session')
}

/** Toggle selection mode on/off. Exiting clears selection. */
function toggleSelectMode(): void {
  selectMode.value = !selectMode.value
  if (!selectMode.value) {
    selectedIds.value = new Set()
  }
}

/** Toggle one session's checked state in select mode. */
function toggleSelect(sessionId: string): void {
  const next = new Set(selectedIds.value)
  if (next.has(sessionId)) {
    next.delete(sessionId)
  } else {
    next.add(sessionId)
  }
  selectedIds.value = next
}

/** Check or uncheck all sessions. */
function toggleSelectAll(): void {
  if (allSelected.value) {
    selectedIds.value = new Set()
  } else {
    selectedIds.value = new Set(sessions.value.map((s) => s.id))
  }
}

async function onDestroy(session: PersistentSession): Promise<void> {
  if (!canRender.value) return
  const sessionId = session.id
  const clientId = session.client_id
  try {
    await withBusy(sessionId, () => sessionStore.destroySession(sessionId))
    ElMessage.success(`会话 ${truncateClientId(clientId)} 已移出列表`)
  } catch (e) {
    ElMessage.error(`移除会话失败: ${errorMessage.value ?? 'unknown error'}`)
  } finally {
    if (props.currentSessionId === sessionId) {
      emit('session-destroyed', sessionId)
    }
  }
}

async function onBatchDestroy(): Promise<void> {
  if (selectedCount.value === 0) return
  try {
    await ElMessageBox.confirm(
      `确定要移出选中的 ${selectedCount.value} 个会话？此操作不可恢复。`,
      '批量移除会话',
      { confirmButtonText: '移除', cancelButtonText: '取消', type: 'warning' },
    )
  } catch {
    return // user cancelled
  }

  setBusyFlag(true)
  const ids = [...selectedIds.value]
  try {
    const completed = await sessionStore.batchDestroySessions(ids)
    selectedIds.value = new Set()
    selectMode.value = false
    if (completed.length > 0) {
      ElMessage.success(`已移除 ${completed.length} 个会话`)
    }
    const removed = completed.length
    const failed = ids.length - removed
    if (failed > 0) {
      ElMessage.warning(`${failed} 个会话移除失败`)
    }
    // Notify parent if current session was among the removed ones.
    if (props.currentSessionId && ids.includes(props.currentSessionId)) {
      emit('session-destroyed', props.currentSessionId)
    }
  } catch {
    ElMessage.error('批量移除失败')
  } finally {
    setBusyFlag(false)
  }
}

async function onDestroyAll(): Promise<void> {
  if (sessions.value.length === 0) return
  try {
    await ElMessageBox.confirm(
      `确定要移出全部 ${sessions.value.length} 个会话？此操作不可恢复。`,
      '移除全部会话',
      { confirmButtonText: '移除', cancelButtonText: '取消', type: 'warning' },
    )
  } catch {
    return // user cancelled
  }

  setBusyFlag(true)
  try {
    const completed = await sessionStore.destroyAllSessions()
    selectedIds.value = new Set()
    selectMode.value = false
    if (completed.length > 0) {
      ElMessage.success(`已移除全部 ${completed.length} 个会话`)
    }
    if (props.currentSessionId) {
      emit('session-destroyed', props.currentSessionId)
    }
  } catch {
    ElMessage.error('移除全部会话失败')
  } finally {
    setBusyFlag(false)
  }
}

/** Format an ISO datetime to a compact display string. */
function formatIso(iso: string): string {
  if (!iso) return '—'
  const d = new Date(iso)
  const now = new Date()
  const diffMs = now.getTime() - d.getTime()
  const diffMin = Math.floor(diffMs / 60_000)
  if (diffMin < 1) return '刚刚'
  if (diffMin < 60) return `${diffMin}分前`
  const diffH = Math.floor(diffMin / 60)
  if (diffH < 24) return `${diffH}小时前`
  const diffD = Math.floor(diffH / 24)
  if (diffD < 30) return `${diffD}天前`
  return d.toLocaleDateString('zh-CN', { month: 'short', day: 'numeric' })
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
        <div class="session-list__header-actions">
          <el-button
            v-if="hasSessions && !selectMode"
            size="small"
            text
            bg
            :icon="Select"
            :disabled="!canRender || isBulkBusy"
            @click="toggleSelectMode"
          >
            选择
          </el-button>
          <el-button
            v-if="selectMode"
            size="small"
            text
            bg
            type="info"
            :disabled="isBulkBusy"
            @click="toggleSelectMode"
          >
            取消选择
          </el-button>
          <el-button
            class="session-list__new-btn"
            type="primary"
            size="small"
            :icon="Plus"
            data-testid="new-session"
            :disabled="!canRender || selectMode"
            @click="onNewSession"
          >
            新建会话
          </el-button>
        </div>
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
          <span>{{ isLoading ? '加载中…' : `${sessions.length} 个会话` }}</span>
        </el-tag>

        <!-- Batch action bar (visible in select mode) -->
        <template v-if="selectMode && hasSessions">
          <el-checkbox
            :model-value="allSelected"
            :indeterminate="selectedCount > 0 && !allSelected"
            :disabled="isBulkBusy"
            @change="toggleSelectAll"
          >
            {{ allSelected ? '取消全选' : '全选' }}
          </el-checkbox>
          <span class="session-list__selected-info">已选 {{ selectedCount }}</span>
          <el-button
            size="small"
            type="danger"
            plain
            :icon="Delete"
            :disabled="selectedCount === 0 || isBulkBusy"
            :loading="isBulkBusy"
            @click="onBatchDestroy"
          >
            批量删除
          </el-button>
          <el-popconfirm
            title="确定要移出全部会话？"
            confirm-button-text="移除"
            cancel-button-text="取消"
            width="220"
            @confirm="onDestroyAll"
          >
            <template #reference>
              <el-button
                size="small"
                type="danger"
                text
                bg
                :icon="Delete"
                :disabled="isBulkBusy || sessions.length === 0"
                :loading="isBulkBusy"
              >
                全部删除
              </el-button>
            </template>
          </el-popconfirm>
        </template>
      </div>
    </header>

    <el-alert
      v-if="!canRender"
      type="info"
      :closable="false"
      show-icon
      title="请先选择一个 Agent"
      description="先在包列表中选一个 Agent 来查看其会话。"
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
      description="暂无会话"
      :image-size="80"
      data-testid="session-list-empty"
    >
      <el-button type="primary" :icon="Plus" @click="onNewSession">
        创建第一个会话
      </el-button>
    </el-empty>

    <ul v-else class="session-list__items" role="listbox" aria-label="会话列表">
      <li
        v-for="session in sessions"
        :key="session.id"
        class="session-item"
        :class="{
          'session-item--selected': !selectMode && session.id === props.currentSessionId,
          'session-item--checked': selectMode && selectedIds.has(session.id),
        }"
        :data-testid="`session-${session.id}`"
        role="option"
        :aria-selected="session.id === props.currentSessionId"
        @click="onRowClick(session)"
        @keydown.enter="onRowClick(session)"
        @keydown.space.prevent="onRowClick(session)"
        tabindex="0"
      >
        <!-- Checkbox in select mode -->
        <div v-if="selectMode" class="session-item__check" @click.stop="toggleSelect(session.id)">
          <el-checkbox
            :model-value="selectedIds.has(session.id)"
            :disabled="isBulkBusy"
            @click.stop="toggleSelect(session.id)"
          />
        </div>

        <div class="session-item__body">
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

          <p class="session-item__title">
            {{ session.title || '（无标题）' }}
          </p>

          <footer class="session-item__foot">
            <span class="session-item__age">
              <el-icon><Clock /></el-icon>
              <span>{{ formatIso(session.last_active_at) }}</span>
            </span>
            <div v-if="!selectMode" class="session-item__actions" @click.stop>
              <el-popconfirm
                :title="`移出会话 ${truncateClientId(session.client_id)}?`"
                confirm-button-text="移除"
                cancel-button-text="取消"
                width="240"
                @confirm="onDestroy(session)"
              >
                <template #reference>
                  <el-button
                    size="small"
                    type="danger"
                    text
                    bg
                    :icon="Delete"
                    :loading="isBusy(session.id)"
                    :disabled="isBusy(session.id)"
                    :data-testid="`destroy-${session.id}`"
                  >
                    移除
                  </el-button>
                </template>
              </el-popconfirm>
            </div>
          </footer>
        </div>
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
.session-list__header-actions {
  display: flex;
  align-items: center;
  gap: 6px;
  flex-shrink: 0;
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
  flex-wrap: wrap;
}
.session-list__selected-info {
  font-size: 12px;
  color: #606266;
  font-variant-numeric: tabular-nums;
}
.session-list__loading {
  animation: rotating 1.4s linear infinite;
  margin-right: 4px;
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
  flex-direction: row;
  gap: 10px;
  padding: 12px 14px;
  border: 1px solid #ebeef5;
  border-radius: 8px;
  background: #fafbfc;
  cursor: pointer;
  transition: border-color 0.15s ease, background-color 0.15s ease, transform 0.15s ease;
  outline: none;
  align-items: flex-start;
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
.session-item--checked {
  border-color: #e6a23c;
  background: #fdf6ec;
}
.session-item--checked:hover {
  border-color: #e6a23c;
  background: #fdf6ec;
}
.session-item__check {
  display: flex;
  align-items: center;
  padding-top: 2px;
  flex-shrink: 0;
}
.session-item__body {
  flex: 1;
  min-width: 0;
  display: flex;
  flex-direction: column;
  gap: 8px;
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
.session-item__title {
  margin: 0;
  font-size: 13px;
  color: #303133;
  font-weight: 500;
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
  line-height: 1.4;
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
  from { transform: rotate(0deg); }
  to { transform: rotate(360deg); }
}
</style>
