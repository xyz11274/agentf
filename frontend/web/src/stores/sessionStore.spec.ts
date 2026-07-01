import { afterEach, beforeEach, describe, expect, it } from 'vitest'
import { createPinia, setActivePinia } from 'pinia'
import type { SessionInfo } from '@/types/agentflow'
import { resetApiClient, setApiClient } from '@/api/context'
import { useSessionStore } from '@/stores/sessionStore'
import { err, makeClient, mockDel, mockGet, mockPost, mockPut, ok } from '@/__tests__/testUtils'

const sessions: SessionInfo[] = [
  { session_id: 's1', user_id: 'u', client_id: 'c1', age_ms: 10, status: 'active' },
  { session_id: 's2', user_id: 'u', client_id: 'c2', age_ms: 20, status: 'active' },
]

describe('useSessionStore', () => {
  beforeEach(() => {
    setActivePinia(createPinia())
  })

  afterEach(() => {
    resetApiClient()
  })

  it('starts empty', () => {
    const store = useSessionStore()
    expect(store.sessions).toEqual([])
    expect(store.agentId).toBeNull()
  })

  it('loadSessions fetches and stores the list with the agent id', async () => {
    const get = mockGet(async () => ok({ sessions }))
    setApiClient(makeClient(get, mockPut(), mockPost(), mockDel()))

    const store = useSessionStore()
    const result = await store.loadSessions('a1')

    expect(result).toEqual(sessions)
    expect(store.sessions).toEqual(sessions)
    expect(store.agentId).toBe('a1')
    const call = get.mock.calls[0]
    expect(call?.[0]).toBe('/api/sessions?agent_id=a1')
  })

  it('loadSessions records error on failure', async () => {
    const get = mockGet(async () => err(500, 'no daemon'))
    setApiClient(makeClient(get, mockPut(), mockPost(), mockDel()))

    const store = useSessionStore()
    await expect(store.loadSessions('a1')).rejects.toThrow('no daemon')
    expect(store.error).toBe('no daemon')
  })

  it('destroySession removes the session from the local list after success', async () => {
    const get = mockGet(async () => ok({ sessions }))
    const del = mockDel(async () => ok({}))
    setApiClient(makeClient(get, mockPut(), mockPost(), del))

    const store = useSessionStore()
    await store.loadSessions('a1')
    await store.destroySession('s1', 'a1')

    expect(store.sessions.map((s) => s.session_id)).toEqual(['s2'])
    const call = del.mock.calls[0]
    expect(call?.[0]).toBe('/api/sessions/s1?agent_id=a1')
  })

  it('destroySession does not mutate state on failure', async () => {
    const get = mockGet(async () => ok({ sessions }))
    const del = mockDel(async () => err(404, 'not found'))
    setApiClient(makeClient(get, mockPut(), mockPost(), del))

    const store = useSessionStore()
    await store.loadSessions('a1')
    await expect(store.destroySession('s1', 'a1')).rejects.toThrow('not found')
    expect(store.sessions).toHaveLength(2)
  })

  it('clearDialogue calls DELETE with all three params encoded', async () => {
    const get = mockGet(async () => ok({ sessions }))
    const del = mockDel(async () => ok({}))
    setApiClient(makeClient(get, mockPut(), mockPost(), del))

    const store = useSessionStore()
    await store.loadSessions('a1')
    await store.clearDialogue('s1', 'd/1', 'a1')

    const call = del.mock.calls[0]
    const url = call?.[0] as string | undefined
    expect(url).toBe('/api/sessions/s1/dialogue?agent_id=a1&dialogue_id=d%2F1')
  })

  it('clearDialogue propagates the error', async () => {
    const del = mockDel(async () => err(400, 'bad input'))
    setApiClient(makeClient(mockGet(), mockPut(), mockPost(), del))

    const store = useSessionStore()
    await expect(store.clearDialogue('s1', 'd1', 'a1')).rejects.toThrow('bad input')
    expect(store.error).toBe('bad input')
  })

  it('reset clears state', async () => {
    const get = mockGet(async () => ok({ sessions }))
    setApiClient(makeClient(get, mockPut(), mockPost(), mockDel()))
    const store = useSessionStore()
    await store.loadSessions('a1')
    store.reset()
    expect(store.sessions).toEqual([])
    expect(store.agentId).toBeNull()
  })
})
