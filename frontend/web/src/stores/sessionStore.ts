import { defineStore } from 'pinia'
import { ref } from 'vue'
import type { PersistentSession } from '@/types/agentflow'
import { getApiClient } from '@/api/context'

export const useSessionStore = defineStore('session', () => {
  const sessions = ref<PersistentSession[]>([])
  const isLoading = ref(false)
  const error = ref<string | null>(null)
  /** 已删除会话 ID 集合，防止 auto-refresh 重新加载回来 */
  const deletedSessionIds = ref<Set<string>>(new Set())

  async function loadSessions(userId?: string): Promise<PersistentSession[]> {
    isLoading.value = true
    error.value = null
    try {
      const res = await getApiClient().listPersistentSessions(userId, 50)
      const list = res.data?.sessions ?? []
      sessions.value = list.filter((s) => !deletedSessionIds.value.has(s.id))
      return sessions.value
    } catch (e) {
      error.value = e instanceof Error ? e.message : String(e)
      throw e
    } finally {
      isLoading.value = false
    }
  }

  async function destroySession(sessionId: string): Promise<void> {
    error.value = null
    deletedSessionIds.value = new Set([...deletedSessionIds.value, sessionId])
    try {
      await getApiClient().deletePersistentSession(sessionId)
    } catch (e) {
      error.value = e instanceof Error ? e.message : String(e)
      throw e
    } finally {
      sessions.value = sessions.value.filter((s) => s.id !== sessionId)
    }
  }

  /**
   * 批量删除多个会话。所有请求并行发送，单个失败不影响其他。
   * 返回成功删除的 session ID 列表。不会抛出异常——错误通过 error ref 暴露。
   */
  async function batchDestroySessions(sessionIds: string[]): Promise<string[]> {
    if (sessionIds.length === 0) return []
    error.value = null
    const completed: string[] = []
    const results = await Promise.allSettled(
      sessionIds.map((id) => getApiClient().deletePersistentSession(id)),
    )
    for (let i = 0; i < sessionIds.length; i++) {
      if (results[i]?.status === 'fulfilled') {
        completed.push(sessionIds[i]!)
        deletedSessionIds.value = new Set([...deletedSessionIds.value, sessionIds[i]!])
      } else {
        const reason = (results[i] as PromiseRejectedResult).reason
        const msg = reason instanceof Error ? reason.message : String(reason)
        error.value = error.value ? `${error.value}; ${msg}` : msg
      }
    }
    sessions.value = sessions.value.filter((s) => !completed.includes(s.id))
    return completed
  }

  /**
   * 删除当前列表中的所有会话。等价于 batchDestroySessions(sessions.map(s => s.id))。
   */
  async function destroyAllSessions(): Promise<string[]> {
    const allIds = sessions.value.map((s) => s.id)
    return batchDestroySessions(allIds)
  }

  function addPendingSession(userId: string, clientId: string): PersistentSession {
    const now = new Date().toISOString()
    const entry: PersistentSession = {
      id: `pending_${clientId}`,
      user_id: userId,
      client_id: clientId,
      title: '',
      created_at: now,
      last_active_at: now,
      status: 'new',
    }
    sessions.value = [entry, ...sessions.value]
    return entry
  }

  function reset(): void {
    sessions.value = []
    isLoading.value = false
    error.value = null
    deletedSessionIds.value = new Set()
  }

  return {
    sessions,
    isLoading,
    error,
    loadSessions,
    destroySession,
    batchDestroySessions,
    destroyAllSessions,
    addPendingSession,
    deletedSessionIds,
    reset,
  }
})
