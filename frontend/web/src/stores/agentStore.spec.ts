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

  it('action errors clear isLoading in the finally block', async () => {
    const post = mockPost(async () => err(500, 'oops'))
    setApiClient(makeClient(mockGet(), mockPut(), post, mockDel()))
    const store = useAgentStore()

    await expect(store.pauseAgent('a1')).rejects.toThrow('oops')
    expect(store.isLoading).toBe(false)
    expect(store.error).toBe('oops')
  })
})
