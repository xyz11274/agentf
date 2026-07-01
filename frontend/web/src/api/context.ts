import { ApiClient, apiClient as defaultApiClient } from './client'

let activeClient: ApiClient = defaultApiClient

export function setApiClient(client: ApiClient): void {
  activeClient = client
}

export function getApiClient(): ApiClient {
  return activeClient
}

export function resetApiClient(): void {
  activeClient = defaultApiClient
}

export { ApiClient }
