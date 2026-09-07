#!/usr/bin/env python3
"""generate_image tool — 文生图（multimodal-generation 同步端点）。

skill-scoped tool，由 skill_executor 动态加载。
模块契约：TOOL_NAME / TOOL_DESCRIPTION / TOOL_PARAMS / execute(args)。
零第三方依赖：HTTP 走标准库 urllib，代理不可达时回退直连。

凭据只从环境变量读取（IMAGE_API_KEY，见 .env），绝不进 prompt / 工具参数 /
返回值。返回前对结果做 secret 脱敏（agentflow_runtime.security 可用时）。
"""

import json
import os
import re
import urllib.error
import urllib.parse
import urllib.request
import uuid
from pathlib import Path

TOOL_NAME = "generate_image"
TOOL_DESCRIPTION = (
    "Generate one image from a text prompt. The image is downloaded into the "
    "agent workspace and returned as a local file path. Provider credentials "
    "are injected internally and must never be supplied."
)
TOOL_PARAMS = {
    "type": "object",
    "properties": {
        "prompt": {
            "type": "string",
            "description": "画面描述：主体 + 场景 + 风格 + 构图（中英文皆可）",
        },
        "output": {
            "type": "string",
            "description": "workspace 内输出路径，如 assets/cat.png（默认 assets/<uuid>.png）",
        },
    },
    "required": ["prompt"],
    "additionalProperties": False,
}

DEFAULT_BASE_URL = "https://llm-7ztiqbmxzp9wduvz.cn-beijing.maas.aliyuncs.com"
GEN_PATH = "/api/v1/services/aigc/multimodal-generation/generation"

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
    """返回值脱敏：agentflow_runtime.security 可用时用统一注册表，否则宽松兜底。"""
    try:
        from agentflow_runtime.security import redact_secrets
        safe, _ = redact_secrets(text)
        return safe
    except Exception:  # noqa: BLE001 — 脱敏不可用时兜底：抹掉所有 key 形态的串
        return re.sub(r"sk-[A-Za-z0-9._\-]+", "sk-***", text)


def _agent_package_dir():
    # skill_executor 加载前会注入 AF_AGENT_PACKAGE_DIR（见 tool_registry）
    here = Path(__file__).resolve()
    default = here.parents[3]  # harness/skills/image-gen/scripts -> 包根
    return Path(os.environ.get("AF_AGENT_PACKAGE_DIR") or default)


def _extract_image_urls(payload):
    """从响应中递归抽取图片 URL（兼容 image / image_url / url 键）。"""
    urls = []
    stack = [payload]
    while stack:
        node = stack.pop()
        if isinstance(node, dict):
            for key, value in node.items():
                if key in ("image", "image_url", "url") and isinstance(value, str) \
                        and value.startswith(("http://", "https://")):
                    urls.append(value)
                else:
                    stack.append(value)
        elif isinstance(node, list):
            stack.extend(node)
    return urls


def execute(args, sandbox_endpoint=None):
    args = args if isinstance(args, dict) else {}
    prompt = str(args.get("prompt", "")).strip()
    if not prompt:
        return {"status": "failed", "error": "prompt is required"}
    if len(prompt) > 20000:
        return {"status": "failed", "error": "prompt exceeds 20000 characters"}

    api_key = (os.environ.get("IMAGE_API_KEY")
               or os.environ.get("DASHSCOPE_API_KEY", "")).strip()
    if not api_key:
        return {"status": "failed",
                "error": "IMAGE_API_KEY not configured (see .env)"}
    base_url = os.environ.get("IMAGE_BASE_URL", DEFAULT_BASE_URL).rstrip("/")
    # 默认 wan2.7-image：实测当前 key 已开通（qwen-image-3.0-pro 未开通返回 403
    # AccessDenied.Unpurchased）。其他模型通过 IMAGE_MODEL 覆盖。
    model = os.environ.get("IMAGE_MODEL", "wan2.7-image")

    output = str(args.get("output", "")).strip()
    if not output:
        output = f"assets/{uuid.uuid4().hex}.png"
    output = output.lstrip("/")
    if ".." in output.split("/"):
        return {"status": "failed",
                "error": "output path must stay inside the workspace"}

    payload = {
        "model": model,
        "input": {
            "messages": [{
                "role": "user",
                "content": [{"text": prompt}],
            }],
        },
        "parameters": {"prompt_extend": False},
    }
    req = urllib.request.Request(
        f"{base_url}{GEN_PATH}",
        data=json.dumps(payload).encode("utf-8"),
        headers={
            "Content-Type": "application/json",
            "Authorization": f"Bearer {api_key}",
        },
        method="POST",
    )
    try:
        with _urlopen(req, 180) as resp:
            data = json.loads(resp.read().decode("utf-8"))
    except urllib.error.HTTPError as exc:
        body = exc.read().decode("utf-8", "replace")[:500]
        return {"status": "failed",
                "error": _redact(f"image API HTTP {exc.code}: {body}")}
    except Exception as exc:  # noqa: BLE001
        return {"status": "failed", "error": _redact(f"image generation failed: {exc}")}

    urls = _extract_image_urls(data.get("output") or data)
    if not urls:
        return {"status": "failed",
                "error": _redact(f"no image in response: "
                                 f"{json.dumps(data, ensure_ascii=False)[:500]}")}

    target_dir = _agent_package_dir() / "workspace" / Path(output).parent
    target_dir.mkdir(parents=True, exist_ok=True)
    target = _agent_package_dir() / "workspace" / output

    url = urls[0]
    try:
        dl = urllib.request.Request(url, headers={"Accept": "*/*"})
        with _urlopen(dl, 120) as resp:
            target.write_bytes(resp.read())
    except Exception as exc:  # noqa: BLE001 — 下载失败仍返回 URL
        return {"status": "failed",
                "error": f"image downloaded failed: {exc}",
                "url": url}

    return {
        "status": "succeeded",
        "model": model,
        "files": [{
            "path": str(target),
            "workspace_path": output,
            "url": url,
        }],
    }
