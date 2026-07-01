import { ApiClient } from '@/api/client'
import type { ApiResponse } from '@/types/agentflow'
import { vi, type Mock } from 'vitest'

export type FetchMock = Mock<(url: string, init?: RequestInit) => Promise<Response>>

export function mockGet(impl: () => Promise<Response> = async () => new Response()): FetchMock {
  return vi.fn(async (_url: string, _init?: RequestInit) => impl())
}

export function mockPut(impl: () => Promise<Response> = async () => new Response()): FetchMock {
  return vi.fn(async (_url: string, _init?: RequestInit) => impl())
}

export function mockPost(impl: () => Promise<Response> = async () => new Response()): FetchMock {
  return vi.fn(async (_url: string, _init?: RequestInit) => impl())
}

export function mockDel(impl: () => Promise<Response> = async () => new Response()): FetchMock {
  return vi.fn(async (_url: string, _init?: RequestInit) => impl())
}

export function makeClient(get: FetchMock, put: FetchMock, post: FetchMock, del: FetchMock): ApiClient {
  const dispatch: typeof fetch = (async (input: RequestInfo | URL, init?: RequestInit) => {
    const method = (init?.method ?? 'GET').toUpperCase()
    if (method === 'GET') return get(String(input), init)
    if (method === 'PUT') return put(String(input), init)
    if (method === 'POST') return post(String(input), init)
    if (method === 'DELETE') return del(String(input), init)
    throw new Error(`unexpected method ${method}`)
  }) as unknown as typeof fetch
  return new ApiClient({ fetchImpl: dispatch })
}

export function ok<T>(data: T): Response {
  return new Response(JSON.stringify({ success: true, data } satisfies ApiResponse<T>), {
    status: 200,
    headers: { 'Content-Type': 'application/json' },
  })
}

export function err(status: number, message: string): Response {
  return new Response(JSON.stringify({ success: false, data: {}, error: message }), {
    status,
    headers: { 'Content-Type': 'application/json' },
  })
}

export function trackGet(get: FetchMock): { url: string; init: RequestInit | undefined } {
  const calls = get.mock.calls
  const last = calls[calls.length - 1]
  if (!last) throw new Error('trackGet: no calls recorded')
  const [url, init] = last
  return { url, init }
}
