import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import { createPinia, setActivePinia } from 'pinia'
import type { DaemonStatus } from '@/types/agentflow'
import { resetApiClient, setApiClient } from '@/api/context'
import { DEFAULT_POLL_INTERVAL_MS, useDaemonStore } from '@/stores/daemonStore'
import { err, makeClient, mockDel, mockGet, mockPost, mockPut, ok } from '@/__tests__/testUtils'

const status: DaemonStatus = {
  status: 'running',
  version: '0.1.0',
  uptime: 100,
  active_agents: 2,
  running: true,
}

describe('useDaemonStore', () => {
  beforeEach(() => {
    setActivePinia(createPinia())
    vi.useFakeTimers()
  })

  afterEach(() => {
    vi.useRealTimers()
    resetApiClient()
  })

  it('starts with no status and disconnected', () => {
    const store = useDaemonStore()
    expect(store.status).toBeNull()
    expect(store.isConnected).toBe(false)
    expect(store.error).toBeNull()
  })

  it('checkHealth returns true and marks connected when status is ok', async () => {
    const get = mockGet(async () => ok({ status: 'ok' }))
    setApiClient(makeClient(get, mockPut(), mockPost(), mockDel()))
    const store = useDaemonStore()
    const ok1 = await store.checkHealth()
    expect(ok1).toBe(true)
    expect(store.isConnected).toBe(true)
    expect(store.error).toBeNull()
  })

  it('checkHealth marks disconnected and stores error on failure', async () => {
    const get = mockGet(async () => err(503, 'bridge down'))
    setApiClient(makeClient(get, mockPut(), mockPost(), mockDel()))
    const store = useDaemonStore()
    const ok1 = await store.checkHealth()
    expect(ok1).toBe(false)
    expect(store.isConnected).toBe(false)
    expect(store.error).toBe('bridge down')
  })

  it('checkHealth treats a non-ok status as not-connected with an error message', async () => {
    const get = mockGet(async () => ok({ status: 'degraded' }))
    setApiClient(makeClient(get, mockPut(), mockPost(), mockDel()))
    const store = useDaemonStore()
    const ok1 = await store.checkHealth()
    expect(ok1).toBe(false)
    expect(store.isConnected).toBe(false)
    expect(store.error).toContain('degraded')
  })

  it('getDaemonStatus stores status and reflects running flag', async () => {
    const get = mockGet(async () => ok(status))
    setApiClient(makeClient(get, mockPut(), mockPost(), mockDel()))
    const store = useDaemonStore()
    const r = await store.getDaemonStatus()
    expect(r).toEqual(status)
    expect(store.status).toEqual(status)
    expect(store.isConnected).toBe(true)
  })

  it('getDaemonStatus flips isConnected to false when running is false', async () => {
    const get = mockGet(async () => ok({ ...status, running: false }))
    setApiClient(makeClient(get, mockPut(), mockPost(), mockDel()))
    const store = useDaemonStore()
    await store.getDaemonStatus()
    expect(store.isConnected).toBe(false)
  })

  it('getDaemonStatus throws and clears connected state on HTTP failure', async () => {
    const get = mockGet(async () => err(500, 'crash'))
    setApiClient(makeClient(get, mockPut(), mockPost(), mockDel()))
    const store = useDaemonStore()
    await expect(store.getDaemonStatus()).rejects.toThrow('crash')
    expect(store.isConnected).toBe(false)
    expect(store.error).toBe('crash')
  })

  it('init kicks off polling and triggers checkHealth once immediately', async () => {
    const get = mockGet(async () => ok({ status: 'ok' }))
    setApiClient(makeClient(get, mockPut(), mockPost(), mockDel()))
    const store = useDaemonStore()
    store.init(1000)
    expect(get).toHaveBeenCalledTimes(1)
    await vi.advanceTimersByTimeAsync(1000)
    expect(get).toHaveBeenCalledTimes(2)
    await vi.advanceTimersByTimeAsync(1000)
    expect(get).toHaveBeenCalledTimes(3)
    store.stopPolling()
  })

  it('init defaults to the 15s project-standard interval', () => {
    expect(DEFAULT_POLL_INTERVAL_MS).toBe(15000)
  })

  it('startPolling is idempotent and stopPolling clears the handle', async () => {
    const get = mockGet(async () => ok({ status: 'ok' }))
    setApiClient(makeClient(get, mockPut(), mockPost(), mockDel()))
    const store = useDaemonStore()
    store.startPolling(1000)
    store.startPolling(1000)
    await vi.advanceTimersByTimeAsync(1000)
    expect(get).toHaveBeenCalledTimes(1)
    store.stopPolling()
    await vi.advanceTimersByTimeAsync(1000)
    expect(get).toHaveBeenCalledTimes(1)
  })

  it('dispose stops polling and clears state', async () => {
    const get = mockGet(async () => ok({ status: 'ok' }))
    setApiClient(makeClient(get, mockPut(), mockPost(), mockDel()))
    const store = useDaemonStore()
    store.init(500)
    await store.getDaemonStatus()
    const callsBefore = get.mock.calls.length
    store.dispose()
    expect(store.status).toBeNull()
    expect(store.isConnected).toBe(false)
    await vi.advanceTimersByTimeAsync(1000)
    expect(get.mock.calls.length).toBe(callsBefore)
  })
})
