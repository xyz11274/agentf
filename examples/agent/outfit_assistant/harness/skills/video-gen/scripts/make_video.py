#!/usr/bin/env python3
"""Generate a short video with MiniMax Hailuo models (text-to-video / image-to-video).

Reads API_KEY from the environment. Zero pip dependencies.

API (async): POST https://api.minimax.io/v1/video_generation → task_id
Poll:       GET  https://api.minimax.io/v1/tasks/{task_id} → file_id
Download:   GET  https://api.minimax.io/v1/files/{file_id}/download → MP4

Docs: https://platform.minimax.io/docs/guides/video-generation
"""

import argparse
import base64
import json
import mimetypes
import os
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path

SUBMIT_ENDPOINT = "https://api.minimax.io/v1/video_generation"
TASK_ENDPOINT = "https://api.minimax.io/v1/tasks"
FILE_ENDPOINT = "https://api.minimax.io/v1/files"
DEFAULT_MODEL = "MiniMax-Hailuo-2.3"
POLL_INTERVAL = 10  # seconds between status checks


class GenError(Exception):
    """A user-actionable generation failure."""


def _get_api_key() -> str:
    key = os.environ.get("API_KEY", "")
    if not key:
        raise GenError(
            "API_KEY not set. "
            "Add it to your .env file or export it before running."
        )
    return key


def _headers() -> dict:
    return {
        "Authorization": f"Bearer {_get_api_key()}",
        "Content-Type": "application/json",
    }


def _urlopen(req_or_url, timeout=60):
    """urlopen that retries WITHOUT the environment proxy on transport failure."""
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
        return urllib.request.urlopen(
            urllib.request.Request(
                req_or_url.full_url if isinstance(req_or_url, urllib.request.Request) else req_or_url,
                data=req_or_url.data if isinstance(req_or_url, urllib.request.Request) else None,
                headers=_headers(),
            ),
            timeout=timeout,
        )


def _create_task(prompt: str, model: str, duration: int,
                 resolution: str, image_path: str | None = None) -> str:
    """Create a video generation task, return task_id."""
    payload: dict = {
        "model": model,
        "prompt": prompt,
        "duration": duration,
        "resolution": resolution,
    }
    if image_path:
        # Image-to-video: encode local file as base64 or pass URL directly
        if image_path.startswith(("http://", "https://", "mm_file://")):
            payload["image"] = image_path
        else:
            with open(image_path, "rb") as f:
                img_b64 = base64.b64encode(f.read()).decode("utf-8")
                mime = mimetypes.guess_type(image_path)[0] or "image/jpeg"
                payload["image"] = f"data:{mime};base64,{img_b64}"

    req = urllib.request.Request(
        SUBMIT_ENDPOINT,
        data=json.dumps(payload).encode("utf-8"),
        headers=_headers(),
        method="POST",
    )
    try:
        with _urlopen(req, timeout=30) as resp:
            data = json.loads(resp.read().decode("utf-8"))
    except urllib.error.HTTPError as e:
        body = e.read().decode("utf-8", errors="replace")[:300]
        raise GenError(f"HTTP {e.code}: {body}")
    except urllib.error.URLError as e:
        raise GenError(f"Connection failed: {e.reason}")

    task_id = data.get("task_id")
    if not task_id:
        raise GenError(f"API returned no task_id. Response: {json.dumps(data)[:200]}")
    return task_id


def _poll_task(task_id: str, timeout: int = 600) -> str:
    """Poll task status until completion, return file_id."""
    deadline = time.time() + timeout
    url = f"{TASK_ENDPOINT}/{task_id}"
    while time.time() < deadline:
        req = urllib.request.Request(url, headers=_headers(), method="GET")
        try:
            with _urlopen(req, timeout=15) as resp:
                data = json.loads(resp.read().decode("utf-8"))
        except urllib.error.HTTPError as e:
            body = e.read().decode("utf-8", errors="replace")[:200]
            raise GenError(f"Poll HTTP {e.code}: {body}")

        status = data.get("status", "")  # status in minimax response
        # Also check nested "data.status" or "task.status"
        nested = data.get("data") or {}
        status = nested.get("status") or data.get("status") or ""

        if status.lower() in ("success", "succeeded", "completed"):
            file_id = nested.get("file_id") or data.get("file_id")
            if not file_id:
                raise GenError(f"Task succeeded but no file_id. Response: {json.dumps(data)[:200]}")
            return file_id
        elif status.lower() in ("failed", "error"):
            err_msg = nested.get("error_message") or data.get("error_message") or str(data)[:200]
            raise GenError(f"Task failed: {err_msg}")
        # else: still running
        time.sleep(POLL_INTERVAL)

    raise GenError(f"Task did not complete within {timeout}s. task_id={task_id}")


def _download_file(file_id: str, out_path: Path, timeout: int = 120):
    """Download the video file by file_id."""
    url = f"{FILE_ENDPOINT}/{file_id}/download"
    req = urllib.request.Request(url, headers=_headers(), method="GET")
    try:
        with _urlopen(req, timeout=timeout) as resp:
            out_path.write_bytes(resp.read())
    except urllib.error.HTTPError as e:
        body = e.read().decode("utf-8", errors="replace")[:200]
        raise GenError(f"Download HTTP {e.code}: {body}")
    except urllib.error.URLError as e:
        raise GenError(f"Download failed: {e.reason}")


def generate_video(
    prompt: str,
    *,
    model: str = DEFAULT_MODEL,
    duration: int = 6,
    resolution: str = "1080P",
    image: str | None = None,
    output: str = "video.mp4",
    poll_timeout: int = 600,
) -> str:
    """Generate a video. Returns the output file path."""
    out_path = Path(output)
    out_path.parent.mkdir(parents=True, exist_ok=True)

    task_id = _create_task(prompt, model, duration, resolution, image)
    print(f"[video-gen] Task created: {task_id}", file=sys.stderr)

    file_id = _poll_task(task_id, timeout=poll_timeout)
    print(f"[video-gen] File ready: {file_id}", file=sys.stderr)

    _download_file(file_id, out_path)
    print(f"[video-gen] Downloaded to {out_path}", file=sys.stderr)
    return str(out_path)


def main():
    model_env = os.environ.get("MINIMAX_VIDEO_MODEL", DEFAULT_MODEL)
    parser = argparse.ArgumentParser(
        description="Generate a video with MiniMax Hailuo",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument("--prompt", required=True, help="Video content description")
    parser.add_argument("--image", help="Reference image path/URL (for image-to-video)")
    parser.add_argument("--model", default=model_env, help=f"Model. Default {DEFAULT_MODEL}")
    parser.add_argument("--duration", type=int, default=6, help="Duration in seconds (default 6)")
    parser.add_argument("--resolution", default="1080P", choices=["1080P", "720P"],
                        help="Resolution (default 1080P)")
    parser.add_argument("--out", default="video.mp4", help="Output MP4 path")
    parser.add_argument("--poll-interval", type=int, default=POLL_INTERVAL)
    parser.add_argument("--poll-timeout", type=int, default=600)
    args = parser.parse_args()

    try:
        result = generate_video(
            args.prompt,
            model=args.model,
            duration=args.duration,
            resolution=args.resolution,
            image=args.image,
            output=args.out,
            poll_timeout=args.poll_timeout,
        )
    except GenError as e:
        print(str(e), file=sys.stderr)
        sys.exit(1)

    print(result)


if __name__ == "__main__":
    main()
