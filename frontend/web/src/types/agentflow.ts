/**
 * AgentFlow shared type definitions
 *
 * This file defines TypeScript interfaces that are mirrored exactly by
 * Pydantic v2 models in `frontend/bridge/models.py`. Any change here MUST be
 * reflected in `frontend/bridge/models.py` to keep the two sides in sync.
 *
 * Design principles:
 *  - Use `interface` (not `type`) for all object shapes.
 *  - Field names are snake_case to match the wire format (daemon / bridge JSON).
 *  - Optional fields are explicitly marked with `?`.
 *  - No business logic; this file is types only.
 */

// ---------------------------------------------------------------------------
// Agent
// ---------------------------------------------------------------------------

/**
 * Information about a single agent instance known to the daemon.
 */
export interface AgentInfo {
  id: string;
  name: string;
  workflow_count: number;
  status: 'loaded' | 'running' | 'paused' | 'unloaded';
}

// ---------------------------------------------------------------------------
// Session
// ---------------------------------------------------------------------------

/**
 * Information about an active runtime session.
 */
export interface SessionInfo {
  session_id: string;
  user_id: string;
  client_id: string;
  age_ms: number;
  status: string;
}

// ---------------------------------------------------------------------------
// Package
// ---------------------------------------------------------------------------

/**
 * A packaged agent collection on disk. Files are read directly by the bridge
 * (no daemon round-trip).
 */
export interface PackageInfo {
  name: string;
  path: string;
  file_count: number;
  last_modified: string;
}

// ---------------------------------------------------------------------------
// File tree
// ---------------------------------------------------------------------------

/**
 * A node in the file tree displayed in the file-explorer UI.
 * - `type` distinguishes files from directories.
 * - `children` is present for directories and absent for files.
 * - `content` is only populated when the file has been opened/read.
 */
export interface FileNode {
  name: string;
  path: string;
  type: 'file' | 'dir';
  children?: FileNode[];
  content?: string;
}

// ---------------------------------------------------------------------------
// Workflow
// ---------------------------------------------------------------------------

/**
 * A single node in a workflow DAG.
 */
export interface WorkflowNode {
  id: string;
  name: string;
  type: string;
  execution_mode?: string;
  description?: string;
  risk_level?: 'green' | 'yellow' | 'red';
}

/**
 * A directed edge in a workflow DAG.
 * `from`/`to` reference `WorkflowNode.id`.
 */
export interface WorkflowEdge {
  from: string;
  to: string;
  control_type?: 'none' | 'if' | 'switch_case';
  condition?: string;
}

/**
 * A complete workflow definition: nodes + edges.
 */
export interface WorkflowDef {
  id: string;
  name: string;
  nodes: WorkflowNode[];
  edges: WorkflowEdge[];
}

// ---------------------------------------------------------------------------
// Daemon
// ---------------------------------------------------------------------------

/**
 * Runtime status of the AgentFlow daemon, as reported by the bridge.
 */
export interface DaemonStatus {
  status: string;
  version: string;
  uptime: number;
  active_agents: number;
  running: boolean;
}

// ---------------------------------------------------------------------------
// Skill Hub
// ---------------------------------------------------------------------------

/**
 * Metadata for a skill in the central hub repository.
 */
export interface SkillInfo {
  name: string;
  description: string;
  tags: string[];
  version: string;
  file_count: number;
  last_modified: string;
}

/**
 * Full detail of a skill, including SKILL.md content.
 */
export interface SkillDetail extends SkillInfo {
  content: string;
}

/**
 * Response after installing a skill into an agent package.
 */
export interface SkillInstallResponse {
  name: string;
  target_package: string;
  target_path: string;
  file_count: number;
}

// ---------------------------------------------------------------------------
// Chat
// ---------------------------------------------------------------------------

/**
 * A single chat message exchanged with the agent.
 */
export interface ChatMessage {
  id: string;
  role: 'user' | 'assistant';
  content: string;
  timestamp: number;
}

// ---------------------------------------------------------------------------
// Generic API wrapper
// ---------------------------------------------------------------------------

/**
 * Standard wrapper used by the bridge REST API.
 * - `data` is the typed payload on success.
 * - `error` is populated when `success` is `false`.
 */
export interface ApiResponse<T> {
  success: boolean;
  data: T;
  error?: string;
}

// ---------------------------------------------------------------------------
// WebSocket chat (JSON-RPC over WS between bridge and daemon)
// ---------------------------------------------------------------------------

/**
 * Client -> bridge chat request.
 * Uses ``user_id`` + ``client_id`` to identify the caller.
 * ``session_id`` is a daemon-internal concept and is NOT exposed to clients.
 */
export interface ChatRequest {
  user_id: string;
  client_id: string;
  content: string;
}

/**
 * Streaming chunk emitted by the bridge during a chat turn.
 * - `final: true` means this is the last chunk for the current turn.
 */
export interface ChatChunk {
  type: 'chunk';
  content: string;
  final: boolean;
}

/**
 * Terminal "done" message closing a chat turn.
 * No ``session_id`` is returned — it is a daemon-internal concept.
 */
export interface ChatDone {
  type: 'done';
  final: true;
}

// ---------------------------------------------------------------------------
// LLM Trace
// ---------------------------------------------------------------------------

/**
 * A single LLM call trace summary (returned by the list endpoint).
 */
export interface LlmTraceSummary {
  call_id: string;
  model: string;
  latency_ms: number;
  request_summary: string;
  response_summary: string;
  timestamp: string;
  token_usage: {
    prompt_tokens: number;
    completion_tokens: number;
    total_tokens: number;
  };
}

/**
 * Trace list API response ``data`` payload.
 */
export interface LlmTraceListData {
  traces: LlmTraceSummary[];
  total: number;
}

/**
 * Full detail for a single LLM call.
 */
export interface LlmTraceDetail {
  call_id: string;
  request: Record<string, unknown> | null;
  response: Record<string, unknown> | null;
  meta: Record<string, unknown> | null;
}
