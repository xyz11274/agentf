import { defineStore } from 'pinia'
import { ref } from 'vue'
import type { ChatMessage, LlmTraceSummary } from '@/types/agentflow'
import { getLlmTraces } from '@/api/llmTraceClient'

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
  const error = ref<string | null>(null)
  const streamingMessageId = ref<string | null>(null)

  /**
   * LLM execution traces keyed by assistant-message ID.
   * Populated by {@link fetchLlmTraces} after each turn completes.
   */
  const llmTraces = ref<Record<string, LlmTraceSummary[]>>({})
  const isFetchingTraces = ref(false)

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

  function send(content: string): ChatMessage {
    if (!isConnected.value) {
      throw new Error('chat: cannot send while disconnected')
    }
    const msg: ChatMessage = {
      id: nextId(),
      role: 'user',
      content,
      timestamp: Date.now(),
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

  function appendChunk(messageId: string, chunk: string): void {
    const target = messages.value.find((m) => m.id === messageId)
    if (!target) return
    target.content += chunk
  }

  function endAssistantTurn(): void {
    isStreaming.value = false
    streamingMessageId.value = null
  }

  /**
   * Fetch LLM traces for the most recent assistant turn.
   *
   * Should be called once after every ``done`` WebSocket frame.
   * The result is stored under the last assistant message's ID in
   * {@link llmTraces}, which drives the inline trace panel.
   */
  async function fetchLlmTraces(
    agentId: string,
    userId: string,
    clientId: string,
  ): Promise<void> {
    if (isFetchingTraces.value) return

    const lastAssistant = [...messages.value]
      .reverse()
      .find((m) => m.role === 'assistant')
    if (!lastAssistant) return

    isFetchingTraces.value = true
    try {
      const data = await getLlmTraces(agentId, userId, clientId)
      if (data.traces.length > 0) {
        llmTraces.value = {
          ...llmTraces.value,
          [lastAssistant.id]: data.traces,
        }
      }
    } catch {
      // Traces are best-effort — silence the error so it never
      // interrupts the chat experience.
    } finally {
      isFetchingTraces.value = false
    }
  }

  function stop(): void {
    if (!isStreaming.value) return
    isStreaming.value = false
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
    llmTraces.value = {}
  }

  function reset(): void {
    messages.value = []
    isConnected.value = false
    isStreaming.value = false
    currentAgentId.value = null
    currentClientId.value = null
    currentUserId.value = null
    error.value = null
    streamingMessageId.value = null
    llmTraces.value = {}
  }

  return {
    messages,
    isConnected,
    isStreaming,
    currentAgentId,
    currentClientId,
    currentUserId,
    error,
    streamingMessageId,
    llmTraces,
    isFetchingTraces,
    connect,
    disconnect,
    send,
    beginAssistantTurn,
    appendChunk,
    endAssistantTurn,
    fetchLlmTraces,
    stop,
    setError,
    clearMessages,
    reset,
  }
})
