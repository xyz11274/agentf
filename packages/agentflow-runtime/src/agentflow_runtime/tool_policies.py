"""工具执行混合批次调度。

对应设计文档 `tools/system_tools/2.工具执行并行优化.md` §3：
- 核心思路（§3.1）：**只读并行、写串行**——只读工具互不干扰可并行，
  写工具需保序必须串行
- 批次划分（§3.3）：连续只读归一个**并行批次**，每个写/慢工具单独成**串行批次**，
  批次间按原始顺序排列（保序）
- 可并行独立写（2026-08-16 扩展）：**写新文件且互不覆盖**的生成类命令
  （命令显式指定 `--output` 新文件路径）相互无依赖，归为 `parallel-write`
  同样可并行——与 write_file 覆盖语义不同，不破坏写保序

核心原则（§2.3）：**执行可并行，但副作用（消息回灌、产物合并、文件落盘）
必须收敛回串行**——本模块只负责"按批次调度 execute_fn 拿结果"，副作用由
调用方在拿到结果后按原始顺序统一处理。

**框架层通用性（2026-08-16 用户指正）**：本模块不感知任何业务脚本名
（如天气脚本、图像生成脚本等），只按命令结构特征（`--output` 显式指定、
写/副作用标记）判定类别——业务命令以"生成新文件"形态出现即可被正确并行。
"""

from __future__ import annotations

import json
import posixpath
import shlex
import signal
import sys
import threading
import time
from concurrent.futures import ThreadPoolExecutor
from typing import Callable

# ════════════════════════════════════════════════════════════════════
# 工具类别判定（§3.2 判据：只看参数，不猜 prompt 语义）
# ════════════════════════════════════════════════════════════════════

# 显式只读工具名（白名单）
_READ_ONLY_TOOL_NAMES = frozenset({
    "read_file", "list_skills", "load_skill", "list_directory",
})

# 显式写工具名
_WRITE_TOOL_NAMES = frozenset({
    "write_file", "edit_file",
})

# bash 命令中的写/副作用标记（保守：命中即视为写，串行）
_BASH_WRITE_MARKERS = (
    "write", "mv ", "rm ", "mkdir", "rmdir", ">", ">>",
    "save", "cp ", "touch ", "chmod ", "chown ", "tee ",
    "wget ", "curl ", "sed -i", "install ", "truncate ", "ln ",
)

# bash 命令中的只读标记（仅通用 shell 只读命令；不感知业务脚本名——
# 框架层不得硬编码业务脚本名，无法从命令行判断脚本行为的
# 一律按"拿不准 → 保守串行 write"处理，2026-08-16 用户指正）
_BASH_READ_MARKERS = (
    "cat ", "ls ", "head ", "tail ", "grep ", "echo ",
)

# 与可并行独立写互斥的写/副作用标记（命中即退化为保守串行 write）
# 与 _BASH_WRITE_MARKERS 保持同源（P3：避免两处列表静默漂移——新增写标记若只加
# 其一，会让带该副作用的生成命令被误判可并行）
_PARALLEL_WRITE_EXCLUDE_MARKERS = _BASH_WRITE_MARKERS


def _tool_name(tc: dict) -> str:
    return tc.get("function", {}).get("name", "")


def _tool_args(tc: dict) -> dict:
    raw = tc.get("function", {}).get("arguments", "{}")
    if isinstance(raw, dict):
        return raw
    try:
        return json.loads(raw)
    except (json.JSONDecodeError, TypeError):
        return {}


def _output_path(command: str) -> str | None:
    """提取 bash 命令中的 --output 路径（parallel-write 互不覆盖校验用）。

    仅识别恰好一个显式 `--output <path>`（可带引号）。复合命令或多个
    输出无法证明副作用独立，返回 None 走保守串行。
    """
    if any(marker in command for marker in _BASH_COMPOUND_MARKERS):
        return None
    try:
        argv = shlex.split(command, posix=True)
    except ValueError:
        return None
    outputs = []
    for index, token in enumerate(argv):
        if token == "--output":
            if index + 1 >= len(argv) or argv[index + 1].startswith("-"):
                return None
            outputs.append(argv[index + 1])
        elif token.startswith("--output="):
            outputs.append(token.partition("=")[2])
    if len(outputs) != 1:
        return None
    path = posixpath.normpath(outputs[0].rstrip("/\\"))
    return path if path not in {"", ".", ".."} else None


def classify_tool(name: str, args: dict) -> str:
    """判定工具类别：'read'（可并行）| 'parallel-write'（可并行独立写）| 'write'（必须串行）。

    判据（§3.2）：只看工具名与参数，不猜 prompt 语义；不感知业务脚本名。
    - 'read'：只读，互不干扰 → 可并行（连续归并批次）
    - 'parallel-write'：写**新文件且互不覆盖**的生成类命令（显式指定 `--output`
      新文件路径）→ 可并行；与 write_file 覆盖语义不同，不破坏写保序。
      **必须显式指定 --output 且无互斥写/副作用标记**，否则退化串行 write
      （P2：未指定 output = 落点不确定，不可并行）。
    - 'write'：覆盖/删除/重定向等 → 必须串行；拿不准也算 write（保守串行）
    """
    if name in _READ_ONLY_TOOL_NAMES:
        return "read"
    if name in _WRITE_TOOL_NAMES:
        return "write"
    if name == "bash":
        command = str(args.get("command", ""))
        if any(marker in command for marker in _BASH_COMPOUND_MARKERS):
            return "write"
        # 可并行独立写：命令显式指定 --output 新文件路径（结构特征，不感知业务脚本名）
        # 且无互斥写/副作用标记 → 可并行。框架层按"命令结构"判定，不硬编码业务脚本名
        # ——任何"生成新文件"形态的命令（--output 指定）都适用
        #（2026-08-16 用户指正：框架层不得混入业务相关）。
        if (
            _output_path(command) is not None
            and not any(m in command for m in _PARALLEL_WRITE_EXCLUDE_MARKERS)
        ):
            return "parallel-write"
        if any(marker in command for marker in _BASH_WRITE_MARKERS):
            return "write"
        if any(marker in command for marker in _BASH_READ_MARKERS):
            return "read"
        return "write"  # 拿不准 → 保守串行
    return "write"  # 未知工具 → 保守串行


def build_batches(tool_calls: list) -> list:
    """分析 → 批次图：连续同类归并（可并行），写各自成串行批次，异类断批。

    对应设计文档 §3.3 Step 2：批次间按原始顺序排列（保序）。
    - read 只与连续 read 归并；parallel-write 只与连续 parallel-write 归并
    - **read 与 parallel-write 不互相合并**（P2：generate 后紧跟的 read 若合批会
      在写完成前读到旧/缺失文件——read-after-write 竞态，必须断批保序）
    - write（覆盖/删除/重定向等）各自单独串行批次
    示例 [读,读,写,写,读,读] → [[1,2], [3], [4], [5,6]]
    示例 [写,写,写,写] → [[1,2], [3], [4]]（两个 parallel-write 并行、write 串行）
    示例 [写,读] → [[1], [2]]（parallel-write 后 read 断批，防 read-after-write）
    """
    batches: list[list] = []
    current: list = []
    current_kind: str | None = None  # 当前归并批次的类别（read / parallel-write）
    current_outputs: set[str] = set()
    for tc in tool_calls:
        kind = classify_tool(_tool_name(tc), _tool_args(tc))
        if kind == "write":
            if current:
                batches.append(current)
                current = []
                current_kind = None
                current_outputs.clear()
            batches.append([tc])  # 写/未知 → 单独串行批次
            continue
        # read / parallel-write：同类连续归并，异类断批（P2 read-after-write 保序）
        output = None
        if kind == "parallel-write":
            output = _output_path(str(_tool_args(tc).get("command", "")))
        if current and current_kind == kind:
            if kind == "parallel-write" and output in current_outputs:
                batches.append(current)
                current = [tc]
                current_outputs = {output} if output else set()
            else:
                current.append(tc)
                if output:
                    current_outputs.add(output)
        else:
            if current:
                batches.append(current)
            current = [tc]
            current_kind = kind
            current_outputs = set()
            if kind == "parallel-write" and output:
                current_outputs.add(output)
    if current:
        batches.append(current)
    return batches


# ════════════════════════════════════════════════════════════════════
# batch 执行（§3.3 Step 3）：批次间串行、批次内并行
# ════════════════════════════════════════════════════════════════════


def _max_workers(batch: list) -> int:
    """并发上限 2-4（LLM 单批工具数通常 ≤4，超过无意义）。"""
    return max(1, min(4, len(batch)))


class _ToolDeadlineExpired(BaseException):
    """Bypass broad tool ``except Exception`` handlers and unwind to scheduler."""


def _timeout_result(tool_call, timeout_fn: Callable | None):
    if timeout_fn is None:
        raise TimeoutError("tool execution exceeded its deadline")
    return timeout_fn(tool_call)


def _execute_serial_with_deadline(
    tool_call,
    execute_fn: Callable,
    deadline_monotonic: float | None,
    timeout_fn: Callable | None,
):
    """Run a serial/unknown tool with a real POSIX deadline.

    Harness workers invoke this scheduler on their main thread. SIGALRM is
    intentional here: returning early from a Future cannot stop its worker,
    while raising a BaseException unwinds provider, subprocess and network
    calls that commonly catch ``Exception`` themselves.
    """
    if deadline_monotonic is None:
        return execute_fn(tool_call)
    remaining = deadline_monotonic - time.monotonic()
    if remaining <= 0:
        return _timeout_result(tool_call, timeout_fn)
    if (
        threading.current_thread() is not threading.main_thread()
        or not hasattr(signal, "setitimer")
    ):
        # The production Skill worker is a Linux main-thread process. Do not
        # install a process signal handler from an unsupported caller context.
        return execute_fn(tool_call)

    previous_handler = signal.getsignal(signal.SIGALRM)
    previous_timer = signal.getitimer(signal.ITIMER_REAL)

    def _raise_deadline(_signum, _frame):
        raise _ToolDeadlineExpired()

    started = time.monotonic()
    signal.signal(signal.SIGALRM, _raise_deadline)
    signal.setitimer(signal.ITIMER_REAL, remaining)
    try:
        return execute_fn(tool_call)
    except _ToolDeadlineExpired:
        return _timeout_result(tool_call, timeout_fn)
    finally:
        signal.setitimer(signal.ITIMER_REAL, 0)
        signal.signal(signal.SIGALRM, previous_handler)
        if previous_timer[0] > 0:
            elapsed = time.monotonic() - started
            restored_delay = max(0.000001, previous_timer[0] - elapsed)
            signal.setitimer(
                signal.ITIMER_REAL, restored_delay, previous_timer[1]
            )


def execute_in_batches(
    tool_calls: list,
    execute_fn: Callable,
    *,
    deadline_monotonic: float | None = None,
    timeout_fn: Callable | None = None,
) -> list:
    """按混合批次调度执行一批工具调用，返回结果列表（与 tool_calls 保序一一对应）。

    - 批次间**按原始顺序串行**（写操作可能依赖前序只读结果，保守保序）
    - 批次内**并行**（只读互不干扰）；串行批次（单个写/慢工具）逐个执行
    - execute_fn(tc) 是"执行单个工具、返回原始结果"的注入函数；
      消息回灌、产物合并等副作用由调用方按结果顺序统一处理（§2.3）
    - 可选 deadline 只改变显式传入它的 Skill 调用；Coordinator 保持原行为。
      当前 harness 中业务/未知工具均为串行批次，因此可被真实中断；并行批次
      仅包含白名单只读工具或自身带 timeout 的 bash 调用。
    """
    _t0 = time.monotonic()
    results: list = []
    batches = build_batches(tool_calls)
    for batch_index, batch in enumerate(batches):
        if (
            deadline_monotonic is not None
            and time.monotonic() >= deadline_monotonic
        ):
            for remaining_batch in batches[batch_index:]:
                results.extend(
                    _timeout_result(tc, timeout_fn) for tc in remaining_batch
                )
            break
        if len(batch) == 1:
            results.append(_execute_serial_with_deadline(
                batch[0], execute_fn, deadline_monotonic, timeout_fn
            ))
        else:
            with ThreadPoolExecutor(max_workers=_max_workers(batch)) as pool:
                results.extend(pool.map(execute_fn, batch))  # 并行批次 + pool.map 保序
    _ms = (time.monotonic() - _t0) * 1000
    _parallel = sum(1 for b in build_batches(tool_calls) if len(b) > 1)
    print(f"[timing] tool_batch parallel={_parallel} duration_ms={_ms:.1f}", file=sys.stderr)
    return results
_BASH_COMPOUND_MARKERS = ("&&", "||", ";", "|")
