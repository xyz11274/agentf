#!/usr/bin/env python3
"""web_search tool — 网页实时搜索（compatible-mode enable_search 内置联网）。

skill-scoped tool，由 skill_executor 动态加载。
模块契约：TOOL_NAME / TOOL_DESCRIPTION / TOOL_PARAMS / execute(args)。
零第三方依赖：HTTP 走标准库 urllib，代理不可达时回退直连。

实现说明（2026-09-04 实测）：
- 本端点（maas 专属实例）的 AgentStudio 只有 agent 管理面，未开放调用路由，
  official web_search MCP 也未上架（AGENT_010 / MCP 校验失败）。
- 可用的联网搜索路径是 compatible-mode chat completions + `enable_search: true`
  （DashScope 内置搜索插件，qwen-plus / qwen-max / qwen-turbo 实测返回带来源的
  实时结果），本工具即基于该路径。

凭据只从环境变量读取，绝不进 prompt / 返回值；结果统一脱敏。
"""

import json
import os
import re
import urllib.error
import urllib.request

TOOL_NAME = "web_search"
TOOL_DESCRIPTION = (
    "Search the live web for real-time information (news, prices, policies, "
    "scores, …). Returns a summarized answer with sources."
)
TOOL_PARAMS = {
    "type": "object",
    "properties": {
        "query": {
            "type": "string",
            "description": "搜索问题：写清时间与主体（如 '2026 国庆假期高速免费政策'）",
        },
        "count": {
            "type": "integer",
            "minimum": 1,
            "maximum": 10,
            "description": "期望返回的要点数（默认 5）",
        },
    },
    "required": ["query"],
    "additionalProperties": False,
}

COMPLETIONS_PATH = "/compatible-mode/v1/chat/completions"

_DIRECT_OPENER = urllib.request.build_opener(urllib.request.ProxyHandler({}))


def _urlopen(req, timeout):
    try:
        return urllib.request.urlopen(req, timeout=timeout)
    except urllib.error.HTTPError:
        raise
    except urllib.error.URLError:
        fresh = urllib.request.Request(
            req.full_url, data=req.data,
            headers=dict(req.header_items()), method=req.get_method(),
        )
        return _DIRECT_OPENER.open(fresh, timeout=timeout)


def _redact(text):
    try:
        from agentflow_runtime.security import redact_secrets
        safe, _ = redact_secrets(text)
        return safe
    except Exception:  # noqa: BLE001 — 兜底：抹掉 key 形态的串
        return re.sub(r"sk-[A-Za-z0-9._\-]+", "sk-***", text)


def _collect_sources(data):
    """收集响应中的搜索来源（search_info / citations 等形态，缺省为空）。"""
    sources = []
    for key in ("search_info", "web_search_info", "citations"):
        info = data.get(key)
        if isinstance(info, dict):
            results = info.get("search_results") or info.get("results") or []
            for item in results if isinstance(results, list) else []:
                if isinstance(item, dict):
                    entry = {"title": item.get("title"),
                             "url": item.get("url") or item.get("link")}
                    if entry["title"] or entry["url"]:
                        sources.append(entry)
        elif isinstance(info, list):
            for item in info:
                if isinstance(item, str):
                    sources.append({"url": item})
    return sources


def execute(args):
    args = args if isinstance(args, dict) else {}
    query = str(args.get("query", "")).strip()
    if not query:
        return {"status": "failed", "error": "query is required"}
    try:
        count = max(1, min(10, int(args.get("count") or 5)))
    except (TypeError, ValueError):
        count = 5

    api_key = (os.environ.get("WEBSEARCH_API_KEY")
               or os.environ.get("DASHSCOPE_API_KEY", "")).strip()
    if not api_key:
        return {"status": "failed",
                "error": "WEBSEARCH_API_KEY not configured (see .env)"}
    base_url = os.environ.get(
        "WEBSEARCH_BASE_URL",
        "https://llm-7ztiqbmxzp9wduvz.cn-beijing.maas.aliyuncs.com",
    ).rstrip("/")
    model = os.environ.get("WEBSEARCH_MODEL", "qwen-plus")

    user_prompt = (
        f"{query}\n\n"
        f"请联网搜索后回答：给出 {count} 条以内带来源（站点名）的要点；"
        f"搜索不到确切信息时明确说明，不要编造。"
    )
    payload = {
        "model": model,
        "messages": [{"role": "user", "content": user_prompt}],
        "enable_search": True,
    }
    req = urllib.request.Request(
        f"{base_url}{COMPLETIONS_PATH}",
        data=json.dumps(payload).encode("utf-8"),
        headers={
            "Content-Type": "application/json",
            "Authorization": f"Bearer {api_key}",
        },
        method="POST",
    )
    try:
        with _urlopen(req, 120) as resp:
            data = json.loads(resp.read().decode("utf-8"))
    except urllib.error.HTTPError as exc:
        body = exc.read().decode("utf-8", "replace")[:500]
        return {"status": "failed",
                "error": _redact(f"web search HTTP {exc.code}: {body}")}
    except Exception as exc:  # noqa: BLE001
        return {"status": "failed", "error": _redact(f"web search failed: {exc}")}

    choices = data.get("choices") or []
    content = ""
    if choices:
        message = choices[0].get("message") or {}
        raw = message.get("content")
        if isinstance(raw, str):
            content = raw
        elif isinstance(raw, list):
            content = "\n".join(
                part.get("text", "") for part in raw
                if isinstance(part, dict) and part.get("text"))
    if not content.strip():
        return {"status": "failed",
                "error": _redact("empty content in response: "
                                 + json.dumps(data, ensure_ascii=False)[:300])}

    result = {
        "status": "succeeded",
        "query": query,
        "model": model,
        "results": content,
    }
    sources = _collect_sources(data)
    if sources:
        result["sources"] = sources[:10]
    return result
