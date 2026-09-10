"""Execution helpers shared by native and HTTP clients; no host adapter required."""
import asyncio
import json


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
