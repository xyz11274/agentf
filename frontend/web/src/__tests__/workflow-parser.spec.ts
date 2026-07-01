import { describe, it, expect } from 'vitest'
import { parseWorkflow, toVueFlowElements, validateWorkflow } from '@/utils/workflow-parser'

describe('workflow-parser', () => {
  it('parseWorkflow extracts nodes and edges', () => {
    const wf = parseWorkflow({
      id: 'test',
      name: 'Test Workflow',
      nodes: [
        { id: 'n1', name: 'Node 1', type: 'task' },
        { id: 'n2', name: 'Node 2', type: 'task' },
      ],
      edges: [
        { id: 'e1', source: 'n1', target: 'n2' },
      ],
    })
    expect(wf.id).toBe('test')
    expect(wf.nodes).toHaveLength(2)
    expect(wf.edges).toHaveLength(1)
  })

  it('parseWorkflow handles empty edges with auto-derivation', () => {
    const wf = parseWorkflow({
      nodes: [
        { id: 'a' },
        { id: 'b' },
        { id: 'c' },
      ],
    })
    // Auto-edges: a→b, b→c
    expect(wf.edges).toHaveLength(2)
    expect(wf.edges[0].source).toBe('a')
    expect(wf.edges[0].target).toBe('b')
    expect(wf.edges[1].source).toBe('b')
    expect(wf.edges[1].target).toBe('c')
  })

  it('parseWorkflow handles missing edges', () => {
    const wf = parseWorkflow({ nodes: [{ id: 'a' }] })
    expect(wf.nodes).toHaveLength(1)
    expect(wf.edges).toHaveLength(0)
  })

  it('toVueFlowElements creates node/edge arrays', () => {
    const wf = parseWorkflow({
      nodes: [{ id: 'x' }, { id: 'y' }],
    })
    const { nodes, edges } = toVueFlowElements(wf)
    expect(nodes).toHaveLength(2)
    // Auto edges: x→y
    expect(edges).toHaveLength(1)
    expect(edges[0].source).toBe('x')
    expect(edges[0].target).toBe('y')
  })

  it('nodes have position and style data', () => {
    const wf = parseWorkflow({ nodes: [{ id: 'a' }] })
    const { nodes } = toVueFlowElements(wf)
    expect(nodes[0].position).toBeDefined()
    expect(nodes[0].position.x).toBeGreaterThanOrEqual(0)
    expect(nodes[0].position.y).toBeGreaterThanOrEqual(0)
  })

  it('validateWorkflow detects missing nodes', () => {
    const errors = validateWorkflow({})
    expect(errors.length).toBeGreaterThan(0)
    expect(errors.some((e) => e.includes('nodes'))).toBe(true)
  })

  it('validateWorkflow passes for valid workflow', () => {
    const errors = validateWorkflow({
      nodes: [{ id: 'a' }],
      edges: [],
    })
    expect(errors).toHaveLength(0)
  })

  it('execution_mode colors are applied', () => {
    const wf = parseWorkflow({
      nodes: [
        { id: 'a', execution_mode: 'python_script' },
        { id: 'b', execution_mode: 'shell_script' },
        { id: 'c', execution_mode: 'so_library' },
      ],
    })
    const { nodes } = toVueFlowElements(wf)
    const styles = nodes.map((n) => (n.data as any).color)
    // python=#67c23a, shell=#e6a23c, so=#f56c6c
    expect(styles[0]).toBe('#67c23a')
    expect(styles[1]).toBe('#e6a23c')
    expect(styles[2]).toBe('#f56c6c')
  })
})
