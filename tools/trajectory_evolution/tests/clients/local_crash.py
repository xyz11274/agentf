"""Crash fixture: save the start marker, then exit without a shutdown flush."""
import os
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "clients/python"))
from evolution_local import LocalEvolution

client = LocalEvolution(sys.argv[1], sys.argv[2], os.environ["INTEGRATION_TEST_A"], background=False)
result = client.invoke("run.begin", {"input": "producer will be interrupted", "level": "L0"})
assert not result["degraded"]
client.drain()
os._exit(0)
