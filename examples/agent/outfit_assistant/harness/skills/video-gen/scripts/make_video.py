#!/usr/bin/env python3
"""Generate a short video with Alibaba Cloud's Wan / 通义万相 models (DashScope).

Text-to-video (--prompt) or image-to-video (--image + --prompt). Submits an
ASYNC task to DashScope's video-synthesis endpoint, polls until it finishes, and
downloads the resulting MP4. Reads the key from DASHSCOPE_API_KEY; zero pip deps.

API (async): POST .../services/aigc/video-generation/video-synthesis with header
`X-DashScope-Async: enable` returns a task_id; GET .../tasks/<id> reports status
and, on SUCCEEDED, a video_url to download.

Usage:
  python3 make_video.py --prompt "一只猫在窗台伸懒腰，电影感" --out cat.mp4
  python3 make_video.py --image hero.jpg --prompt "镜头缓缓推进" --out hero.mp4

Model: --model or DASHSCOPE_VIDEO_MODEL (defaults wanx2.1-t2v-turbo /
wanx2.1-i2v-turbo). Prints the written MP4 path to stdout. Exit 0 on success;
non-zero (message on stderr) on a missing key / bad input / API or task failure.
"""

from __future__ import annotations

import argparse
import base64
import json
import mimetypes
import os
import sys
import time
import urllib.error
import urllib.request

SUBMIT_ENDPOINT = "https://dashscope.aliyuncs.com/api/v1/services/aigc/video-generation/video-synthesis"
TASK_ENDPOINT = "https://dashscope.aliyuncs.com/api/v1/tasks/"
DEFAULT_T2V_MODEL = "wanx2.1-t2v-turbo"
DEFAULT_I2V_MODEL = "wanx2.1-i2v-turbo"
POLL_INTERVAL = 10  # seconds between status checks

# A direct opener that ignores environment proxies. The sandbox inherits the
# host's HTTP(S)_PROXY / ALL_PROXY (e.g. a clash proxy on 127.0.0.1:7890); inside
# a container that address is unreachable, so we fall back to a direct connection.
_DIRECT_OPENER = urllib.request.build_opener(urllib.request.ProxyHandler({}))


def _urlopen(req_or_url, timeout):
    """urlopen that retries WITHOUT the environment proxy on a transport-level
    failure (proxy unreachable / connection refused). A real HTTP 4xx/5xx is
    never retried. Rebuilds a clean Request for the retry — the proxied attempt
    mutates a Request in place (ProxyHandler rewrites req.host to the proxy)."""
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


def _new_protocol(model: str) -> bool:
    """Wan request protocols differ by generation. wanx2.1 and wan2.2 use the
    LEGACY payload (`parameters.size`, e.g. "1280*720"). wan2.5 / wan2.6 /
    wan2.7 (and newer) use the NEW protocol (`parameters.resolution` + `ratio` +
    `duration`, no `size`). Detect from the model id so one script serves both."""
    m = model.lower()
    if m.startswith("wanx2.1") or m.startswith("wan2.2"):
        return False
    return True


def _image_url(src: str) -> str:
    """A remote URL passes through; a local file is read + base64-encoded into a
    data: URL (DashScope's i2v endpoint accepts both)."""
    if src.startswith(("http://", "https://", "data:")):
        return src
    if not os.path.exists(src):
        raise SystemExit(f"error: image not found: {src}")
    mime = mimetypes.guess_type(src)[0] or "image/jpeg"
    with open(src, "rb") as fh:
        b64 = base64.b64encode(fh.read()).decode("ascii")
    return f"data:{mime};base64,{b64}"


def _post_json(url: str, payload: dict, headers: dict, timeout: int) -> dict:
    req = urllib.request.Request(
        url, data=json.dumps(payload).encode(), headers=headers, method="POST"
    )
    with _urlopen(req, timeout=timeout) as resp:
        return json.loads(resp.read().decode())


def _get_json(url: str, headers: dict, timeout: int) -> dict:
    req = urllib.request.Request(url, headers=headers, method="GET")
    with _urlopen(req, timeout=timeout) as resp:
        return json.loads(resp.read().decode())


def main(argv=None):
    ap = argparse.ArgumentParser(prog="make_video", description="generate a video with Wan (通义万相)")
    ap.add_argument("--prompt", default="", help="what to generate / how it should move")
    ap.add_argument("--image", help="first-frame image (path or URL) → image-to-video")
    ap.add_argument("--model", default=os.environ.get("DASHSCOPE_VIDEO_MODEL") or "")
    ap.add_argument("--size", default="1280*720", help="text-to-video frame size (LEGACY protocol: wanx2.1/wan2.2)")
    ap.add_argument("--resolution", default="720P", help="frame resolution (NEW protocol: wan2.5+), e.g. 720P / 1080P")
    ap.add_argument("--ratio", default="16:9", help="aspect ratio (NEW protocol text-to-video): 16:9 / 9:16 / 1:1 / 4:3 / 3:4")
    ap.add_argument("--duration", type=int, default=5, help="clip length in seconds (NEW protocol)")
    ap.add_argument("--audio-url", default="", help="optional audio track URL (NEW protocol wan2.5/2.6+)")
    ap.add_argument("--negative-prompt", default="", help="things to avoid")
    ap.add_argument("--out", default="video.mp4", help="output MP4 path")
    ap.add_argument("--poll-timeout", type=int, default=600, help="max seconds to wait")
    args = ap.parse_args(argv)

    key = os.environ.get("DASHSCOPE_API_KEY")
    if not key:
        print(
            "error: DASHSCOPE_API_KEY not set in this environment. Set it to a "
            "DashScope key (sk-...) — e.g. via the Environment's secrets.",
            file=sys.stderr,
        )
        return 2

    is_i2v = bool(args.image)
    if not is_i2v and not args.prompt.strip():
        print("error: --prompt is required for text-to-video.", file=sys.stderr)
        return 2

    model = args.model or (DEFAULT_I2V_MODEL if is_i2v else DEFAULT_T2V_MODEL)
    new_proto = _new_protocol(model)

    # Build the input + parameters per mode AND per protocol generation.
    inp: dict = {}
    if args.prompt.strip():
        inp["prompt"] = args.prompt
    if args.negative_prompt.strip():
        inp["negative_prompt"] = args.negative_prompt
    if is_i2v:
        try:
            inp["img_url"] = _image_url(args.image)
        except SystemExit as exc:
            print(str(exc), file=sys.stderr)
            return 2
    if args.audio_url.strip():
        inp["audio_url"] = args.audio_url

    params: dict = {}
    if new_proto:
        # NEW protocol (wan2.5/2.6/2.7+): resolution + duration; ratio for t2v.
        params["resolution"] = args.resolution
        params["duration"] = args.duration
        if not is_i2v:
            params["ratio"] = args.ratio
    else:
        # LEGACY protocol (wanx2.1/wan2.2): size for t2v; i2v derives it.
        if not is_i2v:
            params["size"] = args.size

    payload = {"model": model, "input": inp, "parameters": params}
    print(
        f"… model={model} mode={'i2v' if is_i2v else 't2v'} protocol={'new' if new_proto else 'legacy'}",
        file=sys.stderr,
    )
    submit_headers = {
        "Content-Type": "application/json",
        "Authorization": f"Bearer {key}",
        "X-DashScope-Async": "enable",
    }
    poll_headers = {"Authorization": f"Bearer {key}"}

    # 1) Submit the async task.
    try:
        submit = _post_json(SUBMIT_ENDPOINT, payload, submit_headers, timeout=60)
    except urllib.error.HTTPError as exc:
        body = exc.read().decode(errors="replace")
        print(f"error: HTTP {exc.code} submitting task: {body[:500]}", file=sys.stderr)
        return 1
    except urllib.error.URLError as exc:
        print(f"error: network error submitting task: {exc.reason}", file=sys.stderr)
        return 1

    task_id = (submit.get("output") or {}).get("task_id")
    if not task_id:
        print(f"error: no task_id in response: {json.dumps(submit)[:400]}", file=sys.stderr)
        return 1
    print(f"… task submitted ({task_id}); polling for completion", file=sys.stderr)

    # 2) Poll until done / timeout.
    deadline = time.monotonic() + args.poll_timeout
    video_url = None
    while True:
        if time.monotonic() > deadline:
            print(f"error: timed out after {args.poll_timeout}s waiting for task {task_id}.", file=sys.stderr)
            return 1
        time.sleep(POLL_INTERVAL)
        try:
            status = _get_json(TASK_ENDPOINT + task_id, poll_headers, timeout=60)
        except (urllib.error.HTTPError, urllib.error.URLError) as exc:
            print(f"… poll error (will retry): {exc}", file=sys.stderr)
            continue
        out = status.get("output") or {}
        state = out.get("task_status")
        if state in ("PENDING", "RUNNING"):
            print(f"… {state.lower()}", file=sys.stderr)
            continue
        if state == "SUCCEEDED":
            video_url = out.get("video_url") or ((out.get("results") or [{}])[0].get("url"))
            if not video_url:
                print(f"error: task succeeded but no video_url: {json.dumps(out)[:400]}", file=sys.stderr)
                return 1
            break
        # FAILED / CANCELED / UNKNOWN
        msg = out.get("message") or status.get("message") or json.dumps(out)[:400]
        print(f"error: task {state}: {msg}", file=sys.stderr)
        return 1

    # 3) Download the MP4.
    out_path = os.path.abspath(args.out)
    out_dir = os.path.dirname(out_path)
    if out_dir:
        os.makedirs(out_dir, exist_ok=True)
    try:
        with _urlopen(video_url, timeout=300) as resp:
            data = resp.read()
    except (urllib.error.HTTPError, urllib.error.URLError) as exc:
        print(f"error: failed to download video: {exc}", file=sys.stderr)
        return 1
    with open(out_path, "wb") as fh:
        fh.write(data)

    print(out_path)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
