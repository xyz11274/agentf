<script setup lang="ts">
import { ref, onMounted } from 'vue'
import {
  FolderOpened,
  ChatDotRound,
  Collection,
  Expand,
  Fold,
} from '@element-plus/icons-vue'
import DaemonStatus from '../components/DaemonStatus.vue'
import AssistantWidget from '../components/AssistantWidget.vue'
import { useDaemonStore } from '../stores/daemonStore'

const collapsed = ref(false)
const daemonStore = useDaemonStore()

onMounted(() => {
  daemonStore.init()
})

const handleToggle = () => {
  collapsed.value = !collapsed.value
}
</script>

<template>
  <el-container class="layout">
    <el-aside
      class="layout__sidebar"
      :width="collapsed ? '64px' : '280px'"
      data-testid="sidebar"
    >
      <div class="sidebar__brand" :class="{ 'sidebar__brand--mini': collapsed }">
        <span v-if="!collapsed" class="sidebar__title">AgentFlow</span>
        <span v-else class="sidebar__title sidebar__title--mini">AF</span>
      </div>
      <div v-if="!collapsed" class="sidebar__slogan">AI 智能体工作流平台</div>

      <el-menu
        class="sidebar__menu"
        :collapse="collapsed"
        :router="true"
        :default-active="$route.path"
        background-color="transparent"
        text-color="#cfd6e4"
        active-text-color="#ffffff"
        data-testid="nav-menu"
      >
        <el-menu-item index="/packages">
          <el-icon><FolderOpened /></el-icon>
          <template #title>智能体包</template>
        </el-menu-item>
        <el-menu-item index="/chat">
          <el-icon><ChatDotRound /></el-icon>
          <template #title>Agent 问答</template>
        </el-menu-item>
        <el-menu-item index="/skill-hub">
          <el-icon><Collection /></el-icon>
          <template #title>技能中心</template>
        </el-menu-item>
      </el-menu>

      <div class="sidebar__status" :title="daemonStore.isConnected ? '已连接' : '已断开'">
        <span class="sidebar__dot" :class="{ 'sidebar__dot--on': daemonStore.isConnected }" />
        <span v-if="!collapsed" class="sidebar__status-label">
          {{ daemonStore.isConnected ? '已连接' : '已断开' }}
        </span>
      </div>
    </el-aside>

    <el-container>
      <el-header class="layout__header">
        <div class="header__left">
          <el-button
            class="header__toggle"
            :icon="collapsed ? Expand : Fold"
            circle
            plain
            @click="handleToggle"
            :aria-label="collapsed ? '展开侧边栏' : '收起侧边栏'"
          />
        </div>
        <div class="header__right">
          <DaemonStatus />
        </div>
      </el-header>

      <el-main class="layout__main" data-testid="main-content">
        <router-view />
      </el-main>
    </el-container>

    <!-- 全局气泡智能助手 -->
    <AssistantWidget />
  </el-container>
</template>

<style scoped>
.layout {
  height: 100vh;
  width: 100vw;
}

.layout__sidebar {
  background: linear-gradient(180deg, #1f2937 0%, #111827 100%);
  color: #e5e7eb;
  display: flex;
  flex-direction: column;
  transition: width 0.3s cubic-bezier(0.4, 0, 0.2, 1);
  border-right: 1px solid rgba(255, 255, 255, 0.04);
  overflow: hidden;
  box-shadow: 2px 0 12px rgba(0, 0, 0, 0.06);
}

.sidebar__brand {
  display: flex;
  align-items: center;
  height: 56px;
  padding: 0 20px;
  border-bottom: 1px solid rgba(255, 255, 255, 0.06);
  flex-shrink: 0;
  background: rgba(255, 255, 255, 0.02);
}
.sidebar__brand--mini {
  justify-content: center;
  padding: 0;
}
.sidebar__title {
  font-size: 18px;
  font-weight: 700;
  letter-spacing: 0.8px;
  color: #f9fafb;
  white-space: nowrap;
  background: linear-gradient(135deg, #60a5fa, #a78bfa);
  -webkit-background-clip: text;
  -webkit-text-fill-color: transparent;
  background-clip: text;
}
.sidebar__title--mini {
  font-size: 14px;
  letter-spacing: 1px;
}

.sidebar__slogan {
  padding: 4px 20px 12px;
  font-size: 11px;
  color: #6b7280;
  white-space: nowrap;
  border-bottom: 1px solid rgba(255, 255, 255, 0.06);
  letter-spacing: 0.3px;
}

.sidebar__menu {
  flex: 1 1 auto;
  border-right: 0 !important;
  --el-menu-bg-color: transparent;
  --el-menu-text-color: #cfd6e4;
  --el-menu-hover-bg-color: rgba(255, 255, 255, 0.06);
}
.sidebar__menu :deep(.el-menu-item) {
  margin: 4px 8px;
  border-radius: 8px;
  height: 44px;
  line-height: 44px;
  transition: all 0.2s ease;
}
.sidebar__menu :deep(.el-menu-item.is-active) {
  background: linear-gradient(135deg, rgba(64, 158, 255, 0.2), rgba(96, 165, 250, 0.12));
  color: #ffffff;
  box-shadow: inset 0 0 0 1px rgba(64, 158, 255, 0.15);
}
.sidebar__menu :deep(.el-menu-item:hover) {
  background: rgba(255, 255, 255, 0.08) !important;
}
.sidebar__menu :deep(.el-menu-item .el-icon) {
  font-size: 18px;
}

.sidebar__status {
  display: flex;
  align-items: center;
  gap: 8px;
  padding: 14px 20px;
  border-top: 1px solid rgba(255, 255, 255, 0.06);
  font-size: 12px;
  color: #9ca3af;
  flex-shrink: 0;
  background: rgba(0, 0, 0, 0.08);
}
.sidebar__status-label {
  white-space: nowrap;
}
.sidebar__dot {
  width: 8px;
  height: 8px;
  border-radius: 50%;
  background: #f56c6c;
  box-shadow: 0 0 0 2px rgba(245, 108, 108, 0.18);
  flex-shrink: 0;
}
.sidebar__dot--on {
  background: #67c23a;
  box-shadow: 0 0 0 2px rgba(103, 194, 58, 0.22);
}

.layout__header {
  display: flex;
  align-items: center;
  justify-content: space-between;
  background: #ffffff;
  border-bottom: 1px solid #ebeef5;
  padding: 0 20px;
  height: 56px;
}
.header__toggle {
  border: none;
}
.header__right {
  display: flex;
  align-items: center;
}

.layout__main {
  background: #f5f7fa;
  padding: 0;
  overflow: auto;
}
</style>
