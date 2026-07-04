import { beforeEach, describe, expect, it } from 'vitest'
import { createPinia, setActivePinia } from 'pinia'
import { useChatStore } from '@/stores/chatStore'

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
})
