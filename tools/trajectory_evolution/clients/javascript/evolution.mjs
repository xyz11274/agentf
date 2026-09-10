// Generated from ../typescript/evolution.ts; edit that source and regenerate.
export class EvolutionError extends Error {
    status;
    constructor(code, status = 0){
        super(code);
        this.name = "EvolutionError";
        this.status = status;
    }
}
export class PointClient {
    gaps = new Set();
    gapsFull = false;
    pointFields(fields) {
        const run = fields?.ref?.run;
        return typeof run === "string" && (this.gapsFull || this.gaps.has(run)) ? {
            ...fields,
            capture_gap: true
        } : fields;
    }
    pointObserved(fields, result) {
        const run = fields?.ref?.run;
        if (typeof run === "string" && run.length <= 256) {
            if (result.degraded) {
                if (this.gaps.size < 1024) this.gaps.add(run);
                else this.gapsFull = true;
            } else if (result.closed || result.run_closed) this.gaps.delete(run);
        }
        return result;
    }
    async call(fields, execute, ...options) {
        const prepared = await this.point({
            ...fields,
            phase: "before"
        }, ...options);
        const identity = fields.ref ? {
            ref: fields.ref
        } : {
            context: prepared.context
        };
        const actual = prepared.input;
        let output;
        if (prepared.decision === "reject") output = {
            status: "rejected",
            reason: prepared.reason ?? "policy_rejected"
        };
        else {
            try {
                output = await execute(actual);
            } catch (error) {
                await this.point({
                    ...identity,
                    kind: fields.kind,
                    phase: "abort",
                    status: "unknown",
                    output: null
                }, ...options);
                throw error;
            }
        }
        await this.point({
            ...identity,
            kind: fields.kind,
            phase: "after",
            executed: prepared.decision !== "reject",
            actual_input: actual,
            output
        }, ...options);
        return output;
    }
    async allowLoop(fields, ...options) {
        const result = await this.point({
            ...fields,
            phase: "guard",
            kind: "loop"
        }, ...options);
        return result.decision !== "stop";
    }
}
export class Evolution extends PointClient {
    #url;
    #token;
    #timeout;
    #maxBytes;
    #retries;
    constructor(options){
        super();
        const env = globalThis.process?.env;
        if (env?.TRAJECTORY_EVOLUTION_TRIAL_URL && env?.TRAJECTORY_EVOLUTION_TRIAL_TOKEN) options = {
            ...options,
            url: env.TRAJECTORY_EVOLUTION_TRIAL_URL,
            token: env.TRAJECTORY_EVOLUTION_TRIAL_TOKEN
        };
        const url = new URL(options.url);
        if (url.username || url.password || url.search || url.hash || ![
            "",
            "/"
        ].includes(url.pathname) || url.protocol !== "https:" && !(url.protocol === "http:" && [
            "localhost",
            "127.0.0.1",
            "[::1]"
        ].includes(url.hostname))) throw new EvolutionError("https_or_loopback_url_required");
        if (options.token.length < 24 || /[\r\n]/.test(options.token)) throw new EvolutionError("token_invalid");
        this.#url = url.origin + "/v1/invoke";
        this.#token = options.token;
        this.#timeout = options.timeoutMs ?? 7000;
        this.#maxBytes = options.maxResponseBytes ?? 2097152;
        this.#retries = options.retries ?? 1;
        if (!Number.isInteger(this.#timeout) || this.#timeout < 100 || this.#timeout > 300000 || !Number.isInteger(this.#maxBytes) || this.#maxBytes < 1024 || this.#maxBytes > 16777216 || !Number.isInteger(this.#retries) || this.#retries < 0 || this.#retries > 3) throw new EvolutionError("client_limits_invalid");
    }
    async invoke(operation, fields, requestId = crypto.randomUUID()) {
        return this.send(operation, fields, requestId, this.#timeout, this.#retries);
    }
    async point(fields, timeoutMs = 20, requestId = crypto.randomUUID()) {
        fields = this.pointFields(fields);
        try {
            if (!Number.isInteger(timeoutMs) || timeoutMs < 1 || timeoutMs > 5000) throw new Error("point_timeout_invalid");
            return this.pointObserved(fields, await this.send("point", fields, requestId, timeoutMs, 0));
        } catch  {
            const context = {
                ...fields?.context ?? {}
            }, kind = fields?.kind ?? context.kind ?? "custom";
            Object.assign(context, {
                kind,
                degraded: true,
                caused_by: [],
                run_id: context.run_id ?? `unobserved-${requestId}`
            });
            return this.pointObserved(fields, {
                context,
                input: fields?.input,
                degraded: true,
                capture_complete: false,
                policy_applied: false,
                decision: kind === "loop" ? "continue" : "allow",
                closed: false,
                ended: false,
                reason: "observer_unavailable"
            });
        }
    }
    async send(operation, fields, requestId, timeoutMs, retries) {
        const body = JSON.stringify({
            ...fields,
            operation,
            request_id: requestId
        });
        if (new TextEncoder().encode(body).length > 2097152) throw new EvolutionError("request_too_large");
        for(let attempt = 0;; ++attempt){
            try {
                const response = await fetch(this.#url, {
                    method: "POST",
                    redirect: "error",
                    cache: "no-store",
                    headers: {
                        "Content-Type": "application/json",
                        Authorization: "Bearer " + this.#token
                    },
                    body,
                    signal: AbortSignal.timeout(timeoutMs)
                });
                const reader = response.body?.getReader();
                if (!reader) throw new EvolutionError("response_missing");
                const chunks = [];
                let length = 0;
                try {
                    for(;;){
                        const part = await reader.read();
                        if (part.done) break;
                        length += part.value.length;
                        if (length > this.#maxBytes) throw new EvolutionError("response_too_large");
                        chunks.push(part.value);
                    }
                } finally{
                    await reader.cancel().catch(()=>{});
                    reader.releaseLock();
                }
                const bytes = new Uint8Array(length);
                let pos = 0;
                for (const chunk of chunks){
                    bytes.set(chunk, pos);
                    pos += chunk.length;
                }
                let payload;
                try {
                    payload = JSON.parse(new TextDecoder("utf-8", {
                        fatal: true
                    }).decode(bytes));
                } catch  {
                    throw new EvolutionError("response_invalid", response.status);
                }
                if (!response.ok || payload.ok !== true) {
                    const code = typeof payload.error === "string" && /^[a-z_]{1,96}$/.test(payload.error) ? payload.error : "rpc_failed";
                    throw new EvolutionError(code, response.status);
                }
                return payload.result;
            } catch (error) {
                const failure = error instanceof EvolutionError ? error : new EvolutionError("observer_unavailable");
                if (attempt >= retries || failure.status > 0 && failure.status < 500) throw failure;
            }
        }
    }
    async start(options) {
        const result = await this.invoke("task.begin", {
            conversation_id: options.conversationId,
            input: options.input,
            level: options.level,
            capabilities: options.capabilities
        });
        return this.resume(result.run_id);
    }
    resume(runId) {
        return new Run(this, runId);
    }
}
export class Run {
    client;
    id;
    telemetryError = null;
    constructor(client, id){
        this.client = client;
        this.id = id;
    }
    async invoke(operation, fields = {}) {
        return this.client.invoke(operation, {
            ...fields,
            run_id: this.id
        });
    }
    async step(state, options = {}) {
        const handle = await this.invoke("step.begin", {
            state,
            execution_id: options.executionId ?? "main",
            ...options.actorId ? {
                actor_id: options.actorId
            } : {},
            ...options.causedBy ? {
                caused_by: options.causedBy
            } : {}
        });
        return this.resumeStep(handle);
    }
    resumeStep(handle) {
        return new Step(this, handle);
    }
    async gap(reason) {
        this.telemetryError = reason;
        await this.invoke("capture.gap", {
            reason
        });
    }
    async observe(operation, fields) {
        try {
            return await this.invoke(operation, fields);
        } catch (error) {
            const privacy = error instanceof EvolutionError && [
                "credential_value_rejected",
                "sensitive_field_rejected"
            ].includes(error.message);
            try {
                await this.gap(privacy ? "privacy_rejected" : "observer_unavailable");
            } catch  {}
            if (operation === "model.observe" || operation === "action.observe") {
                const fallback = {
                    step_id: fields.step_id,
                    call_id: fields.call_id,
                    status: "unknown",
                    output: null,
                    error: {
                        code: "observation_unavailable",
                        retryability: "unknown"
                    }
                };
                if (operation === "model.observe") {
                    fallback.request = {};
                    fallback.transport_allowed = fields.transport_allowed === true;
                } else fallback.executed = fields.executed === true;
                try {
                    await this.invoke(operation, fallback);
                } catch  {}
            }
            return null;
        }
    }
    status() {
        return this.invoke("task.status");
    }
    retryEvaluation() {
        return this.invoke("evaluation.retry");
    }
    finish(output, options = {}) {
        return this.invoke(this.telemetryError ? "task.abort" : "task.end", {
            output: output ?? null,
            status: this.telemetryError ? "unknown" : options.status ?? "completed",
            business_status: options.businessStatus ?? "unknown",
            capture_complete: !this.telemetryError
        });
    }
    abort(output = null, status = "cancelled") {
        return this.invoke("task.abort", {
            output,
            status
        });
    }
}
export class Step {
    run;
    handle;
    constructor(run, handle){
        this.run = run;
        this.handle = handle;
    }
    get decision() {
        return this.handle.decision;
    }
    prepareModel(request) {
        return this.run.invoke("model.prepare", {
            step_id: this.handle.step_id,
            request
        });
    }
    observeModel(handle, actualRequest, output, status = "completed", transportAllowed = true) {
        return this.run.observe("model.observe", {
            step_id: this.handle.step_id,
            call_id: handle.call_id,
            request: actualRequest,
            output: output ?? null,
            status,
            transport_allowed: transportAllowed,
            ...status === "failed" ? {
                error: {
                    code: "model_call_failed",
                    retryability: "unknown"
                }
            } : {}
        });
    }
    async model(request, send) {
        const handle = await this.prepareModel(request);
        let output;
        try {
            output = await send(handle.request);
        } catch (error) {
            await this.observeModel(handle, handle.request, null, "failed");
            throw error;
        }
        await this.observeModel(handle, handle.request, output);
        return output;
    }
    async actions(actions) {
        if (!actions.length) return [];
        const result = await this.run.invoke("actions.prepare", {
            step_id: this.handle.step_id,
            actions
        });
        const byId = new Map(actions.map((a)=>[
                a.id,
                a
            ]));
        if (!Array.isArray(result.actions) || result.actions.length !== actions.length) throw new EvolutionError("action_plan_invalid");
        return result.actions.map((handle)=>{
            const action = byId.get(handle.id);
            if (!action) throw new EvolutionError("action_plan_invalid");
            byId.delete(handle.id);
            return new PlannedAction(this, handle, action);
        });
    }
    recordState(before, after) {
        return this.run.invoke("state.record", {
            step_id: this.handle.step_id,
            before,
            after
        });
    }
    end(output = null, status = "completed") {
        return this.run.invoke("step.end", {
            step_id: this.handle.step_id,
            output,
            status,
            decision: this.decision
        });
    }
}
export class PlannedAction {
    step;
    handle;
    action;
    #executed = false;
    constructor(step, handle, action){
        this.step = step;
        this.handle = handle;
        this.action = action;
    }
    observe(output, status, executed) {
        return this.step.run.observe("action.observe", {
            step_id: this.step.handle.step_id,
            call_id: this.handle.call_id,
            output: output ?? null,
            status,
            executed,
            ...status === "failed" ? {
                error: {
                    code: "action_failed",
                    retryability: "unknown"
                }
            } : {}
        });
    }
    async execute(send) {
        if (this.#executed) throw new EvolutionError("local_action_already_executed");
        this.#executed = true;
        let output = null, status = "cancelled";
        const executed = this.handle.decision === "allow";
        if (executed) {
            try {
                output = await send(this.action);
                status = "completed";
            } catch  {
                status = "failed";
                output = {
                    error: "action_failed"
                };
            }
        } else output = {
            error: "policy_rejected",
            reason: this.handle.reason
        };
        const receipt = await this.observe(output, status, executed);
        return {
            id: this.action.id,
            output,
            status,
            event_id: receipt?.event_id ?? null
        };
    }
}
