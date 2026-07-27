#!/usr/bin/env python3
"""ext-service skill — 调真实外部 API 验证沙箱网络连通性。

使用 Python 标准库 urllib 调公开 API（如 wttr.in 天气服务），
验证沙箱网络出站 + 工具执行全链路。

不绑定任何业务逻辑，仅验证"外部服务连通性"。
"""

import json
import os
import sys
import urllib.error
import urllib.request
from pathlib import Path


def fetch_url(url: str, timeout: int = 10) -> dict:
    """Fetch a URL and return structured result."""
    try:
        req = urllib.request.Request(url, headers={"User-Agent": "agentflow-test/1.0"})
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            body = resp.read().decode("utf-8", errors="replace")
            return {
                "status": "success",
                "status_code": resp.status,
                "body": body[:2000],  # 截断防止超长
                "truncated": len(body) > 2000,
                "url": url,
            }
    except urllib.error.HTTPError as e:
        return {
            "status": "error",
            "error": f"HTTP {e.code}: {e.reason}",
            "url": url,
        }
    except urllib.error.URLError as e:
        return {
            "status": "error",
            "error": f"URL error: {e.reason}",
            "url": url,
        }
    except Exception as e:
        return {
            "status": "error",
            "error": str(e),
            "url": url,
        }


def main() -> int:
    """CLI entry point."""
    import argparse

    parser = argparse.ArgumentParser(description="调外部 API 验证沙箱网络连通性")
    parser.add_argument("--url", default="https://wttr.in?format=%C+%t",
                        help="目标 URL（默认 wttr.in 简略天气）")
    parser.add_argument("--timeout", type=int, default=10,
                        help="请求超时秒数（默认 10）")
    args = parser.parse_args()

    result = fetch_url(args.url, args.timeout)
    print(json.dumps(result, ensure_ascii=False))

    if result.get("status") == "success":
        return 0
    return 1


if __name__ == "__main__":
    sys.exit(main())