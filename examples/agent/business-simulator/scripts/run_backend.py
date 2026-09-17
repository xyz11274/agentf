#!/usr/bin/env python3
"""Run just the business APIs, independently of the Agent or model."""
import argparse
from pathlib import Path
import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "harness"))
from business_sim.backend import BusinessServer

p = argparse.ArgumentParser(description=__doc__)
p.add_argument("--port", type=int, default=17894)
p.add_argument("--database", type=Path)
a = p.parse_args()
server = BusinessServer(("127.0.0.1", a.port), a.database)
print(f"Business APIs: {server.url}", flush=True)
try:
    import time
    server.start()
    while True:
        time.sleep(1)
except KeyboardInterrupt:
    pass
finally:
    server.close()
