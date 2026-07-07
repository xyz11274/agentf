"""read_image — 系统工具：file_url → base64 data URL

供业务 skill 按需读取图片并转为 base64，再自行拼入 LLM 调用。
遵循 S3 原则：工具提供能力，业务自己做决策。

接受 ``file://`` URL（bridge 落盘后的路径引用），解码 → data URL。
安全问题后续由全局沙箱统一处理。
"""

from __future__ import annotations

import base64
import json
import mimetypes
from pathlib import Path

TOOL_NAME = "read_image"
TOOL_DESCRIPTION = (
    "将 file:// 图片路径读取为 base64 data URL。"
    "当 LLM 需要查看图片内容时，调用此工具将图片转为 base64 格式，"
    "然后你可以在后续消息中以 data:image/...;base64,... 的格式发送给多模态模型。"
    "支持 png、jpeg、webp、gif。"
)
TOOL_PARAMS = {
    "type": "object",
    "properties": {
        "file_url": {
            "type": "string",
            "description": "file:// 格式的图片绝对路径（例如 file:///path/to/workspace/upload/abc.png）",
        },
        "max_size_kb": {
            "type": "integer",
            "description": "图片大小上限，超限拒绝（默认 10240 = 10MB）",
            "default": 10240,
        },
    },
    "required": ["file_url"],
}

# 支持的文件类型
_ALLOWED_EXT = {".png", ".jpg", ".jpeg", ".webp", ".gif"}


def execute(tool_input: dict) -> str:
    file_url: str = tool_input.get("file_url", "")
    max_kb: int = tool_input.get("max_size_kb", 10240)

    if not file_url.startswith("file://"):
        return json.dumps({"error": f"Unsupported URL scheme: only file:// is allowed, got {file_url[:20]}..."})

    path = Path(file_url[7:])  # strip "file://"

    if path.suffix.lower() not in _ALLOWED_EXT:
        return json.dumps({"error": f"Unsupported image type: {path.suffix}. Allowed: {sorted(_ALLOWED_EXT)}"})

    if not path.exists():
        return json.dumps({"error": f"File not found: {path}"})

    size_kb = path.stat().st_size // 1024
    if size_kb > max_kb:
        return json.dumps({"error": f"Image too large: {size_kb}KB > {max_kb}KB"})

    mime, _ = mimetypes.guess_type(str(path))
    if not mime:
        mime = f"image/{path.suffix.lstrip('.')}"

    try:
        b64 = base64.b64encode(path.read_bytes()).decode("ascii")
    except OSError as exc:
        return json.dumps({"error": f"Failed to read file: {exc}"})

    return json.dumps({
        "data_url": f"data:{mime};base64,{b64}",
        "mime": mime,
        "size_bytes": path.stat().st_size,
    }, ensure_ascii=False)
