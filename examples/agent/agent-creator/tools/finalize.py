"""finalize tool — mark the agent creation as complete.

When finalized, the frontend can offer a "Save" button to persist
the in-memory file tree to disk.

Usage (LLM function calling):
  {"name": "finalize"}
  → Marks creation as complete.

Returns JSON with file count and summary.

NOTE: Uses ``import file_store`` (module ref) rather than
``from file_store import finalized`` because ``finalized`` is an
immutable (bool) — a ``from`` import would copy the value and
``global finalized`` would only rebind the local module's name,
not ``file_store.finalized``.
"""
import json

import file_store

TOOL_NAME = "finalize"
TOOL_DESCRIPTION = (
    "确认创建完成。调用此命令后，用户可以点击「保存」将生成的包写入磁盘。"
    "调用前请确保所有必要文件都已通过 add_file 添加。"
)
TOOL_PARAMS = {
    "type": "object",
    "properties": {}
}


def is_finalized() -> bool:
    return file_store.finalized


def execute(params: dict | None = None) -> str:
    file_store.finalized = True

    file_list = sorted(file_store.files.keys())
    return json.dumps({
        "ok": True,
        "message": "创建完成！所有文件已就绪。用户可以点击「保存」将包写入磁盘。",
        "file_count": len(file_list),
        "files": file_list,
    }, ensure_ascii=False)
