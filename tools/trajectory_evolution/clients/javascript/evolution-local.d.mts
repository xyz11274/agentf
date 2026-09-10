export interface LocalOptions {
  library: string;
  config: string;
  token: string;
  max_request_bytes?: number;
  max_buffer_bytes?: number;
  max_pending_runs?: number;
  poll_interval_ms?: number;
  background?: boolean;
  contention_budget_us?: number;
}
export class LocalEvolution {
  constructor(options: LocalOptions);
  invoke(operation: string, fields?: Record<string, unknown>, request_id?: string): Record<string, any>;
  point(fields: Record<string, unknown>, request_id?: string): Record<string, any>;
  call(fields: Record<string, unknown>, execute: (input: any) => any, request_id?: string): Promise<any>;
  allowLoop(fields: Record<string, unknown>, request_id?: string): Promise<boolean>;
  metrics(): Record<string, number | string | boolean>;
  setEnabled(enabled: boolean): void;
  /** Blocking maintenance, never a business-boundary call. */
  drain(): void;
  /** Application shutdown only, after callers stop. */
  close(): void;
}
