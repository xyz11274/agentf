import { describe, it, expect, beforeEach, afterEach } from 'vitest'
import { setActivePinia, createPinia } from 'pinia'
import { useAgentStore } from '@/stores/agentStore'
import { useSessionStore } from '@/stores/sessionStore'
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

  it('loadAgentPackage calls POST then GET agent info', async () => {
    const post = mockPost(async () => ok(null))
    const get = mockGet(async () =>
      ok({ agent: { id: 'test', status: 'loaded' } }),
    )
    setApiClient(makeClient(get, mockPut(), post, mockDel()))
    const store = useAgentStore()
    await store.loadAgentPackage('test')
    const { url } = trackGet(get)
    expect(url).toContain('/api/agents/test')
  })
})

describe('sessionStore', () => {
  beforeEach(() => {
    setActivePinia(createPinia())
  })
  afterEach(() => { resetApiClient() })

  it('loadSessions fetches sessions for agent', async () => {
    const get = mockGet(async () =>
      ok({ sessions: [{ session_id: 's1', user_id: 'u1', status: 'active' }] }),
    )
    setApiClient(makeClient(get, mockPut(), mockPost(), mockDel()))
    const store = useSessionStore()
    await store.loadSessions('test-agent')
    expect(store.sessions).toHaveLength(1)
    expect(store.sessions[0].session_id).toBe('s1')
  })

  it('destroySession calls DELETE', async () => {
    const del = mockDel(async () => ok(null))
    setApiClient(makeClient(mockGet(), mockPut(), mockPost(), del))
    const store = useSessionStore()
    await store.destroySession('test-agent', 's1')
    const { url } = trackGet(del)
    expect(url).toContain('s1')
  })
})

describe('daemonStore', () => {
  beforeEach(() => {
    setActivePinia(createPinia())
  })
  afterEach(() => { resetApiClient() })

  it('checkHealth updates status on success', async () => {
    const get = mockGet(async () =>
      ok({ status: 'ok', running: true, active_agents: 2 }),
    )
    setApiClient(makeClient(get, mockPut(), mockPost(), mockDel()))
    const store = useDaemonStore()
    await store.checkHealth()
    expect(store.isConnected).toBe(true)
  })

  it('checkHealth sets disconnected on error', async () => {
    const get = mockGet(async () => { throw new Error('Network') })
    setApiClient(makeClient(get, mockPut(), mockPost(), mockDel()))
    const store = useDaemonStore()
    await store.checkHealth()
    expect(store.isConnected).toBe(false)
  })
})

describe('packageStore', () => {
  beforeEach(() => {
    setActivePinia(createPinia())
  })
  afterEach(() => { resetApiClient() })

  it('loadPackages fetches package list', async () => {
    const get = mockGet(async () =>
      ok({ packages: [{ name: 'pkg1', path: '/pkgs/p1', file_count: 3 }] }),
    )
    setApiClient(makeClient(get, mockPut(), mockPost(), mockDel()))
    const store = usePackageStore()
    await store.loadPackages()
    expect(store.packages).toHaveLength(1)
    expect(store.packages[0].name).toBe('pkg1')
  })
})
