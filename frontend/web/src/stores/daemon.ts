import { defineStore } from 'pinia'
import { ref } from 'vue'
import type { DaemonStatus } from '../types/agentflow'

export const useDaemonStore = defineStore('daemon', () => {
  const connected = ref(false)
  const status = ref<DaemonStatus | null>(null)
  return { connected, status }
})
