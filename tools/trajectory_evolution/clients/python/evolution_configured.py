"""Configuration-owned point access. Hosts only pass existing identity and data."""
import json
import os
from pathlib import Path
import re
import threading
import uuid
from urllib.parse import urlsplit
from evolution import Evolution


_SECRET_NAME = re.compile(r"key|token|secret|password|credential|auth", re.I)
_CREDENTIAL = re.compile(
    r"\b(?:sk-|ark-)[A-Za-z0-9_-]{16,}\b|\bAKIA[A-Z0-9]{16}\b|"
    r"\bAIza[A-Za-z0-9_-]{20,}\b|\b(?:gh[pousr]_|github_pat_)[A-Za-z0-9_]{20,}\b|"
    r"\bxox[a-z]-[A-Za-z0-9-]{16,}\b|\bBearer\s+[A-Za-z0-9._~+/-]{16,}|"
    r"(?:api[_ -]?key|access[_ -]?token|secret|password)\s*[:=]\s*[\"']?[A-Za-z0-9._~+/-]{12,}|"
    r"[?&](?:api[_-]?key|access[_-]?token|token|signature|sig|key)=[^&\s\"']{8,}|"
    r"\beyJ[A-Za-z0-9_-]{16,}\.[A-Za-z0-9_-]{8,}\.[A-Za-z0-9_-]{8,}\b|"
    r"-----BEGIN [A-Z ]*PRIVATE KEY-----", re.I)
_PRIVATE_FIELDS = {"authorization", "apikey", "password", "passwd", "accesstoken", "refreshtoken",
                   "clientsecret", "privatekey", "cookie", "setcookie", "secret", "credentials"}


def _private_payload(value, public_env_names=()):
    """Reject sensitive capture before transport; never change business data."""
    extra = os.environ.get("EVOLUTION_SECRET_ENV_NAMES", "").split(",")
    secrets = tuple(v for k, v in os.environ.items()
                    if len(v) >= 8 and (k in extra or (k not in public_env_names and _SECRET_NAME.search(k))))
    pending = [value]
    visited = 0
    while pending:
        item = pending.pop()
        visited += 1
        if visited > 100000:
            return True
        if isinstance(item, dict):
            for key, child in item.items():
                if not isinstance(key, str) or key.lower().replace("-", "").replace("_", "") in _PRIVATE_FIELDS:
                    return True
                pending.extend((key, child))
        elif isinstance(item, (list, tuple)):
            pending.extend(item)
        elif isinstance(item, str):
            if _CREDENTIAL.search(item) or any(secret in item for secret in secrets):
                return True
    return False


class _LocalPoints(Evolution):
    public_env_names = ()

    def point(self, fields, **options):
        try:
            private = _private_payload(fields, self.public_env_names)
        except Exception:
            private = True
        if not private:
            return super().point(fields, **options)
        # Only a content-free gap/abort marker may cross the local boundary.
        # The original input and exception remain solely with the business call.
        fields = fields if isinstance(fields, dict) else {}
        ref = fields.get("ref", {})
        if isinstance(ref, dict) and ref.get("run") and not _private_payload(ref, self.public_env_names):
            if fields.get("phase") in ("after", "abort"):
                marker = {"phase": "abort", "kind": fields.get("kind", "custom"),
                          "ref": ref, "output": None, "status": "unknown", "capture_gap": True}
            else:
                marker = {"phase": "gap", "kind": "run", "ref": {"run": ref["run"]}}
            super().point(marker, **options)
        return self._point_observed(fields, {
            "input": fields.get("input"), "decision": "continue" if fields.get("kind") == "loop" else "allow",
            "degraded": True, "capture_complete": False, "policy_applied": False, "reason": "privacy_rejected"})


class ConfiguredPoints:
    def __init__(self):
        self._path = None
        self._clients = {}
        self._config = {}
        self._lock = threading.Lock()

    def _client(self, user_id):
        path = os.environ.get("EVOLUTION_POINT_CONFIG", "")
        trial_path = os.environ.get("TRAJECTORY_EVOLUTION_TRIAL_CONFIG", "")
        if trial_path and not (os.environ.get("TRAJECTORY_EVOLUTION_TRIAL_URL")
                               and os.environ.get("TRAJECTORY_EVOLUTION_TRIAL_TOKEN")):
            # This facade is HTTP-only. A native trial must never fall through
            # to a production collector; use the native SDK for native trials.
            return None
        if not path and not trial_path:
            return None
        try:
            with self._lock:
                if self._path != (path, trial_path):
                    self._path = (path, trial_path)
                    self._clients = {}
                    self._config = {}
                    with Path(path or trial_path).open(encoding="utf-8") as source:
                        raw = source.read(65537)
                    if len(raw) > 65536:
                        return None
                    config = json.loads(raw)
                    public_names = config.get("privacy", {}).get("public_env_names", [])
                    if not isinstance(public_names, list) or len(public_names) > 64 or any(
                            not isinstance(name, str) or not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]{0,127}", name)
                            for name in public_names):
                        return None
                    if trial_path:
                        with Path(trial_path).open(encoding="utf-8") as source:
                            trial_raw = source.read(65537)
                        if len(trial_raw) > 65536:
                            return None
                        config["principals"] = json.loads(trial_raw)["principals"]
                        config["url"] = os.environ["TRAJECTORY_EVOLUTION_TRIAL_URL"]
                    endpoint = config.get("url", "http://127.0.0.1:" + str(config.get("port", 8766)))
                    # Implicit application wiring uses a local collector. Remote
                    # deployments configure the explicit HTTP client instead.
                    if urlsplit(endpoint).hostname not in ("localhost", "127.0.0.1", "::1"):
                        return None
                    for principal in config.get("principals", []):
                        token = os.environ.get(principal["token_env"], "")
                        if token:
                            client = _LocalPoints(endpoint, token, retries=0)
                            client.public_env_names = frozenset(public_names)
                            self._clients[principal["user_id"]] = client
                    self._config = config
                if not user_id and len(self._clients) == 1:
                    return next(iter(self._clients.values()))
                return self._clients.get(user_id)
        except Exception:
            return None

    def point(self, fields, *, user_id=""):
        client = self._client(user_id)
        if client is None:
            return {"input": fields.get("input"), "decision": "continue" if fields.get("kind") == "loop" else "allow",
                    "degraded": True, "capture_complete": False, "enabled": False}
        return client.point(fields, timeout_ms=self._config.get("point_timeout_ms", 20))

    def call(self, fields, execute, *, user_id="", result_format="json"):
        client = self._client(user_id)
        if client is None:
            return execute(fields.get("input"))
        fields = dict(fields)
        if "ref" in fields and "key" not in fields["ref"]:
            fields["ref"] = dict(fields["ref"], key=uuid.uuid4().hex)
        return client.call(fields, execute, result_format=result_format,
                           timeout_ms=self._config.get("point_timeout_ms", 20))

    def allow_loop(self, fields, *, user_id=""):
        client = self._client(user_id)
        return client is None or client.allow_loop(fields, timeout_ms=self._config.get("point_timeout_ms", 20))


points = ConfiguredPoints()
