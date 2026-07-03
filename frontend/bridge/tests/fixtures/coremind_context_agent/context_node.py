"""Deterministic node used only by the CoreMind full-chain E2E."""

import json
import os

context = json.loads(os.environ["AF_PLATFORM_CONTEXT"])
request_id = os.environ["AF_REQUEST_ID"]
instance = context["services"]["IMInferScheduler"]["instances"][0]
print(json.dumps({
    "response": f"NODE_CONTEXT_OK:{request_id}:{instance['host']}:{instance['port']}"
}))
