import { describe, it, expect, beforeEach, afterEach } from 'vitest'
import { setActivePinia, createPinia } from 'pinia'
import { useAgentStore } from '@/stores/agentStore'
import { useDaemonStore } from '@/stores/daemonStore'
import { usePackageStore } from '@/stores/packageStore'
import { resetApiClient, setApiClient } from '@/api/context'
import { mockGet, mockPost, mockPut, mockDel, makeClient, ok, err, trackGet } from './testUtils'

describe('agentStore', () => {
  beforeEach(() => {
    setActivePinia(createPinia())
  })
  afterEach(() => { resetApiClient() })

  it('loadAgents fetches and stores agents', async () => {
    const get = mockGet(async () =>
      ok({ agents: [{ id: 'test-agent', name: 'Test', status: 'running', workflow_count: 2 }] }),
    )
    setApiClient(makeClient(get, mockPut(), mockPost(), mockDel()))
    const store = useAgentStore()
    await store.loadAgents()
    expect(store.agents).toHaveLength(1)
    expect(store.agents[0].id).toBe('test-agent')
  })

  it('setCurrentAgent updates selection', () => {
    const store = useAgentStore()
    store.setCurrentAgent('agent-x')
    expect(store.currentAgentId).toBe('agent-x')
  })

  it('currentAgent returns null when none selected', () => {
    const store = useAgentStore()
    expect(store.currentAgent()).toBeNull()
  })

  it('reset clears agentId and state', () => {
    const store = useAgentStore()
    store.setCurrentAgent('agent-x')
    store.reset()
    expect(store.currentAgentId).toBeNull()
    expect(store.agents).toHaveLength(0)
  })
})

describe('daemonStore', () => {
  beforeEach(() => {
    setActivePinia(createPinia())
  })
  afterEach(() => { resetApiClient() })

  it('checkHealth sets disconnected on error', async () => {
    const get = mockGet(async () => { throw new Error('Network') })
    setApiClient(makeClient(get, mockPut(), mockPost(), mockDel()))
    const store = useDaemonStore()
    await store.checkHealth()
    expect(store.isConnected).toBe(false)
  })
})

