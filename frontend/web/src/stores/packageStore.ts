import { defineStore } from 'pinia'
import { ref } from 'vue'
import type { FileNode, PackageInfo } from '@/types/agentflow'
import { getApiClient } from '@/api/context'

interface BuildPackagePayload {
  path: string
  name: string
  size: number
  checksum: string
  last_modified: string
}

function encodePath(p: string): string {
  return p
    .split('/')
    .filter((s) => s.length > 0)
    .map((s) => encodeURIComponent(s))
    .join('/')
}

export const usePackageStore = defineStore('package', () => {
  const packages = ref<PackageInfo[]>([])
  const currentPackage = ref<string | null>(null)
  const fileTree = ref<FileNode[]>([])
  const currentFile = ref<{ path: string; content: string } | null>(null)
  const isLoading = ref(false)
  const error = ref<string | null>(null)

  async function loadPackages(): Promise<PackageInfo[]> {
    isLoading.value = true
    error.value = null
    try {
      const res = await getApiClient().get<PackageInfo[]>('/api/packages') as unknown as PackageInfo[]
      packages.value = res
      return packages.value
    } catch (e) {
      error.value = e instanceof Error ? e.message : String(e)
      throw e
    } finally {
      isLoading.value = false
    }
  }

  async function loadFileTree(name: string): Promise<FileNode[]> {
    isLoading.value = true
    error.value = null
    try {
      const res = await getApiClient().get<FileNode[]>(
        `/api/packages/${encodeURIComponent(name)}/tree`,
      ) as unknown as FileNode[]
      fileTree.value = res
      currentPackage.value = name
      return fileTree.value
    } catch (e) {
      error.value = e instanceof Error ? e.message : String(e)
      throw e
    } finally {
      isLoading.value = false
    }
  }

  async function readFile(name: string, path: string): Promise<{ path: string; content: string }> {
    error.value = null
    try {
      const res = await getApiClient().get<{ path: string; content: string }>(
        `/api/packages/${encodeURIComponent(name)}/file/${encodePath(path)}`,
      ) as unknown as { path: string; content: string }
      const result = { path, content: res.content }
      currentFile.value = result
      return result
    } catch (e) {
      error.value = e instanceof Error ? e.message : String(e)
      throw e
    }
  }

  async function writeFile(name: string, path: string, content: string): Promise<void> {
    error.value = null
    try {
      await getApiClient().put<unknown>(
        `/api/packages/${encodeURIComponent(name)}/file/${encodePath(path)}`,
        { content },
      )
      if (currentFile.value?.path === path) {
        currentFile.value = { path, content }
      }
    } catch (e) {
      error.value = e instanceof Error ? e.message : String(e)
      throw e
    }
  }

  async function deleteFile(name: string, path: string): Promise<void> {
    error.value = null
    try {
      await getApiClient().del<unknown>(
        `/api/packages/${encodeURIComponent(name)}/file/${encodePath(path)}`,
      )
      if (currentFile.value?.path === path) {
        currentFile.value = null
      }
    } catch (e) {
      error.value = e instanceof Error ? e.message : String(e)
      throw e
    }
  }

  async function renameFile(name: string, path: string, newName: string): Promise<void> {
    error.value = null
    try {
      await getApiClient().post<unknown>(
        `/api/packages/${encodeURIComponent(name)}/file/${encodePath(path)}/rename`,
        { new_name: newName },
      )
    } catch (e) {
      error.value = e instanceof Error ? e.message : String(e)
      throw e
    }
  }

  async function createFile(name: string, path: string): Promise<void> {
    error.value = null
    try {
      await getApiClient().post<unknown>(
        `/api/packages/${encodeURIComponent(name)}/file/${encodePath(path)}`,
        {},
      )
    } catch (e) {
      error.value = e instanceof Error ? e.message : String(e)
      throw e
    }
  }

  async function createDirectory(name: string, path: string): Promise<void> {
    error.value = null
    try {
      await getApiClient().post<unknown>(
        `/api/packages/${encodeURIComponent(name)}/dir/${encodePath(path)}`,
        {},
      )
    } catch (e) {
      error.value = e instanceof Error ? e.message : String(e)
      throw e
    }
  }

  async function buildPackage(name: string): Promise<BuildPackagePayload> {
    isLoading.value = true
    error.value = null
    try {
      const res = await getApiClient().post<BuildPackagePayload>(
        `/api/packages/${encodeURIComponent(name)}/package`,
      ) as unknown as BuildPackagePayload
      return res
    } catch (e) {
      error.value = e instanceof Error ? e.message : String(e)
      throw e
    } finally {
      isLoading.value = false
    }
  }

  async function createPackage(name: string): Promise<PackageInfo> {
    error.value = null
    try {
      const res = await getApiClient().post<PackageInfo>(
        '/api/packages/create',
        { name },
      ) as unknown as PackageInfo
      // Prepend to local list so the UI updates immediately
      packages.value.unshift(res)
      return res
    } catch (e) {
      error.value = e instanceof Error ? e.message : String(e)
      throw e
    }
  }

  async function updatePackage(
    name: string,
    file: File,
    mode: 'incremental' | 'full' = 'incremental',
  ): Promise<PackageInfo> {
    error.value = null
    isLoading.value = true
    try {
      const form = new FormData()
      form.append('file', file)
      form.append('mode', mode)

      const url = `/api/packages/${encodeURIComponent(name)}/update`
      const client = getApiClient()
      const res = await client.fetchImpl(url, {
        method: 'PUT',
        headers: { Accept: 'application/json' },
        body: form,
      })
      if (!res.ok) {
        const msg = await res.text()
        throw new Error(msg || `HTTP ${res.status} ${res.statusText}`)
      }
      const info = await res.json() as PackageInfo
      // Refresh package info in local list
      const idx = packages.value.findIndex(p => p.name === name)
      if (idx >= 0) {
        packages.value[idx] = info
      }
      return info
    } catch (e) {
      error.value = e instanceof Error ? e.message : String(e)
      throw e
    } finally {
      isLoading.value = false
    }
  }

  async function uploadPackage(file: File, name?: string): Promise<PackageInfo> {
    error.value = null
    isLoading.value = true
    try {
      const form = new FormData()
      form.append('file', file)
      if (name) form.append('name', name)

      const url = `/api/packages/upload`
      const client = getApiClient()
      const res = await client.fetchImpl(url, {
        method: 'POST',
        headers: { Accept: 'application/json' },
        body: form,
      })
      if (!res.ok) {
        const msg = await res.text()
        throw new Error(msg || `HTTP ${res.status} ${res.statusText}`)
      }
      const info = await res.json() as PackageInfo
      packages.value.unshift(info)
      return info
    } catch (e) {
      error.value = e instanceof Error ? e.message : String(e)
      throw e
    } finally {
      isLoading.value = false
    }
  }

  function setCurrentPackage(name: string | null): void {
    currentPackage.value = name
    if (name === null) {
      fileTree.value = []
      currentFile.value = null
    }
  }

  function reset(): void {
    packages.value = []
    currentPackage.value = null
    fileTree.value = []
    currentFile.value = null
    error.value = null
    isLoading.value = false
  }

  return {
    packages,
    currentPackage,
    fileTree,
    currentFile,
    isLoading,
    error,
    loadPackages,
    loadFileTree,
    readFile,
    writeFile,
    deleteFile,
    renameFile,
    createFile,
    createDirectory,
    buildPackage,
    createPackage,
    updatePackage,
    uploadPackage,
    setCurrentPackage,
    reset,
  }
})
