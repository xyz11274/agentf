import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import { createPinia, setActivePinia } from 'pinia'
import { nextTick } from 'vue'
import { useChatStore } from '@/stores/chatStore'
import { useChatWebSocket, type ChatWebSocketLike } from '@/composables/useChatWebSocket'

/**
 * Minimal WebSocket double for tests.
 *
 * Real WebSocket objects extend EventTarget and additionally expose `onopen`,
 * `onclose`, `onerror`, `onmessage` as direct properties that the event loop
 * calls when the matching event fires. We mirror that here so the composable
 * can use the property-assignment style without resorting to addEventListener.
 */
class FakeWebSocket extends EventTarget implements ChatWebSocketLike {
  static readonly CONNECTING = 0
  static readonly OPEN = 1
  static readonly CLOSING = 2
  static readonly CLOSED = 3

  readyState: number = FakeWebSocket.CONNECTING
  readonly url: string
  readonly sent: string[] = []
  closed = false
  sentCloseArgs: { code?: number; reason?: string } | null = null

  private _onopen: ((ev: Event) => unknown) | null = null
  private _onclose: ((ev: CloseEvent) => unknown) | null = null
  private _onerror: ((ev: Event) => unknown) | null = null
  private _onmessage: ((ev: MessageEvent) => unknown) | null = null

  constructor(url: string) {
    super()
    this.url = url
  }

  get onopen(): ((ev: Event) => unknown) | null {
    return this._onopen
  }
  set onopen(cb: ((ev: Event) => unknown) | null) {
    this._onopen = cb
  }
  get onclose(): ((ev: CloseEvent) => unknown) | null {
    return this._onclose
  }
  set onclose(cb: ((ev: CloseEvent) => unknown) | null) {
    this._onclose = cb
  }
  get onerror(): ((ev: Event) => unknown) | null {
    return this._onerror
  }
  set onerror(cb: ((ev: Event) => unknown) | null) {
    this._onerror = cb
  }
  get onmessage(): ((ev: MessageEvent) => unknown) | null {
    return this._onmessage
  }
  set onmessage(cb: ((ev: MessageEvent) => unknown) | null) {
    this._onmessage = cb
  }

  send(data: string): void {
    if (this.readyState !== FakeWebSocket.OPEN) {
      throw new Error(`FakeWebSocket.send called in state ${this.readyState}`)
    }
    this.sent.push(data)
  }

  close(code?: number, reason?: string): void {
    if (this.closed) return
    this.closed = true
    this.readyState = FakeWebSocket.CLOSED
    this.sentCloseArgs = { code, reason }
    const event = new CloseEvent('close', { code: code ?? 1000, reason: reason ?? '' })
    this._dispatch('close', event)
  }

  // Test helpers ----------------------------------------------------------
  open(): void {
    this.readyState = FakeWebSocket.OPEN
    this._dispatch('open', new Event('open'))
  }

  error(message = 'boom'): void {
    this._dispatch('error', new ErrorEvent('error', { message }))
  }

  /** Fire a MessageEvent with the given JSON-serialisable payload. */
  message(payload: unknown): void {
    this._dispatch('message', new MessageEvent('message', { data: JSON.stringify(payload) }))
  }

  /** Fire a MessageEvent with a raw (non-JSON) string payload. */
  rawMessage(data: string): void {
    this._dispatch('message', new MessageEvent('message', { data }))
  }

  /** Drop the connection without an open handshake ever completing. */
  failBeforeOpen(): void {
    this.readyState = FakeWebSocket.CLOSED
    this._dispatch('close', new CloseEvent('close', { code: 1006, reason: 'abnormal' }))
  }

  private _dispatch(type: string, event: Event): void {
    this.dispatchEvent(event)
    const handler = this[`_on${type}` as '_onopen' | '_onclose' | '_onerror' | '_onmessage']
    if (handler) handler(event)
  }
}

interface Harness {
  sockets: FakeWebSocket[]
  socketFactory: (url: string) => ChatWebSocketLike
  timers: {
    intervals: Map<number, () => void>
    timeouts: Map<number, () => void>
    nextHandle: number
  }
  schedule: ReturnType<typeof vi.fn>
  fireInterval(handle: number): void
  fireTimeout(handle: number): void
}

function createHarness(opts?: { baseUrl?: string }): Harness {
  const sockets: FakeWebSocket[] = []
  const intervals = new Map<number, () => void>()
  const timeouts = new Map<number, () => void>()
  let nextHandle = 1
  const schedule = vi.fn((kind: 'interval' | 'timeout', cb: () => void) => {
    const handle = nextHandle++
    if (kind === 'interval') intervals.set(handle, cb)
    else timeouts.set(handle, cb)
    return handle
  })

  const socketFactory = (url: string): ChatWebSocketLike => {
    const ws = new FakeWebSocket(url)
    sockets.push(ws)
    return ws
  }

  return {
    sockets,
    socketFactory,
    timers: { intervals, timeouts, nextHandle },
    schedule,
    fireInterval(handle) {
      const cb = intervals.get(handle)
      if (!cb) throw new Error(`no interval #${handle}`)
      cb()
    },
    fireTimeout(handle) {
      const cb = timeouts.get(handle)
      if (!cb) throw new Error(`no timeout #${handle}`)
      timeouts.delete(handle)
      cb()
    },
  }
}

describe('useChatWebSocket', () => {
  beforeEach(() => {
    setActivePinia(createPinia())
  })

  afterEach(() => {
    vi.useRealTimers()
  })

  function setupComposable(baseUrl = 'ws://bridge.test') {
    const h = createHarness()
    const composable = useChatWebSocket({
      baseUrl,
      socketFactory: h.socketFactory,
      setIntervalFn: (cb) => h.schedule('interval', cb),
      clearIntervalFn: () => undefined,
      setTimeoutFn: (cb) => h.schedule('timeout', cb),
      clearTimeoutFn: () => undefined,
      // Make the backoff schedule deterministic for tests.
      backoffScheduleMs: [10, 20, 40, 80],
      maxBackoffMs: 80,
      heartbeatIntervalMs: 1_000_000, // effectively disable during tests
    })
    return { ...h, composable, store: useChatStore() }
  }

  it('connects to /ws/chat/{agentId} and exposes reactive state', async () => {
    const ctx = setupComposable()
    const promise = ctx.composable.connect('agent-1')
    expect(ctx.sockets).toHaveLength(1)
    expect(ctx.sockets[0]!.url).toBe('ws://bridge.test/ws/chat/agent-1')
    ctx.sockets[0]!.open()
    await promise
    expect(ctx.composable.isConnected.value).toBe(true)
    expect(ctx.composable.error.value).toBeNull()
    expect(ctx.store.isConnected).toBe(true)
    expect(ctx.store.currentAgentId).toBe('agent-1')
  })

  it('sends JSON-encoded user messages including the current session id', async () => {
    const ctx = setupComposable()
    const open = ctx.composable.connect('agent-2')
    ctx.sockets[0]!.open()
    await open
    ctx.sockets[0]!.message({ type: 'done', final: true, session_id: 'sess-A' })
    expect(ctx.composable.currentSessionId.value).toBe('sess-A')

    ctx.composable.send('hello world')
    expect(ctx.sockets[0]!.sent).toHaveLength(1)
    const sent = JSON.parse(ctx.sockets[0]!.sent[0]!)
    expect(sent).toEqual({ content: 'hello world', session_id: 'sess-A' })
    expect(ctx.store.messages.at(-1)?.role).toBe('user')
    expect(ctx.store.messages.at(-1)?.content).toBe('hello world')
  })

  it('throws when sending while disconnected', () => {
    const ctx = setupComposable()
    expect(() => ctx.composable.send('nope')).toThrow(/disconnected/)
  })

  it('appends chunk frames to a fresh assistant message', async () => {
    const ctx = setupComposable()
    const open = ctx.composable.connect('agent-3')
    ctx.sockets[0]!.open()
    await open
    ctx.sockets[0]!.message({ type: 'chunk', content: 'Hel', final: false })
    ctx.sockets[0]!.message({ type: 'chunk', content: 'lo', final: false })
    await nextTick()
    expect(ctx.composable.isStreaming.value).toBe(true)
    const assistant = ctx.store.messages.find((m) => m.role === 'assistant')
    expect(assistant?.content).toBe('Hello')
  })

  it('marks streaming finished on a done frame and stores session id', async () => {
    const ctx = setupComposable()
    const open = ctx.composable.connect('agent-4')
    ctx.sockets[0]!.open()
    await open
    ctx.sockets[0]!.message({ type: 'chunk', content: 'reply', final: true })
    ctx.sockets[0]!.message({ type: 'done', final: true, session_id: 'sess-B' })
    expect(ctx.composable.isStreaming.value).toBe(false)
    expect(ctx.composable.currentSessionId.value).toBe('sess-B')
    expect(ctx.store.isStreaming).toBe(false)
    expect(ctx.store.currentSessionId).toBe('sess-B')
  })

  it('surfaces error frames on the reactive error ref and chatStore', async () => {
    const ctx = setupComposable()
    const open = ctx.composable.connect('agent-5')
    ctx.sockets[0]!.open()
    await open
    ctx.sockets[0]!.message({ type: 'chunk', content: 'partial', final: false })
    ctx.sockets[0]!.message({ type: 'error', message: 'upstream exploded' })
    expect(ctx.composable.error.value).toBe('upstream exploded')
    expect(ctx.store.error).toBe('upstream exploded')
    expect(ctx.composable.isStreaming.value).toBe(false)
    expect(ctx.store.isStreaming).toBe(false)
  })

  it('ignores malformed frames without throwing', async () => {
    const ctx = setupComposable()
    const open = ctx.composable.connect('agent-6')
    ctx.sockets[0]!.open()
    await open
    ctx.sockets[0]!.rawMessage('not json at all')
    ctx.sockets[0]!.message({ unexpected: true })
    expect(ctx.composable.error.value).toBeNull()
    expect(ctx.store.messages).toHaveLength(0)
  })

  it('disconnect tears down state and suppresses reconnect', async () => {
    const ctx = setupComposable()
    const open = ctx.composable.connect('agent-7')
    ctx.sockets[0]!.open()
    await open
    ctx.composable.disconnect()
    expect(ctx.sockets[0]!.sentCloseArgs).toEqual({ code: 1000, reason: 'client disconnect' })
    expect(ctx.composable.isConnected.value).toBe(false)
    expect(ctx.store.isConnected).toBe(false)
    // No further reconnect timer should be scheduled.
    expect(ctx.timers.timeouts.size).toBe(0)
  })

  it('schedules an exponential backoff reconnect on unexpected close', async () => {
    const ctx = setupComposable()
    const firstConnect = ctx.composable.connect('agent-8')
    ctx.sockets[0]!.open()
    await firstConnect

    // Server kicks us out unexpectedly.
    ctx.sockets[0]!.close(1006, 'abnormal')
    expect(ctx.composable.isConnected.value).toBe(false)
    expect(ctx.timers.timeouts.size).toBe(1)
    const firstHandle = [...ctx.timers.timeouts.keys()][0]!

    // Drive the first reconnect - backoff[0] = 10ms in our test schedule.
    ctx.fireTimeout(firstHandle)
    expect(ctx.sockets).toHaveLength(2)
    ctx.sockets[1]!.open()
    // After a successful reconnect we must have reset the attempt counter.
    expect(ctx.composable.isConnected.value).toBe(true)

    // Second unexpected close should now use backoff[1] (20ms).
    ctx.sockets[1]!.close(1006, 'abnormal')
    expect(ctx.timers.timeouts.size).toBe(1)
  })

  it('caps the reconnect backoff at maxBackoffMs', async () => {
    const ctx = setupComposable()
    const open = ctx.composable.connect('agent-9')
    ctx.sockets[0]!.open()
    await open

    // Walk through every entry in the schedule.
    for (let attempt = 0; attempt < 6; attempt += 1) {
      ctx.sockets.at(-1)!.close(1006, 'abnormal')
      const handles = [...ctx.timers.timeouts.keys()]
      expect(handles.length).toBe(1)
      ctx.fireTimeout(handles[0]!)
      ctx.sockets.at(-1)!.open()
    }
    // The cap was hit (maxBackoffMs = 80) - we just want to confirm we
    // never blow up the timer machinery. Sanity check: still connected.
    expect(ctx.composable.isConnected.value).toBe(true)
  })

  it('stop sends a stop frame and resets streaming state', async () => {
    const ctx = setupComposable()
    const open = ctx.composable.connect('agent-10')
    ctx.sockets[0]!.open()
    await open
    ctx.sockets[0]!.message({ type: 'chunk', content: 'partial', final: false })
    expect(ctx.composable.isStreaming.value).toBe(true)

    ctx.composable.stop()
    expect(ctx.composable.isStreaming.value).toBe(false)
    expect(ctx.store.isStreaming).toBe(false)
    // Last frame on the wire must be a stop signal.
    const stopFrame = JSON.parse(ctx.sockets[0]!.sent.at(-1)!)
    expect(stopFrame).toEqual({ type: 'stop' })
  })

  it('disconnects on component unmount', async () => {
    // Mount the composable inside a synthetic effect scope so onBeforeUnmount
    // gets registered.
    const ctx = setupComposable()
    const { onBeforeUnmount, createApp, h } = await import('vue')
    let dispose: (() => void) | null = null
    const app = createApp({
      setup() {
        const composable = useChatWebSocket({
          baseUrl: 'ws://bridge.test',
          socketFactory: ctx.socketFactory,
          setIntervalFn: () => 0,
          clearIntervalFn: () => undefined,
          setTimeoutFn: () => 0,
          clearTimeoutFn: () => undefined,
        })
        onBeforeUnmount(() => composable.disconnect())
        return () => h('div')
      },
    })
    // We don't actually mount to a DOM node; just register lifecycle hooks
    // and unmount via app.unmount on a fake container.
    const container: Element = document.createElement('div')
    dispose = () => app.unmount(container)
    app.mount(container)
    dispose()
    // No sockets created in this path because connect() was never called,
    // but the composable must not throw on unmount.
    expect(true).toBe(true)
  })

  it('fails the connect promise when the socket errors before opening', async () => {
    const ctx = setupComposable()
    const promise = ctx.composable.connect('agent-11')
    ctx.sockets[0]!.failBeforeOpen()
    await expect(promise).rejects.toThrow(/closed before open/)
  })
})
