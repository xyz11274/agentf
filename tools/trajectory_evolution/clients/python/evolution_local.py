"""Native in-process L0-L3 client. No HTTP, subprocess, retries or implicit flush."""
import ctypes
import json
import os
import uuid
try:
    from evolution_point import PointCalls
except ModuleNotFoundError:
    # Also support loading this installed client directly by its file path.
    import importlib.util
    from pathlib import Path
    _spec = importlib.util.spec_from_file_location("_evolution_point", Path(__file__).with_name("evolution_point.py"))
    _module = importlib.util.module_from_spec(_spec)
    _spec.loader.exec_module(_module)
    PointCalls = _module.PointCalls


class LocalEvolution(PointCalls):
    def __init__(self, library, config, token, **options):
        self._point_init()
        # Initialize once during application startup, before business requests.
        self._pid = os.getpid()
        self._native = ctypes.CDLL(os.fspath(library))
        self._native.te_local_open.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_char_p,
                                               ctypes.POINTER(ctypes.c_void_p)]
        self._native.te_local_open.restype = ctypes.c_void_p
        self._native.te_local_invoke.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_size_t]
        self._native.te_local_invoke.restype = ctypes.c_void_p
        self._native.te_local_metrics.argtypes = [ctypes.c_void_p]
        self._native.te_local_metrics.restype = ctypes.c_void_p
        self._native.te_local_drain.argtypes = [ctypes.c_void_p]
        self._native.te_local_close.argtypes = [ctypes.c_void_p]
        self._native.te_local_set_enabled.argtypes = [ctypes.c_void_p, ctypes.c_int]
        for name in ("te_local_drain", "te_local_close", "te_local_set_enabled"):
            getattr(self._native, name).restype = None
        self._native.te_local_free.argtypes = [ctypes.c_void_p]
        self._native.te_local_free.restype = None
        self._limit = options.get("max_request_bytes", 65536)
        error = ctypes.c_void_p()
        self._handle = self._native.te_local_open(os.fsencode(config), token.encode("utf-8"),
            json.dumps(options, allow_nan=False).encode("utf-8"), ctypes.byref(error))
        if error.value:
            self._native.te_local_free(error)
        if not self._handle:
            raise RuntimeError("local_initialization_failed")

    def _value(self, pointer):
        if not pointer:
            raise RuntimeError("local_unavailable")
        try:
            return json.loads(ctypes.string_at(pointer).decode("utf-8"))
        finally:
            self._native.te_local_free(pointer)

    def _fits(self, value, budget, depth=0):
        # Bound traversal before JSON encoding; do not duplicate huge media input.
        if depth > 32 or budget < 0:
            return -1
        if isinstance(value, str):
            return budget - len(value) * 6 - 2
        if isinstance(value, dict):
            for key, item in value.items():
                if not isinstance(key, str):
                    return -1
                budget = self._fits(key, budget - 2, depth + 1)
                budget = self._fits(item, budget, depth + 1)
                if budget < 0:
                    return -1
            return budget - 2
        if isinstance(value, (tuple, list)):
            for item in value:
                budget = self._fits(item, budget - 1, depth + 1)
                if budget < 0:
                    return -1
            return budget - 2
        if value is None or isinstance(value, (bool, float, int)):
            return budget - 32
        return -1

    def invoke(self, operation, fields, request_id=None):
        fields = fields if isinstance(fields, dict) else {}
        request_id = request_id or uuid.uuid4().hex
        fallback = {"degraded": True, "reason": "local_unavailable", "capture_complete": False,
                    "policy_applied": False, "input": fields.get("input"), "event_id": ""}
        if operation == "point":
            context = dict(fields["context"]) if isinstance(fields.get("context"), dict) else {}
            kind = fields.get("kind", context.get("kind", "custom"))
            context.update(kind=kind, degraded=True, caused_by=[])
            context.setdefault("run_id", "unobserved-" + request_id)
            fallback.update(context=context, decision="continue" if kind == "loop" else "allow", closed=False, ended=False)
        if operation == "run.begin":
            fallback["run_id"] = "unobserved-" + request_id
        if operation == "activity.begin":
            fallback.update(activity_id="unobserved-" + request_id, decision="allow")
            fallback["context"] = {"run_id": fields.get("run_id"), "activity_id": fallback["activity_id"], "degraded": True}
        if operation == "loop.check":
            fallback.update(control_id="unobserved-" + request_id, decision="continue", context="")
        if operation in ("activity.end", "activity.abort", "loop.commit"):
            fallback["ended"] = False
        if operation in ("run.end", "run.abort"):
            fallback.update(closed=False, evaluation_status="quarantined")
        try:
            if not self._handle or os.getpid() != self._pid:
                return fallback
            packet = {**fields, "operation": operation, "request_id": request_id}
            if self._fits(packet, self._limit) < 0:
                raise ValueError("local_request_too_large")
            body = json.dumps(packet, ensure_ascii=False, allow_nan=False).encode("utf-8")
            response = self._value(self._native.te_local_invoke(self._handle, body, len(body)))
            if response.get("ok") is not True:
                raise ValueError("local_unavailable")
            result = response["result"]
            return {**fallback, **result} if result.get("degraded") else result
        except Exception:
            # A skipped record must taint its run, even when local encoding failed.
            try:
                if self._handle and os.getpid() == self._pid:
                    gap = {"operation": "capture.gap", "request_id": uuid.uuid4().hex,
                           "run_id": fields.get("run_id", (fields.get("context") or {}).get("run_id")), "reason": "observer_unavailable"}
                    if operation == "point" and isinstance(fields.get("ref"), dict):
                        gap = {"operation": "point", "request_id": uuid.uuid4().hex, "kind": "run", "phase": "gap",
                               "ref": {"run": fields["ref"].get("run")}}
                    body = json.dumps(gap, allow_nan=False).encode("utf-8")
                    self._value(self._native.te_local_invoke(self._handle, body, len(body)))
            except Exception:
                pass
            return fallback

    def point(self, fields, request_id=None):
        fields = self._point_fields(fields)
        return self._point_observed(fields, self.invoke("point", fields, request_id))

    def metrics(self):
        return self._value(self._native.te_local_metrics(self._handle)) if self._handle and os.getpid() == self._pid else {}

    def set_enabled(self, enabled):
        if self._handle and os.getpid() == self._pid:
            self._native.te_local_set_enabled(self._handle, int(bool(enabled)))

    def drain(self):
        """Blocking maintenance only. Never call at a model/tool/run boundary."""
        if self._handle and os.getpid() == self._pid:
            self._native.te_local_drain(self._handle)

    def manage(self, operation, fields=None, request_id=None):
        """Blocking evaluation administration. Use outside Agent boundaries."""
        if not self._handle or os.getpid() != self._pid:
            raise RuntimeError("local_unavailable")
        packet = {**(fields or {}), "operation": operation, "request_id": request_id or uuid.uuid4().hex}
        if self._fits(packet, self._limit) < 0:
            raise ValueError("local_request_too_large")
        body = json.dumps(packet, ensure_ascii=False, allow_nan=False).encode("utf-8")
        call = self._native.te_local_manage
        call.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_size_t]
        call.restype = ctypes.c_void_p
        result = self._value(call(self._handle, body, len(body)))
        if not result.get("ok"):
            raise RuntimeError(result.get("error", "local_management_unavailable"))
        return result["result"]

    def close(self):
        """Application shutdown only; stop callers first, then drain and join."""
        handle, self._handle = self._handle, None
        if handle and os.getpid() == self._pid:
            self._native.te_local_close(handle)
