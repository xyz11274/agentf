import { afterEach, describe, expect, it, vi } from 'vitest'
import { ApiClient, ApiError } from '@/api/client'

function makeResponse(status: number, body: string | object, contentType = 'application/json'): Response {
  const isObject = typeof body === 'object'
  const text = isObject ? JSON.stringify(body) : body
  return new Response(text, {
    status,
    headers: { 'Content-Type': contentType },
  })
}

describe('ApiClient', () => {
  afterEach(() => {
    vi.restoreAllMocks()
  })

  it('exposes a non-empty baseUrl and stable testId', () => {
    const client = new ApiClient()
    expect(client.baseUrl).toBe('')
    expect(client.testId).toBe('agentflow-api-client')
  })

  it('builds a relative URL when baseUrl is empty', async () => {
    const fetchMock = vi.fn<(input: RequestInfo | URL, init?: RequestInit) => Promise<Response>>(
      async () => makeResponse(200, { success: true, data: {} }),
    )
    const client = new ApiClient({ fetchImpl: fetchMock })
    await client.get('/api/agents')
    const call = fetchMock.mock.calls[0]
    expect(call?.[0]).toBe('/api/agents')
  })

  it('uses the provided baseUrl and preserves leading slash', async () => {
    const fetchMock = vi.fn<(input: RequestInfo | URL, init?: RequestInit) => Promise<Response>>(
      async () => makeResponse(200, { success: true, data: {} }),
    )
    const client = new ApiClient({ baseUrl: 'https://bridge.example.com', fetchImpl: fetchMock })
    await client.get('api/agents')
    const call = fetchMock.mock.calls[0]
    expect(call?.[0]).toBe('https://bridge.example.com/api/agents')
  })

  it('does not duplicate the path when it is already absolute', async () => {
    const fetchMock = vi.fn<(input: RequestInfo | URL, init?: RequestInit) => Promise<Response>>(
      async () => makeResponse(200, { success: true, data: {} }),
    )
    const client = new ApiClient({ baseUrl: 'https://bridge.example.com', fetchImpl: fetchMock })
    await client.get('https://other.example.com/api/agents')
    const call = fetchMock.mock.calls[0]
    expect(call?.[0]).toBe('https://other.example.com/api/agents')
  })

  it('performs GET without body', async () => {
    const fetchMock = vi.fn<(input: RequestInfo | URL, init?: RequestInit) => Promise<Response>>(
      async () => makeResponse(200, { success: true, data: { agents: [] } }),
    )
    const client = new ApiClient({ fetchImpl: fetchMock })

    const res = await client.get<{ agents: unknown[] }>('/api/agents')
    expect(res.data).toEqual({ agents: [] })
    const call = fetchMock.mock.calls[0]
    expect(call?.[1]?.method).toBe('GET')
    expect((call?.[1]?.headers as Record<string, string>).Accept).toBe('application/json')
    expect((call?.[1]?.headers as Record<string, string>)['Content-Type']).toBeUndefined()
  })

  it('performs PUT with JSON body', async () => {
    const fetchMock = vi.fn<(input: RequestInfo | URL, init?: RequestInit) => Promise<Response>>(
      async () => makeResponse(200, { success: true, data: { ok: true } }),
    )
    const client = new ApiClient({ fetchImpl: fetchMock })

    await client.put('/api/packages/foo/file/x', { content: 'hi' })
    const call = fetchMock.mock.calls[0]
    expect(call?.[1]?.method).toBe('PUT')
    expect((call?.[1]?.headers as Record<string, string>)['Content-Type']).toBe('application/json')
    expect(call?.[1]?.body).toBe(JSON.stringify({ content: 'hi' }))
  })

  it('performs POST with and without body', async () => {
    const fetchMock = vi.fn<(input: RequestInfo | URL, init?: RequestInit) => Promise<Response>>(
      async () => makeResponse(200, { success: true, data: {} }),
    )
    const client = new ApiClient({ fetchImpl: fetchMock })

    await client.post('/api/sessions/abc/dialogue')
    const first = fetchMock.mock.calls[0]
    expect(first?.[1]?.method).toBe('POST')
    expect((first?.[1]?.headers as Record<string, string>)['Content-Type']).toBeUndefined()

    await client.post('/api/sessions/abc/dialogue', { force: true })
    const second = fetchMock.mock.calls[1]
    expect((second?.[1]?.headers as Record<string, string>)['Content-Type']).toBe('application/json')
    expect(second?.[1]?.body).toBe(JSON.stringify({ force: true }))
  })

  it('performs DELETE without body', async () => {
    const fetchMock = vi.fn<(input: RequestInfo | URL, init?: RequestInit) => Promise<Response>>(
      async () => makeResponse(200, { success: true, data: {} }),
    )
    const client = new ApiClient({ fetchImpl: fetchMock })

    await client.del('/api/sessions/abc?agent_id=x')
    const call = fetchMock.mock.calls[0]
    expect(call?.[1]?.method).toBe('DELETE')
  })

  it('throws ApiError on non-2xx with parsed error string from JSON body', async () => {
    const fetchMock = vi.fn<(input: RequestInfo | URL, init?: RequestInit) => Promise<Response>>(
      async () => makeResponse(404, { success: false, data: {}, error: 'agent not found' }),
    )
    const client = new ApiClient({ fetchImpl: fetchMock })

    await expect(client.get('/api/agents/missing')).rejects.toBeInstanceOf(ApiError)
    await expect(client.get('/api/agents/missing')).rejects.toMatchObject({
      message: 'agent not found',
      status: 404,
      path: '/api/agents/missing',
    })
  })

  it('falls back to raw text when error body is not JSON', async () => {
    const fetchMock = vi.fn<(input: RequestInfo | URL, init?: RequestInit) => Promise<Response>>(
      async () => new Response('not json at all', { status: 500, headers: { 'Content-Type': 'text/plain' } }),
    )
    const client = new ApiClient({ fetchImpl: fetchMock })

    await expect(client.get('/api/agents')).rejects.toMatchObject({
      message: 'not json at all',
      status: 500,
    })
  })

  it('falls back to status text when error body is empty', async () => {
    const fetchMock = vi.fn<(input: RequestInfo | URL, init?: RequestInit) => Promise<Response>>(
      async () => new Response('', { status: 503, statusText: 'Service Unavailable' }),
    )
    const client = new ApiClient({ fetchImpl: fetchMock })

    await expect(client.get('/api/health')).rejects.toMatchObject({
      message: 'HTTP 503 Service Unavailable',
      status: 503,
    })
  })
})
