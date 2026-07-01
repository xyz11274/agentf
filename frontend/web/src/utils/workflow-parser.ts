/**
 * workflow-parser.ts — workflow JSON → Vue-Flow 节点/边数据转换
 */

export interface WorkflowNode {
  id: string
  name: string
  type: string
  executionMode?: string
  description?: string
  position?: { x: number; y: number }
}

export interface WorkflowEdge {
  id: string
  source: string
  target: string
  label?: string
  condition?: string
}

export interface WorkflowDef {
  id?: string
  name?: string
  description?: string
  nodes: WorkflowNode[]
  edges: WorkflowEdge[]
}

/** Raw workflow JSON node as parsed from the JSON file */
interface RawNode {
  id: string
  name?: string
  type?: string
  execution_mode?: string
  description?: string
  inputs?: Array<{ name: string }>
  outputs?: Array<{ name: string }>
}

/** Raw workflow JSON edge as parsed from the JSON file */
interface RawEdge {
  id?: string
  source?: string
  target?: string
  condition?: string
  label?: string
  from?: string
  to?: string
}

/** Raw workflow JSON structure */
interface RawWorkflow {
  id?: string
  name?: string
  description?: string
  nodes?: RawNode[]
  edges?: RawEdge[]
}

/**
 * Layout constants — arrange nodes in a vertical flow with fixed spacing.
 */
const LAYOUT = {
  START_X: 80,
  START_Y: 60,
  GAP_X: 260,
  GAP_Y: 120,
  NODE_WIDTH: 220,
  NODE_HEIGHT: 60,
} as const

/**
 * Parse raw workflow JSON into a normalized WorkflowDef.
 */
export function parseWorkflow(raw: RawWorkflow): WorkflowDef {
  const nodes: WorkflowNode[] = (raw.nodes ?? []).map((n, i) => ({
    id: n.id || `node-${i}`,
    name: n.name || n.id || `Node ${i}`,
    type: n.type || 'task',
    executionMode: n.execution_mode,
    description: n.description,
  }))

  const edges: WorkflowEdge[] = (raw.edges ?? []).map((e, i) => {
    const source = e.source || e.from || ''
    const target = e.target || e.to || ''
    return {
      id: e.id || `edge-${i}`,
      source,
      target,
      label: e.label || e.condition || '',
      condition: e.condition,
    }
  })

  // Automatically derive edges from GOTO-style node naming (e.g. node_1 → node_2)
  const autoEdges: WorkflowEdge[] = []
  if (edges.length === 0 && nodes.length > 1) {
    for (let i = 0; i < nodes.length - 1; i++) {
      autoEdges.push({
        id: `auto-edge-${i}`,
        source: nodes[i].id,
        target: nodes[i + 1].id,
        label: '',
      })
    }
  }

  return {
    id: raw.id,
    name: raw.name,
    description: raw.description,
    nodes,
    edges: edges.length > 0 ? edges : autoEdges,
  }
}

/**
 * Convert a WorkflowDef to Vue-Flow elements (nodes + edges).
 *
 * Positions are computed automatically in a left-to-right, top-to-bottom
 * layout. Nodes with no incoming edges are placed in the first column.
 */
export function toVueFlowElements(workflow: WorkflowDef) {
  if (!workflow.nodes.length) {
    return { nodes: [], edges: [] }
  }

  // Compute in-degrees for topological ordering
  const inDegrees = new Map<string, number>()
  const children = new Map<string, string[]>()
  for (const n of workflow.nodes) {
    inDegrees.set(n.id, 0)
    children.set(n.id, [])
  }
  for (const e of workflow.edges) {
    inDegrees.set(e.target, (inDegrees.get(e.target) || 0) + 1)
    children.get(e.source)?.push(e.target)
  }

  // Topological sort (BFS from root nodes)
  const sorted: string[] = []
  const queue = workflow.nodes
    .filter((n) => (inDegrees.get(n.id) || 0) === 0)
    .map((n) => n.id)

  while (queue.length > 0) {
    const id = queue.shift()!
    sorted.push(id)
    for (const child of children.get(id) || []) {
      const deg = (inDegrees.get(child) || 1) - 1
      inDegrees.set(child, deg)
      if (deg === 0) queue.push(child)
    }
  }

  // Add any remaining nodes not in the topological order
  for (const n of workflow.nodes) {
    if (!sorted.includes(n.id)) sorted.push(n.id)
  }

  // Assign positions in columns (by depth from roots)
  const depth = new Map<string, number>()
  for (const id of sorted) {
    const d = depth.get(id) ?? 0
    for (const child of children.get(id) || []) {
      depth.set(child, Math.max(depth.get(child) ?? 0, d + 1))
    }
  }

  // Group by depth for Y layout
  const byDepth = new Map<number, string[]>()
  for (const n of workflow.nodes) {
    const d = depth.get(n.id) ?? 0
    if (!byDepth.has(d)) byDepth.set(d, [])
    byDepth.get(d)!.push(n.id)
  }

  // Build Vue-Flow node elements
  const vfNodes = sorted.map((id) => {
    const node = workflow.nodes.find((n) => n.id === id)!
    const d = depth.get(id) ?? 0
    const col = byDepth.get(d) ?? []
    const rowIndex = col.indexOf(id)

    const color = node.executionMode
      ? { python_script: '#67c23a', shell_script: '#e6a23c', so_library: '#f56c6c' }[
          node.executionMode as keyof typeof color
        ] ?? '#409eff'
      : '#409eff'

    return {
      id: node.id,
      type: 'default',
      position: {
        x: LAYOUT.START_X + d * LAYOUT.GAP_X,
        y: LAYOUT.START_Y + rowIndex * LAYOUT.GAP_Y,
      },
      data: {
        label: `${node.name}`,
        sublabel: node.executionMode ? `[${node.executionMode}]` : node.type,
        description: node.description,
        color,
      },
      style: {
        background: color,
        color: '#fff',
        border: 'none',
        borderRadius: '8px',
        padding: '10px 16px',
        width: `${LAYOUT.NODE_WIDTH}px`,
        fontSize: '13px',
      },
    }
  })

  // Build Vue-Flow edge elements
  const vfEdges = workflow.edges.map((e) => ({
    id: e.id,
    source: e.source,
    target: e.target,
    label: e.label || undefined,
    animated: true,
    style: { stroke: '#909399', strokeWidth: 2 },
    labelStyle: { fontSize: 11, color: '#606266' },
    labelBgStyle: { fill: '#fff' },
    labelBgPadding: [4, 2] as [number, number],
    labelBgBorderRadius: 2,
  }))

  return { nodes: vfNodes, edges: vfEdges }
}

/**
 * Validate a workflow JSON object and return error messages.
 */
export function validateWorkflow(raw: unknown): string[] {
  const errors: string[] = []

  if (!raw || typeof raw !== 'object') {
    errors.push('Workflow must be a JSON object')
    return errors
  }

  const wf = raw as Record<string, unknown>

  if (!Array.isArray(wf.nodes)) {
    errors.push('Workflow must have a "nodes" array')
  } else if (wf.nodes.length === 0) {
    errors.push('Workflow must have at least one node')
  }

  if (wf.edges !== undefined && !Array.isArray(wf.edges)) {
    errors.push('"edges" must be an array')
  }

  return errors
}
