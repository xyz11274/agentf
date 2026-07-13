#!/usr/bin/env python3
"""forge-image-gen (MiniMax) — text-to-image & subject-reference image generation.

Reads API_KEY or MINIMAX_API_KEY from the environment. Zero pip dependencies — uses the
Python standard library only (urllib, base64, json).

API: POST https://api.minimaxi.com/v1/image_generation
Docs: https://platform.minimaxi.com/docs/guides/image-generation

For --ref / --subject-reference:
  - An http(s):// URL is passed through as-is.
  - A local file path is base64-encoded into a data: URI automatically.
"""

import argparse
import base64
import json
import mimetypes
import os
import sys
import urllib.error
import urllib.request
from pathlib import Path

ENDPOINT = "https://api.minimaxi.com/v1/image_generation"
DEFAULT_MODEL = "image-01"
DEFAULT_ASPECT_RATIO = "16:9"

VALID_ASPECT_RATIOS = {"16:9", "4:3", "3:2", "2:3", "3:4", "9:16", "21:9"}


class GenError(Exception):
    """A user-actionable generation failure."""


def _get_api_key() -> str:
    key = os.environ.get("API_KEY", "") or os.environ.get("MINIMAX_API_KEY", "")
    if not key:
        raise GenError(
            "API_KEY or MINIMAX_API_KEY not set. "
            "Add it to your .env file or export it before running."
        )
    return key


def _call_api(payload: dict, timeout: int = 60) -> dict:
    """Call the Minimax image generation API."""
    api_key = _get_api_key()
    headers = {
        "Authorization": f"Bearer {api_key}",
        "Content-Type": "application/json",
    }
    req = urllib.request.Request(
        ENDPOINT,
        data=json.dumps(payload).encode("utf-8"),
        headers=headers,
        method="POST",
    )
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            return json.loads(resp.read().decode("utf-8"))
    except urllib.error.HTTPError as e:
        body = e.read().decode("utf-8", errors="replace")[:300]
        raise GenError(f"HTTP {e.code}: {body}")
    except urllib.error.URLError as e:
        raise GenError(f"Connection failed: {e.reason}")


def _resolve_ref(ref: str) -> str:
    """Return an http(s) URL as-is; base64-encode a local file path into a data: URI."""
    if ref.startswith(("http://", "https://", "data:")):
        return ref
    path = Path(ref)
    if not path.exists():
        raise GenError(f"reference image not found: {ref}")
    mime = mimetypes.guess_type(str(path))[0] or "image/jpeg"
    data = path.read_bytes()
    return f"data:{mime};base64,{base64.b64encode(data).decode('ascii')}"


def generate(
    prompt: str,
    *,
    model: str = DEFAULT_MODEL,
    aspect_ratio: str = DEFAULT_ASPECT_RATIO,
    n: int = 1,
    subject_reference: str | None = None,
    timeout: int = 60,
) -> list[bytes]:
    """Generate image(s) from a text prompt.

    Returns a list of decoded image bytes (JPEG).
    ``subject_reference`` accepts an http(s) URL or a local file path.
    """
    if aspect_ratio not in VALID_ASPECT_RATIOS:
        raise GenError(
            f"Invalid aspect_ratio '{aspect_ratio}'. "
            f"Supported: {', '.join(sorted(VALID_ASPECT_RATIOS))}"
        )
    if n < 1 or n > 9:
        raise GenError(f"n must be 1–9, got {n}")

    payload: dict = {
        "model": model,
        "prompt": prompt,
        "aspect_ratio": aspect_ratio,
        "response_format": "base64",
        "n": n,
    }

    if subject_reference:
        payload["subject_reference"] = [
            {"type": "character", "image_file": _resolve_ref(subject_reference)}
        ]

    result = _call_api(payload, timeout=timeout)

    # API response: {"data": {"image_base64": ["...", "..."]}}
    data = result.get("data") or {}
    images_b64 = data.get("image_base64") or []
    if not images_b64:
        raise GenError(f"API returned no images. Response: {json.dumps(result)[:200]}")

    images = []
    for b64_str in images_b64:
        images.append(base64.b64decode(b64_str))
    return images


def main():
    parser = argparse.ArgumentParser(
        description="Generate image(s) with MiniMax Image-01",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=(
            "Examples:\n"
            "  python3 generate.py --prompt \"a white T-shirt on a hanger\" --out tee.png\n"
            "  python3 generate.py --prompt \"casual outfit\" --ar 3:4 --n 3 --out options.png\n"
            "  python3 generate.py --prompt \"same model in red dress\" --ref https://...jpg --out variant.png\n"
        ),
    )
    parser.add_argument("--prompt", "-p", required=True, help="Text prompt for image generation")
    parser.add_argument("--output", "-o", required=True, help="Output file path")
    parser.add_argument("--aspect-ratio", "--ar", default=DEFAULT_ASPECT_RATIO,
                        help=f"Aspect ratio. Default {DEFAULT_ASPECT_RATIO}")
    parser.add_argument("--model", "-m", default=DEFAULT_MODEL, help=f"Model name. Default {DEFAULT_MODEL}")
    parser.add_argument("--n", type=int, default=1, help="Number of images (1–9)")
    parser.add_argument("--ref", "--subject-reference", help="Reference image URL for subject-consistent generation")
    parser.add_argument("--timeout", type=int, default=60, help="API timeout in seconds")
    args = parser.parse_args()

    try:
        images = generate(
            args.prompt,
            model=args.model,
            aspect_ratio=args.aspect_ratio,
            n=args.n,
            subject_reference=args.ref,
            timeout=args.timeout,
        )
    except GenError as e:
        print(str(e), file=sys.stderr)
        sys.exit(1)

    out_path = Path(args.output).resolve()
    out_path.parent.mkdir(parents=True, exist_ok=True)

    if len(images) == 1:
        out_path.write_bytes(images[0])
        print(str(out_path))
    else:
        stem = out_path.stem
        suffix = out_path.suffix or ".png"
        written = []
        for i, img_bytes in enumerate(images, 1):
            path = out_path.with_name(f"{stem}-{i}{suffix}")
            path.write_bytes(img_bytes)
            written.append(str(path))
        for p in written:
            print(p)


if __name__ == "__main__":
    main()
