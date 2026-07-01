<script setup lang="ts">
/**
 * FileTree — package-scoped file browser for AgentFlow.
 *
 * Two modes:
 *   1. Single-package (packageName is set): loads the full tree from bridge,
 *      wraps in a root node, auto-expands.
 *   2. All-packages (packageName unset): shows every package directory on
 *      disk as a root node (from packageStore.packages).
 *
 * Click a file → emit `file-selected` with `{ package, path, name }`.
 */
import { usePackageStore } from '@/stores/packageStore'
import type { FileNode } from '@/types/agentflow'
import {
  Delete,
  Document,
  EditPen,
  Files,
  Folder,
  FolderAdd,
  FolderOpened,
  Monitor,
  Plus,
} from '@element-plus/icons-vue'
import { computed, nextTick, onMounted, ref, watch } from 'vue'

interface TreeNodeData extends FileNode {
  isPackage?: boolean
}

interface MenuContext {
  package: string
  node: TreeNodeData
}

const props = defineProps<{
  packageName?: string
}>()

const emit = defineEmits<{
  (e: 'file-selected', payload: { package: string; path: string; name: string }): void
  (
    e: 'node-action',
    action: 'new-file' | 'new-dir' | 'rename' | 'delete',
    payload: MenuContext,
  ): void
}>()

const packageStore = usePackageStore()
const treeRef = ref<any>(null)

// ── Default exclude dirs (configurable) ──────────────
const EXCLUDE_DIRS = ['.venv', 'venv', 'node_modules']

function filterTree(nodes: FileNode[]): FileNode[] {
  return nodes
    .filter(n => !(n.type === 'dir' && EXCLUDE_DIRS.includes(n.name)))
    .map(n => ({
      ...n,
      children: n.children ? filterTree(n.children) : undefined,
    }))
}

// The actual tree data fed into el-tree.
const treeData = ref<TreeNodeData[]>([])
const isLoading = ref(false)
const errorMessage = ref<string | null>(null)
const expandedKeys = ref<string[]>([])

const treeProps = {
  label: 'name',
  children: 'children',
  isLeaf: (data: TreeNodeData) => data.type === 'file',
}

const isEmpty = computed(
  () => !isLoading.value && !errorMessage.value && treeData.value.length === 0,
)

const contextMenu = ref<{
  visible: boolean
  top: number
  left: number
  node: TreeNodeData | null
}>({
  visible: false,
  top: 0,
  left: 0,
  node: null,
})

function getNodePackage(node: TreeNodeData | undefined | null): string | null {
  if (!node) return null
  if (node.isPackage) return node.name
  return packageStore.currentPackage
}

// ── Data loading ───────────────────────────────────────────

async function loadPackageTree(pkg: string): Promise<FileNode[]> {
  const tree = await packageStore.loadFileTree(pkg)
  return tree
}

async function loadForPackage(name: string) {
  isLoading.value = true
  errorMessage.value = null
  packageStore.setCurrentPackage(name)
  try {
    const fullTree = await loadPackageTree(name)
    treeData.value = [{
      name: name,
      path: name,
      type: 'dir',
      isPackage: true,
      children: filterTree(fullTree),
    }]
    await nextTick()
    expandedKeys.value = [name]
  } catch (e) {
    errorMessage.value = e instanceof Error ? e.message : String(e)
    treeData.value = []
  } finally {
    isLoading.value = false
  }
}

async function loadAllPackages() {
  isLoading.value = true
  errorMessage.value = null
  try {
    const pkgs = await packageStore.loadPackages()
    treeData.value = pkgs.map((p) => ({
      name: p.name,
      path: p.name,
      type: 'dir' as const,
      isPackage: true,
    }))
  } catch (e) {
    errorMessage.value = e instanceof Error ? e.message : String(e)
  } finally {
    isLoading.value = false
  }
}

// ── Lifecycle ──────────────────────────────────────────────

onMounted(async () => {
  if (props.packageName) {
    await loadForPackage(props.packageName)
  } else {
    await loadAllPackages()
  }
})

watch(() => props.packageName, async (newName) => {
  treeData.value = []
  expandedKeys.value = []
  if (newName) {
    await loadForPackage(newName)
  } else {
    await loadAllPackages()
  }
})

// ── File click ─────────────────────────────────────────────

function handleNodeClick(data: TreeNodeData): void {
  if (data.type !== 'file') return
  const pkg = getNodePackage(data)
  if (!pkg) return
  emit('file-selected', { package: pkg, path: data.path, name: data.name })
}

function handleRightClick(event: MouseEvent, data: TreeNodeData): void {
  event.preventDefault()
  contextMenu.value = {
    visible: true,
    top: event.clientY,
    left: event.clientX,
    node: data,
  }
  window.addEventListener('click', closeContextMenu, { once: true })
  window.addEventListener('contextmenu', closeContextMenu, { once: true })
}

function closeContextMenu(): void {
  if (!contextMenu.value.visible) return
  contextMenu.value = { ...contextMenu.value, visible: false, node: null }
}

function handleMenuCommand(action: 'new-file' | 'new-dir' | 'rename' | 'delete'): void {
  const node = contextMenu.value.node
  closeContextMenu()
  if (!node) return
  const pkg = getNodePackage(node)
  if (!pkg) return
  emit('node-action', action, { package: pkg, node })
}

function fileIcon(name: string) {
  const lower = name.toLowerCase()
  if (lower.endsWith('.md')) return EditPen
  if (lower.endsWith('.sh') || lower.endsWith('.py')) return Monitor
  return Document
}

function nodeTestId(data: TreeNodeData): string {
  const pkg = getNodePackage(data) ?? 'unknown'
  const prefix = data.type === 'dir' ? 'dir' : 'file'
  return `${prefix}-${pkg}-${data.name}`
}
</script>

<template>
  <div class="file-tree" data-testid="file-tree">
    <header class="file-tree__header">
      <span class="file-tree__title">
        <el-icon><Files /></el-icon>
        <span>文件树</span>
      </span>
      <span
        v-if="isLoading"
        class="file-tree__loading"
        data-testid="file-tree-loading"
      >
        加载中…
      </span>
    </header>

    <p v-if="errorMessage" class="file-tree__error" data-testid="file-tree-error">
      {{ errorMessage }}
    </p>

    <p
      v-else-if="isEmpty"
      class="file-tree__empty"
      data-testid="file-tree-empty"
    >
      未找到包
    </p>

    <el-tree
      v-else
      ref="treeRef"
      class="file-tree__tree"
      :data="treeData"
      :props="treeProps"
      :node-key="'path'"
      :default-expanded-keys="expandedKeys"
      highlight-current
      empty-text="暂无文件"
      @node-click="handleNodeClick"
      @node-contextmenu="handleRightClick"
    >
      <template #default="{ data }">
        <span
          class="file-tree__node"
          :data-testid="nodeTestId(data as TreeNodeData)"
        >
          <el-icon v-if="data.type === 'dir'" class="file-tree__icon file-tree__icon--dir">
            <FolderOpened />
          </el-icon>
          <el-icon v-else class="file-tree__icon">
            <component :is="fileIcon(data.name)" />
          </el-icon>
          <span class="file-tree__label">{{ data.name }}</span>
        </span>
      </template>
    </el-tree>

    <ul
      v-if="contextMenu.visible && contextMenu.node"
      class="file-tree__context-menu"
      :style="{ top: `${contextMenu.top}px`, left: `${contextMenu.left}px` }"
      data-testid="file-tree-context-menu"
      role="menu"
    >
      <li
        v-if="contextMenu.node.type === 'dir'"
        class="file-tree__context-item"
        role="menuitem"
        data-testid="ctx-new-file"
        @click="handleMenuCommand('new-file')"
      >
        <el-icon><Plus /></el-icon>
        <span>新建文件</span>
      </li>
      <li
        v-if="contextMenu.node.type === 'dir'"
        class="file-tree__context-item"
        role="menuitem"
        data-testid="ctx-new-dir"
        @click="handleMenuCommand('new-dir')"
      >
        <el-icon><FolderAdd /></el-icon>
        <span>新建目录</span>
      </li>
      <li
        class="file-tree__context-item"
        role="menuitem"
        data-testid="ctx-rename"
        @click="handleMenuCommand('rename')"
      >
        <el-icon><EditPen /></el-icon>
        <span>重命名</span>
      </li>
      <li
        class="file-tree__context-item file-tree__context-item--danger"
        role="menuitem"
        data-testid="ctx-delete"
        @click="handleMenuCommand('delete')"
      >
        <el-icon><Delete /></el-icon>
        <span>删除</span>
      </li>
    </ul>
  </div>
</template>

<style scoped>
.file-tree {
  display: flex;
  flex-direction: column;
  height: 100%;
  background: #ffffff;
  border-right: 1px solid #e4e7ed;
  font-size: 13px;
  color: #303133;
  overflow: hidden;
}

.file-tree__header {
  display: flex;
  align-items: center;
  justify-content: space-between;
  padding: 12px 16px;
  border-bottom: 1px solid #e4e7ed;
  font-weight: 600;
  letter-spacing: 0.2px;
  background: #f8f9fb;
  flex-shrink: 0;
}

.file-tree__title {
  display: inline-flex;
  align-items: center;
  gap: 6px;
  color: #1f2937;
  font-size: 14px;
}

.file-tree__loading {
  font-size: 12px;
  color: #909399;
  font-weight: 400;
}

.file-tree__error,
.file-tree__empty {
  margin: 0;
  padding: 16px;
  font-size: 13px;
  color: #909399;
}

.file-tree__error {
  color: #f56c6c;
}

.file-tree__tree {
  flex: 1 1 auto;
  overflow: auto;
  padding: 8px 4px;
}

.file-tree__node {
  display: inline-flex;
  align-items: center;
  gap: 6px;
  width: 100%;
  font-size: 13px;
}

.file-tree__icon {
  font-size: 15px;
  color: #606266;
}

.file-tree__icon--dir {
  color: #e6a23c;
}

.file-tree__label {
  flex: 1 1 auto;
  white-space: nowrap;
  overflow: hidden;
  text-overflow: ellipsis;
}

.file-tree__context-menu {
  position: fixed;
  z-index: 3000;
  min-width: 160px;
  margin: 0;
  padding: 4px 0;
  list-style: none;
  background: #ffffff;
  border: 1px solid #e4e7ed;
  border-radius: 8px;
  box-shadow: 0 4px 16px rgba(0, 0, 0, 0.12);
  overflow: hidden;
}

.file-tree__context-item {
  display: flex;
  align-items: center;
  gap: 8px;
  padding: 8px 14px;
  cursor: pointer;
  font-size: 13px;
  color: #303133;
  user-select: none;
  transition: background-color 0.12s ease;
}

.file-tree__context-item:hover {
  background: #f0f5ff;
}

.file-tree__context-item--danger {
  color: #f56c6c;
}

.file-tree__context-item--danger:hover {
  background: rgba(245, 108, 108, 0.06);
}
</style>
