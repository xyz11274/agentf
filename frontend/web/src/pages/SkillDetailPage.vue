<script setup lang="ts">
import { ref, onMounted } from 'vue'
import { useRoute, useRouter } from 'vue-router'
import { ArrowLeft } from '@element-plus/icons-vue'
import { ElMessage, ElMessageBox } from 'element-plus'
import { useSkillHubStore } from '@/stores/skillHubStore'
import { usePackageStore } from '@/stores/packageStore'
import MarkdownContent from '@/components/MarkdownContent.vue'

const route = useRoute()
const router = useRouter()
const store = useSkillHubStore()
const packageStore = usePackageStore()

const skillName = route.params.name as string
const installDialogVisible = ref(false)
const selectedPackage = ref('')
const packages = ref<{ name: string; label: string }[]>([])
const installing = ref(false)

const skill = computed(() => store.currentSkill)

onMounted(async () => {
  try {
    await store.loadSkillDetail(skillName)
  } catch {
    ElMessage.error('加载技能详情失败')
  }
  // Load packages for install target selector
  try {
    await packageStore.loadPackages()
    packages.value = packageStore.packages.map((p) => ({
      name: p.name,
      label: `${p.name} (${p.file_count} 文件)`,
    }))
  } catch {
    // Packages list is optional for this page
  }
})

async function handleInstall() {
  if (!selectedPackage.value) {
    ElMessage.warning('请选择目标 Agent')
    return
  }
  installing.value = true
  try {
    const res = await store.installSkill(skillName, selectedPackage.value)
    ElMessage.success(`技能已安装到「${selectedPackage.value}」`)
    installDialogVisible.value = false
  } catch (e) {
    ElMessage.error(`安装失败: ${e instanceof Error ? e.message : String(e)}`)
  } finally {
    installing.value = false
  }
}

async function handleDelete() {
  try {
    await ElMessageBox.confirm(
      `确定从中心仓库删除技能「${skillName}」？\n已安装到 Agent 的技能不受影响。`,
      '确认删除',
      { confirmButtonText: '删除', cancelButtonText: '取消', type: 'warning' },
    )
    await store.deleteSkill(skillName)
    ElMessage.success('技能已删除')
    router.push('/skill-hub')
  } catch {
    // cancelled
  }
}

function goBack() {
  router.push('/skill-hub')
}
</script>

<template>
  <div class="skill-detail">
    <!-- Header -->
    <div class="skill-detail__header">
      <el-button :icon="ArrowLeft" text @click="goBack">返回技能中心</el-button>
      <div class="skill-detail__header-right">
        <el-button type="primary" @click="installDialogVisible = true">
          安装到 Agent
        </el-button>
        <el-button type="danger" plain @click="handleDelete">删除</el-button>
      </div>
    </div>

    <!-- Loading -->
    <div v-if="store.isLoading" class="skill-detail__loading">
      <el-skeleton :rows="8" animated />
    </div>

    <!-- Error -->
    <el-alert
      v-if="store.error"
      :title="store.error"
      type="error"
      show-icon
      closable
    />

    <!-- Content -->
    <div v-if="skill" class="skill-detail__body">
      <!-- Metadata -->
      <div class="skill-detail__meta">
        <h1 class="skill-detail__name">{{ skill.name }}</h1>
        <p class="skill-detail__desc">{{ skill.description || '暂无描述' }}</p>
        <div class="skill-detail__info">
          <div class="skill-detail__info-item">
            <span class="skill-detail__label">版本</span>
            <span class="skill-detail__value">v{{ skill.version }}</span>
          </div>
          <div class="skill-detail__info-item">
            <span class="skill-detail__label">文件数</span>
            <span class="skill-detail__value">{{ skill.file_count }}</span>
          </div>
          <div class="skill-detail__info-item">
            <span class="skill-detail__label">更新于</span>
            <span class="skill-detail__value">{{ skill.last_modified }}</span>
          </div>
        </div>
        <div v-if="skill.tags.length > 0" class="skill-detail__tags">
          <el-tag
            v-for="tag in skill.tags"
            :key="tag"
            size="small"
            type="info"
            class="skill-detail__tag"
          >
            {{ tag }}
          </el-tag>
        </div>
      </div>

      <!-- SKILL.md content -->
      <div class="skill-detail__content">
        <h2 class="skill-detail__section-title">技能内容</h2>
        <div class="skill-detail__markdown">
          <MarkdownContent :content="skill.content" />
        </div>
      </div>
    </div>

    <!-- Install dialog -->
    <el-dialog v-model="installDialogVisible" title="安装技能到 Agent" width="420px">
      <el-form label-width="100px">
        <el-form-item label="技能名称">
          <el-input :model-value="skillName" disabled />
        </el-form-item>
        <el-form-item label="目标 Agent">
          <el-select
            v-model="selectedPackage"
            placeholder="选择 Agent Package"
            style="width: 100%"
          >
            <el-option
              v-for="pkg in packages"
              :key="pkg.name"
              :label="pkg.label"
              :value="pkg.name"
            />
          </el-select>
        </el-form-item>
      </el-form>
      <template #footer>
        <el-button @click="installDialogVisible = false">取消</el-button>
        <el-button type="primary" :loading="installing" @click="handleInstall">
          {{ installing ? '安装中...' : '安装' }}
        </el-button>
      </template>
    </el-dialog>
  </div>
</template>

<style scoped>
.skill-detail {
  padding: 24px;
  height: 100%;
  overflow-y: auto;
}

.skill-detail__header {
  display: flex;
  justify-content: space-between;
  align-items: center;
  margin-bottom: 24px;
}

.skill-detail__header-right {
  display: flex;
  gap: 8px;
}

.skill-detail__loading {
  padding: 40px;
}

.skill-detail__body {
  max-width: 960px;
}

.skill-detail__meta {
  background: #ffffff;
  border: 1px solid #e5e7eb;
  border-radius: 12px;
  padding: 24px;
  margin-bottom: 24px;
}

.skill-detail__name {
  margin: 0 0 8px;
  font-size: 28px;
  font-weight: 700;
  color: #1f2937;
}

.skill-detail__desc {
  margin: 0 0 16px;
  font-size: 15px;
  color: #6b7280;
  line-height: 1.5;
}

.skill-detail__info {
  display: flex;
  gap: 32px;
  margin-bottom: 12px;
}

.skill-detail__info-item {
  display: flex;
  flex-direction: column;
  gap: 2px;
}

.skill-detail__label {
  font-size: 12px;
  color: #9ca3af;
  text-transform: uppercase;
  letter-spacing: 0.5px;
}

.skill-detail__value {
  font-size: 14px;
  color: #374151;
  font-weight: 500;
}

.skill-detail__tags {
  display: flex;
  gap: 6px;
  flex-wrap: wrap;
}

.skill-detail__tag {
  font-size: 12px;
}

.skill-detail__content {
  background: #ffffff;
  border: 1px solid #e5e7eb;
  border-radius: 12px;
  padding: 24px;
}

.skill-detail__section-title {
  margin: 0 0 16px;
  font-size: 18px;
  font-weight: 600;
  color: #1f2937;
}

.skill-detail__markdown {
  line-height: 1.7;
  color: #374151;
}
</style>
