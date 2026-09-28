"""Business tools use this API client when a backend is configured."""
import json
import os
import urllib.error
import urllib.parse
import urllib.request

OPENER = urllib.request.build_opener(urllib.request.ProxyHandler({}))


def call(skill, args):
    from .task_context import worker_task
    try:
        base = os.environ["BUSINESS_BACKEND_URL"].rstrip("/")
        parsed = urllib.parse.urlsplit(base)
        if parsed.scheme != "http" or parsed.hostname not in {"127.0.0.1", "localhost", "::1"}:
            raise ValueError("BUSINESS_BACKEND_URL must point to a loopback HTTP service")
        user, client = os.environ.get("AF_USER_ID"), os.environ.get("AF_CLIENT_ID")
        if not user or not client:
            raise ValueError("AF_USER_ID and AF_CLIENT_ID are required")
        req = urllib.request.Request(base + "/v1/services/" + skill,
            data=json.dumps(args, ensure_ascii=False).encode(),
            headers={"Content-Type": "application/json", "X-Business-User": urllib.parse.quote(user),
                     "X-Business-Client": urllib.parse.quote(client),
                     "X-Business-Task": urllib.parse.quote(worker_task(), safe="")}, method="POST")
        with OPENER.open(req, timeout=130) as response:
            return json.loads(response.read())
    except urllib.error.HTTPError as exc:
        try:
            return {**json.loads(exc.read()), "transport": "http"}
        except ValueError:
            return {"status": "failed", "error": "backend_http_error", "UserOutputText": f"HTTP {exc.code}", "transport": "http"}
    except (OSError, ValueError, KeyError) as exc:
        return {"status": "failed", "error": "backend_unavailable", "UserOutputText": str(exc),
                "retryable": True, "transport": "http"}
