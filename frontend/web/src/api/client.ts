import type { ApiResponse } from '@/types/agentflow'

export class ApiError extends Error {
  readonly status: number
  readonly path: string
  constructor(message: string, status: number, path: string) {
    super(message)
    this.name = 'ApiError'
    this.status = status
    this.path = path
  }
}

export interface ApiClientOptions {
  baseUrl?: string
  fetchImpl?: typeof fetch
}

export class ApiClient {
  readonly baseUrl: string
  fetchImpl: typeof fetch
  readonly testId = 'agentflow-api-client'

  constructor(options: ApiClientOptions = {}) {
    this.baseUrl = options.baseUrl ?? ''
    // bind fetch to window to avoid "Illegal invocation" error
    this.fetchImpl = options.fetchImpl ?? ((...args: Parameters<typeof fetch>) => fetch(...args))
  }

  private buildUrl(path: string): string {
    if (path.startsWith('http://') || path.startsWith('https://')) {
      return path
    }
    const leading = path.startsWith('/') ? path : `/${path}`
    return `${this.baseUrl}${leading}`
  }

  private async request<T>(
    method: string,
    path: string,
    body?: unknown,
  ): Promise<ApiResponse<T>> {
    const url = this.buildUrl(path)
    const init: RequestInit = {
      method,
      headers: { Accept: 'application/json' },
    }
    if (body !== undefined) {
      init.headers = { ...init.headers, 'Content-Type': 'application/json' }
      init.body = JSON.stringify(body)
    }
    const res = await this.fetchImpl(url, init)
    if (!res.ok) {
      const message = await this.extractError(res)
      throw new ApiError(message, res.status, path)
    }
    const json = (await res.json()) as ApiResponse<T>
    return json
  }

  private async extractError(res: Response): Promise<string> {
    const text = await res.text()
    if (!text) return `HTTP ${res.status} ${res.statusText}`
    try {
      const parsed = JSON.parse(text) as Partial<ApiResponse<unknown>>
      if (parsed.error && typeof parsed.error === 'string') {
        return parsed.error
      }
    } catch {
      return text
    }
    return text
  }

  get<T>(path: string): Promise<ApiResponse<T>> {
    return this.request<T>('GET', path)
  }

  put<T>(path: string, body?: unknown): Promise<ApiResponse<T>> {
    return this.request<T>('PUT', path, body)
  }

  post<T>(path: string, body?: unknown): Promise<ApiResponse<T>> {
    return this.request<T>('POST', path, body)
  }

  del<T>(path: string): Promise<ApiResponse<T>> {
    return this.request<T>('DELETE', path)
  }

  // ── Bridge persistent session API ──────────────────────────

  /**
   * GET /api/sessions/persistent — list Bridge-side persistent sessions.
   */
  listPersistentSessions(userId?: string, limit?: number, offset?: number) {
    const params = new URLSearchParams()
    if (userId) params.set('user_id', userId)
    if (limit !== undefined) params.set('limit', String(limit))
    if (offset !== undefined) params.set('offset', String(offset))
    const qs = params.toString()
    return this.get<import('@/types/agentflow').PersistentSessionListResponse>(
      `/api/sessions/persistent${qs ? `?${qs}` : ''}`,
    )
  }

  /**
   * GET /api/sessions/persistent/{id}/messages — list historical messages.
   */
  listPersistentMessages(sessionId: string, limit?: number) {
    const params = limit !== undefined ? `?limit=${limit}` : ''
    return this.get<import('@/types/agentflow').PersistentMessageListResponse>(
      `/api/sessions/persistent/${encodeURIComponent(sessionId)}/messages${params}`,
    )
  }

  /**
   * GET /api/sessions/persistent/{id}/traces — list traces for a session.
   */
  listPersistentTraces(sessionId: string, dialogueId?: string) {
    const params = dialogueId ? `?dialogue_id=${encodeURIComponent(dialogueId)}` : ''
    return this.get<unknown>(
      `/api/sessions/persistent/${encodeURIComponent(sessionId)}/traces${params}`,
    )
  }

  /**
   * DELETE /api/sessions/persistent/{id} — hard-delete a persistent session.
   */
  deletePersistentSession(sessionId: string) {
    return this.del<{ deleted: string }>(
      `/api/sessions/persistent/${encodeURIComponent(sessionId)}`,
    )
  }

  /**
   * PATCH /api/sessions/persistent/{id} — update session metadata.
   */
  updatePersistentSession(sessionId: string, data: { status?: string; title?: string }) {
    const params = new URLSearchParams()
    if (data.status) params.set('status', data.status)
    if (data.title) params.set('title', data.title)
    return this.request<unknown>('PATCH',
      `/api/sessions/persistent/${encodeURIComponent(sessionId)}?${params.toString()}`,
    )
  }
}

export const apiClient = new ApiClient()
