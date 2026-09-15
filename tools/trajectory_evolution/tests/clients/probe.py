"""Test driver, not part of the host's application. One durable RPC per line."""
import json
import os
from pathlib import Path
import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "clients/python"))
from evolution import Evolution, EvolutionError

client = Evolution(os.environ["EVOLUTION_URL"], os.environ["EVOLUTION_TEST_TOKEN"], retries=0)
for line in sys.stdin:
    request = json.loads(line)
    try:
        result = client.invoke(request.pop("operation"), request, request.pop("request_id"))
        print(json.dumps({"ok": True, "result": result}, ensure_ascii=False), flush=True)
    except EvolutionError as error:
        print(json.dumps({"ok": False, "error": str(error), "status": error.status}), flush=True)
