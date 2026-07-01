import { defineStore } from 'pinia'
import { ref } from 'vue'
import type { SessionInfo } from '@/types/agentflow'
import { getApiClient } from '@/api/context'

export const useSessionStore = defineStore('session', () => {
  const sessions = ref<SessionInfo[]>([])
  const agentId = ref<string | null>(null)
  const isLoading = ref(false)
  const error = ref<string | null>(null)

  async function loadSessions(targetAgentId: string, userId: string = ''): Promise<SessionInfo[]> {
    isLoading.value = true
    error.value = null
    try {
      let url = `/api/sessions?agent_id=${encodeURIComponent(targetAgentId)}`
      if (userId) {
        url += `&user_id=${encodeURIComponent(userId)}`
      }
      const res = await getApiClient().get<unknown>(url)
      const raw = res.data as Record<string, unknown>
      const rawList: Record<string, unknown>[] = Array.isArray(raw)
        ? (raw as Record<string, unknown>[])
        : ((raw?.sessions ?? []) as Record<string, unknown>[])
      const serverSessions: SessionInfo[] = rawList.map((s) => ({
        session_id: (s.session_id ?? '') as string,
        user_id: (s.user_id ?? '') as string,
        client_id: (s.client_id ?? '') as string,
        age_ms: (s.age_ms ?? 0) as number,
        status: (s.status ?? '') as string,
      }))
      // Merge: keep pending sessions (not yet on server), replace existing ones
      const pendingSessions = sessions.value.filter(
        (s) => s.session_id.startsWith('pending_') &&
               !serverSessions.some((ss) => ss.client_id === s.client_id),
      )
      sessions.value = [...serverSessions, ...pendingSessions]
      agentId.value = targetAgentId
      return sessions.value
    } catch (e) {
      error.value = e instanceof Error ? e.message : String(e)
      throw e
    } finally {
      isLoading.value = false
    }
  }

  async function destroySession(sessionId: string, targetAgentId: string): Promise<void> {
    error.value = null
    try {
      await getApiClient().del<unknown>(
        `/api/sessions/${encodeURIComponent(sessionId)}?agent_id=${encodeURIComponent(targetAgentId)}`,
      )
      sessions.value = sessions.value.filter((s) => s.session_id !== sessionId)
    } catch (e) {
      error.value = e instanceof Error ? e.message : String(e)
      throw e
    }
  }

  async function clearDialogue(
    sessionId: string,
    dialogueId: string,
    targetAgentId: string,
  ): Promise<void> {
    error.value = null
    try {
      await getApiClient().del<unknown>(
        `/api/sessions/${encodeURIComponent(sessionId)}/dialogue?agent_id=${encodeURIComponent(
          targetAgentId,
        )}&dialogue_id=${encodeURIComponent(dialogueId)}`,
      )
    } catch (e) {
      error.value = e instanceof Error ? e.message : String(e)
      throw e
    }
  }

  function addPendingSession(agentId: string, userId: string, clientId: string): void {
    // Add an optimistic local entry so it shows in the list immediately.
    // The real session will be created server-side on first agent.execute.
    const exists = sessions.value.some((s) => s.client_id === clientId)
    if (!exists) {
      sessions.value.push({
        session_id: `pending_${clientId}`,
        user_id: userId,
        client_id: clientId,
        age_ms: 0,
        status: 'new',
      })
    }
  }

  function reset(): void {
    sessions.value = []
    agentId.value = null
    error.value = null
    isLoading.value = false
  }

  return {
    sessions,
    agentId,
    isLoading,
    error,
    loadSessions,
    destroySession,
    clearDialogue,
    addPendingSession,
    reset,
  }
})
