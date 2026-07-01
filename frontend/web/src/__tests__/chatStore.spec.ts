import { describe, it, expect, beforeEach } from 'vitest'
import { useChatStore } from '@/stores/chatStore'
import { setActivePinia, createPinia } from 'pinia'

describe('chatStore', () => {
  beforeEach(() => {
    setActivePinia(createPinia())
  })

  it('initial state is disconnected with empty messages', () => {
    const store = useChatStore()
    expect(store.isConnected).toBe(false)
    expect(store.isStreaming).toBe(false)
    expect(store.messages).toHaveLength(0)
  })

  it('send() appends a user message when connected', () => {
    const store = useChatStore()
    store.isConnected = true  // simulate connected state
    const msg = store.send('hello')
    expect(msg.role).toBe('user')
    expect(msg.content).toBe('hello')
    expect(store.messages).toHaveLength(1)
  })

  it('send() assigns an id string', () => {
    const store = useChatStore()
    store.isConnected = true
    const msg = store.send('a')
    expect(msg.id).toBeDefined()
    expect(typeof msg.id).toBe('string')
  })

  it('send() throws when disconnected', () => {
    const store = useChatStore()
    expect(() => store.send('x')).toThrow('cannot send while disconnected')
  })

  it('clearMessages empties the messages array', () => {
    const store = useChatStore()
    store.isConnected = true
    store.send('x')
    store.clearMessages()
    expect(store.messages).toHaveLength(0)
  })

  it('beginAssistantTurn adds an assistant message and sets streaming', () => {
    const store = useChatStore()
    store.isConnected = true
    store.send('hi')
    const assistant = store.beginAssistantTurn()
    expect(assistant.role).toBe('assistant')
    expect(store.isStreaming).toBe(true)
    expect(store.messages).toHaveLength(2)
  })

  it('endAssistantTurn stops streaming', () => {
    const store = useChatStore()
    store.beginAssistantTurn()
    expect(store.isStreaming).toBe(true)
    store.endAssistantTurn()
    expect(store.isStreaming).toBe(false)
  })

  it('appendChunk appends text to the latest assistant message', () => {
    const store = useChatStore()
    store.isConnected = true
    store.send('q')
    const assistant = store.beginAssistantTurn()
    store.appendChunk(assistant.id, 'Hel')
    store.appendChunk(assistant.id, 'lo')
    expect(store.messages[1].content).toBe('Hello')
  })

  it('setError updates the error ref', () => {
    const store = useChatStore()
    store.setError('Something went wrong')
    expect(store.error).toBe('Something went wrong')
    store.setError(null)
    expect(store.error).toBeNull()
  })

  it('reset clears all state', () => {
    const store = useChatStore()
    store.isConnected = true
    store.send('test')
    store.setError('err')
    store.reset()
    expect(store.messages).toHaveLength(0)
    expect(store.error).toBeNull()
    expect(store.isConnected).toBe(false)
    expect(store.isStreaming).toBe(false)
  })

  it('multiple messages maintain order', () => {
    const store = useChatStore()
    store.isConnected = true
    store.send('a')
    store.beginAssistantTurn()
    store.send('b')
    expect(store.messages).toHaveLength(3)
    expect(store.messages[0].content).toBe('a')
    expect(store.messages[1].role).toBe('assistant')
    expect(store.messages[2].content).toBe('b')
  })
})
