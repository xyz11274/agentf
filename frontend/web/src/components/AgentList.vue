<script setup lang="ts">
/**
 * AgentList.vue — Agent 列表组件（可折叠）
 *
 * 从 agentStore 获取 Agent 列表，显示在聊天页面侧边栏。
 * 支持折叠/展开以节省空间。
 */
import { computed, onBeforeUnmount, onMounted, ref } from 'vue'
import { ElMessage, ElMessageBox } from 'element-plus'
import { ArrowDown, ArrowRight } from '@element-plus/icons-vue'
import { useAgentStore } from '@/stores/agentStore'
import { useRouter } from 'vue-router'
import type { AgentInfo } from '@/types/agentflow'

interface Emits {
  (e: 'agent-click', agentId: string): void
}

const emit = defineEmits<Emits>()

const agentStore = useAgentStore()
const router = useRouter()

const loadingAction = ref<string | null>(null)
const collapsed = ref(false)
let refreshTimer: ReturnType<typeof setInterval> | null = null

function toggleCollapse() {
  collapsed.value = !collapsed.value
}

// ── Refresh ───────────────────────────────────────────────

async function refresh() {
  try {
    await agentStore.loadAgents()
  } catch {
    // silent — daemon may be down
  }
}

onMounted(() => {
  refresh()
  refreshTimer = setInterval(refresh, 10_000)
})

onBeforeUnmount(() => {
  if (refreshTimer) clearInterval(refreshTimer)
})

// ── Status helpers ────────────────────────────────────────

const statusType = (status?: string) => {
  switch (status) {
    case 'running': return 'success'
    case 'paused': return 'warning'
    case 'error': return 'danger'
    default: return 'info'
  }
}

// ── Actions ───────────────────────────────────────────────

async function handleLoad(agent: AgentInfo) {
  loadingAction.value = agent.id
  try {
    await agentStore.loadAgentPackage(agent.id)
    ElMessage.success(`Agent "${agent.id}" loaded`)
  } catch (e) {
    ElMessage.error(`加载 Agent 失败: ${e instanceof Error ? e.message : e}`)
  } finally {
    loadingAction.value = null
  }
}

async function handleUnload(agent: AgentInfo) {
  try {
    await ElMessageBox.confirm(
      `确定要卸载 Agent "${agent.id}" 吗？`,
      '确认',
      { confirmButtonText: '卸载', cancelButtonText: '取消', type: 'warning' },
    )
  } catch {
    return
  }

  loadingAction.value = agent.id
  try {
    await agentStore.unloadAgent(agent.id)
    ElMessage.success(`Agent "${agent.id}" unloaded`)
  } catch (e) {
    ElMessage.error(`卸载 Agent 失败: ${e instanceof Error ? e.message : e}`)
  } finally {
    loadingAction.value = null
  }
}

async function handlePause(agent: AgentInfo) {
  loadingAction.value = agent.id
  try {
    await agentStore.pauseAgent(agent.id)
    ElMessage.info(`Agent "${agent.id}" paused`)
  } catch (e) {
    ElMessage.error(`暂停 Agent 失败: ${e instanceof Error ? e.message : e}`)
  } finally {
    loadingAction.value = null
  }
}

async function handleResume(agent: AgentInfo) {
  loadingAction.value = agent.id
  try {
    await agentStore.resumeAgent(agent.id)
    ElMessage.info(`Agent "${agent.id}" resumed`)
  } catch (e) {
    ElMessage.error(`恢复 Agent 失败: ${e instanceof Error ? e.message : e}`)
  } finally {
    loadingAction.value = null
  }
}

function goToChat(agent: AgentInfo) {
  agentStore.setCurrentAgent(agent.id)
  router.push('/chat')
}

const displayCount = computed(() => agentStore.agents.length)
</script>

<template>
  <div class="agent-list" data-testid="agent-list">
    <!-- Collapsible header bar -->
    <div class="agent-list__header" @click="toggleCollapse">
      <div class="agent-list__header-left">
        <el-icon class="agent-list__toggle-icon">
          <ArrowDown v-if="!collapsed" />
          <ArrowRight v-else />
        </el-icon>
        <h3 class="agent-list__title">Agent</h3>
        <el-tag v-if="displayCount > 0" size="small" type="info" effect="plain" round>
          {{ displayCount }}
        </el-tag>
      </div>
      <el-button
        size="small"
        text
        :loading="agentStore.isLoading"
        @click.stop="refresh"
        data-testid="refresh-agents"
      >
        刷新
      </el-button>
    </div>

    <!-- Body (collapsible) -->
    <div v-show="!collapsed" class="agent-list__body">
      <!-- Error -->
      <el-alert
        v-if="agentStore.error"
        :title="agentStore.error"
        type="error"
        show-icon
        :closable="false"
        class="agent-list__error"
      />

      <!-- Empty state -->
      <el-empty
        v-if="!agentStore.isLoading && agentStore.agents.length === 0 && !agentStore.error"
        description="暂无已加载的 Agent"
        :image-size="60"
      />

      <!-- Agent cards -->
      <div v-else class="agent-list__cards">
        <div
          v-for="agent in agentStore.agents"
          :key="agent.id"
          class="agent-list__card"
          :data-testid="`agent-${agent.id}`"
        >
          <div class="agent-list__card-header">
            <div class="agent-list__card-name" @click="emit('agent-click', agent.id)">{{ agent.id }}</div>
            <el-tag :type="statusType(agent.status)" size="small" effect="dark">
              {{ agent.status === 'running' ? '运行中' : agent.status === 'paused' ? '已暂停' : agent.status === 'error' ? '错误' : '未知' }}
            </el-tag>
          </div>

          <div class="agent-list__card-meta">
            <span v-if="agent.name">名称: {{ agent.name }}</span>
            <span v-if="agent.workflow_count != null">
              工作流: {{ agent.workflow_count }}
            </span>
            <span v-if="agent.description" class="agent-list__card-desc">
              {{ agent.description }}
            </span>
          </div>

          <div class="agent-list__card-actions">
            <el-button
              size="small"
              type="primary"
              :loading="loadingAction === agent.id"
              @click="handleLoad(agent)"
              data-testid="load-agent"
            >
              加载
            </el-button>
            <el-button
              size="small"
              type="danger"
              plain
              :loading="loadingAction === agent.id"
              :disabled="agent.status === 'unloaded'"
              @click="handleUnload(agent)"
            >
              卸载
            </el-button>
            <el-button
              v-if="agent.status === 'running'"
              size="small"
              type="warning"
              plain
              :loading="loadingAction === agent.id"
              @click="handlePause(agent)"
            >
              暂停
            </el-button>
            <el-button
              v-if="agent.status === 'paused'"
              size="small"
              type="success"
              plain
              :loading="loadingAction === agent.id"
              @click="handleResume(agent)"
            >
              恢复
            </el-button>
            <el-button
              size="small"
              type="primary"
              plain
              @click="goToChat(agent)"
              data-testid="chat-with-agent"
            >
              聊天
            </el-button>
          </div>
        </div>
      </div>
    </div>
  </div>
</template>

<style scoped>
.agent-list {
  display: flex;
  flex-direction: column;
}
.agent-list__header {
  display: flex;
  align-items: center;
  justify-content: space-between;
  cursor: pointer;
  user-select: none;
  padding: 4px 2px;
  border-radius: 6px;
}
.agent-list__header:hover {
  background: #f0f2f5;
}
.agent-list__header-left {
  display: flex;
  align-items: center;
  gap: 6px;
}
.agent-list__toggle-icon {
  font-size: 14px;
  color: #909399;
}
.agent-list__title {
  margin: 0;
  font-size: 14px;
  font-weight: 600;
  color: #303133;
}
.agent-list__error {
  margin-bottom: 8px;
}
.agent-list__body {
  padding-top: 8px;
}
.agent-list__cards {
  display: flex;
  flex-direction: column;
  gap: 6px;
}
.agent-list__card {
  border: 1px solid #e4e7ed;
  border-radius: 8px;
  padding: 10px 12px;
  background: #fff;
  transition: box-shadow 0.2s ease, border-color 0.2s ease;
}
.agent-list__card:hover {
  box-shadow: 0 2px 8px rgba(0, 0, 0, 0.05);
  border-color: #c6e2ff;
}
.agent-list__card-header {
  display: flex;
  align-items: center;
  justify-content: space-between;
  margin-bottom: 4px;
}
.agent-list__card-name {
  font-weight: 600;
  font-size: 13px;
  color: #303133;
  font-family: monospace;
  cursor: pointer;
}
.agent-list__card-name:hover {
  color: #409eff;
}
.agent-list__card-meta {
  display: flex;
  flex-wrap: wrap;
  gap: 6px;
  font-size: 11px;
  color: #909399;
  margin-bottom: 6px;
}
.agent-list__card-desc {
  width: 100%;
  font-style: italic;
}
.agent-list__card-actions {
  display: flex;
  gap: 4px;
  flex-wrap: wrap;
  padding-top: 2px;
}
</style>
