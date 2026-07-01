<script setup lang="ts">
/**
 * WorkflowDAG.vue — Vue-Flow 工作流 DAG 可视化组件
 *
 * 渲染 workflow JSON 的节点和边为可交互的有向无环图。
 * 支持拖拽平移、滚轮缩放、节点悬停详情。
 */
import { computed, ref, watch } from 'vue'
import { VueFlow, useVueFlow, type Node, type Edge } from '@vue-flow/core'
import { MiniMap } from '@vue-flow/minimap'
import { Controls } from '@vue-flow/controls'
import '@vue-flow/core/dist/style.css'
import '@vue-flow/core/dist/theme-default.css'
import '@vue-flow/minimap/dist/style.css'
import '@vue-flow/controls/dist/style.css'

import { parseWorkflow, toVueFlowElements, validateWorkflow, type WorkflowDef } from '@/utils/workflow-parser'

const props = withDefaults(defineProps<{
  workflowJson: string | object
  nodeTypes?: Record<string, any>
}>(), {
  workflowJson: '',
  nodeTypes: () => ({}),
})

const emit = defineEmits<{
  'node-click': [nodeId: string]
  'error': [message: string]
}>()

// ── Parse & validate ─────────────────────────────────────

const parseError = ref<string | null>(null)
const workflowDef = ref<WorkflowDef | null>(null)
const selectedNode = ref<string | null>(null)

function parseJson() {
  parseError.value = null
  try {
    const raw = typeof props.workflowJson === 'string'
      ? JSON.parse(props.workflowJson)
      : props.workflowJson

    const validationErrors = validateWorkflow(raw)
    if (validationErrors.length > 0) {
      parseError.value = validationErrors.join('; ')
      emit('error', parseError.value)
      return
    }

    workflowDef.value = parseWorkflow(raw as any)
  } catch (e) {
    parseError.value = e instanceof Error ? e.message : String(e)
    emit('error', parseError.value)
  }
}

// ── Vue-Flow elements ────────────────────────────────────

const { nodes, edges, fitView, onNodeClick } = useVueFlow({ id: 'workflow-dag' })

function updateElements() {
  if (!workflowDef.value) return
  const { nodes: vfNodes, edges: vfEdges } = toVueFlowElements(workflowDef.value)
  nodes.value = vfNodes as any
  edges.value = vfEdges as any

  // Fit view after a tick for layout to settle
  setTimeout(() => fitView({ padding: 0.3 }), 50)
}

onNodeClick((ev) => {
  selectedNode.value = ev.node.id
  emit('node-click', ev.node.id)
})

// ── Reactive ──────────────────────────────────────────────

watch(() => props.workflowJson, () => {
  parseJson()
  updateElements()
}, { immediate: true })

// ── MiniMap node color (matches node style) ───────────────

const miniMapNodeColor = (node: Node) => (node.data as any)?.color ?? '#409eff'
</script>

<template>
  <div class="workflow-dag" data-testid="workflow-dag">
    <!-- Error state -->
    <div v-if="parseError" class="workflow-dag__error">
      <el-alert :title="parseError" type="error" show-icon :closable="false" />
    </div>

    <!-- Empty state -->
    <div v-else-if="!workflowDef || workflowDef.nodes.length === 0" class="workflow-dag__empty">
      <el-empty description="暂无工作流数据" />
    </div>

    <!-- DAG canvas -->
    <div v-else class="workflow-dag__canvas">
      <div class="workflow-dag__info">
        <span v-if="workflowDef.name" class="workflow-dag__name">{{ workflowDef.name }}</span>
        <span v-if="workflowDef.description" class="workflow-dag__desc">{{ workflowDef.description }}</span>
        <span class="workflow-dag__stats">{{ workflowDef.nodes.length }} 个节点 · {{ workflowDef.edges.length }} 条边</span>
      </div>

      <VueFlow
        id="workflow-dag"
        :nodes="nodes"
        :edges="edges"
        :default-viewport="{ zoom: 1.0 }"
        :min-zoom="0.3"
        :max-zoom="3"
        :nodes-draggable="false"
        :nodes-connectable="false"
        :edges-updatable="false"
        fit-view-on-init
      >
        <MiniMap :node-color="miniMapNodeColor" />
        <Controls />

        <template #node-default="nodeProps">
          <div
            class="workflow-dag__node"
            :style="{ borderLeftColor: (nodeProps.data as any)?.color }"
            @click="$emit('node-click', nodeProps.id)"
          >
            <div class="workflow-dag__node-label">{{ (nodeProps.data as any)?.label }}</div>
            <div v-if="(nodeProps.data as any)?.sublabel" class="workflow-dag__node-sublabel">
              {{ (nodeProps.data as any)?.sublabel }}
            </div>
            <div v-if="(nodeProps.data as any)?.description" class="workflow-dag__node-desc">
              {{ (nodeProps.data as any)?.description }}
            </div>
          </div>
        </template>
      </VueFlow>
    </div>
  </div>
</template>

<style scoped>
.workflow-dag {
  height: 100%;
  min-height: 300px;
}
.workflow-dag__info {
  padding: 8px 12px;
  background: #f5f7fa;
  border-bottom: 1px solid #e4e7ed;
  display: flex;
  align-items: center;
  gap: 12px;
  font-size: 13px;
}
.workflow-dag__name {
  font-weight: 600;
  color: #303133;
}
.workflow-dag__desc {
  color: #909399;
  flex: 1;
}
.workflow-dag__stats {
  color: #606266;
  font-size: 12px;
}
.workflow-dag__canvas {
  height: calc(100% - 37px);
  border: 1px solid #dcdfe6;
  border-radius: 4px;
  overflow: hidden;
}
.workflow-dag__error {
  padding: 16px;
}
.workflow-dag__empty {
  display: flex;
  align-items: center;
  justify-content: center;
  height: 100%;
  min-height: 200px;
}
.workflow-dag__node {
  padding: 8px 12px;
  background: #fff;
  border: 1px solid #dcdfe6;
  border-left: 4px solid #409eff;
  border-radius: 6px;
  cursor: pointer;
  min-width: 160px;
  box-shadow: 0 1px 4px rgba(0, 0, 0, 0.08);
  transition: box-shadow 0.2s;
}
.workflow-dag__node:hover {
  box-shadow: 0 2px 8px rgba(0, 0, 0, 0.15);
}
.workflow-dag__node-label {
  font-weight: 600;
  font-size: 13px;
  color: #303133;
}
.workflow-dag__node-sublabel {
  font-size: 11px;
  color: #909399;
  margin-top: 2px;
}
.workflow-dag__node-desc {
  font-size: 11px;
  color: #909399;
  margin-top: 4px;
  line-height: 1.4;
  display: -webkit-box;
  -webkit-line-clamp: 2;
  -webkit-box-orient: vertical;
  overflow: hidden;
}
</style>
