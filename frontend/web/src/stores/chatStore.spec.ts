import { beforeEach, describe, expect, it, vi } from 'vitest'
import { createPinia, setActivePinia } from 'pinia'
import { useChatStore } from '@/stores/chatStore'
import { setApiClient, resetApiClient } from '@/api/context'
import { ApiClient } from '@/api/client'
import type { LlmTraceSummary } from '@/types/agentflow'

describe('useChatStore', () => {
  beforeEach(() => {
    setActivePinia(createPinia())
  })

  it('starts disconnected with no messages', () => {
    const store = useChatStore()
    expect(store.messages).toEqual([])
    expect(store.isConnected).toBe(false)
    expect(store.isStreaming).toBe(false)
    expect(store.currentAgentId).toBeNull()
    expect(store.streamingMessageId).toBeNull()
  })

  it('connect sets connection state and remembers agent', () => {
    const store = useChatStore()
    store.connect('a1')
    expect(store.isConnected).toBe(true)
    expect(store.currentAgentId).toBe('a1')
    expect(store.error).toBeNull()
  })

  it('connect resets messages when the agent changes', () => {
    const store = useChatStore()
    store.connect('a1')
    store.send('hello')
    expect(store.messages).toHaveLength(1)
    store.connect('a2')
    expect(store.messages).toEqual([])
    expect(store.currentAgentId).toBe('a2')
  })

  it('send appends a user message and returns it', () => {
    const store = useChatStore()
    store.connect('a1')
    const msg = store.send('hi there')
    expect(msg.role).toBe('user')
    expect(msg.content).toBe('hi there')
    expect(store.messages).toHaveLength(1)
    expect(store.messages[0]).toEqual(msg)
  })

  it('send throws when disconnected', () => {
    const store = useChatStore()
    expect(() => store.send('nope')).toThrow(/disconnected/)
  })

  it('beginAssistantTurn creates an empty assistant message and starts streaming', () => {
    const store = useChatStore()
    store.connect('a1')
    const msg = store.beginAssistantTurn()
    expect(msg.role).toBe('assistant')
    expect(msg.content).toBe('')
    expect(store.isStreaming).toBe(true)
    expect(store.streamingMessageId).toBe(msg.id)
  })

  it('appendChunk concatenates text to the matching message', () => {
    const store = useChatStore()
    store.connect('a1')
    const msg = store.beginAssistantTurn()
    store.appendChunk(msg.id, 'Hello')
    store.appendChunk(msg.id, ', world')
    expect(store.messages.find((m) => m.id === msg.id)?.content).toBe('Hello, world')
  })

  it('appendChunk is a no-op when the message id is unknown', () => {
    const store = useChatStore()
    store.connect('a1')
    const msg = store.beginAssistantTurn()
    store.appendChunk('does-not-exist', 'ignored')
    expect(store.messages.find((m) => m.id === msg.id)?.content).toBe('')
  })

  it('endAssistantTurn clears streaming flags but keeps the message', () => {
    const store = useChatStore()
    store.connect('a1')
    const msg = store.beginAssistantTurn()
    store.appendChunk(msg.id, 'hi')
    store.endAssistantTurn()
    expect(store.isStreaming).toBe(false)
    expect(store.streamingMessageId).toBeNull()
    expect(store.messages).toHaveLength(1)
  })

  it('stop clears streaming and removes the empty assistant placeholder', () => {
    const store = useChatStore()
    store.connect('a1')
    store.beginAssistantTurn()
    store.stop()
    expect(store.isStreaming).toBe(false)
    expect(store.streamingMessageId).toBeNull()
    expect(store.messages).toEqual([])
  })

  it('stop keeps an assistant message that already has content', () => {
    const store = useChatStore()
    store.connect('a1')
    const msg = store.beginAssistantTurn()
    store.appendChunk(msg.id, 'partial')
    store.stop()
    expect(store.isStreaming).toBe(false)
    expect(store.messages).toHaveLength(1)
    expect(store.messages[0]?.content).toBe('partial')
  })

  it('stop is a no-op when not streaming', () => {
    const store = useChatStore()
    store.connect('a1')
    const sent = store.send('hi')
    expect(store.messages).toHaveLength(1)
    expect(sent.id).toBeTruthy()
    store.stop()
    expect(store.messages).toHaveLength(1)
    expect(store.isStreaming).toBe(false)
  })

  it('endAssistantTurn drops an empty assistant placeholder (symmetric with stop)', () => {
    // P0 #4 invariant: both terminal paths must treat empty turns identically.
    const store = useChatStore()
    store.connect('a1')
    store.send('hi')
    store.beginAssistantTurn() // empty assistant placeholder
    expect(store.messages).toHaveLength(2)
    store.endAssistantTurn()
    expect(store.isStreaming).toBe(false)
    expect(store.streamingMessageId).toBeNull()
    expect(store.messages).toHaveLength(1)
    expect(store.messages[0]?.role).toBe('user')
  })

  it('endAssistantTurn preserves a non-empty assistant turn', () => {
    const store = useChatStore()
    store.connect('a1')
    store.send('hi')
    const a = store.beginAssistantTurn()
    store.appendChunk(a.id, 'reply')
    store.endAssistantTurn()
    expect(store.messages).toHaveLength(2)
    expect(store.messages[1]?.content).toBe('reply')
  })

  it('disconnect clears connection state without dropping messages', () => {
    const store = useChatStore()
    store.connect('a1')
    store.send('hi')
    store.disconnect()
    expect(store.isConnected).toBe(false)
    expect(store.messages).toHaveLength(1)
  })

  it('clearMessages wipes messages and streaming state', () => {
    const store = useChatStore()
    store.connect('a1')
    store.beginAssistantTurn()
    store.clearMessages()
    expect(store.messages).toEqual([])
    expect(store.isStreaming).toBe(false)
    expect(store.streamingMessageId).toBeNull()
  })

  it('reset clears every piece of state', () => {
    const store = useChatStore()
    store.connect('a1')
    store.send('hi')
    store.beginAssistantTurn()
    store.setError('x')
    store.reset()
    expect(store.messages).toEqual([])
    expect(store.isConnected).toBe(false)
    expect(store.currentAgentId).toBeNull()
    expect(store.error).toBeNull()
  })

  // ── loadHistoryTraces ──────────────────────────────────────────

  describe('loadHistoryTraces', () => {
    /** Build a fake Bridge persistent-trace response entry matching
     *  LocalSessionProvider.get_traces() schema. */
    function makePersistentTrace(
      callId: string,
      model = 'gpt-4o',
      latencyMs = 1234,
    ): Record<string, unknown> {
      return {
        id: 1,
        session_id: 'sess-001',
        dialogue_id: '0625152345-abc123def456',
        trace_type: 'llm_call',
        trace_data: {
          call_id: callId,
          request: { model, messages: [{ role: 'user', content: 'Hello' }] },
          response: { choices: [{ message: { content: 'Hi!' } }] },
          meta: {
            model,
            latency_ms: latencyMs,
            request_summary: 'Hello',
            response_summary: 'Hi!',
            timestamp: '2026-07-05T12:00:00Z',
            prompt_tokens: 10,
            completion_tokens: 5,
            total_tokens: 15,
            dialogue_id: 'd-1',
            session_id: 'mock-ses-001',
            node_id: 'coordinator_decide',
            task_id: 'task-001',
            skill_name: '',
            completed_skill: '',
            task_type: 'coordinator',
          },
        },
        created_at: '2026-07-05T12:00:00Z',
      }
    }

    function makeSummaryExpectation(
      callId: string,
      model = 'gpt-4o',
      latencyMs = 1234,
    ): LlmTraceSummary {
      return {
        call_id: callId,
        model,
        latency_ms: latencyMs,
        request_summary: 'Hello',
        response_summary: 'Hi!',
        timestamp: '2026-07-05T12:00:00Z',
        token_usage: { prompt_tokens: 10, completion_tokens: 5, total_tokens: 15 },
        dialogue_id: 'd-1',
        session_id: 'mock-ses-001',
        node_id: 'coordinator_decide',
        task_id: 'task-001',
        skill_name: '',
        completed_skill: '',
        task_type: 'coordinator',
      }
    }

    beforeEach(() => {
      setActivePinia(createPinia())
    })

    afterEach(() => {
      resetApiClient()
    })

    it('transforms bridge persistent trace entries to LlmTraceSummary', async () => {
      const store = useChatStore()

      // Arrange: seed messages (simulating loadHistoryMessages output).
      store.messages.push({
        id: 'hist-0625152345-abc123def456-user-0',
        role: 'user' as const,
        content: 'Hello',
        timestamp: 1000,
        dialogueId: '0625152345-abc123def456',
      })
      store.messages.push({
        id: 'hist-0625152345-abc123def456-assistant-1',
        role: 'assistant' as const,
        content: 'Hi!',
        timestamp: 2000,
        dialogueId: '0625152345-abc123def456',
      })

      // Mock the API to return persistent-trace format.
      const mockClient = new ApiClient()
      vi.spyOn(mockClient, 'listPersistentTraces').mockResolvedValue({
        success: true,
        data: {
          traces: [makePersistentTrace('call-001'), makePersistentTrace('call-002', 'gpt-4o-mini', 567)],
          total: 2,
        },
        error: null,
      })
      setApiClient(mockClient)

      await store.loadHistoryTraces('sess-001')

      // Assert: llmTraces keyed by assistant message id with transformed data.
      const key = 'hist-0625152345-abc123def456-assistant-1'
      expect(store.llmTraces[key]).toHaveLength(2)

      expect(store.llmTraces[key][0]).toEqual(makeSummaryExpectation('call-001'))
      expect(store.llmTraces[key][1]).toEqual(makeSummaryExpectation('call-002', 'gpt-4o-mini', 567))
    })

    it('handles empty trace response gracefully', async () => {
      const store = useChatStore()
      store.messages.push({
        id: 'hist-d1-user-0',
        role: 'user' as const,
        content: 'Q',
        timestamp: 1000,
        dialogueId: 'd1',
      })
      store.messages.push({
        id: 'hist-d1-assistant-1',
        role: 'assistant' as const,
        content: 'A',
        timestamp: 2000,
        dialogueId: 'd1',
      })

      const mockClient = new ApiClient()
      vi.spyOn(mockClient, 'listPersistentTraces').mockResolvedValue({
        success: true,
        data: { traces: [], total: 0 },
        error: null,
      })
      setApiClient(mockClient)

      await store.loadHistoryTraces('sess-001')

      // No traces → llmTraces should remain empty.
      expect(Object.keys(store.llmTraces)).toHaveLength(0)
    })

    it('skips messages without dialogueId', async () => {
      const store = useChatStore()
      // Inject a message without dialogueId (e.g. from real-time chat).
      store.messages.push({
        id: 'm-123',
        role: 'assistant' as const,
        content: 'Real-time reply',
        timestamp: 999,
      })

      const mockClient = new ApiClient()
      const spy = vi.spyOn(mockClient, 'listPersistentTraces')
      setApiClient(mockClient)

      await store.loadHistoryTraces('sess-001')

      // No call to listPersistentTraces since no dialogueId was present.
      expect(spy).not.toHaveBeenCalled()
      expect(Object.keys(store.llmTraces)).toHaveLength(0)
    })

    it('handles API error gracefully (swallows silently)', async () => {
      const store = useChatStore()
      store.messages.push({
        id: 'hist-d1-assistant-1',
        role: 'assistant' as const,
        content: 'A',
        timestamp: 2000,
        dialogueId: 'd1',
      })

      const mockClient = new ApiClient()
      vi.spyOn(mockClient, 'listPersistentTraces').mockRejectedValue(
        new Error('network error'),
      )
      setApiClient(mockClient)

      // Should not throw.
      await expect(store.loadHistoryTraces('sess-001')).resolves.toBeUndefined()
      expect(Object.keys(store.llmTraces)).toHaveLength(0)
    })

    it('reads dialogueId field (not parsed from msg.id)', async () => {
      const store = useChatStore()
      // dialogueId contains '-' (e.g. "0625152345-abc123def456") — this
      // was the root cause of the "refresh doesn't load traces" bug.
      store.messages.push({
        id: 'hist-0625152345-abc123def456-user-0',
        role: 'user' as const,
        content: 'Q',
        timestamp: 1000,
        dialogueId: '0625152345-abc123def456',
      })
      store.messages.push({
        id: 'hist-0625152345-abc123def456-assistant-1',
        role: 'assistant' as const,
        content: 'A',
        timestamp: 2000,
        dialogueId: '0625152345-abc123def456',
      })

      const mockClient = new ApiClient()
      vi.spyOn(mockClient, 'listPersistentTraces').mockImplementation(
        async (_sessionId: string, dialogueId?: string) => {
          // Verify the FULL dialogue_id is passed to the API (not truncated).
          expect(dialogueId).toBe('0625152345-abc123def456')
          return {
            success: true,
            data: { traces: [makePersistentTrace('c1')], total: 1 },
            error: null,
          }
        },
      )
      setApiClient(mockClient)

      await store.loadHistoryTraces('sess-001')

      const key = 'hist-0625152345-abc123def456-assistant-1'
      expect(store.llmTraces[key]).toHaveLength(1)
    })
  })
})
