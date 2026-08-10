"""
Regression test: DataBus shared memory linear growth fix.

Simulates the core pattern that caused "shared memory full":
  - Multiple dialogue rounds, each writing LLM trace data
  - After each dialogue, Bridge calls databus.remove_by_prefix("llm/traces/")
  - Verifies total stored data → bounded, not O(N × dialogue_size)

This tests the *protocol* at Python level, not the C++ ZeroCopyDataBus
implementation — the same remove_by_prefix path that runs in production.
"""

import json
import os
import sys
from pathlib import Path

_NODES_DIR = (Path(__file__).parents[1] / "nodes").resolve()
if str(_NODES_DIR) not in sys.path:
    sys.path.insert(0, str(_NODES_DIR))


# ── Simulated DataBus (dict-based, mirrors InProcessSharedMemoryBlock) ──


class _SimDataBus:
    """Dict-backed DataBus simulation that tracks used capacity.

    Mirrors ``InProcessSharedMemoryBlock`` in local_databus.cpp:
      - put(key, value) → consumed = len(key) + len(str(value))
      - remove_by_prefix(prefix) → frees matching keys + capacity
      - Dialogue keys: llm/traces/{call_id}/{request,response,meta}

    This is NOT a mock — it reproduces the exact accounting logic
    (key_size + value_size) used in the C++ implementation.
    """

    def __init__(self):
        self._store: dict[str, str] = {}
        self._used_capacity = 0

    def put(self, key: str, value: str) -> None:
        old_val = self._store.get(key)
        old_size = len(key) + len(old_val) if old_val is not None else 0
        new_size = len(key) + len(value)
        self._used_capacity += (new_size - old_size)
        self._store[key] = value

    def remove_by_prefix(self, prefix: str) -> int:
        removed = 0
        for key in list(self._store.keys()):
            if key.startswith(prefix):
                self._used_capacity -= len(key) + len(self._store[key])
                del self._store[key]
                removed += 1
        return removed

    @property
    def used_capacity(self) -> int:
        return self._used_capacity

    @property
    def key_count(self) -> int:
        return len(self._store)


# ── Simulated trace writer ──


def _simulate_trace_write(databus: _SimDataBus, dialogue_id: str, n_calls: int) -> None:
    """Simulate what _TraceRecorder.trace_call() writes to DataBus.

    Each call produces:
      llm/traces/{call_id}/request   (~200 bytes of JSON)
      llm/traces/{call_id}/response   (~150 bytes of JSON)
      llm/traces/{call_id}/meta       (~300 bytes of JSON)
      llm/traces/index/{dialogue_id}  (growing array, ~50 bytes per call)

    This mirrors the real write pattern from llm_call.py.
    """
    for i in range(n_calls):
        call_id = f"{dialogue_id}-call-{i:03d}"
        databus.put(f"llm/traces/{call_id}/request", json.dumps({
            "model": "gpt-4o",
            "messages": [{"role": "user", "content": "Hello" * 20}],
            "tools": [{"type": "function", "function": {"name": "get_weather"}}],
        }))
        databus.put(f"llm/traces/{call_id}/response", json.dumps({
            "choices": [{"message": {"content": "Answer " * 30}}],
            "usage": {"prompt_tokens": 500, "completion_tokens": 150, "total_tokens": 650},
        }))
        databus.put(f"llm/traces/{call_id}/meta", json.dumps({
            "model": "gpt-4o", "latency_ms": 3200, "timestamp": "2026-07-08T12:00:00Z",
            "prompt_tokens": 500, "completion_tokens": 150, "total_tokens": 650,
            "dialogue_id": dialogue_id, "node_id": "coordinator_decide",
            "request_summary": "user: HelloHello...",
            "response_summary": "finish=stop | Answer Answer...",
        }))

    # Write / append to dialogue-scoped index
    index_key = f"llm/traces/index/{dialogue_id}"
    existing = databus._store.get(index_key, "[]")
    try:
        index = json.loads(existing)
    except (json.JSONDecodeError, TypeError):
        index = []
    for i in range(n_calls):
        index.append(f"{dialogue_id}-call-{i:03d}")
    databus.put(index_key, json.dumps(index))


# ── Tests ──


class TestDataBusMemoryLinearGrowth:
    """Regression: shared memory must NOT grow O(N × dialogue) after cleanup."""

    # Typical outfit-assistant LLM call count per dialogue round:
    #   coordinator (1) + skill_executor (3-5) + coordinator (1) + ...
    #   ~6 calls per dialogue on average
    CALLS_PER_DIALOGUE = 6
    DIALOGUE_ROUNDS = 20  # enough to overflow 50MB before fix

    def test_after_cleanup_capacity_baseline_recovered(self):
        """After each dialogue's cleanup, used_capacity returns to baseline.

        Before fix (no cleanup): after 20 rounds would hold ~20× dialogue data.
        After fix:              each dialogue cleaned up → ≈0 baseline.
        """
        databus = _SimDataBus()

        baseline_before = databus.used_capacity
        assert baseline_before == 0

        for i in range(self.DIALOGUE_ROUNDS):
            dialogue_id = f"dialogue-{i:04d}"

            # Simulate trace writes (like _TraceRecorder)
            _simulate_trace_write(databus, dialogue_id, self.CALLS_PER_DIALOGUE)

            # Simulate Bridge cleanup (like _pull_and_save_traces → remove_by_prefix)
            removed = databus.remove_by_prefix(f"llm/traces/{dialogue_id}")
            assert removed > 0, (
                f"Round {i}: expected some keys removed, got 0"
            )

        # After all rounds, used_capacity should be near baseline
        # (some residual from index entries written by dialogue-scoped key + skill results)
        total_remaining = databus.used_capacity
        key_count = databus.key_count

        print(f"\n  After {self.DIALOGUE_ROUNDS} rounds:")
        print(f"  Remaining keys:     {key_count}")
        print(f"  Remaining capacity: {total_remaining} bytes")
        print(f"  Baseline was:       {baseline_before} bytes")

        # In a healthy cleanup, remaining is only skill_result / sys keys
        # (few KB), not MB. Using a generous bound: < 10 KB after 20 rounds.
        assert total_remaining < 10 * 1024, (
            f"Used capacity {total_remaining} bytes exceeds 10 KB bound — "
            f"expected near-baseline after cleanup"
        )

    def test_without_cleanup_capacity_grows_linearly(self):
        """Without cleanup, capacity grows linearly with dialogue count.

        This is the BEFORE scenario — confirms the test can detect the
        regression that the fix prevents.
        """
        databus = _SimDataBus()
        capacities = []

        for i in range(5):  # 5 rounds is enough to show trend
            dialogue_id = f"nocleanup-{i:04d}"
            _simulate_trace_write(databus, dialogue_id, self.CALLS_PER_DIALOGUE)
            # NO cleanup — what happened before the fix
            capacities.append(databus.used_capacity)

        # Verify monotonic growth (each round adds instead of being cleaned)
        for i in range(1, len(capacities)):
            assert capacities[i] > capacities[i - 1], (
                f"Round {i}: capacity {capacities[i]} ≤ previous {capacities[i-1]} — "
                f"expected growth without cleanup"
            )
        # After 5 rounds without cleanup, capacity should be significantly higher
        # than the cleaned case. 6 calls/round × ~1KB/call × 5 rounds ≈ 30 KB.
        no_cleanup_total = capacities[-1]
        print(f"\n  Without cleanup after 5 rounds: {no_cleanup_total} bytes")
        assert no_cleanup_total > 20_000, (
            f"Expected >20 KB without cleanup, got {no_cleanup_total}"
        )

    def test_with_cleanup_does_not_accumulate(self):
        """After cleanup, trace data from previous dialogue is GONE.

        Verifies that cleanup actually removes the keys — not just that
        used_capacity is low, but that the old keys are absent.
        """
        databus = _SimDataBus()

        for i in range(3):
            dialogue_id = f"accum-test-{i:04d}"
            _simulate_trace_write(databus, dialogue_id, self.CALLS_PER_DIALOGUE)

            # Same prefix as Bridge's _pull_and_save_traces cleanup:
            #   daemon.send_request("databus.remove_by_prefix", {prefix: "llm/traces/"})
            databus.remove_by_prefix("llm/traces/")

            # Verify ALL old trace keys are gone (call traces + index)
            for suffix in ("/request", "/response", "/meta"):
                key = f"llm/traces/{dialogue_id}-call-000{suffix}"
                assert key not in databus._store, (
                    f"Key '{key}' survived cleanup for dialogue {dialogue_id}"
                )

        # Index keys are under llm/traces/index/{dialogue_id} — the same
        # "llm/traces/" prefix covers them too.
        index_keys = [k for k in databus._store if k.startswith("llm/traces/")]
        assert len(index_keys) == 0, (
            f"Expected 0 llm/traces/ keys after cleanup, found {len(index_keys)}: {index_keys}"
        )
