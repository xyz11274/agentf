"""Small HTTP boundary for the stateful business backends used by dataset tests."""
from __future__ import annotations

from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
from pathlib import Path
import threading
import urllib.parse

from .state import Store, scope_key


class BusinessServer(ThreadingHTTPServer):
    daemon_threads = True

    def __init__(self, address=("127.0.0.1", 17894), database: Path | None = None):
        self.store = Store(database)
        from .reminders import ReminderWorker
        self.reminders = ReminderWorker(self.store)
        from .device_world import WorldWorker
        self.world = WorldWorker(self.store)
        super().__init__(address, Handler)

    @property
    def url(self):
        return f"http://127.0.0.1:{self.server_port}"

    def start(self):
        self.reminders.start()
        self.world.start()
        self.thread = threading.Thread(target=self.serve_forever, daemon=True)
        self.thread.start()
        return self

    def close(self):
        from .browser import close as close_browsers
        close_browsers(self.store)
        self.reminders.close()
        self.world.close()
        self.shutdown()
        self.server_close()
        self.thread.join(timeout=5)


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *_args):
        pass

    def reply(self, code, data):
        body = json.dumps(data, ensure_ascii=False).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def scope(self):
        return scope_key(urllib.parse.unquote(self.headers.get("X-Business-User", "")),
                         urllib.parse.unquote(self.headers.get("X-Business-Client", "")))

    def do_GET(self):
        try:
            if self.path == "/health":
                return self.reply(200, {"status": "ok", "service": "business-simulator", "database": str(self.server.store.path.resolve()), "world_worker_running": self.server.world.thread.is_alive(), "world_worker_error": self.server.world.last_error})
            scope = self.scope()
            if self.path == "/v1/state":
                data = self.server.store.read(scope)
            elif self.path == "/v1/events":
                data = {"events": self.server.store.events(scope)}
            elif self.path == "/v1/devices":
                from .services import dispatch
                data = dispatch("smart-home", {"action": "query"}, store=self.server.store, scope=scope)["UserOutputObject"]
            elif self.path == "/v1/notifications":
                from .reminders import notifications, running
                data = {"notifications": notifications(self.server.store, scope), "scheduler_running": running(self.server.store)}
            elif self.path == "/v1/capabilities":
                from .real_services import capabilities
                data = {"capabilities": capabilities()}
            elif self.path == "/v1/contracts":
                from .contracts import SERVICES, contract
                data = {skill: {"tool": contract(skill)[0], "description": contract(skill)[1], "parameters": contract(skill)[2]} for skill in SERVICES}
            else:
                return self.reply(404, {"status": "failed", "error": "endpoint_not_found"})
            self.reply(200, data)
        except ValueError as exc:
            self.reply(400, {"status": "failed", "error": "invalid_identity", "UserOutputText": str(exc)})

    def do_POST(self):
        try:
            scope = self.scope()
            if self.path.startswith("/v1/devices/") and self.path.endswith("/connection"):
                from .device_world import set_online
                length = int(self.headers.get("Content-Length", "0"))
                if not 0 < length <= 1024:
                    return self.reply(413, {"status": "failed", "error": "invalid_body_size"})
                body = json.loads(self.rfile.read(length))
                if not isinstance(body, dict) or set(body) != {"online"}:
                    raise ValueError('Expected {"online": boolean}')
                return self.reply(200, set_online(self.server.store, scope, self.path.split("/")[3], body["online"]))
            from .contracts import SERVICES
            skill = self.path.removeprefix("/v1/services/")
            if not self.path.startswith("/v1/services/") or skill not in SERVICES:
                return self.reply(404, {"status": "failed", "error": "service_not_found"})
            length = int(self.headers.get("Content-Length", "0"))
            if not 0 < length <= 1024 * 1024:
                return self.reply(413, {"status": "failed", "error": "invalid_body_size"})
            args = json.loads(self.rfile.read(length))
            from .services import dispatch
            result = dispatch(skill, args, store=self.server.store, scope=scope)
            self.reply(200, {**result, "transport": "http"})
        except (ValueError, TypeError) as exc:
            self.reply(400, {"status": "failed", "error": "invalid_request", "UserOutputText": str(exc)})
        except Exception as exc:
            from agentflow_runtime.security import redact_secrets
            self.reply(500, {"status": "failed", "error": "backend_error", "UserOutputText": redact_secrets(str(exc))[0]})
