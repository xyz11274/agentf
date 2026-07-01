<script setup lang="ts">
/**
 * PackageCreator.vue — AgentFlow 智能体包可视化创建器
 * ------------------------------------------------------------------
 *
 * 编辑的是 agent.json 层的 Agent 定义，每个画布节点代表一个 Task。
 *
 * Task 类型（按 AgentFlow agent.json 规范）:
 *   contextupdater — 更新 agent 上下文（有 scope）
 *   dispatcher     — 分发用户请求（有 next_skills / max_concurrent_skills）
 *   coordinator    — 协调 task/skill 输出
 *   skill          — 执行具体技能（有 skill 子对象: name / config_dir / instance_pool_size）
 *
 * 典型编排流: contextupdater → dispatcher → skill(s) → coordinator
 */
import { computed, ref, markRaw, h, watch } from 'vue'
import { useRouter, useRoute } from 'vue-router'
import { ElMessage } from 'element-plus'
import { ArrowLeft, Delete, Plus, TopRight } from '@element-plus/icons-vue'
import {
  VueFlow, useVueFlow, Handle, Position,
  type NodeProps, type Connection,
  type Node as VfNode, type Edge as VfEdge,
} from '@vue-flow/core'
import { MiniMap } from '@vue-flow/minimap'
import { Controls } from '@vue-flow/controls'
import '@vue-flow/core/dist/style.css'
import '@vue-flow/core/dist/theme-default.css'
import '@vue-flow/minimap/dist/style.css'
import '@vue-flow/controls/dist/style.css'
import { usePackageStore } from '@/stores/packageStore'
import { getApiClient } from '@/api/context'

// ═══════════════════════════════════════════════════════════════════════
// Props
// ═══════════════════════════════════════════════════════════════════════

const props = withDefaults(defineProps<{
  /** 只读模式：不可增删节点/连线，仅可编辑属性面板 */
  readonly?: boolean
  /** readonly 模式下加载的智能体包名，自动读取 agent.json */
  packageName?: string
}>(), {
  readonly: false,
  packageName: '',
})

// ═══════════════════════════════════════════════════════════════════════
// AgentFlow agent.json 数据模型
// ═══════════════════════════════════════════════════════════════════════

/** Skill 子对象（仅 skills 类型 task） */
interface SkillDef {
  name: string
  config_dir: string
  instance_pool_size: number
}

/** agent.md 中的 model 配置 */
interface ModelConfig {
  provider: string
  name: string
  base_url: string
  api_key: string
}

/** agent.json 中的单个 task 定义 */
interface AgentTaskDef {
  id: number
  type: 'contextupdater' | 'dispatcher' | 'coordinator' | 'skill' | 'agent_def'
  description?: string
  scope?: string                // contextupdater 特有
  workflow_file?: string         // 关联的工作流 JSON 文件路径
  workflow_pool_size?: number
  next_skills?: string[]         // dispatcher 特有
  max_concurrent_skills?: number // dispatcher 特有
  skill?: SkillDef               // skill 特有
  model?: ModelConfig            // agent_def 特有
  agentSkills?: string[]         // agent_def 特有（agent.md skills 列表）
  system_prompt?: string         // agent_def 特有（agent.md body）
}

/** 工具箱条目 */
interface ToolboxItem {
  type: AgentTaskDef['type']
  label: string
  icon: string
  color: string
  description: string
  defaultTask: Partial<AgentTaskDef>
}

// ═══════════════════════════════════════════════════════════════════════
// Task 类型定义（agent_def 可选拖入）
// ═══════════════════════════════════════════════════════════════════════

const TASK_TYPES: Record<string, ToolboxItem> = {
  agent_def: {
    type: 'agent_def',
    label: 'Agent 定义',
    icon: '🤖',
    color: '#6B7280',
    description: 'Agent 元配置（模型、技能清单）',
    defaultTask: {
      model: { provider: 'openai', name: 'gpt-4', base_url: '', api_key: '' },
      agentSkills: [],
    },
  },
  contextupdater: {
    type: 'contextupdater',
    label: '上下文更新',
    icon: '🔄',
    color: '#8B5CF6',     // 紫色
    description: '更新 agent 上下文状态',
    defaultTask: { scope: 'agent', workflow_pool_size: 1 },
  },
  dispatcher: {
    type: 'dispatcher',
    label: '分发器',
    icon: '📤',
    color: '#F59E0B',     // 琥珀色
    description: '分发用户请求到 skills',
    defaultTask: { workflow_pool_size: 2, max_concurrent_skills: 5 },
  },
  coordinator: {
    type: 'coordinator',
    label: '协调器',
    icon: '🔗',
    color: '#3B82F6',     // 蓝色
    description: '协调 task/skill 输出',
    defaultTask: { workflow_pool_size: 1 },
  },
  skill: {
    type: 'skill',
    label: '技能',
    icon: '⚡',
    color: '#10B981',     // 翠绿
    description: '执行具体技能任务',
    defaultTask: {
      workflow_pool_size: 1,
      skill: { name: 'my-skill', config_dir: 'skills/my-skill', instance_pool_size: 1 },
    },
  },
}

const harnessItems = computed<ToolboxItem[]>(() =>
  Object.values(TASK_TYPES).filter(t => t.type === 'agent_def'),
)
const taskItems = computed<ToolboxItem[]>(() =>
  Object.values(TASK_TYPES).filter(t => t.type !== 'agent_def'),
)

let seqId = 0
function nextTaskId(): number { seqId += 1; return seqId }
function nextNodeId(): string { return `task-${seqId}` }

// ═══════════════════════════════════════════════════════════════════════
// VueFlow 自定义节点
// ═══════════════════════════════════════════════════════════════════════

const COLOR_MAP: Record<string, string> = {
  agent_def: '#6B7280',
  contextupdater: '#8B5CF6',
  dispatcher: '#F59E0B',
  coordinator: '#3B82F6',
  skill: '#10B981',
}

function renderTaskNode(props: NodeProps) {
  const task = props.data as AgentTaskDef & { label: string }
  const color = COLOR_MAP[task.type] || '#6B7280'

  const handles: ReturnType<typeof h>[] = []
  // agent_def 无手柄
  if (task.type !== 'agent_def') {
    // 输入手柄（左侧）
    if (task.type !== 'contextupdater') {
      handles.push(h(Handle, {
        type: 'target', position: Position.Left, id: 'in',
        style: { background: '#fff', width: 10, height: 10, border: `2px solid ${color}` },
      }))
    }
    // 输出手柄（右侧）
    if (task.type !== 'coordinator') {
      handles.push(h(Handle, {
        type: 'source', position: Position.Right, id: 'out',
        style: { background: '#fff', width: 10, height: 10, border: `2px solid ${color}` },
      }))
    }
  }

  // 摘要信息
  const lines: string[] = []
  if (task.description) lines.push(task.description)
  if (task.type === 'skill' && task.skill?.name) lines.push(`skill: ${task.skill.name}`)
  if (task.type === 'dispatcher' && task.next_skills?.length) lines.push(`→ ${task.next_skills.join(', ')}`)
  if (task.workflow_file) lines.push(`wf: ${task.workflow_file}`)
  if (task.type === 'agent_def') {
    if (task.model?.provider) lines.push(`model: ${task.model.provider}/${task.model.name}`)
    if (task.agentSkills?.length) lines.push(`skills: ${task.agentSkills.join(', ')}`)
  }

  return h('div', {
    class: 'task-node',
    style: { borderLeft: `4px solid ${color}`, background: `${color}0d` },
  }, [
    h('div', { class: 'task-node__type', style: { color } },
      `${TASK_TYPES[task.type]?.icon || '•'} ${TASK_TYPES[task.type]?.label || task.type}`),
    h('div', { class: 'task-node__label' }, task.label || `#${task.id}`),
    ...lines.map(l => h('div', { class: 'task-node__meta' }, l)),
    h('div', { class: 'task-node__badge' }, `#${task.id}`),
    ...handles,
  ])
}

const customNodeTypes = {
  agent_def: markRaw({ props: ['id', 'data', 'selected', 'connectable'], setup: (p: NodeProps) => () => renderTaskNode(p) }),
  contextupdater: markRaw({ props: ['id', 'data', 'selected', 'connectable'], setup: (p: NodeProps) => () => renderTaskNode(p) }),
  dispatcher: markRaw({ props: ['id', 'data', 'selected', 'connectable'], setup: (p: NodeProps) => () => renderTaskNode(p) }),
  coordinator: markRaw({ props: ['id', 'data', 'selected', 'connectable'], setup: (p: NodeProps) => () => renderTaskNode(p) }),
  skill: markRaw({ props: ['id', 'data', 'selected', 'connectable'], setup: (p: NodeProps) => () => renderTaskNode(p) }),
}

// ═══════════════════════════════════════════════════════════════════════
// 状态
// ═══════════════════════════════════════════════════════════════════════

const router = useRouter()
const packageStore = usePackageStore()

// agent 元数据
const agentName = ref('')
const agentVersion = ref('1.0.0')
const agentDesc = ref('')
const sessionPoolSize = ref(1)
const maxWorkflows = ref(10)
const maxConcurrentWorkflows = ref(5)

const isCreating = ref(false)
const selectedNodeId = ref<string | null>(null)
const selectedEdgeId = ref<string | null>(null)

// 右侧属性面板宽度（可拖拽调整）
const propsWidth = ref(280)
const MIN_PROPS_WIDTH = 200
const MAX_PROPS_WIDTH = 500

function onPropsResizeStart(event: MouseEvent) {
  event.preventDefault()
  const startX = event.clientX
  const startWidth = propsWidth.value

  function onMouseMove(e: MouseEvent) {
    const delta = startX - e.clientX
    propsWidth.value = Math.min(MAX_PROPS_WIDTH, Math.max(MIN_PROPS_WIDTH, startWidth + delta))
  }

  function onMouseUp() {
    document.removeEventListener('mousemove', onMouseMove)
    document.removeEventListener('mouseup', onMouseUp)
  }

  document.addEventListener('mousemove', onMouseMove)
  document.addEventListener('mouseup', onMouseUp)
}

const flowId = 'agent-creator-flow'
const {
  nodes, edges, addNodes, removeNodes, addEdges, removeEdges,
  screenToFlowCoordinate, fitView,
  onConnect, onNodeClick, onPaneClick, onNodeContextMenu,
} = useVueFlow({ id: flowId })

// ── 右键菜单 ──────────────────────────────────────────────────────────
const ctxMenu = ref<{
  visible: boolean
  top: number
  left: number
  nodeId: string | null
}>({ visible: false, top: 0, left: 0, nodeId: null })

onNodeContextMenu((ev) => {
  ev.event.preventDefault()
  selectedNodeId.value = ev.node.id
  selectedEdgeId.value = null
  ctxMenu.value = {
    visible: true,
    top: ev.event.clientY,
    left: ev.event.clientX,
    nodeId: ev.node.id,
  }
  // 点击其他地方关闭菜单
  window.addEventListener('click', closeCtxMenu, { once: true })
})

function closeCtxMenu() {
  ctxMenu.value = { visible: false, top: 0, left: 0, nodeId: null }
}

function ctxDelete() {
  const id = ctxMenu.value.nodeId
  closeCtxMenu()
  if (!id) return
  const n = nodes.value.find(x => x.id === id)
  if (n) { removeNodes([n]); selectedNodeId.value = null }
}

// ── readonly 模式下自动从 package 加载 agent.json ────────────

const dagLoading = ref(false)

async function loadFromPackage(pkgName: string) {
  dagLoading.value = true
  try {
    const res = await getApiClient().get<{ content: string }>(
      `/api/packages/${encodeURIComponent(pkgName)}/file/agent.json`,
    )
    const agentJson = JSON.parse((res as any).content)

    agentName.value = agentJson.name || ''
    agentVersion.value = agentJson.version || '1.0.0'
    agentDesc.value = agentJson.description || ''
    sessionPoolSize.value = agentJson.session_pool_size ?? 1
    maxWorkflows.value = agentJson.max_workflows ?? 10
    maxConcurrentWorkflows.value = agentJson.max_concurrent_workflows ?? 5

    const tasks: AgentTaskDef[] = agentJson.tasks ?? []
    if (tasks.length === 0) return

    for (const t of tasks) {
      if (t.id >= seqId) seqId = t.id + 1
    }

    const vfNodes: VfNode[] = []
    const vfEdges: VfEdge[] = []
    const taskIdToNodeId = new Map<number, string>()

    // 按 type 分组布局
    const typeOrder = ['agent_def', 'contextupdater', 'dispatcher', 'skill', 'coordinator']
    const byType = new Map<string, AgentTaskDef[]>()
    for (const t of tasks) {
      const key = (t.type === 'skill' && t.skill) ? 'skill' : t.type
      if (!byType.has(key)) byType.set(key, [])
      byType.get(key)!.push(t)
    }

    const LAYOUT = { START_X: 60, START_Y: 40, GAP_X: 280, GAP_Y: 100 }
    let colIdx = 0

    for (const type of typeOrder) {
      const group = byType.get(type)
      if (!group || group.length === 0) continue
      for (let rowIdx = 0; rowIdx < group.length; rowIdx++) {
        const t = group[rowIdx]
        const nodeId = `task-${t.id}`
        taskIdToNodeId.set(t.id, nodeId)
        const tmpl = TASK_TYPES[t.type]
        vfNodes.push({
          id: nodeId,
          type: t.type,
          position: {
            x: LAYOUT.START_X + colIdx * LAYOUT.GAP_X,
            y: LAYOUT.START_Y + rowIdx * LAYOUT.GAP_Y,
          },
          data: { ...t, label: `${tmpl?.label || t.type} #${t.id}` },
        })
      }
      colIdx++
    }

    // 从 next_skills 推导边
    for (const t of tasks) {
      if (t.type === 'dispatcher' && t.next_skills?.length) {
        const srcId = taskIdToNodeId.get(t.id)
        if (!srcId) continue
        for (const skillName of t.next_skills) {
          const skillTask = tasks.find(
            (ot) => ot.type === 'skill' && ot.skill?.name === skillName,
          )
          if (skillTask) {
            const tgtId = taskIdToNodeId.get(skillTask.id)
            if (tgtId) {
              vfEdges.push({
                id: `edge-${t.id}-${skillTask.id}`,
                source: srcId,
                target: tgtId,
                animated: true,
                style: { stroke: '#909399', strokeWidth: 2 },
                labelStyle: { fontSize: 11, color: '#606266' },
                labelBgStyle: { fill: '#fff' },
                labelBgPadding: [4, 2] as [number, number],
                labelBgBorderRadius: 2,
              })
            }
          }
        }
      }
    }

    // 替换画布内容
    if (nodes.value.length > 0) {
      removeNodes([...nodes.value])
      removeEdges([...edges.value])
    }
    addNodes(vfNodes)
    addEdges(vfEdges)

    setTimeout(() => fitView({ padding: 0.2 }), 100)
  } catch (e) {
    console.warn('Failed to load agent.json:', e)
    ElMessage.warning('无法加载 agent.json，请确认包中存在该文件')
  } finally {
    dagLoading.value = false
  }
}

watch(() => props.packageName, (val) => {
  if (val && props.readonly) {
    loadFromPackage(val)
  }
}, { immediate: true })

// ═══════════════════════════════════════════════════════════════════════
// 画布事件
// ═══════════════════════════════════════════════════════════════════════

onNodeClick((ev) => { selectedNodeId.value = ev.node.id; selectedEdgeId.value = null })
onPaneClick(() => { selectedNodeId.value = null; selectedEdgeId.value = null; closeCtxMenu() })

onConnect((conn: Connection) => {
  // 若源节点是 dispatcher、目标节点是 skill，自动设置 next_skills
  const srcNode = nodes.value.find(n => n.id === conn.source)
  const tgtNode = nodes.value.find(n => n.id === conn.target)
  const srcTask = srcNode?.data as AgentTaskDef | undefined
  const tgtTask = tgtNode?.data as AgentTaskDef | undefined

  if (srcTask && tgtTask && srcTask.type === 'dispatcher' && tgtTask.type === 'skill' && tgtTask.skill?.name) {
    if (!srcTask.next_skills) srcTask.next_skills = []
    if (!srcTask.next_skills.includes(tgtTask.skill.name)) {
      srcTask.next_skills.push(tgtTask.skill.name)
    }
  }

  addEdges([{
    id: `edge-${seqId}`,
    source: conn.source,
    target: conn.target,
    animated: true,
    style: { stroke: '#909399', strokeWidth: 2 },
    labelStyle: { fontSize: 11, color: '#606266' },
    labelBgStyle: { fill: '#fff' },
    labelBgPadding: [4, 2] as [number, number],
    labelBgBorderRadius: 2,
  }])
})

// ═══════════════════════════════════════════════════════════════════════
// 拖拽放置
// ═══════════════════════════════════════════════════════════════════════

function onDragStart(event: DragEvent, type: string) {
  if (props.readonly) return
  event.dataTransfer?.setData('application/task-type', type)
  if (event.dataTransfer) event.dataTransfer.effectAllowed = 'move'
}
function onDragOver(event: DragEvent) {
  if (props.readonly) return
  event.preventDefault()
  if (event.dataTransfer) event.dataTransfer.dropEffect = 'move'
}

function onDrop(event: DragEvent) {
  if (props.readonly) return
  event.preventDefault()
  const type = event.dataTransfer?.getData('application/task-type') as AgentTaskDef['type'] | undefined
  if (!type || !TASK_TYPES[type]) return

  const pos = screenToFlowCoordinate({ x: event.clientX, y: event.clientY })
  const tmpl = TASK_TYPES[type]
  const id = nextTaskId()
  const nodeId = nextNodeId()

  const task: AgentTaskDef = {
    id,
    type,
    description: '',
    ...JSON.parse(JSON.stringify(tmpl.defaultTask)),
  }

  addNodes([{
    id: nodeId,
    type,
    position: pos,
    data: { ...task, label: `${tmpl.label} #${id}` },
  }])
}

// ═══════════════════════════════════════════════════════════════════════
// 删除
// ═══════════════════════════════════════════════════════════════════════

function handleDeleteSelected() {
  if (selectedNodeId.value) {
    const n = nodes.value.find(x => x.id === selectedNodeId.value)
    if (!n) return
    removeNodes([n]); selectedNodeId.value = null
  } else if (selectedEdgeId.value) {
    const e = edges.value.find(x => x.id === selectedEdgeId.value)
    if (e) { removeEdges([e]); selectedEdgeId.value = null }
  }
}
function handleKeydown(ev: KeyboardEvent) {
  if (props.readonly) return
  if ((ev.key === 'Delete' || ev.key === 'Backspace') && !['INPUT','TEXTAREA','SELECT'].includes((ev.target as HTMLElement)?.tagName))
    handleDeleteSelected()
}
function clearAll() {
  removeNodes([...nodes.value])
  removeEdges([...edges.value])
  selectedNodeId.value = null; selectedEdgeId.value = null
}

// ═══════════════════════════════════════════════════════════════════════
// 自动布局
// ═══════════════════════════════════════════════════════════════════════

const GAP_X = 240
const GAP_Y = 100
const START_X = 60
const START_Y = 40

function autoLayout() {
  if (nodes.value.length === 0) return

  // 计算入度 → 拓扑排序
  const inDeg = new Map<string, number>()
  const children = new Map<string, string[]>()
  for (const n of nodes.value) {
    inDeg.set(n.id, 0)
    children.set(n.id, [])
  }
  for (const e of edges.value) {
    inDeg.set(e.target, (inDeg.get(e.target) || 0) + 1)
    children.get(e.source)?.push(e.target)
  }

  // BFS 分层
  const layers: string[][] = []
  let queue = nodes.value.filter(n => (inDeg.get(n.id) || 0) === 0).map(n => n.id)
  const visited = new Set<string>(queue)

  while (queue.length > 0) {
    layers.push([...queue])
    const next: string[] = []
    for (const id of queue) {
      for (const child of children.get(id) || []) {
        if (!visited.has(child)) {
          visited.add(child)
          next.push(child)
        }
      }
    }
    queue = next
  }

  // 把未入队的孤立节点加到最后一层
  for (const n of nodes.value) {
    if (!visited.has(n.id)) {
      if (layers.length === 0) layers.push([n.id])
      else layers[layers.length - 1].push(n.id)
    }
  }

  // 更新位置
  for (let depth = 0; depth < layers.length; depth++) {
    const col = layers[depth]
    const totalHeight = col.length * GAP_Y
    const startY = START_Y - totalHeight / 2 + GAP_Y / 2
    for (let row = 0; row < col.length; row++) {
      const node = nodes.value.find(n => n.id === col[row])
      if (node) {
        node.position = {
          x: START_X + depth * GAP_X,
          y: startY + row * GAP_Y,
        }
      }
    }
  }
}

// ═══════════════════════════════════════════════════════════════════════
// 属性面板：选中节点
// ═══════════════════════════════════════════════════════════════════════

const selectedNode = computed<VfNode | null>(() =>
  selectedNodeId.value ? nodes.value.find(n => n.id === selectedNodeId.value) ?? null : null,
)

function updateTask(partial: Partial<AgentTaskDef>) {
  if (!selectedNodeId.value) return
  const n = nodes.value.find(nd => nd.id === selectedNodeId.value)
  if (n) Object.assign(n.data, partial)
}

// ═══════════════════════════════════════════════════════════════════════
// 生成 agent.json
// ═══════════════════════════════════════════════════════════════════════

const agentJson = computed(() => {
  const tasks: AgentTaskDef[] = nodes.value.map(n => {
    const d = n.data as AgentTaskDef
    const t: AgentTaskDef = {
      id: d.id,
      type: d.type,
      description: d.description || '',
    }
    if (d.type === 'contextupdater' && d.scope) t.scope = d.scope
    if (d.workflow_file) t.workflow_file = d.workflow_file
    if (d.workflow_pool_size) t.workflow_pool_size = d.workflow_pool_size
    if (d.type === 'dispatcher') {
      if (d.next_skills?.length) t.next_skills = d.next_skills
      if (d.max_concurrent_skills) t.max_concurrent_skills = d.max_concurrent_skills
    }
    if (d.type === 'skill' && d.skill) {
      t.skill = {
        name: d.skill.name || 'skill',
        config_dir: d.skill.config_dir || 'skills/skill',
        instance_pool_size: d.skill.instance_pool_size || 1,
      }
    }
    return t
  })

  const json: Record<string, any> = {
    name: agentName.value || 'MyAgent',
    version: agentVersion.value || '1.0.0',
    description: agentDesc.value || '',
    session_pool_size: sessionPoolSize.value,
    tasks,
  }
  if (maxWorkflows.value) json.max_workflows = maxWorkflows.value
  if (maxConcurrentWorkflows.value) json.max_concurrent_workflows = maxConcurrentWorkflows.value
  return json
})

const agentJsonStr = computed(() => JSON.stringify(agentJson.value, null, 2))

// ═══════════════════════════════════════════════════════════════════════
// 生成 package.json
// ═══════════════════════════════════════════════════════════════════════

const packageJson = computed(() => ({
  name: (agentName.value || 'my-agent').toLowerCase().replace(/\s+/g, '-'),
  version: agentVersion.value || '1.0.0',
  description: agentDesc.value || '',
  author: '',
  entry_point: '',
  workflows: [...new Set(nodes.value
    .map(n => (n.data as AgentTaskDef).workflow_file)
    .filter(Boolean) as string[])],
  metadata: { type: 'agent', tags: [] },
}))

const packageJsonStr = computed(() => JSON.stringify(packageJson.value, null, 2))

// ═══════════════════════════════════════════════════════════════════════
// 预览折叠
// ═══════════════════════════════════════════════════════════════════════

const previewActive = ref<string>('agent')

// ═══════════════════════════════════════════════════════════════════════
// 创建
// ═══════════════════════════════════════════════════════════════════════

async function handleCreate() {
  const name = agentName.value.trim()
  if (!name) { ElMessage.warning('请输入 Agent 名称'); return }
  if (nodes.value.length === 0) { ElMessage.warning('请至少添加一个 Task 节点'); return }

  isCreating.value = true
  try {
    const pkgName = name.toLowerCase().replace(/\s+/g, '-')
    await packageStore.createPackage(pkgName)
    await getApiClient().put(
      `/api/packages/${encodeURIComponent(pkgName)}/file/agent.json`,
      { content: agentJsonStr.value },
    )
    await getApiClient().put(
      `/api/packages/${encodeURIComponent(pkgName)}/file/package.json`,
      { content: packageJsonStr.value },
    )
    // 若 agent_def 节点配置了模型信息，生成 agent.md
    const hasAgentDef = nodes.value.some(n => (n.data as AgentTaskDef).type === 'agent_def')
    if (hasAgentDef && agentMdStr.value) {
      await getApiClient().put(
        `/api/packages/${encodeURIComponent(pkgName)}/file/agent.md`,
        { content: agentMdStr.value },
      )
    }
    ElMessage.success(`智能体包 "${name}" 创建成功`)
    router.push(`/packages/${encodeURIComponent(pkgName)}`)
  } catch (e) {
    ElMessage.error(`创建失败: ${e instanceof Error ? e.message : e}`)
  } finally {
    isCreating.value = false
  }
}

function handleCancel() { router.push('/packages') }

// ── skill 子对象更新（含级联修改分发器的 next_skills）───────────────
function updateSkill(field: keyof SkillDef, value: any) {
  if (!selectedNodeId.value) return
  const n = nodes.value.find(nd => nd.id === selectedNodeId.value)
  if (!n) return
  const task = n.data as AgentTaskDef
  if (!task.skill) task.skill = { name: '', config_dir: '', instance_pool_size: 1 }

  // 当 skill name 变更时，级联更新所有 dispatcher 的 next_skills
  if (field === 'name') {
    const oldName = task.skill.name
    ;(task.skill as any)[field] = value
    if (oldName && value && oldName !== value) {
      for (const node of nodes.value) {
        const t = node.data as AgentTaskDef
        if (t.type === 'dispatcher' && t.next_skills?.includes(oldName)) {
          t.next_skills = t.next_skills.map(s => s === oldName ? value : s)
        }
      }
    }
  } else {
    ;(task.skill as any)[field] = value
  }
}

// 所有 skill 节点名称列表（供 dispatcher 的 next_skills 选择）
const allSkillNames = computed(() =>
  nodes.value
    .filter(n => (n.data as AgentTaskDef).type === 'skill')
    .map(n => (n.data as AgentTaskDef).skill?.name)
    .filter(Boolean) as string[],
)

// ── agent_def 模型配置更新 ─────────────────────────────────────────────
function updateModel(field: keyof ModelConfig, value: any) {
  if (!selectedNodeId.value) return
  const n = nodes.value.find(nd => nd.id === selectedNodeId.value)
  if (!n) return
  const task = n.data as AgentTaskDef
  if (!task.model) task.model = { provider: '', name: '', base_url: '', api_key: '' }
  ;(task.model as any)[field] = value
}

// ── agent.md 内容生成 ─────────────────────────────────────────────────
const agentMdStr = computed(() => {
  const task = nodes.value.find(n => (n.data as AgentTaskDef).type === 'agent_def')
  const data = task?.data as AgentTaskDef | undefined
  const model = data?.model
  const skills = data?.agentSkills

  const lines: string[] = ['---']
  lines.push(`name: ${agentName.value || 'my-agent'}`)
  lines.push(`description: "${agentDesc.value || ''}"`)
  if (model?.provider || model?.name) {
    lines.push('model:')
    lines.push(`  provider: ${model.provider || 'openai'}`)
    lines.push(`  name: ${model.name || 'gpt-4'}`)
    if (model.base_url) lines.push(`  base_url: ${model.base_url}`)
    if (model.api_key) lines.push(`  api_key: \${API_KEY}  # ${model.api_key.slice(0, 8)}...`)
  }
  if (skills?.length) {
    lines.push('skills:')
    skills.forEach(s => lines.push(`  - ${s}`))
  }
  lines.push('---')
  lines.push('')
  lines.push(`# ${agentName.value || 'MyAgent'}`)
  lines.push('')
  // 使用 system_prompt 作为 body，若无则 fallback 到描述
  const body = data?.system_prompt || agentDesc.value || 'Agent description'
  lines.push(body)
  return lines.join('\n')
})
</script>

<template>
  <div class="pc" @keydown="handleKeydown" tabindex="-1">
    <!-- ═══ 顶部：Agent 元数据 ════════════════════════════════════════ -->
    <header class="pc-header">
      <button class="pc-header__back" @click="handleCancel"><el-icon><ArrowLeft /></el-icon><span>返回</span></button>

      <div class="pc-header__meta">
        <el-input v-model="agentName" placeholder="Agent 名称（如 BasicAgent）" size="small" class="pc-h-fld" clearable data-testid="creator-name" />
        <el-input v-model="agentVersion" placeholder="版本" size="small" style="width:80px" clearable />
        <el-input v-model="agentDesc" placeholder="描述" size="small" class="pc-h-fld" clearable />
        <el-input-number v-model="sessionPoolSize" :min="0" :max="100" size="small" style="width:90px" placeholder="会话池" title="session_pool_size" />
        <el-input-number v-model="maxWorkflows" :min="1" :max="100" size="small" style="width:90px" placeholder="最大工作流" title="max_workflows" />
        <el-input-number v-model="maxConcurrentWorkflows" :min="1" :max="100" size="small" style="width:90px" placeholder="并发" title="max_concurrent_workflows" />
      </div>

      <div class="pc-header__actions">
        <el-button size="small" @click="handleCancel">取消</el-button>
        <el-button type="primary" size="small" :loading="isCreating" :disabled="!agentName.trim() || nodes.length === 0" @click="handleCreate" data-testid="creator-submit">
          {{ isCreating ? '创建中…' : '创建智能体包' }}
        </el-button>
      </div>
    </header>

    <!-- ═══ 主体 ═════════════════════════════════════════════════════ -->
    <div class="pc-body">
      <!-- 左侧：工具箱（readonly 模式下隐藏） -->
      <aside v-if="!props.readonly" class="pc-tools">
        <h3 class="pc-tools__title">节点类型</h3>
        <p class="pc-tools__hint">拖拽到画布添加</p>

        <!-- Harness 分组 -->
        <div class="pc-tools__group-label">Harness</div>
        <div v-for="item in harnessItems" :key="item.type"
             class="pc-tools__item" draggable="true"
             @dragstart="onDragStart($event, item.type)">
          <span class="pc-tools__badge" :style="{ background: item.color }">{{ item.icon }}</span>
          <div class="pc-tools__info">
            <span class="pc-tools__label">{{ item.label }}</span>
            <span class="pc-tools__desc">{{ item.description }}</span>
          </div>
        </div>

        <!-- Task 分组 -->
        <div class="pc-tools__group-label">Task</div>
        <div v-for="item in taskItems" :key="item.type"
             class="pc-tools__item" draggable="true"
             @dragstart="onDragStart($event, item.type)">
          <span class="pc-tools__badge" :style="{ background: item.color }">{{ item.icon }}</span>
          <div class="pc-tools__info">
            <span class="pc-tools__label">{{ item.label }}</span>
            <span class="pc-tools__desc">{{ item.description }}</span>
          </div>
        </div>

        <el-divider style="margin:6px 0" />
        <div class="pc-tools__actions">
          <button class="pc-tools__action" @click="autoLayout"><el-icon><TopRight /></el-icon>自动布局</button>
          <button class="pc-tools__action pc-tools__action--danger" @click="clearAll"><el-icon><Delete /></el-icon>清空</button>
        </div>
      </aside>

      <!-- 中间：画布 -->
      <main class="pc-canvas" @drop="onDrop" @dragover="onDragOver">
        <div v-if="dagLoading" class="pc-canvas__loading">
          <el-skeleton :rows="4" animated />
          <p class="pc-canvas__loading-text">正在加载 agent.json…</p>
        </div>
        <div v-else-if="nodes.length === 0" class="pc-canvas__empty">
          <p v-if="props.readonly">该智能体包中暂无 Task 节点</p>
          <p v-else>从左侧拖拽 Task 类型到此处开始编排 Agent</p>
          <p v-if="!props.readonly" class="pc-canvas__hint">典型流程: 上下文更新 → 分发器 → 技能 → 协调器</p>
        </div>
        <VueFlow :id="flowId" :nodes="nodes" :edges="edges"
                 :node-types="customNodeTypes"
                 :default-viewport="{ zoom: 0.85 }" :min-zoom="0.2" :max-zoom="3"
                 :nodes-draggable="!props.readonly"
                 :nodes-connectable="!props.readonly"
                 :nodes-focusable="!props.readonly"
                 :elements-selectable="!props.readonly"
                 :snap-to-grid="!props.readonly" :snap-grid="[20,20]"
                 fit-view-on-init class="pc-flow">
          <MiniMap :node-color="(n: VfNode) => COLOR_MAP[(n.data as any)?.type] || '#6B7280'" />
          <Controls />
        </VueFlow>

        <!-- 右键菜单 -->
        <ul v-if="ctxMenu.visible && !props.readonly"
            class="pc-ctx-menu"
            :style="{ top: `${ctxMenu.top}px`, left: `${ctxMenu.left}px` }"
            data-testid="ctx-menu">
          <li class="pc-ctx-menu__item pc-ctx-menu__item--danger" @click="ctxDelete">
            <el-icon><Delete /></el-icon><span>删除节点</span>
          </li>
        </ul>
      </main>

      <!-- 右侧：属性 + 预览（可拖拽调整宽度） -->
      <div
        class="pc-props-resizer"
        @mousedown="onPropsResizeStart"
      />
      <aside class="pc-props" :style="{ width: `${propsWidth}px`, minWidth: `${MIN_PROPS_WIDTH}px` }">
        <!-- 选中 Task -->
        <template v-if="selectedNode">
          <h3 class="pc-props__title">Task 属性</h3>
          <div class="pc-props__form">
            <el-form label-position="top" size="small">
              <!-- agent_def 不显示 ID/类型标签/描述/工作流文件等 Task 属性 -->
              <template v-if="(selectedNode.data as AgentTaskDef).type !== 'agent_def'">
                <el-form-item label="ID">
                  <el-input :model-value="(selectedNode.data as AgentTaskDef).id" disabled />
                </el-form-item>
                <el-form-item label="类型">
                  <el-tag :color="COLOR_MAP[(selectedNode.data as AgentTaskDef).type]" effect="dark" style="color:#fff">
                    {{ TASK_TYPES[(selectedNode.data as AgentTaskDef).type]?.label }}
                  </el-tag>
                </el-form-item>
                <el-form-item label="描述">
                  <el-input :model-value="(selectedNode.data as AgentTaskDef).description"
                            @update:model-value="updateTask({ description: $event })"
                            type="textarea" :rows="2" placeholder="Task 功能描述" />
                </el-form-item>
                <!-- 公共字段 -->
                <el-form-item label="工作流文件">
                  <el-input :model-value="(selectedNode.data as AgentTaskDef).workflow_file"
                            @update:model-value="updateTask({ workflow_file: $event })"
                            placeholder="workflow.json" clearable />
                </el-form-item>
                <el-form-item label="工作流池大小">
                  <el-input-number :model-value="(selectedNode.data as AgentTaskDef).workflow_pool_size ?? 1"
                                   @update:model-value="updateTask({ workflow_pool_size: $event })"
                                   :min="1" :max="100" style="width:100%" />
                </el-form-item>
              </template>

              <!-- contextupdater 特有 -->
              <el-form-item v-if="(selectedNode.data as AgentTaskDef).type === 'contextupdater'" label="范围 (scope)">
                <el-input :model-value="(selectedNode.data as AgentTaskDef).scope"
                          @update:model-value="updateTask({ scope: $event })"
                          placeholder="agent" clearable />
              </el-form-item>

              <!-- dispatcher 特有 -->
              <template v-if="(selectedNode.data as AgentTaskDef).type === 'dispatcher'">
                <el-form-item label="下一技能 (next_skills)">
                  <el-select :model-value="(selectedNode.data as AgentTaskDef).next_skills ?? []"
                             @update:model-value="updateTask({ next_skills: $event })"
                             multiple allow-create filterable default-first-option
                             placeholder="输入技能名称后回车" style="width:100%">
                    <el-option v-for="sk in allSkillNames" :key="sk" :label="sk" :value="sk" />
                  </el-select>
                </el-form-item>
                <el-form-item label="最大并发技能">
                  <el-input-number :model-value="(selectedNode.data as AgentTaskDef).max_concurrent_skills ?? 1"
                                   @update:model-value="updateTask({ max_concurrent_skills: $event })"
                                   :min="1" :max="100" style="width:100%" />
                </el-form-item>
              </template>

              <!-- skill 特有 -->
              <template v-if="(selectedNode.data as AgentTaskDef).type === 'skill'">
                <el-divider style="margin:8px 0">Skill 配置</el-divider>
                <el-form-item label="技能名称">
                  <el-input :model-value="(selectedNode.data as AgentTaskDef).skill?.name"
                            @update:model-value="updateSkill('name', $event)"
                            placeholder="my-skill" />
                </el-form-item>
                <el-form-item label="配置目录">
                  <el-input :model-value="(selectedNode.data as AgentTaskDef).skill?.config_dir"
                            @update:model-value="updateSkill('config_dir', $event)"
                            placeholder="skills/my-skill" />
                </el-form-item>
                <el-form-item label="实例池大小">
                  <el-input-number :model-value="(selectedNode.data as AgentTaskDef).skill?.instance_pool_size ?? 1"
                                   @update:model-value="updateSkill('instance_pool_size', $event)"
                                   :min="1" :max="100" style="width:100%" />
                </el-form-item>
              </template>

              <!-- agent_def 特有：模型配置 + 技能清单 -->
              <template v-if="(selectedNode.data as AgentTaskDef).type === 'agent_def'">
                <el-divider style="margin:8px 0">模型配置</el-divider>
                <el-form-item label="提供商">
                  <el-select :model-value="(selectedNode.data as AgentTaskDef).model?.provider"
                             @update:model-value="updateModel('provider', $event)"
                             placeholder="openai" style="width:100%">
                    <el-option label="openai" value="openai" />
                    <el-option label="anthropic" value="anthropic" />
                    <el-option label="deepseek" value="deepseek" />
                    <el-option label="自定义" value="custom" />
                  </el-select>
                </el-form-item>
                <el-form-item label="模型名称">
                  <el-input :model-value="(selectedNode.data as AgentTaskDef).model?.name"
                            @update:model-value="updateModel('name', $event)"
                            placeholder="gpt-4 / deepseek-chat" />
                </el-form-item>
                <el-form-item label="接口地址">
                  <el-input :model-value="(selectedNode.data as AgentTaskDef).model?.base_url"
                            @update:model-value="updateModel('base_url', $event)"
                            placeholder="https://api.openai.com/v1" clearable />
                </el-form-item>
                <el-form-item label="API Key">
                  <el-input :model-value="(selectedNode.data as AgentTaskDef).model?.api_key"
                            @update:model-value="updateModel('api_key', $event)"
                            type="password" show-password
                            placeholder="sk-..." clearable />
                </el-form-item>
                <el-divider style="margin:8px 0">技能清单</el-divider>
                <el-form-item label="可用技能">
                  <el-select :model-value="(selectedNode.data as AgentTaskDef).agentSkills ?? []"
                             @update:model-value="updateTask({ agentSkills: $event })"
                             multiple allow-create filterable default-first-option
                             placeholder="输入技能名称后回车" style="width:100%">
                    <el-option v-for="sk in allSkillNames" :key="sk" :label="sk" :value="sk" />
                  </el-select>
                </el-form-item>
                <el-form-item label="系统提示词 (System Prompt)">
                  <el-input :model-value="(selectedNode.data as AgentTaskDef).system_prompt"
                            @update:model-value="updateTask({ system_prompt: $event })"
                            type="textarea" :rows="8"
                            placeholder="你是一个任务型 AI 助手，运行在工具驱动的内部循环中。&#10;&#10;## 工作方式&#10;1. 分析用户需求，决定下一步做什么。&#10;2. 通过 function calling 调用可用工具完成任务。&#10;3. 当任务完成或需要回复用户时，直接输出回答内容。" />
                </el-form-item>
              </template>
            </el-form>
          </div>
        </template>

        <!-- 未选中 → JSON 预览 -->
        <template v-else>
          <h3 class="pc-props__title">生成预览</h3>
          <el-collapse v-model="previewActive" accordion>
            <el-collapse-item title="agent.json" name="agent">
              <pre class="pc-props__code">{{ agentJsonStr }}</pre>
            </el-collapse-item>
            <el-collapse-item title="agent.md" name="agent-md">
              <pre class="pc-props__code">{{ agentMdStr || '(未配置模型信息)' }}</pre>
            </el-collapse-item>
            <el-collapse-item title="package.json" name="package">
              <pre class="pc-props__code">{{ packageJsonStr }}</pre>
            </el-collapse-item>
          </el-collapse>
        </template>
      </aside>
    </div>

    <!-- ═══ 底部状态 ═══════════════════════════════════════════════ -->
    <footer class="pc-footer">
      <span>{{ nodes.length }} 个 Task</span><span class="pc-dot">·</span>
      <span>{{ edges.length }} 条边</span><span class="pc-dot">·</span>
      <span v-if="props.readonly">点击节点编辑属性</span>
      <span v-else>拖拽添加 · 连线编排 · 点击编辑 · <kbd>Delete</kbd> 删除</span>
    </footer>
  </div>
</template>

<style scoped>
/* ════════════════════════════════════════════════════════════════════════
   布局
   ════════════════════════════════════════════════════════════════════════ */
.pc { display:flex; flex-direction:column; height:100%; background:#f5f7fa; outline:none; user-select:none; }

/* 顶部栏 */
.pc-header {
  display:flex; align-items:center; gap:8px; padding:6px 14px;
  background:#fff; border-bottom:1px solid #e4e7ed; flex-shrink:0; z-index:10;
}
.pc-header__back { display:inline-flex; align-items:center; gap:4px; border:none; background:none; cursor:pointer; font-size:12px; color:#606266; padding:3px 6px; border-radius:4px; }
.pc-header__back:hover { background:#f0f2f5; color:#409eff; }
.pc-header__meta { flex:1; display:flex; gap:4px; align-items:center; flex-wrap:wrap; }
.pc-h-fld { max-width:150px; }
.pc-header__actions { display:flex; gap:6px; flex-shrink:0; }

/* 主体 */
.pc-body { flex:1; display:flex; overflow:hidden; min-height:0; }

/* 工具箱 */
.pc-tools {
  width:180px; min-width:180px; background:#fff; border-right:1px solid #e4e7ed;
  padding:12px 10px; display:flex; flex-direction:column; gap:4px; overflow-y:auto;
}
.pc-tools__title { margin:0; font-size:13px; font-weight:600; color:#1f2937; }
.pc-tools__hint { margin:0 0 6px; font-size:10px; color:#9ca3af; }
.pc-tools__group-label { font-size:11px; font-weight:600; color:#6b7280; margin:8px 0 4px; padding:0 2px; letter-spacing:0.5px; }
.pc-tools__item {
  display:flex; align-items:center; gap:8px; padding:7px 8px;
  border:1px solid #e8e8e8; border-radius:8px; cursor:grab;
  transition:border-color .15s,box-shadow .15s; background:#fafbfc;
}
.pc-tools__item:hover { border-color:#409eff; box-shadow:0 2px 8px rgba(64,158,255,.12); }
.pc-tools__item:active { cursor:grabbing; }
.pc-tools__badge { width:24px; height:24px; border-radius:50%; display:flex; align-items:center; justify-content:center; font-size:12px; flex-shrink:0; }
.pc-tools__info { display:flex; flex-direction:column; gap:1px; min-width:0; }
.pc-tools__label { font-size:12px; font-weight:600; color:#303133; }
.pc-tools__desc { font-size:9px; color:#909399; line-height:1.3; }
.pc-tools__actions { display:flex; gap:6px; margin-top:auto; }
.pc-tools__action { display:flex; align-items:center; gap:4px; padding:5px 10px; border:1px solid #e8e8e8; border-radius:6px; background:#fff; cursor:pointer; font-size:11px; color:#606266; flex:1; justify-content:center; transition:border-color .15s,color .15s; }
.pc-tools__action:hover { border-color:#409eff; color:#409eff; }
.pc-tools__action--danger:hover { border-color:#f56c6c; color:#f56c6c; }

/* 画布 */
.pc-canvas { flex:1; position:relative; overflow:hidden; background:#f8f9fb; }
.pc-canvas__empty { position:absolute; inset:0; display:flex; flex-direction:column; align-items:center; justify-content:center; z-index:1; pointer-events:none; color:#9ca3af; font-size:13px; gap:4px; }
.pc-canvas__hint { font-size:10px; color:#c0c4cc; }
.pc-canvas__loading { position:absolute; inset:0; display:flex; flex-direction:column; align-items:center; justify-content:center; z-index:1; padding:40px; gap:12px; }
.pc-canvas__loading-text { font-size:13px; color:#909399; }
.pc-flow { width:100%; height:100%; }

/* 右键菜单 */
.pc-ctx-menu {
  position:fixed; z-index:3000; min-width:140px; margin:0; padding:4px 0;
  list-style:none; background:#fff; border:1px solid #e4e7ed;
  border-radius:8px; box-shadow:0 4px 16px rgba(0,0,0,.12); overflow:hidden;
}
.pc-ctx-menu__item {
  display:flex; align-items:center; gap:6px; padding:7px 14px;
  cursor:pointer; font-size:12px; color:#303133; user-select:none;
  transition:background-color .12s;
}
.pc-ctx-menu__item:hover { background:#f0f5ff; }
.pc-ctx-menu__item--danger { color:#f56c6c; }
.pc-ctx-menu__item--danger:hover { background:rgba(245,108,108,.06); }

/* 自定义 Task 节点 */
:deep(.task-node) {
  padding:6px 12px; border-radius:8px; border:1px solid #dcdfe6;
  min-width:160px; font-size:12px; box-shadow:0 1px 4px rgba(0,0,0,.06);
  transition:box-shadow .2s; position:relative;
}
:deep(.task-node:hover) { box-shadow:0 3px 12px rgba(0,0,0,.12); }
:deep(.task-node__type) { font-size:10px; font-weight:500; margin-bottom:2px; }
:deep(.task-node__label) { font-weight:600; color:#303133; font-size:13px; }
:deep(.task-node__meta) { font-size:10px; color:#6b7280; margin-top:1px; line-height:1.3; }
:deep(.task-node__badge) { position:absolute; top:4px; right:8px; font-size:9px; color:#9ca3af; background:#f0f2f5; padding:0 5px; border-radius:8px; }

/* 属性面板 */
.pc-props {
  width:280px; background:#fff; border-left:1px solid #e4e7ed;
  padding:14px; overflow-y:auto; font-size:12px;
  flex-shrink:0;
}
/* 拖拽调整宽度的把手 */
.pc-props-resizer {
  width:5px; cursor:col-resize; flex-shrink:0; position:relative; z-index:5;
  background:transparent; transition:background 0.15s;
  margin-left:-3px;
}
.pc-props-resizer:hover, .pc-props-resizer:active {
  background:#409eff;
}
.pc-props__title { margin:0 0 10px; font-size:14px; font-weight:600; color:#1f2937; }
.pc-props__form :deep(.el-form-item) { margin-bottom:8px; }
.pc-props__form :deep(.el-form-item__label) { padding-bottom:2px; font-size:11px; color:#606266; }
.pc-props__code {
  background:#f5f7fa; padding:6px; border-radius:4px;
  font-family:'JetBrains Mono','SF Mono',Menlo,monospace; font-size:10px;
  line-height:1.5; overflow-x:auto; white-space:pre; max-height:280px; overflow-y:auto;
}

/* 底部 */
.pc-footer {
  display:flex; align-items:center; gap:6px; padding:5px 16px;
  background:#fff; border-top:1px solid #e4e7ed; font-size:11px; color:#909399; flex-shrink:0;
}
.pc-dot { color:#c0c4cc; }
.pc-footer kbd { background:#f0f2f5; border:1px solid #dcdfe6; border-radius:3px; padding:0 4px; font-size:10px; font-family:'JetBrains Mono',monospace; }
</style>
