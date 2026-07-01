import { defineStore } from 'pinia'
import { ref } from 'vue'
import type { SkillInfo, SkillDetail, SkillInstallResponse } from '@/types/agentflow'
import { getApiClient } from '@/api/context'

export const useSkillHubStore = defineStore('skillHub', () => {
  const skills = ref<SkillInfo[]>([])
  const currentSkill = ref<SkillDetail | null>(null)
  const isLoading = ref(false)
  const installingName = ref<string | null>(null)
  const error = ref<string | null>(null)

  async function loadSkills(): Promise<SkillInfo[]> {
    isLoading.value = true
    error.value = null
    try {
      const res = await getApiClient().get<SkillInfo[]>('/api/skill-hub')
      // Backend returns SkillInfo[] directly (no ApiResponse wrapper)
      skills.value = res as unknown as SkillInfo[]
      return skills.value
    } catch (e) {
      error.value = e instanceof Error ? e.message : String(e)
      throw e
    } finally {
      isLoading.value = false
    }
  }

  async function loadSkillDetail(name: string): Promise<SkillDetail> {
    isLoading.value = true
    error.value = null
    try {
      const res = await getApiClient().get<SkillDetail>(
        `/api/skill-hub/${encodeURIComponent(name)}`,
      ) as unknown as SkillDetail
      currentSkill.value = res
      return res
    } catch (e) {
      error.value = e instanceof Error ? e.message : String(e)
      throw e
    } finally {
      isLoading.value = false
    }
  }

  async function uploadSkill(file: File, name?: string, overwrite = false): Promise<SkillInfo> {
    error.value = null
    isLoading.value = true
    try {
      const form = new FormData()
      form.append('file', file)
      if (name) form.append('name', name)
      if (overwrite) form.append('overwrite', 'true')

      const url = '/api/skill-hub/upload'
      const client = getApiClient()
      const res = await client.fetchImpl(url, {
        method: 'POST',
        headers: { Accept: 'application/json' },
        body: form,
      })
      if (!res.ok) {
        const msg = await res.text()
        throw new Error(msg || `HTTP ${res.status}`)
      }
      const json = await res.json() as SkillInfo
      // Refresh the list
      await loadSkills()
      return json
    } catch (e) {
      error.value = e instanceof Error ? e.message : String(e)
      throw e
    } finally {
      isLoading.value = false
    }
  }

  async function deleteSkill(name: string): Promise<void> {
    error.value = null
    try {
      await getApiClient().del<unknown>(`/api/skill-hub/${encodeURIComponent(name)}`)
      skills.value = skills.value.filter((s) => s.name !== name)
      if (currentSkill.value?.name === name) {
        currentSkill.value = null
      }
    } catch (e) {
      error.value = e instanceof Error ? e.message : String(e)
      throw e
    }
  }

  async function installSkill(name: string, targetPackage: string): Promise<SkillInstallResponse> {
    error.value = null
    installingName.value = name
    try {
      const res = await getApiClient().post<SkillInstallResponse>(
        `/api/skill-hub/${encodeURIComponent(name)}/install`,
        { target_package: targetPackage },
      ) as unknown as SkillInstallResponse
      return res
    } catch (e) {
      error.value = e instanceof Error ? e.message : String(e)
      throw e
    } finally {
      installingName.value = null
    }
  }

  function reset(): void {
    skills.value = []
    currentSkill.value = null
    error.value = null
    isLoading.value = false
    installingName.value = null
  }

  return {
    skills,
    currentSkill,
    isLoading,
    installingName,
    error,
    loadSkills,
    loadSkillDetail,
    uploadSkill,
    deleteSkill,
    installSkill,
    reset,
  }
})
