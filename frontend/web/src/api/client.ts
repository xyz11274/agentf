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
}

export const apiClient = new ApiClient()
