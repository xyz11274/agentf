import assert from "node:assert/strict";
import { Evolution, EvolutionError, PlannedAction, PointClient } from "./evolution.mjs";

class FakePoints extends PointClient {
  constructor(before, failAbort = false) {
    super();
    this.before = before;
    this.failAbort = failAbort;
    this.events = [];
  }
  async point(fields) {
    this.events.push(fields);
    if(fields.phase === "abort" && this.failAbort) throw new Error("observer_failed");
    if(fields.phase === "before") return { input: fields.input, ...this.before };
    return { event_id: "receipt-1" };
  }
}

{
  const points = new FakePoints({ decision: "reject", reason: "policy_rejected" });
  let calls = 0;
  const result = await points.call(
    { kind: "action", ref: { run: "run", key: "call" }, input: { value: 1 } },
    () => { ++calls; });
  assert.equal(calls, 0);
  assert.deepEqual(result, { status: "rejected", reason: "policy_rejected", error: "policy_rejected" });
  assert.equal(points.events.at(-1).executed, false);
}

{
  const points = new FakePoints({ decision: "allow" }, true);
  const business = new Error("business_failure");
  await assert.rejects(
    points.call({ kind: "model", ref: { run: "run", key: "call" }, input: {} },
      () => { throw business; }),
    error => error === business);
  assert.equal(points.events.at(-1).phase, "abort");
}

{
  const observations = [];
  const step = {
    handle: { step_id: "step" },
    run: { observe: async (...args) => { observations.push(args); throw new Error("observer_failed"); } },
  };
  const action = new PlannedAction(step,
    { id: "action", call_id: "call", decision: "allow", reason: "", event_id: "event" },
    { id: "action", operation: "tool", arguments: {} });
  const business = new Error("tool_failure");
  await assert.rejects(action.execute(async () => { throw business; }), error => error === business);
  assert.equal(observations.length, 1);
  assert.equal(observations[0][1].status, "failed");
  await assert.rejects(action.execute(async () => ({})), /local_action_already_executed/);
}

{
  const client = new Evolution({
    url: "http://127.0.0.1:1",
    token: "fixture-client-token-00000001",
    timeoutMs: 100,
    retries: 0,
  });
  await assert.rejects(client.invoke("point", { value: Number.NaN }),
    error => error instanceof EvolutionError && error.message === "request_invalid");
  const circular = {}; circular.self = circular;
  await assert.rejects(client.invoke("point", circular),
    error => error instanceof EvolutionError && error.message === "request_invalid");
}

console.log("JavaScript client contract passed");
