import { afterEach, beforeEach, describe, expect, it } from 'vitest'
import { createPinia, setActivePinia } from 'pinia'
import type { AgentInfo } from '@/types/agentflow'
import { resetApiClient, setApiClient } from '@/api/context'
import { useAgentStore } from '@/stores/agentStore'
import { err, makeClient, mockDel, mockGet, mockPost, mockPut, ok, trackGet } from '@/__tests__/testUtils'

const sampleAgents: AgentInfo[] = [
  { id: 'a1', name: 'agent-one', workflow_count: 2, status: 'loaded' },
  { id: 'a2', name: 'agent-two', workflow_count: 0, status: 'unloaded' },
]

describe('useAgentStore', () => {
  beforeEach(() => {
    setActivePinia(createPinia())
  })

  afterEach(() => {
    resetApiClient()
  })

  it('starts with empty state', () => {
    const store = useAgentStore()
    expect(store.agents).toEqual([])
    expect(store.currentAgentId).toBeNull()
    expect(store.isLoading).toBe(false)
    expect(store.error).toBeNull()
    expect(store.currentAgent()).toBeNull()
  })

  it('loadAgents fetches /api/agents and stores the list', async () => {
    const get = mockGet(async () => ok({ agents: sampleAgents }))
    setApiClient(makeClient(get, mockPut(), mockPost(), mockDel()))

    const store = useAgentStore()
    const result = await store.loadAgents()

    expect(result).toEqual(sampleAgents)
    expect(store.agents).toEqual(sampleAgents)
    expect(get).toHaveBeenCalledTimes(1)
    expect(trackGet(get).url).toBe('/api/agents')
  })

  it('loadAgents captures the error and rethrows on failure', async () => {
    const get = mockGet(async () => err(500, 'boom'))
    setApiClient(makeClient(get, mockPut(), mockPost(), mockDel()))

    const store = useAgentStore()
    await expect(store.loadAgents()).rejects.toThrow('boom')
    expect(store.error).toBe('boom')
    expect(store.agents).toEqual([])
  })

  it('setCurrentAgent updates the current id and currentAgent() resolves it', () => {
    const store = useAgentStore()
    store.setCurrentAgent('a1')
    expect(store.currentAgentId).toBe('a1')
    expect(store.currentAgent()).toBeNull()
  })

  it('currentAgent() returns the matching agent once loaded', async () => {
    const get = mockGet(async () => ok({ agents: sampleAgents }))
    setApiClient(makeClient(get, mockPut(), mockPost(), mockDel()))
    const store = useAgentStore()
    await store.loadAgents()
    store.setCurrentAgent('a2')
    expect(store.currentAgent()).toEqual(sampleAgents[1])
  })

  it('loadAgent fetches a single agent and merges it into the list', async () => {
    const get = mockGet(async () => ok({ agent: sampleAgents[1] }))
    setApiClient(makeClient(get, mockPut(), mockPost(), mockDel()))
    const store = useAgentStore()
    const agent = await store.loadAgent('a2')

    expect(agent).toEqual(sampleAgents[1])
    expect(store.agents).toEqual([sampleAgents[1]])
    expect(trackGet(get).url).toBe('/api/agents/a2')
  })

  it('loadAgent replaces an existing entry with the same id', async () => {
    const get = mockGet(async () =>
      ok({ agent: { id: 'a1', name: 'agent-one', workflow_count: 5, status: 'running' } }),
    )
    setApiClient(makeClient(get, mockPut(), mockPost(), mockDel()))
    const store = useAgentStore()
    await store.loadAgent('a1')
    const updated = await store.loadAgent('a1')

    expect(updated.workflow_count).toBe(5)
    expect(store.agents).toHaveLength(1)
    expect(store.agents[0]?.status).toBe('running')
  })

  it('reset clears all state', async () => {
    const get = mockGet(async () => ok({ agents: sampleAgents }))
    setApiClient(makeClient(get, mockPut(), mockPost(), mockDel()))
    const store = useAgentStore()
    await store.loadAgents()
    store.setCurrentAgent('a1')
    store.reset()

    expect(store.agents).toEqual([])
    expect(store.currentAgentId).toBeNull()
    expect(store.error).toBeNull()
  })

  it('loadAgentPackage POSTs to /api/agents/{id}/load and refreshes the agent', async () => {
    const refreshed: AgentInfo = { id: 'a2', name: 'agent-two', workflow_count: 3, status: 'loaded' }
    const post = mockPost(async () => ok({}))
    const get = mockGet(async () => ok({ agent: refreshed }))
    setApiClient(makeClient(get, mockPut(), post, mockDel()))
    const store = useAgentStore()

    const result = await store.loadAgentPackage('a2')

    expect(result).toEqual(refreshed)
    expect(store.agents).toEqual([refreshed])
    expect(post).toHaveBeenCalledTimes(1)
    expect(get).toHaveBeenCalledTimes(1)
    expect(trackGet(get).url).toBe('/api/agents/a2')
  })

  it('loadAgentPackage passes package_path in the request body when provided', async () => {
    const post = mockPost(async () => ok({}))
    const get = mockGet(async () => ok({ agent: sampleAgents[1] }))
    setApiClient(makeClient(get, mockPut(), post, mockDel()))
    const store = useAgentStore()

    await store.loadAgentPackage('a2', '/tmp/custom-package')

    expect(post).toHaveBeenCalledTimes(1)
    const call = post.mock.calls[0]
    expect(call?.[0]).toBe('/api/agents/a2/load')
    const init = call?.[1]
    expect(JSON.parse(String(init?.body))).toEqual({ package_path: '/tmp/custom-package' })
  })

  it('loadAgentPackage captures errors and rethrows', async () => {
    const post = mockPost(async () => err(502, 'daemon down'))
    setApiClient(makeClient(mockGet(), mockPut(), post, mockDel()))
    const store = useAgentStore()

    await expect(store.loadAgentPackage('a2')).rejects.toThrow('daemon down')
    expect(store.error).toBe('daemon down')
  })

  it('unloadAgent POSTs to /api/agents/{id}/unload and refreshes', async () => {
    const refreshed: AgentInfo = { id: 'a1', name: 'agent-one', workflow_count: 2, status: 'unloaded' }
    const post = mockPost(async () => ok({}))
    const get = mockGet(async () => ok({ agent: refreshed }))
    setApiClient(makeClient(get, mockPut(), post, mockDel()))
    const store = useAgentStore()

    const result = await store.unloadAgent('a1')

    expect(result).toEqual(refreshed)
    expect(store.agents[0]?.status).toBe('unloaded')
    expect(post).toHaveBeenCalledTimes(1)
    expect(post.mock.calls[0]?.[0]).toBe('/api/agents/a1/unload')
  })

  it('pauseAgent POSTs to /api/agents/{id}/pause and refreshes', async () => {
    const refreshed: AgentInfo = { id: 'a1', name: 'agent-one', workflow_count: 2, status: 'paused' }
    const post = mockPost(async () => ok({}))
    const get = mockGet(async () => ok({ agent: refreshed }))
    setApiClient(makeClient(get, mockPut(), post, mockDel()))
    const store = useAgentStore()

    const result = await store.pauseAgent('a1')

    expect(result).toEqual(refreshed)
    expect(store.agents[0]?.status).toBe('paused')
    expect(post.mock.calls[0]?.[0]).toBe('/api/agents/a1/pause')
  })

  it('resumeAgent POSTs to /api/agents/{id}/resume and refreshes', async () => {
    const refreshed: AgentInfo = { id: 'a1', name: 'agent-one', workflow_count: 2, status: 'running' }
    const post = mockPost(async () => ok({}))
    const get = mockGet(async () => ok({ agent: refreshed }))
    setApiClient(makeClient(get, mockPut(), post, mockDel()))
    const store = useAgentStore()

    const result = await store.resumeAgent('a1')

    expect(result).toEqual(refreshed)
    expect(store.agents[0]?.status).toBe('running')
    expect(post.mock.calls[0]?.[0]).toBe('/api/agents/a1/resume')
  })

  it('action errors clear isLoading in the finally block', async () => {
    const post = mockPost(async () => err(500, 'oops'))
    setApiClient(makeClient(mockGet(), mockPut(), post, mockDel()))
    const store = useAgentStore()

    await expect(store.pauseAgent('a1')).rejects.toThrow('oops')
    expect(store.isLoading).toBe(false)
    expect(store.error).toBe('oops')
  })
})
