/**
 * Composable for SSE streaming or synchronous chat via `POST /api/chat/{agent_id}`.
 *
 * Replaces `useChatWebSocket` (Phase-1 Bridge session persistence adaptation).
 *
 * **流式模式** (`streamingEnabled=true`, 默认):
 *   URL: `POST /api/chat/{agent_id}?stream=true`
 *   通过 SSE `data:` 事件流式接收内容块 → `chatStore.appendChunk()`
 *
 * **同步模式** (`streamingEnabled=false`):
 *   URL: `POST /api/chat/{agent_id}`
 *   返回完整 JSON 响应 → 一次性设置 assistant 消息内容
 *
 * 可通过 `streamingEnabled` 运行时动态切换。
 */

import { onBeforeUnmount, ref } from 'vue'
import { useChatStore } from '@/stores/chatStore'
import { useSessionStore } from '@/stores/sessionStore'
import type { ContentPart } from '@/types/agentflow'

// ---------------------------------------------------------------------------
// Return type
// ---------------------------------------------------------------------------

export interface UseChatSseReturn {
  isConnected: ReturnType<typeof ref<boolean>>
  isStreaming: ReturnType<typeof ref<boolean>>
  currentSessionId: ReturnType<typeof ref<string | null>>
  error: ReturnType<typeof ref<string | null>>
  /** 是否启用流式输出（SSE），可运行时切换 */
  streamingEnabled: ReturnType<typeof ref<boolean>>
  connect: (agentId: string, userId: string, clientId: string) => Promise<void>
  send: (text: string, images?: string[]) => void
  disconnect: () => void
  stop: () => void
}

/**
 * Parse an SSE stream body from a `Response` body and yield parsed events.
 *
 * Yields `{ type: 'content' | 'done' | 'error', content?, message?, code? }`.
 */
async function* parseSseStream(
  reader: ReadableStreamDefaultReader<Uint8Array>,
  signal: AbortSignal,
): AsyncGenerator<{
  type: 'content' | 'done' | 'error'
  content?: string
  message?: string
  code?: number
  dialogue_id?: string
  artifacts?: Array<{type:string;url:string;name:string;mime_type:string}>
  progress?: boolean
  chunk_type?: string
  skill?: string
  round?: string
  status?: string
}> {
  const decoder = new TextDecoder()
  let buffer = ''

  while (!signal.aborted) {
    const { done, value } = await reader.read()
    if (done) break

    buffer += decoder.decode(value, { stream: true })
    const lines = buffer.split('\n')
    // Keep the last potentially incomplete line in the buffer.
    buffer = lines.pop() ?? ''

    for (const line of lines) {
      if (line.startsWith('data: ')) {
        const payload = line.slice(6).trim()
        if (payload === '[DONE]') {
          yield { type: 'done' }
        } else {
          try {
            const parsed = JSON.parse(payload)
            if (parsed.error) {
              yield {
                type: 'error',
                message: parsed.error.message ?? 'SSE error',
                code: parsed.error.code ?? -32000,
              }
            } else {
              const dialogueId: string | undefined = parsed._dialogue_id
              const isProgress: boolean = parsed._progress === true
              const chunkType: string | undefined = parsed._chunk_type
              const delta = parsed.choices?.[0]?.delta
              const msgContent = parsed.choices?.[0]?.message?.content
              const content = delta?.content || msgContent || ''
              // Structured artifact metadata from the bridge (_artifacts on final chunk)
              const artifacts: Array<{type:string;url:string;name:string;mime_type:string}> | undefined =
                Array.isArray(parsed._artifacts) ? parsed._artifacts as any : undefined
              if (content || dialogueId) {
                yield {
                  type: 'content', content,
                  dialogue_id: dialogueId, artifacts,
                  progress: isProgress, chunk_type: chunkType,
                  skill: parsed._skill, round: parsed._round, status: parsed._status,
                }
              }
            }
          } catch {
            // Malformed JSON — skip line.
          }
        }
      }
    }
  }
}

export function useChatSse(baseUrl?: string): UseChatSseReturn {
  const chatStore = useChatStore()

  // ── State ───────────────────────────────────────────────────────
  const isConnected = ref(false)
  const isStreaming = ref(false)
  const currentSessionId = ref<string | null>(null)
  const error = ref<string | null>(null)

  // ── 流式/同步模式开关 ──────────────────────────────────────────
  /** true=SSE 流式输出（默认），false=同步返回完整响应 */
  const streamingEnabled = ref(true)

  // ── Internal state ──────────────────────────────────────────────
  let currentAgentId: string | null = null
  let currentUserId: string | null = null
  let currentClientId: string | null = null
  let abortController: AbortController | null = null
  let inFlightSend = false
  let pendingDialogueId: string | null = null

  const apiBase = baseUrl ?? (typeof window !== 'undefined' && window.location
    ? `${window.location.protocol}//${window.location.host}`
    : 'http://localhost:8000')

  // ── Public API ──────────────────────────────────────────────────

  async function connect(
    agentId: string,
    userId: string,
    clientId: string,
  ): Promise<void> {
    // If already connected to the same agent + client, treat as no-op.
    if (isConnected.value && currentAgentId === agentId
        && currentUserId === userId && currentClientId === clientId) {
      return
    }

    // Clean up any previous connection.
    disconnect()

    currentAgentId = agentId
    currentUserId = userId
    currentClientId = clientId
    isConnected.value = true
    chatStore.connect(agentId, userId, clientId)
    error.value = null
  }

  function send(text: string, images?: string[]): void {
    if (!isConnected.value || !currentAgentId) {
      throw new Error('chat sse: cannot send while disconnected')
    }
    if (inFlightSend) {
      throw new Error('chat sse: another send is already in flight')
    }

    // Build content parts.
    const contentParts: ContentPart[] = []
    if (text.trim()) {
      contentParts.push({ type: 'text', text })
    }
    if (images && images.length > 0) {
      for (const url of images) {
        contentParts.push({ type: 'image_url', image_url: { url } })
      }
    }
    const wireContent = contentParts.length === 1 && contentParts[0]!.type === 'text'
      ? (contentParts[0] as { type: 'text'; text: string }).text
      : contentParts

    // Optimistic UI update.
    chatStore.send(text, images)
    chatStore.beginAssistantTurn()

    inFlightSend = true
    isStreaming.value = streamingEnabled.value
    abortController = new AbortController()

    const baseUrl = apiBase.replace(/\/$/, '')
    const streamParam = streamingEnabled.value ? '?stream=true' : ''
    const url = `${baseUrl}/api/chat/${encodeURIComponent(currentAgentId)}${streamParam}`
    const body = JSON.stringify({
      user_id: currentUserId ?? '',
      client_id: currentClientId ?? '',
      content: wireContent,
    })

    void executeSseRequest(url, body, abortController.signal)
  }

  async function executeSseRequest(
    url: string,
    body: string,
    signal: AbortSignal,
  ): Promise<void> {
    try {
      const response = await fetch(url, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body,
        signal,
      })

      if (!response.ok) {
        const errText = await response.text().catch(() => '')
        chatStore.setError(`请求失败 (${response.status}): ${errText || response.statusText}`)
        chatStore.endAssistantTurn()
        return
      }

      if (streamingEnabled.value) {
        // ── 流式模式：解析 SSE `data:` 事件 ──────────────────────
        const reader = response.body?.getReader()
        if (!reader) {
          chatStore.setError('SSE 响应体不可读')
          chatStore.endAssistantTurn()
          return
        }

        for await (const event of parseSseStream(reader, signal)) {
          if (event.type === 'content') {
            // 与 WebSocket 路径一致的分类处理：
            //   progress → chatStore.setProgress() 显示进度条
            //   content → appendChunk + clearProgress
            if (event.progress || event.chunk_type === 'progress') {
              // 技能执行进度消息 — 显示在独立进度指示器
              chatStore.setProgress(event.content || '')
              // 同时更新 skill 状态（供技能卡片 UI 使用）
              if (event.skill) {
                chatStore.updateSkillStatus(event.skill, event.status || 'running', event.round || '')
              }
            } else if (event.chunk_type === 'llm_chunk' && event.content) {
              // LLM 流式 chunk — 节点推送的是原始 OpenAI SSE chunk JSON（见
              // streaming-response.md §3.4），前端解析 delta.content 取 token，
              // 按 (skill, round) 分组存储供 ChatView 独立卡片渲染。
              // tool_calls 字段透传给前端 OpenAI parser 处理（本期 UI 暂不展示）。
              let token = event.content
              try {
                const parsed = JSON.parse(event.content)
                const delta = parsed?.choices?.[0]?.delta
                if (delta?.content) {
                  token = delta.content
                }
              } catch {
                // content 不是合法 JSON — 降级为原样追加（节点契约偏离时不丢数据）
              }
              chatStore.addLlmChunk(event.skill || '', event.round || '', token)
            } else if (event.content && chatStore.streamingMessageId) {
              // 普通文本 chunk（最终回答到达）
              chatStore.appendChunk(chatStore.streamingMessageId, event.content, event.artifacts)
              // 清除中间 LLM 流式分组和进度消息
              chatStore.clearLlmChunks()
              // 有实际内容到达时清除进度消息
              chatStore.clearProgress()
              chatStore.clearAllSkillStatus()
            }
          } else if (event.type === 'done') {
            chatStore.clearProgress()
            chatStore.clearLlmChunks()
            chatStore.clearAllSkillStatus()
            break
          } else if (event.type === 'error') {
            chatStore.setError(event.message ?? 'SSE error')
          }
          // 捕获首个 chunk 携带的 dialogue_id
          if (event.dialogue_id) {
            pendingDialogueId = event.dialogue_id
          }
        }
      } else {
        // ── 同步模式：直接解析 JSON 完整响应 ─────────────────────
        const json = await response.json() as Record<string, unknown>
        if (json.error) {
          chatStore.setError(typeof json.error === 'string' ? json.error : '请求失败')
        } else {
          const data = json.data as Record<string, unknown> | undefined
          if (data?.content && chatStore.streamingMessageId) {
            const artifacts = Array.isArray(data.artifacts)
              ? (data.artifacts as Array<{type:string;url:string;name:string;mime_type:string}>)
              : undefined
            chatStore.appendChunk(chatStore.streamingMessageId, String(data.content), artifacts)
          }
        }
      }

      // Turn complete.
      chatStore.endAssistantTurn()
      const dlgId = pendingDialogueId
      pendingDialogueId = null

      // If no session ID is set yet (new session), resolve it from the
      // session store after the Bridge session is created by the chat.
      if (!chatStore.currentSessionId && currentClientId) {
        try {
          const sessionStore = useSessionStore()
          await sessionStore.loadSessions()
          const match = sessionStore.sessions.find(
            (s) => s.client_id === currentClientId && s.user_id === (currentUserId ?? ''),
          )
          if (match) {
            chatStore.currentSessionId = match.id
          }
        } catch {
          // Best-effort — trace fetch will be skipped but chat is unaffected.
        }
      }

      chatStore.fetchLlmTraces(dlgId ?? undefined).catch(() => { /* best-effort */ })
    } catch (err: unknown) {
      if (signal.aborted) {
        // Intentional abort (disconnect/stop) — not an error.
        return
      }
      const msg = err instanceof Error ? err.message : String(err)
      chatStore.setError(`SSE error: ${msg}`)
      chatStore.endAssistantTurn()
    } finally {
      isStreaming.value = false
      inFlightSend = false
      abortController = null
    }
  }

  function stop(): void {
    if (abortController) {
      abortController.abort()
      abortController = null
    }
    isStreaming.value = false
    inFlightSend = false
    chatStore.stop()
    // 同步模式下 streming 始终为 false，但 send() 可能正在执行；
    // abort AbortController 已保证 fetch 立即中断。
  }

  function disconnect(): void {
    stop()
    isConnected.value = false
    currentAgentId = null
    currentUserId = null
    currentClientId = null
    chatStore.disconnect()
  }

  onBeforeUnmount(() => {
    disconnect()
  })

  return {
    isConnected,
    isStreaming,
    currentSessionId,
    error,
    streamingEnabled,
    connect,
    send,
    disconnect,
    stop,
  }
}
