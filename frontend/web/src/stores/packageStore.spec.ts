import { afterEach, beforeEach, describe, expect, it } from 'vitest'
import { createPinia, setActivePinia } from 'pinia'
import type { FileNode, PackageInfo } from '@/types/agentflow'
import { resetApiClient, setApiClient } from '@/api/context'
import { usePackageStore } from '@/stores/packageStore'
import { err, makeClient, mockDel, mockGet, mockPost, mockPut, ok } from '@/__tests__/testUtils'

const packages: PackageInfo[] = [
  { name: 'pkg1', path: '/x/pkg1', file_count: 3, last_modified: '2026-01-01T00:00:00Z' },
  { name: 'pkg2', path: '/x/pkg2', file_count: 1, last_modified: '2026-01-02T00:00:00Z' },
]

const tree: FileNode[] = [
  { name: 'agent.json', path: 'agent.json', type: 'file' },
  { name: 'nodes', path: 'nodes', type: 'dir', children: [] },
]

function urlOf(mock: { mock: { calls: unknown[][] } }, index = 0): string {
  const call = mock.mock.calls[index]
  const url = call?.[0] as string | undefined
  if (url === undefined) throw new Error('expected call')
  return url
}

function bodyOf(mock: { mock: { calls: unknown[][] } }, index = 0): unknown {
  const call = mock.mock.calls[index]
  const init = call?.[1] as RequestInit | undefined
  if (init?.body === undefined) return undefined
  return JSON.parse(init.body as string)
}

describe('usePackageStore', () => {
  beforeEach(() => {
    setActivePinia(createPinia())
  })

  afterEach(() => {
    resetApiClient()
  })

  it('starts empty', () => {
    const store = usePackageStore()
    expect(store.packages).toEqual([])
    expect(store.currentPackage).toBeNull()
    expect(store.fileTree).toEqual([])
    expect(store.currentFile).toBeNull()
  })

  it('loadPackages fetches the list and stores it', async () => {
    const get = mockGet(async () => ok({ packages }))
    setApiClient(makeClient(get, mockPut(), mockPost(), mockDel()))
    const store = usePackageStore()
    const result = await store.loadPackages()
    expect(result).toEqual(packages)
    expect(urlOf(get)).toBe('/api/packages')
  })

  it('loadPackages surfaces errors', async () => {
    const get = mockGet(async () => err(500, 'fs broken'))
    setApiClient(makeClient(get, mockPut(), mockPost(), mockDel()))
    const store = usePackageStore()
    await expect(store.loadPackages()).rejects.toThrow('fs broken')
  })

  it('loadFileTree sets currentPackage and tree', async () => {
    const get = mockGet(async () => ok(tree))
    setApiClient(makeClient(get, mockPut(), mockPost(), mockDel()))
    const store = usePackageStore()
    const t = await store.loadFileTree('pkg1')
    expect(t).toEqual(tree)
    expect(store.fileTree).toEqual(tree)
    expect(store.currentPackage).toBe('pkg1')
    expect(urlOf(get)).toBe('/api/packages/pkg1/tree')
  })

  it('readFile returns content and caches currentFile', async () => {
    const get = mockGet(async () => ok({ content: '{}' }))
    setApiClient(makeClient(get, mockPut(), mockPost(), mockDel()))
    const store = usePackageStore()
    const r = await store.readFile('pkg1', 'agent.json')
    expect(r).toEqual({ path: 'agent.json', content: '{}' })
    expect(store.currentFile).toEqual({ path: 'agent.json', content: '{}' })
    expect(urlOf(get)).toBe('/api/packages/pkg1/file/agent.json')
  })

  it('readFile URL-encodes nested paths', async () => {
    const get = mockGet(async () => ok({ content: 'x' }))
    setApiClient(makeClient(get, mockPut(), mockPost(), mockDel()))
    const store = usePackageStore()
    await store.readFile('pkg1', 'nodes/agent.py')
    expect(urlOf(get)).toBe('/api/packages/pkg1/file/nodes/agent.py')
  })

  it('writeFile sends PUT with the content', async () => {
    const put = mockPut(async () => ok({}))
    setApiClient(makeClient(mockGet(), put, mockPost(), mockDel()))
    const store = usePackageStore()
    await store.writeFile('pkg1', 'agent.json', 'updated')
    expect(urlOf(put)).toBe('/api/packages/pkg1/file/agent.json')
    expect(bodyOf(put)).toEqual({ content: 'updated' })
  })

  it('writeFile updates cached currentFile when the path matches', async () => {
    const get = mockGet(async () => ok({ content: '{}' }))
    const put = mockPut(async () => ok({}))
    setApiClient(makeClient(get, put, mockPost(), mockDel()))
    const store = usePackageStore()
    await store.readFile('pkg1', 'agent.json')
    expect(store.currentFile?.content).toBe('{}')
    await store.writeFile('pkg1', 'agent.json', 'updated')
    expect(store.currentFile?.content).toBe('updated')
  })

  it('deleteFile calls DELETE and clears currentFile if matching', async () => {
    const get = mockGet(async () => ok({ content: '{}' }))
    const del = mockDel(async () => ok({}))
    setApiClient(makeClient(get, mockPut(), mockPost(), del))
    const store = usePackageStore()
    await store.readFile('pkg1', 'agent.json')
    await store.deleteFile('pkg1', 'agent.json')
    expect(urlOf(del)).toBe('/api/packages/pkg1/file/agent.json')
    expect(store.currentFile).toBeNull()
  })

  it('renameFile posts new_name to the /rename endpoint', async () => {
    const post = mockPost(async () => ok({}))
    setApiClient(makeClient(mockGet(), mockPut(), post, mockDel()))
    const store = usePackageStore()
    await store.renameFile('pkg1', 'old.txt', 'new.txt')
    expect(urlOf(post)).toBe('/api/packages/pkg1/file/old.txt/rename')
    expect(bodyOf(post)).toEqual({ new_name: 'new.txt' })
  })

  it('createFile posts to the file path with no body', async () => {
    const post = mockPost(async () => ok({}))
    setApiClient(makeClient(mockGet(), mockPut(), post, mockDel()))
    const store = usePackageStore()
    await store.createFile('pkg1', 'sub/new.txt')
    expect(urlOf(post)).toBe('/api/packages/pkg1/file/sub/new.txt')
    expect(bodyOf(post)).toEqual({})
  })

  it('createDirectory posts to the /dir path', async () => {
    const post = mockPost(async () => ok({}))
    setApiClient(makeClient(mockGet(), mockPut(), post, mockDel()))
    const store = usePackageStore()
    await store.createDirectory('pkg1', 'sub')
    expect(urlOf(post)).toBe('/api/packages/pkg1/dir/sub')
  })

  it('buildPackage posts to /package and returns the envelope', async () => {
    const post = mockPost(async () =>
      ok({ path: 'p/x.tar.gz', name: 'x.tar.gz', size: 10, checksum: 'abc', last_modified: 'now' }),
    )
    setApiClient(makeClient(mockGet(), mockPut(), post, mockDel()))
    const store = usePackageStore()
    const r = await store.buildPackage('pkg1')
    expect(r.name).toBe('x.tar.gz')
    expect(urlOf(post)).toBe('/api/packages/pkg1/package')
  })

  it('setCurrentPackage(null) clears tree and currentFile', async () => {
    const get = mockGet(async () => ok(tree))
    setApiClient(makeClient(get, mockPut(), mockPost(), mockDel()))
    const store = usePackageStore()
    await store.loadFileTree('pkg1')
    expect(store.fileTree).toEqual(tree)
    store.setCurrentPackage(null)
    expect(store.fileTree).toEqual([])
    expect(store.currentFile).toBeNull()
  })

  it('reset clears all state', async () => {
    const get = mockGet(async () => ok({ packages }))
    setApiClient(makeClient(get, mockPut(), mockPost(), mockDel()))
    const store = usePackageStore()
    await store.loadPackages()
    store.reset()
    expect(store.packages).toEqual([])
    expect(store.currentPackage).toBeNull()
  })
})
