/**
 * Composable for managing a WebSocket chat connection to the bridge.
 *
 * Lives in a single component instance. Multiple simultaneous connections
 * are NOT supported — the composable manages exactly one socket.
 *
 * Key design: the client identifies itself via ``user_id`` + ``client_id``.
 * ``session_id`` is a daemon-internal concept and is NOT part of the protocol.
 */

import { onBeforeUnmount, ref, type Ref } from 'vue'
import { useChatStore } from '@/stores/chatStore'
import type { ChatDone } from '@/types/agentflow'

// ---------------------------------------------------------------------------
// Types
// ---------------------------------------------------------------------------

/**
 * Minimal WebSocket-like interface that can be mocked in tests.
 */
export interface ChatWebSocketLike {
  readyState: number
  send(data: string): void
  close(): void
  onopen: ((ev: Event) => void) | null
  onmessage: ((ev: MessageEvent) => void) | null
  onerror: ((ev: Event) => void) | null
  onclose: ((ev: CloseEvent) => void) | null
}

/**
 * Client -> bridge frame sent over the WebSocket.
 * Uses ``user_id`` + ``client_id`` instead of ``session_id``.
 */
export interface ChatClientFrame {
  user_id: string
  client_id: string
  content: string
}

export interface UseChatWebSocketOptions {
  /** Base URL for the WebSocket endpoint (default: derived from window.location). */
  baseUrl?: string
  /** Factory for creating WebSocket instances (default: native WebSocket). */
  socketFactory?: (url: string) => ChatWebSocketLike
}

export interface UseChatWebSocketReturn {
  isConnected: Ref<boolean>
  isStreaming: Ref<boolean>
  currentSessionId: Ref<string | null>
  error: Ref<string | null>
  connect(agentId: string, userId: string, clientId: string): Promise<void>
  send(content: string): void
  disconnect(): void
  stop(): void
}

// ---------------------------------------------------------------------------
// Composable
// ---------------------------------------------------------------------------

export function useChatWebSocket(options: UseChatWebSocketOptions = {}): UseChatWebSocketReturn {
  const chatStore = useChatStore()

  // --- state ---
  let socket: ChatWebSocketLike | null = null
  let intentionalClose = false
  let pendingAgentId: string | null = null
  let pendingUserId: string | null = null
  let pendingClientId: string | null = null
  let reconnectAttempt = 0
  const MAX_RECONNECT_ATTEMPTS = 5
  const RECONNECT_BASE_DELAY_MS = 1000
  const HEARTBEAT_INTERVAL_MS = 15000
  let heartbeatTimer: ReturnType<typeof setInterval> | null = null
  let reconnectTimer: ReturnType<typeof setTimeout> | null = null

  const isConnected = ref(false)
  const isStreaming = ref(false)
  const currentSessionId = ref<string | null>(null)
  const error = ref<string | null>(null)

  // --- helpers ---

  function isOpen(target: ChatWebSocketLike | null): boolean {
    return target !== null && target.readyState === 1 // WebSocket.OPEN
  }

  function clearHeartbeat(): void {
    if (heartbeatTimer !== null) {
      clearInterval(heartbeatTimer)
      heartbeatTimer = null
    }
  }

  function clearReconnect(): void {
    if (reconnectTimer !== null) {
      clearTimeout(reconnectTimer)
      reconnectTimer = null
    }
  }

  function buildWsUrl(agentId: string): string {
    const base = options.baseUrl ?? (typeof window !== 'undefined' && window.location
      ? `${window.location.protocol === 'https:' ? 'wss:' : 'ws:'}//${window.location.host}`
      : 'ws://localhost:8000')
    const trimmed = base.replace(/\/$/, '')
    return `${trimmed}/ws/chat/${encodeURIComponent(agentId)}`
  }

  function defaultSocketFactory(baseUrl?: string): (url: string) => ChatWebSocketLike {
    // eslint-disable-next-line @typescript-eslint/no-unused-vars
    const _base = baseUrl
    return (url: string) => {
      if (typeof WebSocket === 'undefined') {
        throw new Error('chat ws: WebSocket is not available in this environment')
      }
      return new WebSocket(url) as unknown as ChatWebSocketLike
    }
  }

  const socketFactory = options.socketFactory ?? defaultSocketFactory(options.baseUrl)

  function resolveDefaultBase(url: string): string {
    return url
  }

  function startHeartbeat(): void {
    clearHeartbeat()
    heartbeatTimer = setInterval(() => {
      if (isOpen(socket)) {
        socket!.send(JSON.stringify({ type: 'ping' }))
      }
    }, HEARTBEAT_INTERVAL_MS)
  }

  function scheduleReconnect(): void {
    if (reconnectAttempt >= MAX_RECONNECT_ATTEMPTS) {
      error.value = `chat ws: max reconnect attempts (${MAX_RECONNECT_ATTEMPTS}) reached`
      return
    }
    reconnectAttempt += 1
    const delay = RECONNECT_BASE_DELAY_MS * Math.pow(2, reconnectAttempt - 1)
    reconnectTimer = setTimeout(() => {
      if (pendingAgentId && pendingUserId && pendingClientId) {
        void openSocket(pendingAgentId, pendingUserId, pendingClientId)
      }
    }, delay)
  }

  function openSocket(agentId: string, userId: string, clientId: string): Promise<void> {
    return new Promise<void>((resolve, reject) => {
      let settled = false
      const finishOk = () => {
        if (settled) return
        settled = true
        resolve()
      }
      const finishErr = (err: unknown) => {
        if (settled) return
        settled = true
        reject(err instanceof Error ? err : new Error(String(err)))
      }

      const url = buildWsUrl(agentId)
      let next: ChatWebSocketLike
      try {
        next = socketFactory(url)
      } catch (err) {
        finishErr(err)
        return
      }

      const previous = socket
      socket = next

      next.onopen = () => {
        reconnectAttempt = 0
        isConnected.value = true
        chatStore.connect(agentId, userId, clientId)
        startHeartbeat()
        finishOk()
      }

      next.onmessage = (event: MessageEvent) => {
        handleMessage(event)
      }

      next.onerror = (event: Event) => {
        error.value = `chat ws error: ${(event as ErrorEvent).message ?? 'unknown'}`
      }

      next.onclose = () => {
        clearHeartbeat()
        const wasConnected = isConnected.value
        isConnected.value = false
        chatStore.disconnect()
        if (!intentionalClose) {
          if (wasConnected) {
            scheduleReconnect()
          } else if (!settled) {
            // Connection never opened; treat as transient failure.
            finishErr(new Error('chat ws closed before open'))
          }
        } else {
          finishOk()
        }
        if (previous === next) {
          socket = null
        }
      }
    })
  }

  function handleMessage(event: MessageEvent): void {
    const raw = typeof event.data === 'string' ? event.data : ''
    if (!raw) return
    let payload: unknown
    try {
      payload = JSON.parse(raw)
    } catch {
      return
    }
    if (!payload || typeof payload !== 'object') return
    const frame = payload as Record<string, unknown>
    const type = frame.type
    if (type === 'chunk') {
      const content = typeof frame.content === 'string' ? frame.content : ''
      // Lazily allocate the assistant message on first chunk so empty turns
      // don't pollute the message list before streaming begins.
      if (!chatStore.streamingMessageId) {
        chatStore.beginAssistantTurn()
      }
      if (chatStore.streamingMessageId) {
        chatStore.appendChunk(chatStore.streamingMessageId, content)
      }
      isStreaming.value = true
      return
    }
    if (type === 'done') {
      isStreaming.value = false
      chatStore.endAssistantTurn()
      return
    }
    if (type === 'error') {
      const message = typeof frame.message === 'string' ? frame.message : 'chat ws error'
      error.value = message
      chatStore.setError(message)
      isStreaming.value = false
      chatStore.endAssistantTurn()
      return
    }
  }

  // --- public API ---

  async function connect(agentId: string, userId: string, clientId: string): Promise<void> {
    // If we're already connected to the same agent + client, treat as no-op.
    if (socket !== null && isConnected.value && chatStore.currentAgentId === agentId
        && chatStore.currentUserId === userId && chatStore.currentClientId === clientId) {
      return
    }
    if (socket !== null) {
      // Close any stale socket before opening a fresh one.
      intentionalClose = true
      try {
        socket.close()
      } catch {
        // Ignore: the onclose handler will clean up state.
      }
      socket = null
    }
    intentionalClose = false
    pendingAgentId = agentId
    pendingUserId = userId
    pendingClientId = clientId
    reconnectAttempt = 0
    clearReconnect()
    await openSocket(agentId, userId, clientId)
  }

  function send(content: string): void {
    if (!isOpen(socket)) {
      throw new Error('chat ws: cannot send while disconnected')
    }
    const userMessage = chatStore.send(content)
    const frame: ChatClientFrame = {
      user_id: chatStore.currentUserId ?? '',
      client_id: chatStore.currentClientId ?? '',
      content: userMessage.content,
    }
    socket!.send(JSON.stringify(frame))
  }

  function disconnect(): void {
    intentionalClose = true
    clearHeartbeat()
    clearReconnect()
    if (socket !== null) {
      try {
        socket.close()
      } catch {
        // ignore
      }
      socket = null
    }
    isConnected.value = false
    chatStore.disconnect()
  }

  function stop(): void {
    if (!isOpen(socket)) return
    socket!.send(JSON.stringify({ type: 'stop' }))
  }

  // --- lifecycle ---
  onBeforeUnmount(() => {
    disconnect()
  })

  return {
    isConnected,
    isStreaming,
    currentSessionId,
    error,
    connect,
    send,
    disconnect,
    stop,
  }
}
