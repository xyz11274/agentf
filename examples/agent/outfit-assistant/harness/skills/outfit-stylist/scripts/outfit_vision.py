#!/usr/bin/env python3
"""Analyze an uploaded outfit/garment or full-body photo with Qwen-VL (DashScope).

Unlike a generic describe-the-image call, this emits STRUCTURED JSON tuned for
outfit recommendation, so the stylist agent gets schema-stable fields instead of
free prose. Three modes:

  --mode garment  (default) — identify the clothing item(s) in the picture:
      {"items":[{"category","subtype","colors","pattern","material","fit",
                 "formality","season","notes"}], "summary"}
  --mode person   — coarse, RESPECTFUL personalization cues from a full-body
      photo (NOT a judgement; the model is told to be tentative):
      {"body_shape_hint","height_impression","skin_undertone","coloring_notes",
       "disclaimer"}
  --mode outfit   — a COMPLETE worn look (for style critique): all visible
      items + overall styling cues (NOT a judgement — the agent critiques):
      {"items":[{"category","subtype","colors","fit"}], "palette","style_tags",
       "formality","layering","proportion_notes","summary"}

Reads DASHSCOPE_API_KEY from the environment (inject it via the Environment's
secrets / a Vault — the sandbox drops *_API_KEY from the per-call env path).
Zero pip dependencies; uses DashScope's OpenAI-compatible endpoint.

Two transport paths, auto-picked by model name prefix (same OpenAI multimodal
payload — only the transport layer changes):

  model "aisf/<name>"  → UDS path: send the payload through the local
      `_uds_client` package (a self-contained copy of nodes/common/clients'
      UDS subset) → agentflow plugin socket → SysAgent → FusionMind's OpenAI
      endpoint. FusionMind supports VL multimodal request bodies, so image_url
      content blocks pass through unchanged. No DASHSCOPE_API_KEY needed —
      the server side owns credentials. Zero pip deps: UDS client is stdlib only.
  model "<name>"       → HTTP path: POST to DashScope's OpenAI-compatible
      endpoint, authed by DASHSCOPE_API_KEY. Zero pip deps; stdlib urllib only.

Usage:
  python3 outfit_vision.py --image uploads/coat.jpg
  python3 outfit_vision.py --image uploads/me.jpg --mode person
  python3 outfit_vision.py --image uploads/x.png --model aisf/qwen-vl-max   # UDS

Prints one JSON object to stdout. Exit 0 on success; non-zero (message on
stderr) on a missing key / unreadable image / API error. If the model's reply
can't be parsed as JSON it is still returned under {"raw": "..."} so the agent
can fall back to it rather than failing hard.
"""

from __future__ import annotations

import argparse
import base64
import json
import logging
import mimetypes
import os
import ssl
import sys
import urllib.error
import urllib.request

# UDS 路径用本地 _uds_client 包（nodes/common/clients UDS 子集的自包含拷贝，
# 不 import 框架 nodes/，守 SKILL.md "Zero pip dependencies" 约束）
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

ssl._create_default_https_context = ssl._create_unverified_context
ENDPOINT = "https://dashscope.aliyuncs.com/compatible-mode/v1/chat/completions"
DEFAULT_MODEL = "qwen-vl-max"
AISF_PREFIX = "aisf/"  # 模型名带此前缀走 UDS，否则走 DashScope HTTP

GARMENT_PROMPT = (
    "你是服装识别助手。识别这张图片里的服饰单品，只输出一个 JSON 对象，不要任何额外文字或代码块标记。"
    "结构：{\"items\":[{\"category\":\"上装/外套/下装/连衣裙/鞋/包/配饰 等\","
    "\"subtype\":\"更具体的款式，如 风衣/针织衫/直筒牛仔裤/乐福鞋\",\"colors\":[\"主要颜色，中文\"],"
    "\"pattern\":\"纯色/条纹/格纹/印花/拼接 等\",\"material\":\"看起来的材质，如 棉/牛仔/针织/皮，不确定填 unknown\","
    "\"fit\":\"修身/合身/宽松/oversize 等\",\"formality\":\"休闲/商务休闲/正式/运动\","
    "\"season\":[\"适合季节：春/夏/秋/冬\"],\"notes\":\"搭配相关的简短观察\"}],"
    "\"summary\":\"一句话概述这些单品\"}。"
    "只描述真实可见的内容，不要臆造看不清的细节（不确定就用 unknown 或空数组）。"
)

PERSON_PROMPT = (
    "你是穿搭个性化助手。基于这张全身/人物照，给出用于穿搭建议的【粗略】视觉线索，"
    "只输出一个 JSON 对象，不要任何额外文字或代码块标记。"
    "结构：{\"body_shape_hint\":\"对整体身形轮廓的中性、试探性描述（如 偏直筒/偏沙漏/上宽下窄/不确定）\","
    "\"height_impression\":\"高挑/中等/娇小/不确定\",\"skin_undertone\":\"冷调/暖调/中性/不确定\","
    "\"coloring_notes\":\"发色、肤色明度等与配色相关的观察\","
    "\"disclaimer\":\"这些是基于单张照片的粗略推测，可能不准，仅供搭配参考。\"}。"
    "措辞必须尊重、不评判身材；看不清就填『不确定』。不要描述或推断与穿搭无关的个人身份信息。"
)

OUTFIT_PROMPT = (
    "你是穿搭识别助手。识别这张【整体穿搭/上身照】里的全部服饰单品和整体造型线索，"
    "只输出一个 JSON 对象，不要任何额外文字或代码块标记。"
    "结构：{\"items\":[{\"category\":\"上装/外套/下装/连衣裙/鞋/包/配饰 等\",\"subtype\":\"具体款式\","
    "\"colors\":[\"颜色，中文\"],\"fit\":\"修身/合身/宽松/oversize\"}],"
    "\"palette\":[\"整体主要配色，中文\"],\"style_tags\":[\"风格词，如 简约/通勤/休闲/复古/街头/甜美/运动\"],"
    "\"formality\":\"休闲/商务休闲/正式/运动\",\"layering\":\"层次与廓形的简短观察\","
    "\"proportion_notes\":\"上下比例、廓形的中性观察（不评判身材）\","
    "\"summary\":\"一句话概述这套造型\"}。"
    "只描述真实可见的内容，不臆造看不清的细节；措辞中性、不评判身材，不输出对人物外貌的主观评价。"
)

# A direct opener that ignores environment proxies — inside a container a
# host loopback proxy (127.0.0.1:7890) is unreachable, so fall back to direct.
_DIRECT_OPENER = urllib.request.build_opener(urllib.request.ProxyHandler({}))


def _urlopen(req_or_url, timeout):
    """urlopen that retries WITHOUT the env proxy on a transport-level failure.
    A real HTTP 4xx/5xx is never retried."""
    try:
        return urllib.request.urlopen(req_or_url, timeout=timeout)
    except urllib.error.HTTPError:
        raise
    except urllib.error.URLError:
        if isinstance(req_or_url, urllib.request.Request):
            req_or_url = urllib.request.Request(
                req_or_url.full_url,
                data=req_or_url.data,
                headers=dict(req_or_url.header_items()),
                method=req_or_url.get_method(),
            )
        return _DIRECT_OPENER.open(req_or_url, timeout=timeout)


def _image_url(src: str) -> str:
    """Remote URL passes through; a local file is base64-encoded into a data: URL."""
    if src.startswith(("http://", "https://", "data:")):
        return src
    if not os.path.exists(src):
        raise SystemExit(f"error: image not found: {src}")
    mime = mimetypes.guess_type(src)[0] or "image/jpeg"
    with open(src, "rb") as fh:
        b64 = base64.b64encode(fh.read()).decode("ascii")
    return f"data:{mime};base64,{b64}"


def _extract_json(text: str):
    """The model is told to emit bare JSON, but be defensive: strip ``` fences
    and slice to the outermost {...} before parsing. Returns the parsed object,
    or {"raw": text} if it still isn't valid JSON."""
    s = text.strip()
    if s.startswith("```"):
        s = s.split("\n", 1)[-1] if "\n" in s else s
        if s.endswith("```"):
            s = s[: -3]
        if s.lstrip().startswith("json"):
            s = s.lstrip()[4:]
    start, end = s.find("{"), s.rfind("}")
    if start != -1 and end != -1 and end > start:
        s = s[start : end + 1]
    try:
        return json.loads(s)
    except (json.JSONDecodeError, ValueError):
        return {"raw": text.strip()}


def main(argv=None):
    # 配置 logging，让 _uds_client 等子模块的日志打到 stderr
    # 以及独立文件 /tmp/uds_req.log（避免被 CODE_LOADER_DEBUG 截断）
    logging.basicConfig(
        level=logging.INFO,
        format="%(asctime)s [%(levelname)s] [%(name)s] %(message)s",
        handlers=[
            logging.StreamHandler(sys.stderr),
            logging.FileHandler("/tmp/uds_req.log", mode="a"),
        ],
        force=True,
    )
    ap = argparse.ArgumentParser(prog="outfit_vision", description="structured outfit/person image analysis via Qwen-VL")
    ap.add_argument("--image", required=True, help="image file path or http(s) URL")
    ap.add_argument("--mode", choices=("garment", "person", "outfit"), default="garment")
    ap.add_argument("--model", default=os.environ.get("DASHSCOPE_VL_MODEL") or DEFAULT_MODEL,
                    help="model name; 'aisf/<name>' prefix → UDS path (no key needed), "
                         "bare name → DashScope HTTP (needs DASHSCOPE_API_KEY)")
    ap.add_argument("--max-tokens", type=int, default=1200)
    args = ap.parse_args(argv)

    use_uds = args.model.startswith(AISF_PREFIX)

    key = ""
    if not use_uds:
        key = os.environ.get("DASHSCOPE_API_KEY")
        if not key:
            print(
                "error: DASHSCOPE_API_KEY not set in this environment. Set it to a "
                "DashScope key (sk-...) — e.g. via the Environment's secrets, "
                "or use --model aisf/<name> to go through the agentflow plugin socket.",
                file=sys.stderr,
            )
            return 2

    try:
        image_url = _image_url(args.image)
    except SystemExit as exc:
        print(str(exc), file=sys.stderr)
        return 2

    prompt = (
        OUTFIT_PROMPT
        if args.mode == "outfit"
        else (PERSON_PROMPT if args.mode == "person" else GARMENT_PROMPT)
    )
    # OpenAI multimodal 格式 payload —— 两条路径共用，只换传输层
    messages = [
        {
            "role": "user",
            "content": [
                {"type": "text", "text": prompt},
                {"type": "image_url", "image_url": {"url": image_url}},
            ],
        }
    ]

    if use_uds:
        # 用本地 _uds_client 包（nodes/common/clients UDS 子集的自包含拷贝）
        from _uds_client import create_llm_client, LlmUdsError
        try:
            client = create_llm_client(model=args.model)
            resp = client.chat_completion(
                messages=messages,
                max_completion_tokens=args.max_tokens,
                stream=False,
            )
        except LlmUdsError as exc:
            print(f"error: UDS LLM call failed: {exc}", file=sys.stderr)
            return 1
        except Exception as exc:
            print(f"error: UDS path failed: {exc}", file=sys.stderr)
            return 1
        data = resp
    else:
        payload = {
            "model": args.model,
            "messages": messages,
            "max_tokens": args.max_tokens,
        }
        req = urllib.request.Request(
            ENDPOINT,
            data=json.dumps(payload).encode(),
            headers={"Content-Type": "application/json", "Authorization": f"Bearer {key}"},
            method="POST",
        )
        try:
            with _urlopen(req, timeout=120) as resp:
                data = json.loads(resp.read().decode())
        except urllib.error.HTTPError as exc:
            body = exc.read().decode(errors="replace")
            print(f"error: HTTP {exc.code} from DashScope: {body[:500]}", file=sys.stderr)
            return 1
        except urllib.error.URLError as exc:
            print(f"error: network error calling DashScope: {exc.reason}", file=sys.stderr)
            return 1

    try:
        text = data["choices"][0]["message"]["content"]
        if isinstance(text, list):  # some VL models return content parts
            text = "".join(p.get("text", "") for p in text if isinstance(p, dict))
    except (KeyError, IndexError, TypeError):
        print(f"error: unexpected response shape: {json.dumps(data)[:400]}", file=sys.stderr)
        return 1

    result = _extract_json(text)
    result["mode"] = args.mode
    print(json.dumps(result, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
