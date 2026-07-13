#!/usr/bin/env python3
"""Analyze an uploaded outfit/garment or full-body photo with MiniMax-M3 vision.

Uses MiniMax-M3's multimodal chat completion to identify and describe clothing
items. Reads API_KEY from the environment. Zero pip dependencies.

Three modes:
  --mode garment  (default) — identify the clothing item(s):
      {"items":[{"category","subtype","colors","pattern","material","fit",
                 "formality","season","notes"}], "summary"}
  --mode person   — coarse, RESPECTFUL personalization cues:
      {"body_shape_hint","height_impression","skin_undertone","disclaimer"}
  --mode outfit   — complete worn look analysis:
      {"items":[...], "palette","style_tags","formality","summary"}

API: POST https://api.minimaxi.com/v1/chat/completions (OpenAI-compatible)
Docs: https://platform.minimax.io/docs/guides/text-generation

Image size limit: files > 5 MB will be rejected (base64 encoding of very
large images causes OOM / curl timeout on the LLM proxy).
"""

import argparse
import base64
import json
import logging
import mimetypes
import os
import sys
import time
import urllib.error
import urllib.request

ENDPOINT = "https://api.minimaxi.com/v1/chat/completions"
VISION_MODEL = "MiniMax-M3"
MAX_IMAGE_BYTES = 5 * 1024 * 1024  # 5 MB

GARMENT_PROMPT = (
    "你是服装识别助手。识别这张图片里的服饰单品，只输出一个 JSON 对象，不要任何额外文字或代码块标记。"
    "结构：{\"items\":[{\"category\":\"上装/外套/下装/连衣裙/鞋/包/配饰 等\","
    "\"subtype\":\"更具体的款式，如 风衣/针织衫/直筒牛仔裤/乐福鞋\",\"colors\":[\"主要颜色，中文\"],"
    "\"pattern\":\"纯色/条纹/格纹/印花/拼接 等\",\"material\":\"看的材质，如 棉/牛仔/针织/皮，不确定填 unknown\","
    "\"fit\":\"修身/合身/宽松/oversize 等\",\"formality\":\"休闲/商务休闲/正式/运动\","
    "\"season\":[\"适合季节：春/夏/秋/冬\"],\"notes\":\"搭配相关的简短观察\"}],"
    "\"summary\":\"一句话概述这些单品\"}。"
    "只描述真实可见的内容，不要臆造看不清的细节（不确定就用 unknown 或空数组）。"
)

PERSON_PROMPT = (
    "你是一个穿搭顾问的辅助 AI。从这张全身/半身照中提取与穿搭相关的特征，"
    "只输出一个 JSON 对象，不要任何额外文字或代码块标记。"
    "注意：这是为穿搭推荐服务的，不是外貌评判，语气要 tentative（可能/看起来）。"
    "结构：{\"body_shape_hint\":\"偏瘦/匀称/梨形/苹果形 等，不确定填 null\","
    "\"height_impression\":\"看起来偏高/中等/娇小，不推测具体数字，不确定填 null\","
    "\"skin_undertone\":\"暖调/冷调/中性，不确定填 null\","
    "\"coloring_notes\":\"额外的颜色建议（如：适合暖色系）\","
    "\"disclaimer\":\"以上是基于照片的粗估，实际以用户自述为准\"}"
)

OUTFIT_PROMPT = (
    "你是时尚点评助手。分析这张图片里的完整穿搭，只输出一个 JSON 对象，不要任何额外文字或代码块标记。"
    "结构：{\"items\":[{\"category\":\"上装/外套/下装/连衣裙/鞋/包/配饰\","
    "\"subtype\":\"具体款式\",\"colors\":[\"主要颜色\"],\"fit\":\"修身/合身/宽松\"}],"
    "\"palette\":\"整体配色描述\",\"style_tags\":[\"简约\",\"休闲\",\"商务\" 等风格标签],"
    "\"formality\":\"休闲/商务休闲/正式\",\"layering\":\"叠穿情况描述\","
    "\"proportion_notes\":\"比例和剪裁的观察\",\"summary\":\"一句话总结\"}"
)


class VisionError(Exception):
    """A user-actionable vision API failure."""


def _get_api_key() -> str:
    key = os.environ.get("API_KEY", "")
    if not key:
        raise VisionError("API_KEY not set in environment")
    return key


def _encode_image(image_path: str) -> str:
    """Read an image file and return a base64 data URI.
    
    Raises VisionError if the file exceeds MAX_IMAGE_BYTES (5 MB) to prevent
    OOM / curl timeout on the upstream LLM proxy.
    """
    size = os.path.getsize(image_path)
    if size > MAX_IMAGE_BYTES:
        mb = size / (1024 * 1024)
        limit_mb = MAX_IMAGE_BYTES / (1024 * 1024)
        raise VisionError(
            f"Image too large: {mb:.1f} MB (max {limit_mb:.0f} MB). "
            f"Please resize or compress before uploading."
        )
    with open(image_path, "rb") as f:
        img_bytes = f.read()
    mime = mimetypes.guess_type(image_path)[0] or "image/jpeg"
    return f"data:{mime};base64,{base64.b64encode(img_bytes).decode('utf-8')}"


def _call_vision(image_data: str, system_prompt: str, model: str = VISION_MODEL,
                 timeout: int = 120, max_tokens: int = 1200) -> dict:
    """Call MiniMax-M3 with an image + system prompt."""
    api_key = _get_api_key()
    payload = {
        "model": model,
        "messages": [
            {"role": "system", "content": system_prompt},
            {
                "role": "user",
                "content": [
                    {"type": "image_url", "image_url": {"url": image_data}},
                    {"type": "text", "text": "请分析这张图片"},
                ],
            },
        ],
        "max_completion_tokens": max_tokens,
    }
    headers = {
        "Authorization": f"Bearer {api_key}",
        "Content-Type": "application/json",
    }
    logger = logging.getLogger("outfit_vision")
    t0 = time.monotonic()
    req = urllib.request.Request(
        ENDPOINT,
        data=json.dumps(payload).encode("utf-8"),
        headers=headers,
        method="POST",
    )
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            data = json.loads(resp.read().decode("utf-8"))
    except urllib.error.HTTPError as e:
        body = e.read().decode("utf-8", errors="replace")[:300]
        logger.warning("HTTP %d from MiniMax: %.200s", e.code, body)
        raise VisionError(f"HTTP {e.code}: {body}")
    except urllib.error.URLError as e:
        logger.warning("network error calling MiniMax: %s", e.reason)
        raise VisionError(f"Connection failed: {e.reason}")

    logger.info("MiniMax OK in %.1fs", time.monotonic() - t0)

    # Extract content from OpenAI-compatible response
    choice = (data.get("choices") or [{}])[0]
    content = (choice.get("message") or {}).get("content", "")

    return _extract_json(content)


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
    except (json.JSONDecodeError, ValueError) as exc:
        logging.getLogger("outfit_vision").warning(
            "response JSON parse failed: %s — text=%.300r", exc, text.strip()
        )
        return {"raw": text.strip()}


def analyze_image(image_path: str, mode: str = "garment",
                  model: str = VISION_MODEL,
                  max_tokens: int = 1200) -> dict:
    """Analyze a clothing/outfit image.

    Args:
        image_path: Path to image file, or an http(s):// URL
        mode: "garment", "person", or "outfit"
        model: Vision model name
        max_tokens: Max completion tokens for the API call

    Returns:
        Parsed JSON dict with mode-specific structure.
    """
    if image_path.startswith(("http://", "https://")):
        image_data = image_path  # URL passthrough
    else:
        image_data = _encode_image(image_path)

    prompt_map = {
        "garment": GARMENT_PROMPT,
        "person": PERSON_PROMPT,
        "outfit": OUTFIT_PROMPT,
    }
    system_prompt = prompt_map.get(mode, GARMENT_PROMPT)
    return _call_vision(image_data, system_prompt, model, max_tokens=max_tokens)


def main():
    # 配置 logging（stderr + 独立文件，避免被父进程二次截断）
    logging.basicConfig(
        level=logging.INFO,
        format="%(asctime)s [%(levelname)s] [%(name)s] %(message)s",
        handlers=[
            logging.StreamHandler(sys.stderr),
            logging.FileHandler("/tmp/outfit_vision.log", mode="a"),
        ],
        force=True,
    )
    parser = argparse.ArgumentParser(
        description="Analyze outfit/garment image with MiniMax-M3 vision",
    )
    parser.add_argument("--image", required=True, help="Image file path or URL")
    parser.add_argument("--mode", choices=["garment", "person", "outfit"],
                        default="garment", help="Analysis mode")
    parser.add_argument("--model", default=VISION_MODEL, help="Vision model")
    parser.add_argument("--max-tokens", type=int, default=1200,
                        help="Max completion tokens")
    args = parser.parse_args()

    try:
        result = analyze_image(args.image, args.mode, args.model,
                               max_tokens=args.max_tokens)
    except VisionError as e:
        print(str(e), file=sys.stderr)
        sys.exit(1)

    result["mode"] = args.mode
    print(json.dumps(result, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
