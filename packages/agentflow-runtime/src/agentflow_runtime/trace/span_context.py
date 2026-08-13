"""SpanContext — 进程内 span 栈，仿 C++ ``TraceContext``。

Python 节点在子进程中执行，与 C++ daemon 是不同进程，无法用 C++ 线程局部
TraceContext 维护 span 树。本模块经环境变量 AF_TRACE_ID / AF_PARENT_SPAN_ID
注入根父链，进程内子 span 的父子关系靠栈自动维护。

设计文档 §6.2：跨进程 span 树机制。
"""

from __future__ import annotations

import os
import threading
from typing import Iterator


class SpanContext:
    """进程单例 span 栈（线程隔离，仿 C++ TraceContext thread_local）。

    生命周期：
    - 进程入口调 ``read_env()`` 读 AF_TRACE_ID / AF_PARENT_SPAN_ID 建立根父链
    - emit_span() 进入时 push(trace_id, span_id)，退出时 pop(span_id)
    - 进程内子 span 自动挂 parent=栈顶 span_id
    """

    _instance: "SpanContext | None" = None
    _instance_lock = threading.Lock()

    def __init__(self) -> None:
        self._trace_id: str = ""
        self._root_parent_span_id: str = ""  # 从 AF_PARENT_SPAN_ID 读入的根 parent
        # 栈：每个 Frame = (trace_id, span_id)；栈顶 = 当前 span
        self._stack: list[tuple[str, str]] = []
        self._stack_lock = threading.Lock()

    # ── 单例 ──

    @staticmethod
    def current() -> "SpanContext":
        """返回进程单例（线程安全）。"""
        with SpanContext._instance_lock:
            if SpanContext._instance is None:
                SpanContext._instance = SpanContext()
            return SpanContext._instance

    # ── 环境变量注入（进程入口调一次）──

    def read_env(self) -> None:
        """从 AF_TRACE_ID / AF_PARENT_SPAN_ID 读入根父链。

        应在 Python 节点入口最早处调用（如 observability.emit 模块 init）。
        AF_PARENT_SPAN_ID 只此处读一次，进程内子 span 的 parent 由栈自动维护。
        """
        self._trace_id = os.environ.get("AF_TRACE_ID", "")
        self._root_parent_span_id = os.environ.get("AF_PARENT_SPAN_ID", "")

    def read_env_if_stale(self) -> None:
        """幂等读环境变量：仅在 trace_id 为空时读（避免重复读覆盖已建立栈）。"""
        if not self._trace_id:
            self.read_env()

    # ── 栈操作（emit_span 调用）──

    def push(self, trace_id: str, span_id: str) -> None:
        with self._stack_lock:
            self._stack.append((trace_id, span_id))

    def pop(self, span_id: str) -> None:
        with self._stack_lock:
            if self._stack and self._stack[-1][1] == span_id:
                self._stack.pop()
            else:
                # 防御：栈顶不匹配时仍尝试移除该 span_id（避免栈泄漏）
                for i in range(len(self._stack) - 1, -1, -1):
                    if self._stack[i][1] == span_id:
                        del self._stack[i]
                        break

    # ── 状态访问器 ──

    def trace_id(self) -> str:
        """当前 trace_id（栈顶优先，退回根 trace_id）。"""
        with self._stack_lock:
            if self._stack:
                return self._stack[-1][0]
        return self._trace_id

    def current_span_id(self) -> str:
        """栈顶 span_id（无活跃 span 时返回空）。"""
        with self._stack_lock:
            if self._stack:
                return self._stack[-1][1]
        return ""

    def parent_span_id(self) -> str:
        """当前父 span_id：栈顶的父 = 栈下一层；栈空时 = 根 AF_PARENT_SPAN_ID。"""
        with self._stack_lock:
            if len(self._stack) >= 2:
                return self._stack[-2][1]
        return self._root_parent_span_id

    def root_parent_span_id(self) -> str:
        """从 AF_PARENT_SPAN_ID 读入的根 parent（不变量追溯用）。"""
        return self._root_parent_span_id

    def clear(self) -> None:
        with self._stack_lock:
            self._stack.clear()
        self._trace_id = ""
        self._root_parent_span_id = ""

    def is_active(self) -> bool:
        """是否有活跃 trace（trace_id 非空）。"""
        return bool(self.trace_id())


# ── 模块级便捷访问器（仿 C++ TraceContext::current().trace_id()）──

def current_trace_id() -> str:
    return SpanContext.current().trace_id()


def current_span_id() -> str:
    return SpanContext.current().current_span_id()


def parent_span_id() -> str:
    return SpanContext.current().parent_span_id()
