"""SpanWriter — 落盘 Python span 到 NDJSON，与 C++ trace_output/ 合并检索。

设计文档 §6.4：为什么不走 DataBus 回传给 C++ TraceManager。
"""

from __future__ import annotations

import json
import os
import threading
from pathlib import Path
from typing import Any

from .types import TraceSpan
from .span_context import SpanContext


class SpanWriter:
    """落盘 Python span 到 NDJSON（一行一 span，与 C++ 格式一致）。

    文件名：``{output_dir}/python_spans_<dialogue_id>.ndjson``
    output_dir 默认 ``${AF_TRACE_OUTPUT_DIR}`` 或 ``<project>/trace_output/python/``，
    与 C++ trace_output/ 同根，viewer 同目录扫两侧。
    """

    _instance: "SpanWriter | None" = None
    _instance_lock = threading.Lock()

    def __init__(self, output_dir: str | None = None) -> None:
        if output_dir is None:
            output_dir = os.environ.get("AF_TRACE_OUTPUT_DIR", "")
            if not output_dir:
                # 退回项目根 trace_output/python/（与 C++ trace_output/ 同根）
                # 找项目根：从本文件向上找 pyproject.toml
                project_root = self._find_project_root()
                output_dir = str(project_root / "trace_output" / "python")
            else:
                # 注入值语义 = trace 根目录（daemon 已绝对化，与 C++ task_*.ndjson 同根），
                # Python span 统一追加 /python 子目录（与兜底路径、文档 §6.4 布局一致，
                # viewer 单目录扫两侧时在 trace_output/python/ 下找到 Python span）
                output_dir = str(Path(output_dir) / "python")
        self._output_dir = Path(output_dir)
        self._file_lock = threading.Lock()

    @staticmethod
    def current() -> "SpanWriter":
        with SpanWriter._instance_lock:
            if SpanWriter._instance is None:
                SpanWriter._instance = SpanWriter()
            return SpanWriter._instance

    @staticmethod
    def _find_project_root() -> Path:
        """从本文件位置向上找仓库根标记（.git / CMakeLists.txt）。

        不能以 pyproject.toml 为标记：本包位于 packages/agentflow-runtime/，
        向上找第一个 pyproject.toml 会命中包自身目录，导致兜底目录错落为
        packages/agentflow-runtime/trace_output/python/，与 C++ trace_output/ 不同根。
        仓库根以 .git（git 仓库）或 CMakeLists.txt（agentflow 构建根）为标记。
        """
        p = Path(__file__).resolve()
        for parent in [p] + list(p.parents):
            if (parent / ".git").exists() or (parent / "CMakeLists.txt").exists():
                return parent
        # fallback：无标记时退回包根（与旧行为一致，仅异常环境触发）
        return p.parents[5] if len(p.parents) > 5 else p.parent

    def write_span(self, span: TraceSpan) -> None:
        """span 结束时写一行 NDJSON append 到文件尾。

        线程安全：文件锁 + append 模式（POSIX append 原子）。
        """
        self._output_dir.mkdir(parents=True, exist_ok=True)

        # 文件名按 dialogue_id 分片：dialogue_id 是进程级不变量（AF_DIALOGUE_ID 环境变量注入，
        # 同次对话所有 span 共享），从环境变量取而非 span.tags，避免嵌套子 span 漏设 tags 时
        # 落到 _no_dialogue 分片与父 span 不一致（F3 不变量 I-1：viewer 合并时漏分片 = span 树断链）。
        # span.tags 中的 dialogue_id 仍保留（to_dict 序列化），供单 span 元数据检索。
        dialogue_id = os.environ.get("AF_DIALOGUE_ID", "")
        if not dialogue_id:
            # 兜底：环境变量未注入时退回 span.tags（如单元测试手动构造 span 场景）
            dialogue_id = span.tags.get("dialogue_id", "_no_dialogue")
        file_path = self._output_dir / f"python_spans_{dialogue_id}.ndjson"

        line = json.dumps(span.to_dict(), ensure_ascii=False, separators=(",", ":"))

        with self._file_lock:
            with open(file_path, "a", encoding="utf-8") as f:
                f.write(line + "\n")

    def output_dir(self) -> Path:
        return self._output_dir
