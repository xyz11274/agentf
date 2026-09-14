"""Bind an existing task receiver; active only inside a component-owned trial."""
import asyncio
import inspect
import os
import time
import uuid


class _Entry:
    def __init__(self):
        from evolution import Evolution
        url = os.environ.get("TRAJECTORY_EVOLUTION_TRIAL_URL", "")
        token = os.environ.get("TRAJECTORY_EVOLUTION_TRIAL_TOKEN", "")
        self.client = Evolution(url, token, timeout_ms=5000, retries=0)
        self.consumer = uuid.uuid4().hex

    def next(self, index):
        return self.client.invoke("evaluation.next", {"consumer": self.consumer, "index": index})

    def fail(self):
        try:
            self.client.invoke("evaluation.fail", {"consumer": self.consumer})
        except Exception:
            pass


def evaluate(receive):
    """Call an existing synchronous receiver once per turn. Return False online.

    The callback accepts the configured JSON value; its return value is only an
    acknowledgement. Real run-end points supply the answer and unlock the next turn.
    """
    if os.environ.get("TRAJECTORY_EVOLUTION_EVALUATION_ENTRY") != "1":
        return False
    entry = _Entry()
    index = 0
    try:
        while True:
            packet = entry.next(index)
            state = packet.get("state")
            if state == "done":
                return True
            if state == "waiting":
                time.sleep(0.1)
                continue
            if state != "input" or packet.get("index") != index:
                raise RuntimeError("evaluation_entry_response_invalid")
            result = receive(packet["input"])
            if inspect.isawaitable(result):
                if inspect.iscoroutine(result):
                    result.close()
                raise RuntimeError("evaluation_entry_async_receiver_requires_aevaluate")
            index += 1
    except BaseException:
        entry.fail()
        # Never print provider errors, credentials or business exception text.
        raise RuntimeError("evaluation_entry_failed") from None


async def aevaluate(receive):
    """Run the existing receiver on the caller's event loop, never a new loop."""
    if os.environ.get("TRAJECTORY_EVOLUTION_EVALUATION_ENTRY") != "1":
        return False
    entry = _Entry()
    index = 0
    try:
        while True:
            packet = await asyncio.to_thread(entry.next, index)
            state = packet.get("state")
            if state == "done":
                return True
            if state == "waiting":
                await asyncio.sleep(0.1)
                continue
            if state != "input" or packet.get("index") != index:
                raise RuntimeError("evaluation_entry_response_invalid")
            result = receive(packet["input"])
            if inspect.isawaitable(result):
                await result
            index += 1
    except BaseException:
        await asyncio.to_thread(entry.fail)
        raise RuntimeError("evaluation_entry_failed") from None
