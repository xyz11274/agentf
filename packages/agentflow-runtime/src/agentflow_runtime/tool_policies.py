"""工具执行混合批次调度。

对应设计文档 `tools/system_tools/2.工具执行并行优化.md` §3：
- 核心思路（§3.1）：**只读并行、写串行**——只读工具互不干扰可并行，
  写工具需保序必须串行
- 批次划分（§3.3）：连续只读归一个**并行批次**，每个写/慢工具单独成**串行批次**，
  批次间按原始顺序排列（保序）

核心原则（§2.3）：**执行可并行，但副作用（消息回灌、产物合并、文件落盘）
必须收敛回串行**——本模块只负责"按批次调度 execute_fn 拿结果"，副作用由
调用方在拿到结果后按原始顺序统一处理。
"""

from __future__ import annotations

import json
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

# bash 命令中的写/副作用标记（保守：命中即视为写）
_BASH_WRITE_MARKERS = (
    "write", "mv ", "rm ", "mkdir", "rmdir", ">", ">>",
    "generate.py", "save", "cp ",
)

# bash 命令中的只读标记
_BASH_READ_MARKERS = (
    "cat ", "ls ", "head ", "tail ", "grep ", "echo ", "weather.py",
)


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


def classify_tool(name: str, args: dict) -> str:
    """判定工具类别：'read'（可并行）| 'write'（必须串行）。

    判据（§3.2）：只看工具名与参数，不猜 prompt 语义。
    保守策略：拿不准算 write（串行）。
    """
    if name in _READ_ONLY_TOOL_NAMES:
        return "read"
    if name in _WRITE_TOOL_NAMES:
        return "write"
    if name == "bash":
        command = str(args.get("command", ""))
        if any(marker in command for marker in _BASH_WRITE_MARKERS):
            return "write"
        if any(marker in command for marker in _BASH_READ_MARKERS):
            return "read"
        return "write"  # 拿不准 → 保守串行
    return "write"  # 未知工具 → 保守串行


def build_batches(tool_calls: list) -> list:
    """分析 → 批次图：连续只读归并（可并行），写/慢各自成串行批次。

    对应设计文档 §3.3 Step 2：批次间按原始顺序排列（保序）。
    示例 [读,读,读,写,写,读,读] → [[1,2,3], [4], [5], [6,7]]
    """
    batches: list[list] = []
    current: list = []
    for tc in tool_calls:
        if classify_tool(_tool_name(tc), _tool_args(tc)) == "read":
            current.append(tc)
        else:
            if current:
                batches.append(current)
                current = []
            batches.append([tc])
    if current:
        batches.append(current)
    return batches


# ════════════════════════════════════════════════════════════════════
# batch 执行（§3.3 Step 3）：批次间串行、批次内并行
# ════════════════════════════════════════════════════════════════════


def _max_workers(batch: list) -> int:
    """并发上限 2-4（LLM 单批工具数通常 ≤4，超过无意义）。"""
    return max(1, min(4, len(batch)))


def execute_in_batches(tool_calls: list, execute_fn: Callable) -> list:
    """按混合批次调度执行一批工具调用，返回结果列表（与 tool_calls 保序一一对应）。

    - 批次间**按原始顺序串行**（写操作可能依赖前序只读结果，保守保序）
    - 批次内**并行**（只读互不干扰）；串行批次（单个写/慢工具）逐个执行
    - execute_fn(tc) 是"执行单个工具、返回原始结果"的注入函数；
      消息回灌、产物合并等副作用由调用方按结果顺序统一处理（§2.3）
    """
    results: list = []
    for batch in build_batches(tool_calls):
        if len(batch) == 1:
            results.append(execute_fn(batch[0]))  # 串行批次：逐个执行
        else:
            with ThreadPoolExecutor(max_workers=_max_workers(batch)) as pool:
                results.extend(pool.map(execute_fn, batch))  # 并行批次 + pool.map 保序
    return results
