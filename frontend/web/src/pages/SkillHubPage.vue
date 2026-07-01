<script setup lang="ts">
import { ref, onMounted, computed } from 'vue'
import { useRouter } from 'vue-router'
import { Upload, Delete } from '@element-plus/icons-vue'
import { ElMessage, ElMessageBox } from 'element-plus'
import { useSkillHubStore } from '@/stores/skillHubStore'

const router = useRouter()
const store = useSkillHubStore()

const uploadDialogVisible = ref(false)
const uploadFile = ref<File | null>(null)
const uploadName = ref('')
const uploadOverwrite = ref(false)
const uploading = ref(false)

const filterText = ref('')

const filteredSkills = computed(() => {
  if (!filterText.value) return store.skills
  const q = filterText.value.toLowerCase()
  return store.skills.filter(
    (s) =>
      s.name.toLowerCase().includes(q) ||
      s.description.toLowerCase().includes(q) ||
      s.tags.some((t) => t.toLowerCase().includes(q)),
  )
})

onMounted(async () => {
  try {
    await store.loadSkills()
  } catch {
    ElMessage.error('加载技能列表失败')
  }
})

function handleView(name: string) {
  router.push(`/skill-hub/${encodeURIComponent(name)}`)
}

async function handleDelete(name: string) {
  try {
    await ElMessageBox.confirm(`确定删除技能「${name}」？此操作不可恢复。`, '确认删除', {
      confirmButtonText: '删除',
      cancelButtonText: '取消',
      type: 'warning',
    })
    await store.deleteSkill(name)
    ElMessage.success(`技能「${name}」已删除`)
  } catch {
    // cancelled
  }
}

function handleUploadClick() {
  uploadFile.value = null
  uploadName.value = ''
  uploadOverwrite.value = false
  uploadDialogVisible.value = true
}

async function handleUploadSubmit() {
  if (!uploadFile.value) {
    ElMessage.warning('请选择文件')
    return
  }
  uploading.value = true
  try {
    await store.uploadSkill(
      uploadFile.value,
      uploadName.value || undefined,
      uploadOverwrite.value,
    )
    ElMessage.success('上传成功')
    uploadDialogVisible.value = false
  } catch (e) {
    ElMessage.error(`上传失败: ${e instanceof Error ? e.message : String(e)}`)
  } finally {
    uploading.value = false
  }
}

function handleFileChange(e: Event) {
  const target = e.target as HTMLInputElement
  if (target.files && target.files.length > 0) {
    uploadFile.value = target.files[0]
    // Auto-fill name from filename if not manually set
    if (!uploadName.value) {
      const fn = target.files[0].name
      // Strip extension
      const stem = fn.replace(/\.(md|zip|tar\.gz|tgz)$/i, '')
      uploadName.value = stem
    }
  }
}
</script>

<template>
  <div class="skill-hub">
    <!-- Header -->
    <div class="skill-hub__header">
      <h1 class="skill-hub__title">技能中心</h1>
      <div class="skill-hub__actions">
        <el-input
          v-model="filterText"
          class="skill-hub__search"
          placeholder="搜索技能..."
          clearable
          prefix-icon="Search"
        />
        <el-button type="primary" :icon="Upload" @click="handleUploadClick">
          上传技能
        </el-button>
      </div>
    </div>

    <!-- Loading -->
    <div v-if="store.isLoading && store.skills.length === 0" class="skill-hub__loading">
      <el-skeleton :rows="4" animated />
    </div>

    <!-- Error -->
    <el-alert
      v-if="store.error"
      :title="store.error"
      type="error"
      show-icon
      closable
      class="skill-hub__error"
    />

    <!-- Empty state -->
    <el-empty v-if="!store.isLoading && filteredSkills.length === 0" description="暂无技能">
      <el-button type="primary" :icon="Upload" @click="handleUploadClick">
        上传第一个技能
      </el-button>
    </el-empty>

    <!-- Skill cards grid -->
    <div v-if="filteredSkills.length > 0" class="skill-hub__grid">
      <div
        v-for="skill in filteredSkills"
        :key="skill.name"
        class="skill-card"
        @click="handleView(skill.name)"
      >
        <div class="skill-card__icon">📄</div>
        <div class="skill-card__body">
          <h3 class="skill-card__name">{{ skill.name }}</h3>
          <p class="skill-card__desc">{{ skill.description || '暂无描述' }}</p>
          <div class="skill-card__meta">
            <div class="skill-card__tags">
              <el-tag
                v-for="tag in skill.tags.slice(0, 3)"
                :key="tag"
                size="small"
                type="info"
                class="skill-card__tag"
              >
                {{ tag }}
              </el-tag>
              <span v-if="skill.tags.length > 3" class="skill-card__more">+{{ skill.tags.length - 3 }}</span>
            </div>
            <span class="skill-card__version">v{{ skill.version }}</span>
          </div>
        </div>
        <div class="skill-card__actions" @click.stop>
          <el-button size="small" @click="handleView(skill.name)">查看</el-button>
          <el-button size="small" type="danger" :icon="Delete" @click="handleDelete(skill.name)" />
        </div>
      </div>
    </div>

    <!-- Upload dialog -->
    <el-dialog v-model="uploadDialogVisible" title="上传技能" width="480px">
      <el-form label-width="100px">
        <el-form-item label="技能文件">
          <input
            type="file"
            accept=".md,.zip,.tar.gz,.tgz"
            @change="handleFileChange"
            class="skill-hub__file-input"
          />
          <div v-if="uploadFile" class="skill-hub__file-info">
            已选择: {{ uploadFile.name }} ({{ (uploadFile.size / 1024).toFixed(1) }} KB)
          </div>
        </el-form-item>
        <el-form-item label="技能名称">
          <el-input v-model="uploadName" placeholder="留空则从文件名自动推断" />
        </el-form-item>
        <el-form-item label="覆盖已存在">
          <el-switch v-model="uploadOverwrite" />
          <span class="skill-hub__hint">开启后将替换同名技能</span>
        </el-form-item>
      </el-form>
      <template #footer>
        <el-button @click="uploadDialogVisible = false">取消</el-button>
        <el-button type="primary" :loading="uploading" @click="handleUploadSubmit">
          {{ uploading ? '上传中...' : '上传' }}
        </el-button>
      </template>
    </el-dialog>
  </div>
</template>

<style scoped>
.skill-hub {
  padding: 24px;
  height: 100%;
  overflow-y: auto;
}

.skill-hub__header {
  display: flex;
  justify-content: space-between;
  align-items: center;
  margin-bottom: 24px;
  flex-wrap: wrap;
  gap: 12px;
}

.skill-hub__title {
  margin: 0;
  font-size: 24px;
  font-weight: 600;
  color: #1f2937;
}

.skill-hub__actions {
  display: flex;
  gap: 12px;
  align-items: center;
}

.skill-hub__search {
  width: 240px;
}

.skill-hub__loading {
  padding: 40px;
}

.skill-hub__error {
  margin-bottom: 16px;
}

.skill-hub__grid {
  display: grid;
  grid-template-columns: repeat(auto-fill, minmax(320px, 1fr));
  gap: 16px;
}

.skill-card {
  background: #ffffff;
  border: 1px solid #e5e7eb;
  border-radius: 12px;
  padding: 20px;
  cursor: pointer;
  transition: all 0.2s ease;
  display: flex;
  gap: 16px;
  align-items: flex-start;
}

.skill-card:hover {
  border-color: #60a5fa;
  box-shadow: 0 4px 12px rgba(96, 165, 250, 0.12);
  transform: translateY(-2px);
}

.skill-card__icon {
  font-size: 32px;
  line-height: 1;
  flex-shrink: 0;
}

.skill-card__body {
  flex: 1;
  min-width: 0;
}

.skill-card__name {
  margin: 0 0 4px;
  font-size: 16px;
  font-weight: 600;
  color: #1f2937;
}

.skill-card__desc {
  margin: 0 0 8px;
  font-size: 13px;
  color: #6b7280;
  line-height: 1.4;
  display: -webkit-box;
  -webkit-line-clamp: 2;
  -webkit-box-orient: vertical;
  overflow: hidden;
}

.skill-card__meta {
  display: flex;
  justify-content: space-between;
  align-items: center;
  gap: 8px;
}

.skill-card__tags {
  display: flex;
  gap: 4px;
  flex-wrap: wrap;
  align-items: center;
}

.skill-card__tag {
  max-width: 80px;
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
}

.skill-card__more {
  font-size: 11px;
  color: #9ca3af;
}

.skill-card__version {
  font-size: 12px;
  color: #9ca3af;
  white-space: nowrap;
}

.skill-card__actions {
  display: flex;
  gap: 4px;
  flex-shrink: 0;
  padding-top: 4px;
}

.skill-hub__file-input {
  width: 100%;
}

.skill-hub__file-info {
  font-size: 12px;
  color: #6b7280;
  margin-top: 4px;
}

.skill-hub__hint {
  font-size: 12px;
  color: #9ca3af;
  margin-left: 8px;
}
</style>
