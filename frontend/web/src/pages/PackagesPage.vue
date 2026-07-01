<script setup lang="ts">
/**
 * PackagesPage
 * ------------------------------------------------------------------
 * Browse, create, upload, and update Agent packages. Each package is a
 * directory under the configured ``agent_packages_path``. Click a card
 * to open the package editor. Use the dropdown on each card to upload
 * a new version of an existing package.
 */
import { computed, onMounted, ref } from 'vue'
import { useRouter } from 'vue-router'
import { ElMessage, ElMessageBox } from 'element-plus'
import { FolderOpened, MoreFilled, Plus, Upload } from '@element-plus/icons-vue'
import { usePackageStore } from '@/stores/packageStore'
import type { PackageInfo } from '@/types/agentflow'

const packageStore = usePackageStore()
const router = useRouter()

const packages = computed<PackageInfo[]>(() => packageStore.packages)
const isLoading = computed<boolean>(() => packageStore.isLoading)
const errorMessage = computed<string | null>(() => packageStore.error)
const isEmpty = computed<boolean>(
  () => !isLoading.value && !errorMessage.value && packages.value.length === 0,
)

function openCreatePage() {
  router.push('/packages/create')
}

// ── Upload (create new) ────────────────────────────────────────────────
const uploadLoading = ref(false)
const fileInputRef = ref<HTMLInputElement | null>(null)

function triggerUpload() {
  fileInputRef.value?.click()
}

async function handleFileChange(event: Event) {
  const input = event.target as HTMLInputElement
  const file = input.files?.[0]
  if (!file) return

  uploadLoading.value = true
  try {
    await packageStore.uploadPackage(file)
    ElMessage.success(`智能体包 "${file.name}" 上传并解压成功`)
  } catch (e) {
    ElMessage.error(`上传失败: ${e instanceof Error ? e.message : e}`)
  } finally {
    uploadLoading.value = false
    // Reset the file input so the same file can be re-selected
    input.value = ''
  }
}

// ── Update (replace existing) ──────────────────────────────────────────
const updateTarget = ref<PackageInfo | null>(null)
const updateFile = ref<File | null>(null)
const updateMode = ref<'incremental' | 'full'>('incremental')
const updateDialogVisible = ref(false)
const updateLoading = ref(false)
const updateFileInputRef = ref<HTMLInputElement | null>(null)

function triggerUpdateFilePicker(pkg: PackageInfo) {
  updateTarget.value = pkg
  // Use a separate hidden input for the update file picker
  updateFileInputRef.value?.click()
}

function handleUpdateFileSelected(event: Event) {
  const input = event.target as HTMLInputElement
  const file = input.files?.[0]
  if (!file || !updateTarget.value) return

  updateFile.value = file
  updateMode.value = 'incremental'
  updateDialogVisible.value = true
  input.value = ''
}

async function confirmUpdate() {
  if (!updateTarget.value || !updateFile.value) return

  updateLoading.value = true
  const pkgName = updateTarget.value.name
  try {
    await packageStore.updatePackage(pkgName, updateFile.value, updateMode.value)
    ElMessage.success(`智能体包 "${pkgName}" 更新成功（${updateMode.value === 'incremental' ? '增量覆盖' : '全量替换'}）`)
    updateDialogVisible.value = false
    updateFile.value = null
    updateTarget.value = null
  } catch (e) {
    ElMessage.error(`更新失败: ${e instanceof Error ? e.message : e}`)
  } finally {
    updateLoading.value = false
  }
}

function cancelUpdate() {
  updateDialogVisible.value = false
  updateFile.value = null
  updateTarget.value = null
}

function formatBytes(bytes?: number): string {
  if (bytes === undefined || bytes === null) return ''
  if (bytes < 1024) return `${bytes} B`
  if (bytes < 1024 * 1024) return `${(bytes / 1024).toFixed(1)} KB`
  return `${(bytes / (1024 * 1024)).toFixed(1)} MB`
}

// ── Lifecycle ─────────────────────────────────────────────────────────
onMounted(async () => {
  if (packageStore.packages.length === 0) {
    try {
      await packageStore.loadPackages()
    } catch (e) {
      ElMessage.error(`加载智能体包失败: ${e instanceof Error ? e.message : e}`)
    }
  }
})

function openPackage(pkg: PackageInfo) {
  router.push(`/packages/${encodeURIComponent(pkg.name)}`)
}

function refresh() {
  packageStore.loadPackages().catch((e) => {
    ElMessage.error(`刷新失败: ${e instanceof Error ? e.message : e}`)
  })
}

function formatDate(iso: string): string {
  if (!iso) return ''
  const d = new Date(iso)
  if (Number.isNaN(d.getTime())) return iso
  return d.toLocaleString()
}
</script>

<template>
  <section class="packages-page" data-testid="packages-page">
    <header class="packages-page__header">
      <div>
        <h2 class="packages-page__title">智能体包</h2>
        <p class="packages-page__subtitle">
          浏览已打包的智能体集合。点击卡片进入编辑器查看和修改。
        </p>
      </div>
      <div class="packages-page__header-actions">
        <el-button
          type="primary"
          :icon="Plus"
          @click="openCreatePage"
          data-testid="packages-create"
        >
          新建
        </el-button>
        <el-button
          type="success"
          :icon="Upload"
          :loading="uploadLoading || isLoading"
          @click="triggerUpload"
          data-testid="packages-upload"
        >
          上传
        </el-button>
        <el-button :loading="isLoading" @click="refresh" data-testid="packages-refresh">
          刷新
        </el-button>
      </div>
    </header>

    <!-- Hidden file input for upload -->
    <input
      ref="fileInputRef"
      type="file"
      accept=".zip,.tar.gz,.tgz"
      style="display: none"
      @change="handleFileChange"
      data-testid="packages-file-input"
    />

    <!-- Hidden file input for update (separate so each can reset independently) -->
    <input
      ref="updateFileInputRef"
      type="file"
      accept=".zip,.tar.gz,.tgz"
      style="display: none"
      @change="handleUpdateFileSelected"
      data-testid="packages-update-file-input"
    />

    <!-- ── Update confirmation dialog ────────────────────────────────── -->
    <el-dialog
      v-model="updateDialogVisible"
      title="更新智能体包"
      width="480px"
      :close-on-click-modal="false"
      @close="cancelUpdate"
      data-testid="packages-update-dialog"
    >
      <div v-if="updateTarget && updateFile" class="packages-update__body">
        <div class="packages-update__row">
          <span class="packages-update__label">包名</span>
          <span class="packages-update__value">{{ updateTarget.name }}</span>
        </div>
        <div class="packages-update__row">
          <span class="packages-update__label">当前文件数</span>
          <span class="packages-update__value">{{ updateTarget.file_count }} 个文件</span>
        </div>
        <div class="packages-update__row">
          <span class="packages-update__label">更新文件</span>
          <span class="packages-update__value">{{ updateFile.name }}（{{ formatBytes(updateFile.size) }}）</span>
        </div>

        <el-divider style="margin: 12px 0" />

        <div class="packages-update__mode">
          <span class="packages-update__label">更新模式</span>
          <el-radio-group v-model="updateMode" class="packages-update__radios">
            <el-radio value="incremental" class="packages-update__radio">
              <div>
                <strong>增量覆盖</strong>
                <p class="packages-update__radio-desc">新增文件，覆盖同名文件，不删除已有文件（推荐）</p>
              </div>
            </el-radio>
            <el-radio value="full" class="packages-update__radio">
              <div>
                <strong>全量替换</strong>
                <p class="packages-update__radio-desc">清空除 dist/ 外的所有内容，再解压归档</p>
              </div>
            </el-radio>
          </el-radio-group>
        </div>
      </div>

      <template #footer>
        <el-button @click="cancelUpdate">取消</el-button>
        <el-button type="primary" :loading="updateLoading" @click="confirmUpdate">
          {{ updateLoading ? '更新中…' : '确认更新' }}
        </el-button>
      </template>
    </el-dialog>

    <!-- Loading skeleton -->
    <div v-if="isLoading && packages.length === 0" class="packages-page__skeleton">
      <el-skeleton v-for="i in 6" :key="i" class="packages-page__skeleton-card" animated>
        <template #template>
          <el-skeleton-item variant="h3" style="width: 50%" />
          <el-skeleton-item variant="text" style="width: 80%; margin-top: 12px" />
          <el-skeleton-item variant="text" style="width: 40%; margin-top: 8px" />
        </template>
      </el-skeleton>
    </div>

    <!-- Error state -->
    <div v-else-if="errorMessage" class="packages-page__error" data-testid="packages-error">
      <el-alert
        type="error"
        :title="'加载智能体包失败'"
        :description="errorMessage"
        show-icon
        :closable="false"
      />
    </div>

    <!-- Empty state -->
    <div v-else-if="isEmpty" class="packages-page__empty">
      <el-empty
        description="磁盘上未找到智能体包。点击「新建」创建一个空白包，或「上传」导入已有包。"
        :image-size="120"
      />
    </div>

    <!-- Package cards grid -->
    <ul v-else class="packages-page__grid" data-testid="packages-grid">
      <li
        v-for="pkg in packages"
        :key="pkg.name"
        class="packages-page__card"
        :data-testid="`package-card-${pkg.name}`"
        @click="openPackage(pkg)"
        @keydown.enter="openPackage(pkg)"
        tabindex="0"
        role="button"
      >
        <div class="packages-page__card-head">
          <el-icon class="packages-page__card-icon"><FolderOpened /></el-icon>
          <span class="packages-page__card-name">{{ pkg.name }}</span>
          <el-dropdown
            class="packages-page__card-menu"
            trigger="click"
            @click.stop
            @command="(cmd: string) => { if (cmd === 'update') triggerUpdateFilePicker(pkg) }"
          >
            <el-button
              class="packages-page__card-menu-btn"
              size="small"
              :icon="MoreFilled"
              circle
              @click.stop
              @mousedown.stop
            />
            <template #dropdown>
              <el-dropdown-menu>
                <el-dropdown-item command="update">
                  <el-icon style="margin-right: 6px"><Upload /></el-icon>
                  上传更新
                </el-dropdown-item>
              </el-dropdown-menu>
            </template>
          </el-dropdown>
        </div>
        <div class="packages-page__card-path" :title="pkg.path">{{ pkg.path }}</div>
        <div class="packages-page__card-meta">
          <span>{{ pkg.file_count }} 个文件</span>
          <span class="packages-page__card-dot">·</span>
          <span>{{ formatDate(pkg.last_modified) }}</span>
        </div>
      </li>
    </ul>
  </section>
</template>

<style scoped>
.packages-page {
  padding: 24px 32px;
  height: 100%;
  display: flex;
  flex-direction: column;
  gap: 20px;
  overflow: auto;
}
.packages-page__header {
  display: flex;
  align-items: flex-start;
  justify-content: space-between;
  gap: 16px;
  padding-bottom: 4px;
}
.packages-page__header-actions {
  display: flex;
  gap: 8px;
  flex-shrink: 0;
}
.packages-page__title {
  margin: 0 0 4px;
  font-size: 24px;
  font-weight: 700;
  color: #1f2937;
  letter-spacing: 0.5px;
}
.packages-page__subtitle {
  margin: 0;
  color: #6b7280;
  font-size: 14px;
  line-height: 1.6;
}
.packages-page__skeleton {
  display: grid;
  grid-template-columns: repeat(auto-fill, minmax(260px, 1fr));
  gap: 16px;
}
.packages-page__skeleton-card {
  padding: 16px;
  border: 1px solid #ebeef5;
  border-radius: 10px;
  background: #ffffff;
}
.packages-page__error {
  max-width: 560px;
}
.packages-page__grid {
  list-style: none;
  margin: 0;
  padding: 0;
  display: grid;
  grid-template-columns: repeat(auto-fill, minmax(280px, 1fr));
  gap: 16px;
}
.packages-page__card {
  display: flex;
  flex-direction: column;
  gap: 10px;
  padding: 20px;
  border: 1px solid #ebeef5;
  border-radius: 12px;
  background: #ffffff;
  cursor: pointer;
  transition: border-color 0.2s ease, box-shadow 0.25s ease, transform 0.25s ease;
  outline: none;
}
.packages-page__card:hover,
.packages-page__card:focus-visible {
  border-color: #409eff;
  box-shadow: 0 8px 24px rgba(64, 158, 255, 0.12);
  transform: translateY(-2px);
}
.packages-page__card-head {
  display: flex;
  align-items: center;
  gap: 10px;
}
.packages-page__card-icon {
  font-size: 20px;
  color: #409eff;
}
.packages-page__card-name {
  font-size: 16px;
  font-weight: 600;
  color: #1f2937;
  font-family: 'JetBrains Mono', 'SF Mono', Menlo, monospace;
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
}
.packages-page__card-path {
  font-size: 12px;
  color: #9ca3af;
  font-family: 'JetBrains Mono', 'SF Mono', Menlo, monospace;
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
}
.packages-page__card-meta {
  display: flex;
  align-items: center;
  gap: 6px;
  font-size: 12px;
  color: #6b7280;
  margin-top: 4px;
}
.packages-page__card-dot {
  color: #c0c4cc;
}
.packages-page__card-menu {
  margin-left: auto;
  flex-shrink: 0;
}
.packages-page__card-menu-btn {
  border: none;
  background: transparent;
  color: #9ca3af;
  font-size: 16px;
}
.packages-page__card-menu-btn:hover {
  background: #f0f2f5;
  color: #409eff;
}

/* ── Update dialog ── */
.packages-update__body {
  padding: 4px 0;
}
.packages-update__row {
  display: flex;
  align-items: center;
  gap: 12px;
  padding: 6px 0;
}
.packages-update__label {
  font-size: 13px;
  font-weight: 600;
  color: #606266;
  min-width: 80px;
  flex-shrink: 0;
}
.packages-update__value {
  font-size: 13px;
  color: #303133;
  font-family: 'JetBrains Mono', 'SF Mono', Menlo, monospace;
  word-break: break-all;
}
.packages-update__mode {
  margin: 4px 0;
}
.packages-update__radios {
  display: flex;
  flex-direction: column;
  gap: 12px;
  margin-top: 8px;
}
.packages-update__radio {
  display: flex;
  align-items: flex-start;
  margin-right: 0;
  margin-bottom: 8px;
}
.packages-update__radio:last-child {
  margin-bottom: 0;
}
.packages-update__radio :deep(.el-radio) {
  display: flex;
  align-items: flex-start;
  height: auto;
  min-height: 22px;
}
.packages-update__radio :deep(.el-radio__input) {
  margin-top: 1px;
}
.packages-update__radio :deep(.el-radio__label) {
  padding-left: 8px;
  white-space: normal;
  line-height: 1.5;
}
.packages-update__radio-desc {
  margin: 2px 0 0;
  font-size: 12px;
  color: #909399;
  line-height: 1.4;
}
</style>
