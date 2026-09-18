"""Execution helpers shared by native and HTTP clients; no host adapter required."""
import asyncio
import json
import uuid


class PointCalls:
    def _point_init(self):
        self._point_gaps, self._point_gaps_full = set(), False

    def _point_fields(self, fields):
        ref = fields.get("ref") if isinstance(fields, dict) else None
        run = ref.get("run") if isinstance(ref, dict) else None
        if isinstance(run, str) and (run in self._point_gaps or self._point_gaps_full):
            return dict(fields, capture_gap=True)
        return fields

    def _point_observed(self, fields, result):
        ref = fields.get("ref") if isinstance(fields, dict) else None
        run = ref.get("run") if isinstance(ref, dict) else None
        if isinstance(run, str) and len(run) <= 256:
            if result.get("degraded"):
                if len(self._point_gaps) < 1024:
                    self._point_gaps.add(run)
                else:
                    self._point_gaps_full = True
            elif result.get("closed") or result.get("run_closed"):
                self._point_gaps.discard(run)
        return result

    def _finish_fields(self, fields, prepared, **outcome):
        identity = {"ref": fields["ref"]} if "ref" in fields else {"context": prepared["context"]}
        return dict(identity, kind=fields.get("kind", "custom"), **outcome)

    def call(self, fields, execute, *, result_format="json", **point_options):
        """Wrap one existing JSON-in/JSON-out call; never retry the business call."""
        prepared = self.point(dict(fields, phase="before"), **point_options)
        actual = prepared.get("input", fields.get("input"))
        if prepared.get("decision") == "reject":
            reason = prepared.get("reason", "policy_rejected")
            result = {"status": "rejected", "reason": reason, "error": reason}
            if result_format == "json_text":
                result = json.dumps(result, ensure_ascii=False)
            self.point(self._finish_fields(fields, prepared, phase="after", executed=False,
                                           actual_input=actual, output=result), **point_options)
            return result
        try:
            result = execute(actual)
        except BaseException:
            # Exception text may contain credentials. Keep the original exception
            # for the business caller, but record only an unknown terminal state.
            self.point(self._finish_fields(fields, prepared, phase="abort", status="unknown",
                                           output=None), **point_options)
            raise
        self.point(self._finish_fields(fields, prepared, phase="after", executed=True,
                                       actual_input=actual, output=result), **point_options)
        return result

    async def acall(self, fields, execute, *, result_format="json", **point_options):
        """Await the original coroutine once; HTTP observation runs off the event loop."""
        prepared = await asyncio.to_thread(self.point, dict(fields, phase="before"), **point_options)
        actual = prepared.get("input", fields.get("input"))
        if prepared.get("decision") == "reject":
            reason = prepared.get("reason", "policy_rejected")
            result = {"status": "rejected", "reason": reason, "error": reason}
            if result_format == "json_text":
                result = json.dumps(result, ensure_ascii=False)
            await asyncio.to_thread(self.point, self._finish_fields(fields, prepared, phase="after", executed=False,
                                                                  actual_input=actual, output=result), **point_options)
            return result
        try:
            result = await execute(actual)
        except BaseException:
            await asyncio.to_thread(self.point, self._finish_fields(fields, prepared, phase="abort", status="unknown",
                                                                  output=None), **point_options)
            raise
        await asyncio.to_thread(self.point, self._finish_fields(fields, prepared, phase="after", executed=True,
                                                              actual_input=actual, output=result), **point_options)
        return result

    def allow_loop(self, fields, **point_options):
        """Use directly in the existing loop condition; commit exactly that decision."""
        if "ref" in fields:
            return self.point(dict(fields, phase="guard", kind="loop"), **point_options).get("decision", "continue") != "stop"
        prepared = self.point(dict(fields, phase="before", kind="loop"), **point_options)
        decision = prepared.get("decision", "continue")
        self.point(self._finish_fields(dict(fields, kind="loop"), prepared, phase="after", decision=decision), **point_options)
        return decision != "stop"

    async def aallow_loop(self, fields, **point_options):
        return await asyncio.to_thread(self.allow_loop, fields, **point_options)

    def _dispatch_wave(self, prepared, execute, result_format, point_options):
        """Claim each batch once; an ACK never substitutes for its completion."""
        last_output = None
        for _ in range(128):
            if not prepared.get("wave_id"):
                return dict(prepared, output=prepared.get("output", last_output))
            wave_id, plan_ref = prepared["wave_id"], prepared["plan_ref"]

            def event(name, payload, unique=False):
                ref = {"run": plan_ref["run"], "parent": plan_ref["key"],
                       "key": wave_id + ":" + name + (":" + uuid.uuid4().hex if unique else "")}
                return self.point({"phase": "event", "kind": "dispatch", "name": name,
                                   "ref": ref, "input": dict(payload, wave_id=wave_id)}, **point_options)

            claimed = event("claim", {}, unique=True)
            if claimed.get("degraded"):
                # The runtime may have accepted this claim despite a lost reply.
                # Never retry the business callback or redispatch the full batch.
                event("abort", {})
                raise RuntimeError("dispatch_claim_uncertain")
            if not claimed.get("execute"):
                return dict(claimed, managed=True)
            try:
                last_output = execute(claimed["input"])
                decoded = json.loads(last_output) if result_format == "json_text" else last_output
                if not isinstance(decoded, dict):
                    raise ValueError("dispatch_ack_invalid")
                last_output = decoded
            except BaseException:
                event("abort", {})
                raise
            prepared = event("accept", {"output": decoded})
            if prepared.get("degraded"):
                # Submission already happened. Preserve the host ACK and mark
                # the capture gap; no callback replay can repair an unknown ACK.
                return dict(prepared, managed=True, pending=True, output=decoded)
        raise RuntimeError("dispatch_wave_limit")

    def dispatch(self, fields, execute, *, result_format="json", **point_options):
        """Wrap an existing asynchronous batch submitter; persist waits in the package."""
        prepared = self.point(dict(fields, phase="before", kind="dispatch"), **point_options)
        if prepared.get("degraded") or not prepared.get("managed"):
            if not prepared.get("degraded") and prepared.get("context", {}).get("activity_id"):
                self.point(self._finish_fields(dict(fields, kind="custom"), prepared,
                           phase="abort", status="unknown", output=None, capture_gap=True), **point_options)
            return execute(fields.get("input"))
        result = self._dispatch_wave(prepared, execute, result_format, point_options)
        output = result.get("output")
        if output is None:
            raise RuntimeError("dispatch_submission_pending")
        return json.dumps(output, ensure_ascii=False) if result_format == "json_text" else output

    def resume_dispatch(self, fields, execute, *, result_format="json", **point_options):
        """Use at the host's existing batch-completion entry, before another model call."""
        prepared = self.point(dict(fields, phase="event", kind="dispatch", name="complete"), **point_options)
        if prepared.get("degraded") or not prepared.get("managed"):
            return dict(prepared, pending=False, completion=fields.get("input"))
        return self._dispatch_wave(prepared, execute, result_format, point_options)
