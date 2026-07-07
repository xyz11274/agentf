import { defineStore } from 'pinia'
import { ref } from 'vue'
import type { ArtifactInfo, ChatMessage, ContentPart, LlmTraceSummary, PersistentMessage } from '@/types/agentflow'
import { getApiClient } from '@/api/context'

let messageSeq = 0
function nextId(): string {
  messageSeq += 1
  return `m-${Date.now()}-${messageSeq}`
}

export const useChatStore = defineStore('chat', () => {
  const messages = ref<ChatMessage[]>([])
  const isConnected = ref(false)
  const isStreaming = ref(false)
  const currentAgentId = ref<string | null>(null)
  const currentClientId = ref<string | null>(null)
  const currentUserId = ref<string | null>(null)
  /** Current Bridge persistent session ID (set by ChatPage when selecting a session). */
  const currentSessionId = ref<string | null>(null)
  const error = ref<string | null>(null)
  const streamingMessageId = ref<string | null>(null)
  /** Current progress/status message displayed while skills are executing. */
  const progressMessage = ref<string | null>(null)

  /**
   * Skill execution status map — keyed by skill name.
   * Each entry: { status: "running"|"completed"|"error", round: string, message: string }.
   * Populated by progress chunks during skill execution, cleared when final response arrives.
   */
  const skillStatus = ref<Record<string, { status: string; round: string; message: string }>>({})

  /**
   * LLM execution traces keyed by assistant-message ID.
   * Populated by {@link fetchLlmTraces} after each turn completes.
   */
  const llmTraces = ref<Record<string, LlmTraceSummary[]>>({})
  /**
   * Full trace detail data (request, response, meta) keyed by call_id.
   * Populated alongside {@link llmTraces} when traces are fetched from
   * the Bridge persistent API.  Used by LlmTracePanel.showDetail to
   * avoid an extra daemon DataBus round-trip.
   */
  const llmTraceDetailCache = ref<Record<string, Record<string, unknown>>>({})
  const isFetchingTraces = ref(false)

  /**
   * LLM streaming chunks grouped by source.
   * Key format: "{skill}:{round}" (e.g. "skill_executor:a1b2c3d4e5f6").
   * Value is the accumulated token content for that LLM call.
   * Cleared when the final text response arrives.
   */
  const llmChunks = ref<Record<string, string>>({})

  /** Detail mode: 'coordinator' = hide skill execution details, 'all' = show everything */
  const detailMode = ref<'coordinator' | 'all'>('coordinator')

  function toggleDetailMode(): void {
    detailMode.value = detailMode.value === 'coordinator' ? 'all' : 'coordinator'
  }

  function connect(agentId: string, userId: string, clientId: string): void {
    if (currentAgentId.value !== agentId) {
      messages.value = []
    }
    currentAgentId.value = agentId
    currentUserId.value = userId
    currentClientId.value = clientId
    isConnected.value = true
    error.value = null
    isStreaming.value = false
    streamingMessageId.value = null
  }

  function disconnect(): void {
    isConnected.value = false
    isStreaming.value = false
    streamingMessageId.value = null
    currentClientId.value = null
  }

  function send(content: string, images?: string[]): ChatMessage {
    if (!isConnected.value) {
      throw new Error('chat: cannot send while disconnected')
    }
    const msg: ChatMessage = {
      id: nextId(),
      role: 'user',
      content,
      timestamp: Date.now(),
      images,
    }
    messages.value.push(msg)
    return msg
  }

  function beginAssistantTurn(): ChatMessage {
    const msg: ChatMessage = {
      id: nextId(),
      role: 'assistant',
      content: '',
      timestamp: Date.now(),
    }
    messages.value.push(msg)
    streamingMessageId.value = msg.id
    isStreaming.value = true
    return msg
  }

  function appendChunk(messageId: string, chunk: string, artifacts?: ArtifactInfo[]): void {
    const target = messages.value.find((m) => m.id === messageId)
    if (!target) return
    target.content += chunk
    // artifacts come only with the final chunk; collect (not replace)
    if (artifacts && artifacts.length > 0) {
      if (!target.artifacts) target.artifacts = []
      for (const a of artifacts) {
        if (!target.artifacts.some((existing) => existing.url === a.url)) {
          target.artifacts.push(a)
        }
      }
    }
  }

  /**
   * Accumulate a streaming LLM token grouped by source.
   * Key = "{skill}:{round}" so chunks from different nodes/calls are kept separate.
   */
  function addLlmChunk(skill: string, round: string, token: string): void {
    const key = `${skill || 'coordinator'}:${round || '0'}`
    llmChunks.value[key] = (llmChunks.value[key] || '') + token
  }

  /** Clear all grouped LLM chunks (called when final text arrives). */
  function clearLlmChunks(): void {
    llmChunks.value = {}
  }

  function endAssistantTurn(): void {
    isStreaming.value = false
    progressMessage.value = null
    llmChunks.value = {}
    skillStatus.value = {}
    // Symmetric with stop(): drop an empty assistant placeholder so the
    // timeline doesn't show a stray empty bubble when the daemon returned
    // no content. Non-empty turns are preserved.
    if (streamingMessageId.value !== null) {
      const target = messages.value.find((m) => m.id === streamingMessageId.value)
      if (target && target.content.length === 0) {
        messages.value = messages.value.filter((m) => m.id !== target.id)
      }
    }
    streamingMessageId.value = null
  }

  /**
   * Fetch LLM traces from the Bridge persistent API after a turn completes.
   *
   * Uses the store's ``currentSessionId`` (set by ChatPage when the user
   * selects a persistent session) to query ``/api/sessions/persistent/{id}/traces``.
   * Falls back silently when no session is active (real-time chat without
   * a persistent session).
   *
   * The result is stored under the last assistant message's ID in
   * {@link llmTraces}, which drives the inline trace panel.
   */
  async function fetchLlmTraces(dialogueId?: string): Promise<void> {
    if (isFetchingTraces.value) return
    if (!currentSessionId.value) return
    if (currentSessionId.value.startsWith('pending_')) return  // skip optimistic pending entries

    const lastAssistant = [...messages.value]
      .reverse()
      .find((m) => m.role === 'assistant')
    if (!lastAssistant) return

    isFetchingTraces.value = true
    try {
      const dlgId = dialogueId || lastAssistant.dialogueId
      if (!dlgId) return
      const res = await getApiClient().listPersistentTraces(
        currentSessionId.value, dlgId,
      )
      const summaries = _transformBridgeTraces(res)
      if (summaries.length > 0) {
        llmTraces.value = {
          ...llmTraces.value,
          [lastAssistant.id]: summaries,
        }
      }
    } catch {
      // Traces are best-effort — silence the error so it never
      // interrupts the chat experience.
    } finally {
      isFetchingTraces.value = false
    }
  }

  function setProgress(msg: string): void {
    progressMessage.value = msg
  }

  function clearProgress(): void {
    progressMessage.value = null
  }

  /** Update the execution status of a named skill (from progress chunks). */
  function updateSkillStatus(skill: string, status: string, round: string, message?: string): void {
    skillStatus.value = {
      ...skillStatus.value,
      [skill]: { status, round, message: message ?? skillStatus.value[skill]?.message ?? '' },
    }
  }

  /** Clear all skill status (when final response arrives). */
  function clearAllSkillStatus(): void {
    skillStatus.value = {}
  }

  function stop(): void {
    if (!isStreaming.value) return
    isStreaming.value = false
    progressMessage.value = null
    if (streamingMessageId.value !== null) {
      const target = messages.value.find((m) => m.id === streamingMessageId.value)
      if (target && target.content.length === 0) {
        messages.value = messages.value.filter((m) => m.id !== target.id)
      }
    }
    streamingMessageId.value = null
  }

  function setError(message: string | null): void {
    error.value = message
  }

  function clearMessages(): void {
    messages.value = []
    isStreaming.value = false
    streamingMessageId.value = null
    progressMessage.value = null
    llmTraces.value = {}
    llmTraceDetailCache.value = {}
    historyLoaded.value = false
  }

  function reset(): void {
    messages.value = []
    isConnected.value = false
    isStreaming.value = false
    currentAgentId.value = null
    currentClientId.value = null
    currentUserId.value = null
    currentSessionId.value = null
    error.value = null
    streamingMessageId.value = null
    progressMessage.value = null
    llmTraces.value = {}
    llmTraceDetailCache.value = {}
    historyLoaded.value = false
  }

  // ── Bridge persistent session history ─────────────────────

  /**
   * Whether historical messages have been loaded for the current session.
   */
  const historyLoaded = ref(false)

  /**
   * Load historical messages from the Bridge persistent session API
   * and inject them into the local messages array.
   *
   * Should be called once when the user selects a persistent session,
   * BEFORE WebSocket connect. The messages are injected as local entries
   * and will be displayed in the ChatView timeline.
   */
  async function loadHistoryMessages(sessionId: string): Promise<void> {
    if (historyLoaded.value) return
    if (sessionId.startsWith('pending_')) return  // skip optimistic pending entries
    try {
      const res = await getApiClient().listPersistentMessages(sessionId, 50)
      const msgs = res.data?.messages ?? []
      if (msgs.length === 0) {
        historyLoaded.value = true
        return
      }
      // Convert PersistentMessage → ChatMessage and inject.
      const injected: ChatMessage[] = []
      for (const pm of msgs) {
        injected.push({
          id: `hist-${pm.dialogue_id}-${pm.role}-${injected.length}`,
          role: pm.role,
          content: pm.content,
          timestamp: pm.created_at ? new Date(pm.created_at).getTime() : Date.now(),
          dialogueId: pm.dialogue_id,
          artifacts: pm.artifacts as ArtifactInfo[] | undefined,
        })
      }
      messages.value = injected
      historyLoaded.value = true
    } catch {
      // History loading is best-effort; silence errors so it never
      // blocks the user from starting a new chat.
      // 不设置 historyLoaded = true，下次重试可再次加载
    }
  }

  /**
   * Load LLM traces for historical messages from the Bridge persistent trace API.
   *
   * Must be called AFTER ``loadHistoryMessages()``. For each unique dialogue_id
   * among the loaded messages, fetches traces and stores them under the
   * corresponding assistant message's ID in ``llmTraces``.
   *
   * Bridges the gap between page refreshes: daemon's DataBus may have cleaned
   * up old dialogue data, but the Bridge provider retains traces permanently.
   */
  async function loadHistoryTraces(sessionId: string): Promise<void> {
    if (messages.value.length === 0) return
    if (sessionId.startsWith('pending_')) return  // skip optimistic pending entries
    // Group messages by dialogue_id to find all dialogues.
    const dialogues = new Set<string>()
    const assistantByDialogue = new Map<string, string>()
    for (const msg of messages.value) {
      // Use the dialogueId field set by loadHistoryMessages rather than
      // parsing the message ID — dialogue_id format contains '-' (e.g.
      // "0625152345-abc123def456") so split('-') would truncate it.
      const dlgId = msg.dialogueId
      if (!dlgId) continue
      dialogues.add(dlgId)
      if (msg.role === 'assistant') {
        assistantByDialogue.set(dlgId, msg.id)
      }
    }
    if (dialogues.size === 0) return

    for (const dlgId of dialogues) {
      try {
        const res = await getApiClient().listPersistentTraces(sessionId, dlgId)
        const summaries = _transformBridgeTraces(res)
        const assistantId = assistantByDialogue.get(dlgId)
        if (assistantId && summaries.length > 0) {
          llmTraces.value = {
            ...llmTraces.value,
            [assistantId]: summaries,
          }
        }
      } catch {
        // Traces are best-effort — silence errors.
      }
    }
  }

  // ── Trace data transformation ────────────────────────────

  /**
   * Transform a Bridge persistent trace API response (listPersistentTraces)
   * into the LlmTraceSummary[] format expected by LlmTracePanel.
   *
   * The Bridge stores trace data nested under ``trace_data`` (with ``meta``,
   * ``request``, ``response`` sub-keys).  This helper flattens the ``meta``
   * fields to the top-level LlmTraceSummary shape.
   */
  function _transformBridgeTraces(
    res: Awaited<ReturnType<ReturnType<typeof getApiClient>['listPersistentTraces']>>,
  ): LlmTraceSummary[] {
    const rawTraces = (res.data as Record<string, unknown>)?.traces as Record<string, unknown>[] | undefined
    if (!rawTraces || rawTraces.length === 0) return []
    const summaries: LlmTraceSummary[] = []
    for (const t of rawTraces) {
      const td = (t as Record<string, unknown>).trace_data as Record<string, unknown> | undefined
      const meta = td?.meta as Record<string, unknown> | undefined
      const callId = (td?.call_id as string) ?? (t.call_id as string) ?? ''
      summaries.push({
        call_id: callId,
        model: (meta?.model as string) ?? '',
        latency_ms: (meta?.latency_ms as number) ?? 0,
        request_summary: (meta?.request_summary as string) ?? '',
        response_summary: (meta?.response_summary as string) ?? '',
        timestamp: (meta?.timestamp as string) ?? '',
        token_usage: {
          prompt_tokens: (meta?.prompt_tokens as number) ?? 0,
          completion_tokens: (meta?.completion_tokens as number) ?? 0,
          total_tokens: (meta?.total_tokens as number) ?? 0,
        },
        dialogue_id: (meta?.dialogue_id as string) ?? '',
        session_id: (meta?.session_id as string) ?? '',
        node_id: (meta?.node_id as string) ?? '',
        task_id: (meta?.task_id as string) ?? '',
        skill_name: (meta?.skill_name as string) ?? '',
        completed_skill: (meta?.completed_skill as string) ?? '',
        task_type: (meta?.task_type as string) ?? '',
      })
      // Also cache full detail (request, response, meta) for
      // LlmTracePanel.showDetail / ChatView export.
      if (callId && td) {
        llmTraceDetailCache.value = {
          ...llmTraceDetailCache.value,
          [callId]: {
            request: td.request ?? null,
            response: td.response ?? null,
            meta: td.meta ?? null,
          },
        }
      }
    }
    return summaries
  }

  return {
    messages,
    isConnected,
    isStreaming,
    currentAgentId,
    currentClientId,
    currentUserId,
    currentSessionId,
    error,
    streamingMessageId,
    progressMessage,
    skillStatus,
    llmTraces,
    llmTraceDetailCache,
    isFetchingTraces,
    llmChunks,
    detailMode,
    toggleDetailMode,
    historyLoaded,
    connect,
    disconnect,
    send,
    beginAssistantTurn,
    appendChunk,
    addLlmChunk,
    clearLlmChunks,
    endAssistantTurn,
    fetchLlmTraces,
    loadHistoryMessages,
    loadHistoryTraces,
    setProgress,
    clearProgress,
    updateSkillStatus,
    clearAllSkillStatus,
    stop,
    setError,
    clearMessages,
    reset,
  }
})
