/**
 * LLM Trace API client — wraps calls to the bridge's LLM trace endpoints.
 *
 * All requests go through the shared {@link apiClient} which reads
 * Vite's ``/api`` proxy so no base URL maintenance is needed here.
 */

import { apiClient } from './client'
import type { LlmTraceDetail, LlmTraceListData } from '@/types/agentflow'

/**
 * Fetch the LLM trace list for a given agent + user identity.
 * Optionally filter by ``dialogueId`` to scope to a single dialogue turn.
 */
export async function getLlmTraces(
  agentId: string,
  userId: string,
  clientId: string,
  dialogueId?: string,
  limit = 50,
  offset = 0,
): Promise<LlmTraceListData> {
  let path =
    `/api/agents/${encodeURIComponent(agentId)}/llm-traces` +
    `?user_id=${encodeURIComponent(userId)}` +
    `&client_id=${encodeURIComponent(clientId)}` +
    `&limit=${limit}&offset=${offset}`
  if (dialogueId) {
    path += `&dialogue_id=${encodeURIComponent(dialogueId)}`
  }
  const res = await apiClient.get<LlmTraceListData>(path)
  if (!res.success) {
    throw new Error(res.error ?? 'failed to fetch LLM traces')
  }
  return res.data
}

/**
 * Fetch the full detail for a single LLM call.
 */
export async function getLlmTraceDetail(
  agentId: string,
  userId: string,
  clientId: string,
  callId: string,
): Promise<LlmTraceDetail> {
  const path =
    `/api/agents/${encodeURIComponent(agentId)}/llm-traces/${encodeURIComponent(callId)}` +
    `?user_id=${encodeURIComponent(userId)}` +
    `&client_id=${encodeURIComponent(clientId)}`
  const res = await apiClient.get<LlmTraceDetail>(path)
  if (!res.success) {
    throw new Error(res.error ?? 'failed to fetch LLM trace detail')
  }
  return res.data
}
