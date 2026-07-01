<script setup lang="ts">
/**
 * PackagePage.vue — Agent Package 编辑-打包-加载流程页面
 *
 * 左侧：文件树浏览器（FileTree）
 * 右侧上部：双标签页（编辑器 | DAG 视图）
 * 右侧下部：打包 + 加载按钮
 */
import { computed, ref, watch } from 'vue'
import { useRoute } from 'vue-router'
import { ElMessage, ElMessageBox } from 'element-plus'
import { usePackageStore } from '@/stores/packageStore'
import { useAgentStore } from '@/stores/agentStore'
import { useDaemonStore } from '@/stores/daemonStore'
import FileTree from '@/components/FileTree.vue'
import CodeEditor from '@/components/CodeEditor.vue'
import PackageCreator from '@/components/PackageCreator.vue'
import { getApiClient } from '@/api/context'

const route = useRoute()
const packageStore = usePackageStore()
const agentStore = useAgentStore()
const daemonStore = useDaemonStore()

const packageName = computed(() => route.params.name as string || packageStore.currentPackage || '')
const currentFile = ref<{ path: string; content: string } | null>(null)
const editorContent = ref('')
const isDirty = ref(false)
const isPackaging = ref(false)
const isLoading = ref(false)
const packageResult = ref<{ path?: string; size?: number; checksum?: string } | null>(null)

// ── Tab switching (editor / dag) ──────────────────────

const activeTab = ref<'editor' | 'dag'>('editor')

// ── File selection ────────────────────────────────────

async function selectFile(filePath: string) {
  if (isDirty.value) {
    try {
      await ElMessageBox.confirm(
        '您有未保存的更改，确定要丢弃吗？',
        '未保存的更改',
        { confirmButtonText: '丢弃', cancelButtonText: '取消', type: 'warning' },
      )
    } catch {
      return // cancelled
    }
  }

  isLoading.value = true
  activeTab.value = 'editor'
  try {
    const res = await getApiClient().get<{ content: string }>(
      `/api/packages/${encodeURIComponent(packageName.value)}/file/${encodeURIComponent(filePath)}`,
    ) as unknown as { path: string; content: string }
    currentFile.value = { path: filePath, content: res.content }
    editorContent.value = res.content
    isDirty.value = false
  } catch (e) {
    ElMessage.error(`读取文件失败: ${e instanceof Error ? e.message : e}`)
  } finally {
    isLoading.value = false
  }
}

// ── Save ──────────────────────────────────────────────────

async function saveFile() {
  if (!currentFile.value) return
  isLoading.value = true
  try {
    await getApiClient().put(
      `/api/packages/${encodeURIComponent(packageName.value)}/file/${encodeURIComponent(currentFile.value.path)}`,
      { content: editorContent.value },
    )
    isDirty.value = false
    ElMessage.success('文件已保存')
  } catch (e) {
    ElMessage.error(`保存失败: ${e instanceof Error ? e.message : e}`)
  } finally {
    isLoading.value = false
  }
}

// ── Package ───────────────────────────────────────────────

async function packageAgent() {
  isPackaging.value = true
  packageResult.value = null
  try {
    const res = await getApiClient().post<{ path: string; size: number; checksum: string }>(
      `/api/packages/${encodeURIComponent(packageName.value)}/package`,
    ) as unknown as { path: string; size: number; checksum: string }
    packageResult.value = res
    ElMessage.success(`打包完成: ${res.path}`)
  } catch (e) {
    ElMessage.error(`打包失败: ${e instanceof Error ? e.message : e}`)
  } finally {
    isPackaging.value = false
  }
}

// ── Load to daemon ────────────────────────────────────────

async function loadToDaemon() {
  isLoading.value = true
  try {
    const pkg = packageName.value
    if (!pkg) {
      ElMessage.error('未选择智能体包')
      return
    }
    await agentStore.loadAgentPackage(pkg, pkg)
    ElMessage.success(`Agent "${pkg}" 已加载到守护进程`)
  } catch (e) {
    ElMessage.error(`加载失败: ${e instanceof Error ? e.message : e}`)
  } finally {
    isLoading.value = false
  }
}

// ── Watch route changes ───────────────────────────────────

watch(() => route.params.name, () => {
  currentFile.value = null
  editorContent.value = ''
  isDirty.value = false
  packageResult.value = null
  activeTab.value = 'editor'
})
</script>

<template>
  <div class="package-page" data-testid="package-page">
    <!-- Left: File tree -->
    <aside class="package-page__sidebar">
      <div class="package-page__sidebar-header">
        <h3 class="package-page__sidebar-title">{{ packageName }}</h3>
      </div>
      <FileTree
        :package-name="packageName"
        @file-selected="(p) => selectFile(p.path)"
      />
    </aside>

    <!-- Right: Editor / DAG + Actions -->
    <main class="package-page__main">
      <!-- Tab bar -->
      <div class="package-page__tabs">
        <button
          class="package-page__tab"
          :class="{ 'package-page__tab--active': activeTab === 'editor' }"
          @click="activeTab = 'editor'"
        >
          <span class="package-page__tab-icon">📝</span>
          编辑器
        </button>
        <button
          class="package-page__tab"
          :class="{ 'package-page__tab--active': activeTab === 'dag' }"
          @click="activeTab = 'dag'"
        >
          <span class="package-page__tab-icon">🔀</span>
          DAG 视图
        </button>
      </div>

      <!-- ────────── Editor tab ────────── -->
      <div v-show="activeTab === 'editor'" class="package-page__tab-content">
        <!-- File loading -->
        <div v-if="isLoading && !currentFile" class="package-page__loading">
          <el-skeleton :rows="10" animated />
        </div>

        <!-- No file selected -->
        <div v-else-if="!currentFile" class="package-page__empty">
          <el-empty description="从左侧文件树中选择一个文件进行编辑" :image-size="80" />
        </div>

        <!-- File editor section -->
        <template v-else>
          <div class="package-page__toolbar">
            <div class="package-page__toolbar-left">
              <span class="package-page__file-path">{{ currentFile.path }}</span>
              <span v-if="isDirty" class="package-page__dirty-badge">● 未保存</span>
            </div>
            <div class="package-page__toolbar-right">
              <el-button
                size="small"
                type="primary"
                :disabled="!isDirty"
                :loading="isLoading"
                @click="saveFile"
                data-testid="save-file"
              >
                Save
              </el-button>
            </div>
          </div>

          <div class="package-page__editor">
            <CodeEditor
              v-model="editorContent"
              :file-name="currentFile.path"
              @save="saveFile"
            />
          </div>
        </template>
      </div>

      <!-- ────────── DAG tab ────────── -->
      <div v-show="activeTab === 'dag'" class="package-page__tab-content">
        <PackageCreator
          :readonly="true"
          :package-name="packageName"
        />
      </div>

      <!-- Bottom actions (visible in both tabs) -->
      <div class="package-page__actions">
        <el-divider />

        <div class="package-page__actions-row">
          <el-button
            type="success"
            :loading="isPackaging"
            :disabled="!daemonStore.isConnected"
            @click="packageAgent"
            data-testid="package-button"
          >
            {{ isPackaging ? '打包中…' : '打包' }}
          </el-button>
          <el-button
            type="primary"
            :loading="isLoading"
            :disabled="!daemonStore.isConnected"
            @click="loadToDaemon"
            data-testid="load-button"
          >
            加载到守护进程
          </el-button>
        </div>

        <!-- Package result -->
        <div v-if="packageResult" class="package-page__result">
          <el-alert
            type="success"
            show-icon
            :closable="false"
          >
            <template #title>
              打包完成: <code>{{ packageResult.path }}</code>
            </template>
            <template #default>
              大小: {{ (packageResult.size / 1024).toFixed(1) }} KB ·
              校验和: <code>{{ packageResult.checksum?.slice(0, 16) }}…</code>
            </template>
          </el-alert>
        </div>
      </div>
    </main>
  </div>
</template>

<style scoped>
.package-page {
  display: flex;
  height: 100%;
}
.package-page__sidebar {
  width: 280px;
  min-width: 280px;
  border-right: 1px solid #e4e7ed;
  display: flex;
  flex-direction: column;
  overflow: hidden;
  background: #ffffff;
}
.package-page__sidebar-header {
  padding: 12px 16px;
  border-bottom: 1px solid #e4e7ed;
  background: #f8f9fb;
}
.package-page__sidebar-title {
  margin: 0;
  font-size: 15px;
  font-weight: 600;
  color: #1f2937;
  font-family: 'JetBrains Mono', 'SF Mono', Menlo, monospace;
}
.package-page__main {
  flex: 1;
  display: flex;
  flex-direction: column;
  overflow: hidden;
  background: #ffffff;
}

/* ── Tab bar ── */
.package-page__tabs {
  display: flex;
  border-bottom: 1px solid #e4e7ed;
  background: #f8f9fb;
  flex-shrink: 0;
}
.package-page__tab {
  padding: 10px 20px;
  border: none;
  background: transparent;
  cursor: pointer;
  font-size: 14px;
  color: #606266;
  position: relative;
  transition: color 0.2s, background 0.2s;
  display: flex;
  align-items: center;
  gap: 6px;
  font-family: inherit;
}
.package-page__tab:hover {
  color: #409eff;
  background: #ecf5ff;
}
.package-page__tab--active {
  color: #409eff;
  background: #ffffff;
  font-weight: 600;
}
.package-page__tab--active::after {
  content: '';
  position: absolute;
  bottom: 0;
  left: 0;
  right: 0;
  height: 2px;
  background: #409eff;
}
.package-page__tab-icon {
  font-size: 16px;
}

/* ── Tab content ── */
.package-page__tab-content {
  flex: 1;
  display: flex;
  flex-direction: column;
  overflow: hidden;
}
.package-page__loading {
  padding: 24px;
}
.package-page__empty {
  flex: 1;
  display: flex;
  align-items: center;
  justify-content: center;
}

/* ── Editor toolbar ── */
.package-page__toolbar {
  display: flex;
  align-items: center;
  justify-content: space-between;
  padding: 8px 16px;
  border-bottom: 1px solid #e4e7ed;
  background: #f8f9fb;
}
.package-page__toolbar-left {
  display: flex;
  align-items: center;
  gap: 8px;
}
.package-page__file-path {
  font-family: 'JetBrains Mono', 'SF Mono', Menlo, monospace;
  font-size: 13px;
  color: #606266;
}
.package-page__dirty-badge {
  color: #e6a23c;
  font-size: 12px;
}
.package-page__toolbar-right {
  display: flex;
  gap: 6px;
}
.package-page__editor {
  flex: 1;
  overflow: hidden;
}

/* ── Bottom actions ── */
.package-page__actions {
  padding: 0 16px 16px;
  background: #ffffff;
  flex-shrink: 0;
}
.package-page__actions-row {
  display: flex;
  gap: 8px;
}
.package-page__result {
  margin-top: 12px;
}
</style>
