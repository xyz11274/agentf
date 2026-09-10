"""HTTP/JSON transport for every L0-L3 boundary; Python standard library only."""
import http.client
import json
import os
import re
import time
import uuid
from urllib.parse import urlsplit
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


class EvolutionError(Exception):
    def __init__(self, code, status=0):
        super().__init__(code)
        self.status = status


class Evolution(PointCalls):
    def __init__(self, url, token, timeout_ms=7000, max_response_bytes=2097152, retries=1):
        self._point_init()
        if os.environ.get("TRAJECTORY_EVOLUTION_TRIAL_URL") and os.environ.get("TRAJECTORY_EVOLUTION_TRIAL_TOKEN"):
            url, token = os.environ["TRAJECTORY_EVOLUTION_TRIAL_URL"], os.environ["TRAJECTORY_EVOLUTION_TRIAL_TOKEN"]
        endpoint = urlsplit(url)
        if (endpoint.username or endpoint.password or endpoint.query or endpoint.fragment
                or endpoint.path not in ("", "/") or not endpoint.hostname
                or (endpoint.scheme != "https" and not (endpoint.scheme == "http"
                    and endpoint.hostname in ("localhost", "127.0.0.1", "::1")))):
            raise EvolutionError("https_or_loopback_url_required")
        if not isinstance(token, str) or len(token) < 24 or any(ord(c) < 33 or ord(c) > 126 for c in token):
            raise EvolutionError("token_invalid")
        if (type(timeout_ms) is not int or not 100 <= timeout_ms <= 300000
                or type(max_response_bytes) is not int or not 1024 <= max_response_bytes <= 16777216
                or type(retries) is not int or not 0 <= retries <= 3):
            raise EvolutionError("client_limits_invalid")
        self._endpoint, self._token = endpoint, token
        self._timeout, self._max_bytes, self._retries = timeout_ms / 1000, max_response_bytes, retries

    def invoke(self, operation, fields, request_id=None):
        """Return result, or a fixed-code error. Retry this RPC, never a business call."""
        return self._invoke(operation, fields, request_id, self._timeout, self._retries)

    def point(self, fields, timeout_ms=20, request_id=None):
        """One bounded attempt; transport failure preserves the business input."""
        request_id = request_id or uuid.uuid4().hex
        try:
            fields = self._point_fields(fields)
            if type(timeout_ms) is not int or not 1 <= timeout_ms <= 5000:
                raise ValueError("point_timeout_invalid")
            return self._point_observed(fields, self._invoke("point", fields, request_id, timeout_ms / 1000, 0))
        except Exception:
            fields = fields if isinstance(fields, dict) else {}
            context = dict(fields["context"]) if isinstance(fields.get("context"), dict) else {}
            kind = fields.get("kind", context.get("kind", "custom"))
            context.update(kind=kind, degraded=True, caused_by=[])
            context.setdefault("run_id", "unobserved-" + request_id)
            return self._point_observed(fields, {"context": context, "input": fields.get("input"), "degraded": True, "capture_complete": False,
                    "policy_applied": False, "decision": "continue" if kind == "loop" else "allow",
                    "closed": False, "ended": False, "reason": "observer_unavailable"})

    def _invoke(self, operation, fields, request_id, timeout, retries):
        if not isinstance(fields, dict):
            raise EvolutionError("object_required")
        try:
            body = json.dumps({**fields, "operation": operation, "request_id": request_id or uuid.uuid4().hex},
                              ensure_ascii=False, allow_nan=False).encode("utf-8")
        except (TypeError, ValueError, UnicodeError):
            raise EvolutionError("request_invalid") from None
        if len(body) > 2097152:
            raise EvolutionError("request_too_large")
        for attempt in range(retries + 1):
            connection = None
            try:
                deadline = time.monotonic() + timeout
                kind = http.client.HTTPSConnection if self._endpoint.scheme == "https" else http.client.HTTPConnection
                # Direct connection: no environment proxy and no redirect follow.
                connection = kind(self._endpoint.hostname, self._endpoint.port, timeout=timeout)
                connection.connect()
                transport = connection.sock
                transport.settimeout(max(0.001, deadline - time.monotonic()))
                connection.request("POST", "/v1/invoke", body=body,
                                   headers={"Content-Type": "application/json", "Authorization": "Bearer " + self._token})
                transport.settimeout(max(0.001, deadline - time.monotonic()))
                response = connection.getresponse()
                content = bytearray()
                while not response.isclosed():
                    remaining = deadline - time.monotonic()
                    if remaining <= 0:
                        raise EvolutionError("observer_unavailable")
                    transport.settimeout(remaining)
                    part = response.read1(min(65536, self._max_bytes + 1 - len(content)))
                    if not part:
                        break
                    content.extend(part)
                    if len(content) > self._max_bytes:
                        raise EvolutionError("response_too_large")
                try:
                    payload = json.loads(content.decode("utf-8"))
                except (ValueError, UnicodeError):
                    raise EvolutionError("response_invalid", response.status) from None
                if not isinstance(payload, dict):
                    raise EvolutionError("response_invalid", response.status)
                if not 200 <= response.status < 300 or payload.get("ok") is not True:
                    code = payload.get("error", "rpc_failed")
                    code = code if isinstance(code, str) and re.fullmatch(r"[a-z_]{1,96}", code) else "rpc_failed"
                    raise EvolutionError(code, response.status)
                if not isinstance(payload.get("result"), dict):
                    raise EvolutionError("response_invalid", response.status)
                return payload["result"]
            except Exception as error:
                failure = error if isinstance(error, EvolutionError) else EvolutionError("observer_unavailable")
                if attempt == retries or 0 < failure.status < 500:
                    raise failure from None
            finally:
                if connection:
                    connection.close()
