"""web_fetch tool — fetch a URL and return its content as text.

ToolExecutor 迁移（Phase 3，ADR 20260724-tools-runtime-refactor.md）：
- 通过 create_executor() 抽象层路由，不再内部 if-else
- sandbox-mode 由 factory 读取 AF_SANDBOX_MODE 决定 Host/HTTP/E2B
- 宿主侧保留 blocked_prefixes / blocked_metadata security 检查（始终执行，B2 防线）
- execute() 签名不变（skill_executor 兼容）
"""

import json

TOOL_NAME = "web_fetch"
TOOL_DESCRIPTION = "Fetch a URL and return its content as plain text. Supports http and https URLs. Useful for reading documentation, API responses, or web pages."
TOOL_PARAMS = {
    "type": "object",
    "properties": {
        "url": {
            "type": "string",
            "description": "URL to fetch (http or https only)"
        },
        "max_chars": {
            "type": "integer",
            "description": "Maximum characters to return (default 5000, use -1 for no limit)",
            "default": 5000
        },
        "timeout": {
            "type": "integer",
            "description": "Timeout in seconds (default 15)",
            "default": 15
        }
    },
    "required": ["url"]
}

# 私有/内部地址前缀（宿主侧第一道防线，B2）
_BLOCKED_PREFIXES = [
    "http://localhost", "https://localhost",
    "http://127.0.0.1", "https://127.0.0.1",
    "http://10.", "https://10.",
    "http://172.16.", "https://172.16.",
    "http://192.168.", "https://192.168.",
    "http://169.254.", "https://169.254.",
    "http://[::1]", "https://[::1]",
    "http://0.0.0.0", "https://0.0.0.0",
]

# 云元数据端点
_BLOCKED_METADATA = [
    "169.254.169.254",  # cloud metadata
    "metadata.google.internal",
    "metadata.aws.internal",
]


def _is_blocked(url: str) -> str | None:
    """检查 URL 是否命中安全黑名单，返回拒绝原因或 None。"""
    if not url.startswith("http://") and not url.startswith("https://"):
        return "Only http and https URLs are allowed"
    for prefix in _BLOCKED_PREFIXES:
        if url.startswith(prefix):
            return "Access denied: requests to private/internal addresses are not allowed"
    for blocked in _BLOCKED_METADATA:
        if blocked in url:
            return "Access denied: requests to metadata endpoints are not allowed"
    return None


def execute(tool_input: dict, sandbox_endpoint: str = None) -> str:
    """获取 URL 内容。

    迁移后：通过 ToolExecutor 抽象层路由（create_executor），不再内部 if-else。
    保持签名不变（skill_executor.py 兼容）。

    Args:
        tool_input: {"url": str, "max_chars": int, "timeout": int}
        sandbox_endpoint: 由 daemon 注入的沙箱 HTTP endpoint。

    Returns:
        JSON 字符串——FileContent 格式（content/error/total_lines）
        加 tool 字段标识来源。
    """
    from agentflow_runtime.executors.factory import create_executor

    url = tool_input.get("url", "")
    max_chars = tool_input.get("max_chars", 5000)
    timeout = tool_input.get("timeout", 15)

    if not url:
        return json.dumps({"error": "url is required", "tool": "web_fetch"},
                          ensure_ascii=False)

    # 宿主侧第一道防线：URL 安全检查（始终执行，B2）
    blocked_reason = _is_blocked(url)
    if blocked_reason:
        return json.dumps({"error": blocked_reason, "tool": "web_fetch"},
                          ensure_ascii=False)

    # 通过 Factory 获取 executor
    executor = create_executor(sandbox_endpoint=sandbox_endpoint)
    result = executor.fetch_url(url, timeout)

    # 统一序列化（FileContent.to_dict() + 补 tool 字段）
    data = result.to_dict()
    data["tool"] = "web_fetch"
    data["url"] = url

    # max_chars 截断（宿主侧逻辑，保留不变）
    if max_chars > 0 and data.get("content") and len(data["content"]) > max_chars:
        data["content"] = data["content"][:max_chars] + f"\n... (truncated at {max_chars} chars)"

    return json.dumps(data, ensure_ascii=False)
