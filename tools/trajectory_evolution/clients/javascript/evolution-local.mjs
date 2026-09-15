import { createRequire } from 'node:module';
import { randomUUID } from 'node:crypto';
import { PointClient } from './evolution.mjs';

// Construct once at application startup. No Promise/HTTP/child process on invoke.
export class LocalEvolution extends PointClient {
  constructor({ library, config, token, ...options }) {
    super();
    this.native = createRequire(import.meta.url)(library);
    this.handle = this.native.open(config, token, JSON.stringify(options));
    this.limit = options.max_request_bytes ?? 65536;
  }
  fits(value, budget, depth = 0) {
    if (depth > 32 || budget < 0) return -1;
    if (typeof value === 'string') return budget - value.length * 6 - 2;
    if (value === null || typeof value === 'boolean') return budget - 32;
    if (typeof value === 'number') return Number.isFinite(value) ? budget - 32 : -1;
    if (typeof value !== 'object') return -1;
    for (const key of Object.keys(value)) {
      budget = this.fits(key, budget - 2, depth + 1);
      budget = this.fits(value[key], budget, depth + 1);
      if (budget < 0) return -1;
    }
    return budget - 2;
  }
  invoke(operation, fields = {}, request_id = randomUUID()) {
    fields = fields && typeof fields === 'object' && !Array.isArray(fields) ? fields : {};
    const fallback = { degraded: true, reason: 'local_unavailable', capture_complete: false,
      policy_applied: false, input: fields.input, event_id: '' };
    if (operation === 'point') {
      const context = { ...(fields.context ?? {}) }, kind = fields.kind ?? context.kind ?? 'custom';
      Object.assign(context, { kind, degraded: true, caused_by: [], run_id: context.run_id ?? `unobserved-${request_id}` });
      Object.assign(fallback, { context, decision: kind === 'loop' ? 'continue' : 'allow', closed: false, ended: false });
    }
    if (operation === 'run.begin') fallback.run_id = `unobserved-${request_id}`;
    if (operation === 'activity.begin') Object.assign(fallback, {
      activity_id: `unobserved-${request_id}`, decision: 'allow',
      context: { run_id: fields.run_id, activity_id: `unobserved-${request_id}`, degraded: true } });
    if (operation === 'loop.check') Object.assign(fallback, {
      control_id: `unobserved-${request_id}`, decision: 'continue', context: '' });
    if (['activity.end', 'activity.abort', 'loop.commit'].includes(operation)) fallback.ended = false;
    if (['run.end', 'run.abort'].includes(operation)) Object.assign(fallback, {closed: false, evaluation_status: 'quarantined'});
    try {
      const packet = { ...fields, operation, request_id };
      if (this.fits(packet, this.limit) < 0) throw new Error('local_request_too_large');
      const response = JSON.parse(this.native.invoke(this.handle, JSON.stringify(packet)));
      if (response.ok !== true) throw new Error('local_unavailable');
      return response.result.degraded ? { ...fallback, ...response.result } : response.result;
    } catch {
      try {
        const gap = operation === 'point' && fields.ref ? { operation: 'point', phase: 'gap', kind: 'run',
          ref: { run: fields.ref.run } } : { operation: 'capture.gap', run_id: fields.run_id ?? fields.context?.run_id,
          reason: 'observer_unavailable' };
        this.native.invoke(this.handle, JSON.stringify({ ...gap, request_id: randomUUID() }));
      } catch { /* Fail open without exposing input or native exception text. */ }
      return fallback;
    }
  }
  point(fields, request_id = randomUUID()) {
    fields = this.pointFields(fields);
    return this.pointObserved(fields, this.invoke('point', fields, request_id));
  }
  setEnabled(enabled) { this.native.setEnabled(this.handle, Boolean(enabled)); }
  metrics() { return JSON.parse(this.native.metrics(this.handle)); }
  drain() { this.native.drain(this.handle); } // Maintenance only; may wait for storage.
  close() { this.native.close(this.handle); } // Shutdown only, after callers stop.
}
