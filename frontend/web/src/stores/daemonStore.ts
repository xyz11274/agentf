import { defineStore } from 'pinia'
import { ref } from 'vue'
import type { DaemonStatus } from '@/types/agentflow'
import { getApiClient } from '@/api/context'

interface HealthPayload {
  status: string
}

export const DEFAULT_POLL_INTERVAL_MS = 15000

export const useDaemonStore = defineStore('daemon', () => {
  const status = ref<DaemonStatus | null>(null)
  const isConnected = ref(false)
  const isLoading = ref(false)
  const error = ref<string | null>(null)
  let pollHandle: ReturnType<typeof setInterval> | null = null

  async function checkHealth(): Promise<boolean> {
    isLoading.value = true
    try {
      const res = await getApiClient().get<HealthPayload>('/api/health') as unknown as HealthPayload
      const ok = res.status === 'ok'
      isConnected.value = ok
      if (!ok) {
        error.value = `health: unexpected status "${res.status}"`
      } else {
        error.value = null
      }
      return ok
    } catch (e) {
      isConnected.value = false
      error.value = e instanceof Error ? e.message : String(e)
      return false
    } finally {
      isLoading.value = false
    }
  }

  async function getDaemonStatus(): Promise<DaemonStatus | null> {
    isLoading.value = true
    try {
      const res = await getApiClient().get<DaemonStatus>('/api/daemon/status') as unknown as DaemonStatus
      status.value = res
      isConnected.value = Boolean(res.running)
      error.value = null
      return res
    } catch (e) {
      isConnected.value = false
      error.value = e instanceof Error ? e.message : String(e)
      throw e
    } finally {
      isLoading.value = false
    }
  }

  function startPolling(intervalMs: number = DEFAULT_POLL_INTERVAL_MS): void {
    if (pollHandle !== null) return
    pollHandle = setInterval(() => {
      void checkHealth()
    }, intervalMs)
  }

  function stopPolling(): void {
    if (pollHandle === null) return
    clearInterval(pollHandle)
    pollHandle = null
  }

  function init(intervalMs: number = DEFAULT_POLL_INTERVAL_MS): void {
    void checkHealth()
    startPolling(intervalMs)
  }

  function dispose(): void {
    stopPolling()
    status.value = null
    isConnected.value = false
    error.value = null
    isLoading.value = false
  }

  return {
    status,
    isConnected,
    isLoading,
    error,
    checkHealth,
    getDaemonStatus,
    startPolling,
    stopPolling,
    init,
    dispose,
  }
})
