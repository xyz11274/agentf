"""Regression fixture: ordinary sync/async receivers with package entry binding.

This deterministic test is not a real-Agent benchmark or an experiment driver.
"""
import asyncio
import ctypes
import json
import os
from pathlib import Path
import subprocess
import sys
import time

SOURCE = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(SOURCE / "clients/python"))
from evolution import Evolution
from evolution_entry import evaluate, aevaluate

client = Evolution("https://deployment.invalid", "deployment-placeholder-credential", retries=0)
if "--native" in sys.argv:
    from evolution_local import LocalEvolution
    native_client = LocalEvolution(sys.argv[sys.argv.index("--native") + 1], "unused.json", "deployment-placeholder-credential")
    class NativePoints:
        def point(self, fields, **_):
            return native_client.point(fields)
    client = NativePoints()


def receive(message):
    assert set(message) == {"text", "user", "conversation", "key", "turn", "initial", "history"}
    turn = message["turn"]
    assert len(message["history"]) == turn * 2 + 1
    if turn:
        assert message["history"][-2]["role"] == "assistant"
        assert message["history"][-2]["content"] == "20"
    if "--error" in sys.argv:
        raise ValueError(os.environ.get("EVAL_TEST_API_KEY", "secret"))
    if "--no-run" in sys.argv:
        return "accepted"
    ref = {"run": message["key"]}
    if turn and "--late-previous-failure" in sys.argv:
        previous = {"run": message["key"].rsplit("-", 1)[0] + "-" + str(turn - 1)}
        client.point(dict(phase="after", kind="run", ref=previous,
                          output={"api_key": "never-store-this-value"}), timeout_ms=5000)
    if turn and "--rejected-start" in sys.argv:
        client.point(dict(phase="before", kind="run", ref=ref,
                          input={"api_key": os.environ["EVAL_TEST_API_KEY"]}), timeout_ms=5000)
        return "accepted"
    if turn and "--missing-start-gap" in sys.argv:
        client.point(dict(phase="gap", kind="run", ref=ref), timeout_ms=5000)
        return "accepted"
    run = client.point(dict(phase="before", kind="run", ref=ref,
                           conversation_id=message["conversation"], input=message["text"]), timeout_ms=5000)
    assert not run.get("degraded"), run
    if turn and "--open-gap" in sys.argv:
        client.point(dict(phase="gap", kind="run", ref=ref), timeout_ms=5000)
        return "accepted"
    if turn and "--rejected-child" in sys.argv:
        client.point(dict(phase="before", kind="model", ref=dict(ref, key="rejected"),
                          input={"api_key": os.environ["EVAL_TEST_API_KEY"]}), timeout_ms=5000)
        return "accepted"
    if "--extra-run" in sys.argv:
        client.point(dict(phase="before", kind="run", ref={"run": message["key"] + "-extra"}, input="extra"), timeout_ms=5000)
    # A background worker receives the same existing message, not a whole Agent.
    if "--dispatch" in sys.argv and "--worker" not in sys.argv:
        process = subprocess.Popen([sys.executable, "-B", __file__, "--worker"], stdin=subprocess.PIPE,
                                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        process.stdin.write(json.dumps(message).encode())
        process.stdin.close()
        return "accepted"
    if "--worker" in sys.argv:
        time.sleep(0.25)
    call = client.point(dict(phase="before", kind="model", ref=dict(ref, key="model"),
                             name="test-model", input=message["text"]), timeout_ms=5000)
    client.point(dict(phase="after", kind="model", ref=dict(ref, key="model"), executed=True,
                      actual_input=call["input"], output=20, metrics={"input_tokens": 5, "output_tokens": 1}), timeout_ms=5000)
    if "--missing-end" in sys.argv:
        return "accepted"
    output = {"answer": "20", "state": {"value": message["initial"]["value"]}, "turn": turn}
    ended = client.point(dict(phase="after", kind="run", ref=ref, output=output,
                              business_status="failed" if "--business-failure" in sys.argv else "succeeded"), timeout_ms=5000)
    assert not ended.get("degraded"), ended
    return "accepted"


async def receive_async(message):
    loop = asyncio.get_running_loop()
    await asyncio.sleep(0.01)
    assert asyncio.get_running_loop() is loop
    return receive(message)


if "--worker" in sys.argv:
    # Reuse the already-started run. Duplicate run-before uses stable ref IDs.
    receive(json.load(sys.stdin))
elif "--async" in sys.argv:
    asyncio.run(aevaluate(receive_async))
elif "--cabi" in sys.argv:
    native = ctypes.CDLL(sys.argv[sys.argv.index("--cabi") + 1])
    callback_type = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_char_p, ctypes.c_void_p)
    def callback(value, _):
        try:
            receive(json.loads(value))
            return 0
        except Exception:
            return -1
    native.te_evaluate.argtypes = [callback_type, ctypes.c_void_p]
    assert native.te_evaluate(callback_type(callback), None) == 1
else:
    evaluate(receive)
if "--stay-alive" in sys.argv:
    time.sleep(30)
