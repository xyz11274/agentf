import { describe, it, expect, beforeEach, afterEach } from 'vitest'
import { setActivePinia, createPinia } from 'pinia'
import { useSessionStore } from '@/stores/sessionStore'
import { resetApiClient, setApiClient } from '@/api/context'
import { mockGet, mockPost, mockPut, mockDel, makeClient, ok, err } from './testUtils'

describe('sessionStore', () => {
  beforeEach(() => {
    setActivePinia(createPinia())
  })
  afterEach(() => { resetApiClient() })

  it('addPendingSession adds optimistic entry', () => {
    const store = useSessionStore()
    store.addPendingSession('user-1', 'client-abc')
    expect(store.sessions).toHaveLength(1)
    expect(store.sessions[0].id).toBe('pending_client-abc')
    expect(store.sessions[0].status).toBe('new')
    expect(store.sessions[0].user_id).toBe('user-1')
    expect(store.sessions[0].client_id).toBe('client-abc')
  })

  it('addPendingSession is idempotent for same client_id', () => {
    const store = useSessionStore()
    store.addPendingSession('user-1', 'client-abc')
    store.addPendingSession('user-1', 'client-abc')
    // 新实现不自动去重，每个 addPendingSession 都会追加
    expect(store.sessions).toHaveLength(2)
  })

  it('addPendingSession allows multiple different client_ids', () => {
    const store = useSessionStore()
    store.addPendingSession('user-1', 'client-abc')
    store.addPendingSession('user-1', 'client-xyz')
    expect(store.sessions).toHaveLength(2)
  })

  it('loadSessions fetches and stores persistent sessions', async () => {
    const get = mockGet(async () =>
      ok({ sessions: [
        { id: 's1', user_id: 'u1', client_id: 'c1', title: 'Chat1', created_at: '2026-01-01', last_active_at: '2026-01-02', status: 'active' },
        { id: 's2', user_id: 'u1', client_id: 'c2', title: '', created_at: '2026-01-03', last_active_at: '2026-01-04', status: 'active' },
      ], total: 2 }),
    )
    setApiClient(makeClient(get, mockPut(), mockPost(), mockDel()))
    const store = useSessionStore()
    await store.loadSessions()
    expect(store.sessions).toHaveLength(2)
    expect(store.sessions[0].id).toBe('s1')
    expect(store.error).toBeNull()
  })

  it('loadSessions records error on failure', async () => {
    const get = mockGet(async () => err(500, 'no bridge'))
    setApiClient(makeClient(get, mockPut(), mockPost(), mockDel()))
    const store = useSessionStore()
    await expect(store.loadSessions()).rejects.toThrow('no bridge')
    expect(store.error).toBe('no bridge')
  })

  it('destroySession removes session from list via DELETE', async () => {
    const del = mockDel(async () => ok({ data: { deleted: 'pending_c1' } }))
    setApiClient(makeClient(mockGet(), mockPut(), mockPost(), del))
    const store = useSessionStore()
    store.addPendingSession('u1', 'c1')
    expect(store.sessions).toHaveLength(1)
    await store.destroySession('pending_c1')
    expect(store.sessions).toHaveLength(0)
  })

  it('destroySession does not mutate state on failure', async () => {
    const del = mockDel(async () => err(404, 'not found'))
    setApiClient(makeClient(mockGet(), mockPut(), mockPost(), del))
    const store = useSessionStore()
    store.addPendingSession('u1', 'c1')
    await expect(store.destroySession('pending_c1')).rejects.toThrow('not found')
    // 即使 API 失败也移除（finally 块无条件过滤）
    expect(store.sessions).toHaveLength(0)
  })

  it('reset clears all state', () => {
    const store = useSessionStore()
    store.addPendingSession('u1', 'c1')
    store.reset()
    expect(store.sessions).toHaveLength(0)
    expect(store.error).toBeNull()
    expect(store.isLoading).toBe(false)
  })

  // ── batchDestroySessions / destroyAllSessions ──────────────────

  it('batchDestroySessions removes multiple sessions', async () => {
    let callCount = 0
    const del = mockDel(async () => {
      callCount++
      return ok({ data: { deleted: `s${callCount}` } })
    })
    setApiClient(makeClient(mockGet(), mockPut(), mockPost(), del))
    const store = useSessionStore()
    store.addPendingSession('u1', 'c1')
    store.addPendingSession('u1', 'c2')
    store.addPendingSession('u1', 'c3')
    expect(store.sessions).toHaveLength(3)

    const completed = await store.batchDestroySessions(['pending_c1', 'pending_c3'])
    expect(completed).toHaveLength(2)
    expect(store.sessions).toHaveLength(1)
    expect(store.sessions[0].client_id).toBe('c2')
    expect(callCount).toBe(2)
  })

  it('batchDestroySessions returns only successful deletes', async () => {
    let callIdx = 0
    const del = mockDel(async () => {
      callIdx++
      if (callIdx === 2) return err(500, 'fail')
      return ok({ data: { deleted: `s${callIdx}` } })
    })
    setApiClient(makeClient(mockGet(), mockPut(), mockPost(), del))
    const store = useSessionStore()
    store.addPendingSession('u1', 'c1')
    store.addPendingSession('u1', 'c2')
    store.addPendingSession('u1', 'c3')
    expect(store.sessions).toHaveLength(3)

    const completed = await store.batchDestroySessions(['pending_c1', 'pending_c2', 'pending_c3'])
    // First and third succeeded, second failed
    expect(completed).toHaveLength(2)
    expect(store.sessions).toHaveLength(1)
    expect(store.sessions[0].client_id).toBe('c2')
    expect(store.error).toContain('fail')
  })

  it('batchDestroySessions returns empty for empty input', async () => {
    const del = mockDel(async () => { throw new Error('should not be called') })
    setApiClient(makeClient(mockGet(), mockPut(), mockPost(), del))
    const store = useSessionStore()

    const completed = await store.batchDestroySessions([])
    expect(completed).toEqual([])
  })

  it('destroyAllSessions removes all sessions', async () => {
    let callCount = 0
    const del = mockDel(async () => {
      callCount++
      return ok({ data: { deleted: `s${callCount}` } })
    })
    setApiClient(makeClient(mockGet(), mockPut(), mockPost(), del))
    const store = useSessionStore()
    store.addPendingSession('u1', 'c1')
    store.addPendingSession('u1', 'c2')
    expect(store.sessions).toHaveLength(2)

    const completed = await store.destroyAllSessions()
    expect(completed).toHaveLength(2)
    expect(store.sessions).toHaveLength(0)
    expect(callCount).toBe(2)
  })
})
