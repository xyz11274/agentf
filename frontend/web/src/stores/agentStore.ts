import { defineStore } from 'pinia'
import { ref } from 'vue'
import type { AgentInfo } from '@/types/agentflow'
import { getApiClient } from '@/api/context'

/** Normalize daemon agent info (wire format uses ``agent_id``) to the frontend
 *  ``AgentInfo`` interface (uses ``id``). */
function toAgentInfo(raw: Record<string, unknown>): AgentInfo {
  return {
    id: (raw.agent_id ?? raw.id) as string,
    name: (raw.name ?? '') as string,
    workflow_count: (raw.workflow_count ?? 0) as number,
    status: (raw.status ?? 'loaded') as AgentInfo['status'],
  }
}

export const useAgentStore = defineStore('agent', () => {
  const agents = ref<AgentInfo[]>([])
  const currentAgentId = ref<string | null>(null)
  const isLoading = ref(false)
  const error = ref<string | null>(null)

  const currentAgent = (): AgentInfo | null => {
    if (currentAgentId.value === null) return null
    return agents.value.find((a) => a.id === currentAgentId.value) ?? null
  }

  async function loadAgents(): Promise<AgentInfo[]> {
    isLoading.value = true
    error.value = null
    try {
      const res = await getApiClient().get<unknown>('/api/agents')
      const raw = res.data as Record<string, unknown>
      const rawList: Record<string, unknown>[] = Array.isArray(raw)
        ? (raw as Record<string, unknown>[])
        : ((raw?.agents ?? []) as Record<string, unknown>[])
      agents.value = rawList.map(toAgentInfo)
      return agents.value
    } catch (e) {
      error.value = e instanceof Error ? e.message : String(e)
      throw e
    } finally {
      isLoading.value = false
    }
  }

  function setCurrentAgent(id: string | null): void {
    currentAgentId.value = id
  }

  async function loadAgent(id: string): Promise<AgentInfo> {
    isLoading.value = true
    error.value = null
    try {
      const res = await getApiClient().get<unknown>(`/api/agents/${encodeURIComponent(id)}`)
      const agent = toAgentInfo(res.data as Record<string, unknown>)
      const idx = agents.value.findIndex((a) => a.id === agent.id)
      if (idx >= 0) {
        agents.value.splice(idx, 1, agent)
      } else {
        agents.value.push(agent)
      }
      return agent
    } catch (e) {
      error.value = e instanceof Error ? e.message : String(e)
      throw e
    } finally {
      isLoading.value = false
    }
  }

  /**
   * Load an agent package via the daemon ``agent.load`` RPC.
   *
   * ``id`` is the canonical agent id (used as the URL segment). When
   * ``packagePath`` is omitted the bridge falls back to ``id``; passing
   * an explicit path lets the caller load a non-default package on disk.
   *
   * Returns the refreshed AgentInfo and replaces/inserts it into the
   * in-memory list so the UI updates without a follow-up fetch.
   */
  async function loadAgentPackage(
    id: string,
    packagePath?: string,
  ): Promise<AgentInfo> {
    isLoading.value = true
    error.value = null
    try {
      const body = packagePath ? { package_path: packagePath } : {}
      const res = await getApiClient().post<Record<string, unknown>>(
        `/api/agents/${encodeURIComponent(id)}/load`,
        body,
      )
      // The daemon returns the actual agent_id from the package manifest;
      // use it to find the agent in the refreshed list rather than trying
      // to match by the package directory name (which may differ).
      const loadedAgentId = (res.data?.agent_id as string | undefined) ?? id
      await loadAgents()
      const loaded = agents.value.find((a) => a.id === loadedAgentId)
      if (loaded) return loaded
      throw new Error(`Agent '${id}' loaded but not found in list`)
    } catch (e) {
      error.value = e instanceof Error ? e.message : String(e)
      throw e
    } finally {
      isLoading.value = false
    }
  }

  /** Unload an agent via ``POST /api/agents/{id}/unload`` and refresh state. */
  async function unloadAgent(id: string): Promise<AgentInfo> {
    isLoading.value = true
    error.value = null
    try {
      await getApiClient().post<unknown>(
        `/api/agents/${encodeURIComponent(id)}/unload`,
      )
      await loadAgents()
      const remaining = agents.value.find((a) => a.id === id || a.name === id)
      if (remaining) return remaining
      throw new Error(`Agent '${id}' unloaded`)
    } catch (e) {
      error.value = e instanceof Error ? e.message : String(e)
      throw e
    } finally {
      isLoading.value = false
    }
  }

  /** Pause a running agent via ``POST /api/agents/{id}/pause`` and refresh state. */
  async function pauseAgent(id: string): Promise<AgentInfo> {
    isLoading.value = true
    error.value = null
    try {
      await getApiClient().post<unknown>(
        `/api/agents/${encodeURIComponent(id)}/pause`,
      )
      await loadAgents()
      const updated = agents.value.find((a) => a.id === id || a.name === id)
      if (updated) return updated
      throw new Error(`Agent '${id}' paused but not found in list`)
    } catch (e) {
      error.value = e instanceof Error ? e.message : String(e)
      throw e
    } finally {
      isLoading.value = false
    }
  }

  /** Resume a paused agent via ``POST /api/agents/{id}/resume`` and refresh state. */
  async function resumeAgent(id: string): Promise<AgentInfo> {
    isLoading.value = true
    error.value = null
    try {
      await getApiClient().post<unknown>(
        `/api/agents/${encodeURIComponent(id)}/resume`,
      )
      await loadAgents()
      const updated = agents.value.find((a) => a.id === id || a.name === id)
      if (updated) return updated
      throw new Error(`Agent '${id}' resumed but not found in list`)
    } catch (e) {
      error.value = e instanceof Error ? e.message : String(e)
      throw e
    } finally {
      isLoading.value = false
    }
  }

  function reset(): void {
    agents.value = []
    currentAgentId.value = null
    error.value = null
    isLoading.value = false
  }

  return {
    agents,
    currentAgentId,
    isLoading,
    error,
    currentAgent,
    loadAgents,
    setCurrentAgent,
    loadAgent,
    loadAgentPackage,
    unloadAgent,
    pauseAgent,
    resumeAgent,
    reset,
  }
})
