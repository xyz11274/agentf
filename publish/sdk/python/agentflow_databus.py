import base64
import json
import os
import time
import urllib.parse
from pathlib import Path


class FileDataBus:
    def __init__(self, root=None):
        self.root = Path(root or os.environ["AF_DATABUS_DIR"])
        self.kv_dir = self.root / "kv"
        self.events_dir = self.root / "events"
        self.kv_dir.mkdir(parents=True, exist_ok=True)
        self.events_dir.mkdir(parents=True, exist_ok=True)
        self._seen = set()

    def _key_path(self, key):
        return self.kv_dir / f"{urllib.parse.quote(key, safe='._-')}.kv"

    def set(self, key, value, value_type="string"):
        encoded = base64.b64encode(str(value).encode()).decode()
        self._key_path(key).write_text(
            f"key={key}\ntype={value_type}\nvalue_b64={encoded}\ntimestamp_ms={int(time.time() * 1000)}\n",
            encoding="utf-8",
        )
        self.publish(key, value, value_type=value_type, kind="set")

    def get(self, key):
        path = self._key_path(key)
        if not path.exists():
            return None
        for line in path.read_text(encoding="utf-8").splitlines():
            if line.startswith("value_b64="):
                return base64.b64decode(line.split("=", 1)[1]).decode()
        return None

    def publish(self, topic, value, value_type="string", kind="pub"):
        encoded = base64.b64encode(str(value).encode()).decode()
        event = self.events_dir / f"{int(time.time() * 1000)}_{os.getpid()}.evt"
        event.write_text(
            f"topic={topic}\nkind={kind}\ntype={value_type}\nvalue_b64={encoded}\ntimestamp_ms={int(time.time() * 1000)}\n",
            encoding="utf-8",
        )

    def next_event(self, topic, timeout_ms=5000):
        deadline = time.time() + timeout_ms / 1000.0
        while time.time() <= deadline:
            for path in sorted(self.events_dir.glob("*.evt")):
                if path.name in self._seen:
                    continue
                self._seen.add(path.name)
                lines = dict(
                    line.split("=", 1)
                    for line in path.read_text(encoding="utf-8").splitlines()
                    if "=" in line
                )
                if lines.get("topic") != topic:
                    continue
                return base64.b64decode(lines.get("value_b64", "")).decode()
            time.sleep(0.05)
        return None

    def request_llm(self, request_id, prompt, channel=None):
        payload = json.dumps(
            {"request_id": request_id, "prompt": prompt, "channel": channel or f"agentflow.llm.chunk.{request_id}"}
        )
        self.publish("agentflow.llm.request", payload)
