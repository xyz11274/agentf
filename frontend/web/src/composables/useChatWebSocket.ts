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
import type { ChatDone, ContentPart } from '@/types/agentflow'

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
 * ``content`` is either a plain string or an array of multimodal content parts.
 */
export interface ChatClientFrame {
  user_id: string
  client_id: string
  content: string | ContentPart[]
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
      const isProgress = frame.progress === true
      const chunkType = typeof frame.chunk_type === 'string' ? frame.chunk_type : ''

      if (isProgress || chunkType === 'progress') {
        // Progress status message — display separately from model output.
        chatStore.setProgress(content)
      } else if (chunkType === 'llm_chunk') {
        // LLM streaming chunk — 节点推送的是原始 OpenAI SSE chunk JSON（见
        // streaming-response.md §3.4），前端解析 delta.content 取 token，
        // 按 (skill, round) 分组存储供 ChatView 独立卡片渲染。
        // 与 SSE 路径行为对齐（不变量 S-4：WS/SSE 同一 chunk 处理一致）。
        if (content) {
          let token = content
          try {
            const parsed = JSON.parse(content)
            const delta = parsed?.choices?.[0]?.delta
            if (delta?.content) {
              token = delta.content
            }
          } catch {
            // content 不是合法 JSON — 降级为原样追加（节点契约偏离时不丢数据）
          }
          // WS chunk 携带 skill/round 元数据时按分组存储；缺元数据时
          // 用空串占位但保留 token（addLlmChunk 严格拒空，需本地兜底）。
          const skill = typeof frame.progress_skill === 'string' ? frame.progress_skill : ''
          const round = typeof frame.progress_round === 'string' ? frame.progress_round : ''
          if (skill && round) {
            chatStore.addLlmChunk(skill, round, token)
          } else if (chatStore.streamingMessageId) {
            // 元数据缺失的兜底：追加到主 streaming bubble，避免数据丢失（I-1）。
            chatStore.appendChunk(chatStore.streamingMessageId, token)
          }
        }
      } else {
        // The assistant placeholder is created eagerly in send(), so
        // streamingMessageId is always set by the time chunks arrive.
        // (No lazy allocation here — single source of truth in chatStore.)
        if (chatStore.streamingMessageId) {
          const artifacts = Array.isArray(frame.artifacts)
            ? (frame.artifacts as Array<{type:string;url:string;name:string;mime_type:string}>)
            : undefined
          chatStore.appendChunk(chatStore.streamingMessageId, content, artifacts)
        }
        // Clear any previous progress message once real content arrives.
        chatStore.clearProgress()
      }
      // isStreaming local ref mirrors chatStore.isStreaming for the
      // composable's public API contract; the store is the source of truth.
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

  function send(text: string, images?: string[]): void {
    if (!isOpen(socket)) {
      throw new Error('chat ws: cannot send while disconnected')
    }
    // Build content parts: text + images as data URL parts
    const contentParts: ContentPart[] = []
    if (text.trim()) {
      contentParts.push({ type: 'text', text })
    }
    if (images && images.length > 0) {
      for (const url of images) {
        contentParts.push({ type: 'image_url', image_url: { url } })
      }
    }
    // If only text, keep plain string for backward compatibility
    const wireContent: string | ContentPart[] =
      contentParts.length === 1 && contentParts[0]!.type === 'text'
        ? (contentParts[0] as { type: 'text'; text: string }).text
        : contentParts

    chatStore.send(text, images)
    // Immediately create assistant placeholder so send button → stop button,
    // and typing animation shows, without waiting for first chunk.
    chatStore.beginAssistantTurn()
    const frame: ChatClientFrame = {
      user_id: chatStore.currentUserId ?? '',
      client_id: chatStore.currentClientId ?? '',
      content: wireContent,
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
    chatStore.stop()
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
