"""Trace data types — 镜像 C++ ``trace_types.h``。

保持与 C++ 侧字段名/枚举顺序一致，viewer 合并两侧 NDJSON 时可统一解析。
"""

from __future__ import annotations

import time
from dataclasses import dataclass, field
from typing import Any


# ── SpanKind — 镜像 C++ SpanKind（含新增 SKILL/TOOL/LLM/COORDINATOR）──

class SpanKind:
    REQUEST = "REQUEST"          # 端到端请求（daemon 入口）
    AGENT = "AGENT"              # MMAgent 实例
    WORKFLOW = "WORKFLOW"        # MMWorkflow 执行
    NODE = "NODE"                # MMNode 执行（C++ process_node）
    EDGE = "EDGE"                # edge traversal（预留）
    COORDINATOR = "COORDINATOR"  # Python coordinator_decide（新增，Python 侧独有）
    SKILL = "SKILL"              # skill 执行
    TOOL = "TOOL"                # 工具调用
    LLM = "LLM"                  # LLM 调用（两侧对等：c++_worker_proxy / python_direct）
    CUSTOM = "CUSTOM"


def span_kind_name(k: str) -> str:
    return k if k else "CUSTOM"


# ── SpanStatus — 镜像 C++ SpanStatus ──

class SpanStatus:
    RUNNING = "RUNNING"
    OK = "OK"
    ERROR = "ERROR"
    CANCELLED = "CANCELLED"


def span_status_name(s: str) -> str:
    return s if s else "UNKNOWN"


# ── Timestamp helpers（微秒 UTC epoch，对齐 C++ now_us）──

def now_us() -> int:
    """当前时间微秒 UTC epoch（与 C++ trace_types.h now_us 同构）。"""
    return time.time_ns() // 1_000


# ── SpanEvent — span 内瞬时事件（镜像 C++ SpanEvent）──

@dataclass
class SpanEvent:
    timestamp_us: int = 0
    name: str = ""
    attributes: dict[str, str] = field(default_factory=dict)

    def to_dict(self) -> dict[str, Any]:
        return {
            "timestamp_us": self.timestamp_us,
            "name": self.name,
            "attributes": self.attributes,
        }


# ── TraceSpan — 一个执行单元（镜像 C++ TraceSpan）──

@dataclass
class TraceSpan:
    trace_id: str = ""
    span_id: str = ""
    parent_span_id: str = ""     # 空 → 根
    kind: str = SpanKind.CUSTOM
    name: str = ""
    thread_id: int = 0

    # Timing（微秒 UTC epoch）
    start_us: int = 0
    end_us: int = 0

    @property
    def duration_us(self) -> int:
        return self.end_us - self.start_us if self.end_us > 0 else 0

    status: str = SpanStatus.RUNNING
    error_msg: str = ""

    # Tags（可检索键值对）
    tags: dict[str, str] = field(default_factory=dict)

    # Events within this span
    events: list[SpanEvent] = field(default_factory=list)

    # Resource usage snapshot（placeholder，与 C++ 侧字段对齐）
    cpu_percent: float = 0.0
    rss_kb: int = 0

    def to_dict(self) -> dict[str, Any]:
        """序列化为 NDJSON 行格式（与 C++ 侧一行一 span 格式一致）。"""
        d: dict[str, Any] = {
            "trace_id": self.trace_id,
            "span_id": self.span_id,
            "parent_span_id": self.parent_span_id,
            "kind": self.kind,
            "name": self.name,
            "start_us": self.start_us,
            "end_us": self.end_us,
            "duration_us": self.duration_us,
            "status": self.status,
        }
        if self.error_msg:
            d["error_msg"] = self.error_msg
        if self.tags:
            d["tags"] = self.tags
        if self.events:
            d["events"] = [e.to_dict() for e in self.events]
        if self.thread_id:
            d["thread_id"] = self.thread_id
        if self.cpu_percent or self.rss_kb:
            d["resource"] = {"cpu_percent": self.cpu_percent, "rss_kb": self.rss_kb}
        return d


def generate_span_id() -> str:
    """生成短唯 span_id（仿 C++ generate_id，用纳秒时间戳 + 随机后缀）。"""
    import secrets
    return f"sp_{now_us()}_{secrets.token_hex(4)}"
