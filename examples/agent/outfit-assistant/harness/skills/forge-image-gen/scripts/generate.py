#!/usr/bin/env python3
"""forge-image-gen — multi-provider text-to-image generator (shared skill).

A small, self-contained CLI that other forge skills call to turn a prompt into
an image file. It reads the provider's API key from an environment variable
(e.g. OPENAI_API_KEY); see references/providers.md for how that key reaches the
sandbox. The script is deliberately decoupled from *how* the key is injected.

Zero third-party dependencies: HTTP via the Python standard library only.

Supports text-to-image and image-to-image (via --ref) on both OpenAI Images and
Alibaba DashScope (a reference image switches DashScope to the qwen-image-edit
model). The PROVIDERS registry is the single extension point — add a backend by
registering one entry plus one generate function.
"""

import argparse
import base64
import json
import mimetypes
import os
import ssl
import sys
import urllib.error
import urllib.request
import uuid
from pathlib import Path

# ⚠️ 忽略 SSL 证书验证（自签名/代理证书场景下使用）
ssl._create_default_https_context = ssl._create_unverified_context

DEFAULT_OPENAI_MODEL = "gpt-image-1"


class GenError(Exception):
    """A user-actionable generation failure (missing key, HTTP error, …)."""


# A direct opener that ignores environment proxies. The sandbox inherits the
# host's HTTP(S)_PROXY / ALL_PROXY (e.g. a clash/VPN proxy on 127.0.0.1:7890);
# that proxy may refuse or fail to reach a public API endpoint ("Connection
# refused"), even though the endpoint is directly reachable. We fall back to a
# direct connection when the (possibly proxied) default opener can't connect.
_DIRECT_OPENER = urllib.request.build_opener(urllib.request.ProxyHandler({}))


def _urlopen(req_or_url, timeout):
    """urlopen that retries WITHOUT the environment proxy when the proxied
    attempt fails to connect. A real HTTP response (4xx/5xx) is never retried —
    only transport-level URLErrors (proxy unreachable / connection refused)."""
    try:
        return urllib.request.urlopen(req_or_url, timeout=timeout)
    except urllib.error.HTTPError:
        raise  # a genuine server response — do not retry direct
    except urllib.error.URLError:
        # The proxied attempt MUTATES a Request object in place (ProxyHandler
        # rewrites req.host to the proxy + sets the tunnel), so reusing it would
        # just hit the dead proxy again. Rebuild a clean Request from the
        # original URL/data/headers for the direct retry. A plain URL string
        # carries no such state and is reused as-is.
        if isinstance(req_or_url, urllib.request.Request):
            fresh = urllib.request.Request(
                req_or_url.full_url,
                data=req_or_url.data,
                headers=dict(req_or_url.header_items()),
                method=req_or_url.get_method(),
            )
        else:
            fresh = req_or_url
        return _DIRECT_OPENER.open(fresh, timeout=timeout)


def _http_post_json(url, payload, headers, timeout=180):
    data = json.dumps(payload).encode()
    req = urllib.request.Request(
        url,
        data=data,
        headers={"Content-Type": "application/json", **headers},
        method="POST",
    )
    try:
        with _urlopen(req, timeout) as resp:
            return json.loads(resp.read().decode())
    except urllib.error.HTTPError as exc:
        err_body = exc.read().decode(errors="replace")
        raise GenError(f"HTTP {exc.code} from {url}: {err_body[:500]}") from exc
    except urllib.error.URLError as exc:
        raise GenError(f"network error calling {url}: {exc.reason}") from exc


def _http_get(url, timeout=180):
    try:
        with _urlopen(url, timeout) as resp:
            return resp.read()
    except urllib.error.URLError as exc:
        raise GenError(f"failed to download image: {exc}") from exc


def _encode_multipart(fields, files):
    """Encode fields + files as multipart/form-data. Returns (body, boundary).

    `files` is a list of (field_name, file_path). A missing/unreadable file
    raises GenError so the caller surfaces a clear message.
    """
    boundary = uuid.uuid4().hex
    body = bytearray()
    for key, value in fields.items():
        body += (
            f"--{boundary}\r\n"
            f'Content-Disposition: form-data; name="{key}"\r\n\r\n{value}\r\n'
        ).encode()
    for field, path in files:
        try:
            with open(path, "rb") as handle:
                data = handle.read()
        except OSError as exc:
            raise GenError(f"reference image not readable: {path} ({exc})") from exc
        filename = os.path.basename(path)
        ctype = mimetypes.guess_type(path)[0] or "application/octet-stream"
        body += (
            f"--{boundary}\r\n"
            f'Content-Disposition: form-data; name="{field}"; filename="{filename}"\r\n'
            f"Content-Type: {ctype}\r\n\r\n"
        ).encode()
        body += data + b"\r\n"
    body += f"--{boundary}--\r\n".encode()
    return bytes(body), boundary


def _http_post_multipart(url, body, boundary, headers, timeout=300):
    req = urllib.request.Request(
        url,
        data=body,
        headers={"Content-Type": f"multipart/form-data; boundary={boundary}", **headers},
        method="POST",
    )
    try:
        with _urlopen(req, timeout) as resp:
            return json.loads(resp.read().decode())
    except urllib.error.HTTPError as exc:
        err_body = exc.read().decode(errors="replace")
        raise GenError(f"HTTP {exc.code} from {url}: {err_body[:500]}") from exc
    except urllib.error.URLError as exc:
        raise GenError(f"network error calling {url}: {exc.reason}") from exc


def _decode_images(out):
    images = []
    for item in out.get("data") or []:
        if item.get("b64_json"):
            images.append(base64.b64decode(item["b64_json"]))
        elif item.get("url"):
            images.append(_http_get(item["url"]))
    if not images:
        raise GenError("provider returned no image data")
    return images


# ---- OpenAI backend (text-to-image + image-to-image) ----------------------


def openai_generate(prompt, *, model, size, n, refs=None, **_):
    key = os.environ.get("OPENAI_API_KEY")
    if not key:
        raise GenError(
            "OPENAI_API_KEY not set in this environment. "
            "See references/providers.md for sandbox env injection."
        )
    base = os.environ.get("OPENAI_BASE_URL", "https://api.openai.com/v1").rstrip("/")
    model = model or DEFAULT_OPENAI_MODEL
    auth = {"Authorization": f"Bearer {key}"}
    if refs:
        # Image-to-image: send reference image(s) to the edits endpoint so the
        # model conditions on real pixels (powers the xhs anchor-chain).
        fields = {"model": model, "prompt": prompt, "n": str(n), "size": size or "1024x1024"}
        files = [("image[]", ref) for ref in refs]
        body, boundary = _encode_multipart(fields, files)
        out = _http_post_multipart(f"{base}/images/edits", body, boundary, auth)
    else:
        payload = {"model": model, "prompt": prompt, "n": n, "size": size or "1024x1024"}
        out = _http_post_json(f"{base}/images/generations", payload, auth)
    return _decode_images(out)


# ---- DashScope backend (Alibaba, synchronous multimodal endpoint) ---------
#
# Both Alibaba image families reachable here speak the SAME synchronous
# multimodal-generation endpoint and the same request/response shape, so they
# share one core (`_dashscope_mm`):
#   • z-image-turbo  — fast, lightweight (provider "dashscope")
#   • Wan 2.x        — 通义万相 wan2.7-image / wan2.7-image-pro / wan2.6-image
#                      (provider "wan"); higher fidelity. Verified shape:
#                      POST /api/v1/services/aigc/multimodal-generation/generation
#                      → output.choices[].message.content[].image (a URL we GET).
# No async task polling is needed for these models.

DEFAULT_DASHSCOPE_MODEL = "z-image-turbo"
DEFAULT_WAN_MODEL = "wan2.7-image"
# The text-to-image models above ignore input images; a reference image switches
# us to this image-EDIT model, which takes the anchor as a base64 data-URI on the
# SAME sync endpoint and returns the SAME response shape (powers cross-sprite
# style consistency — generate an anchor, then condition the rest on it).
DEFAULT_DASHSCOPE_EDIT_MODEL = "qwen-image-edit-plus"
_DASHSCOPE_MM_PATH = "/api/v1/services/aigc/multimodal-generation/generation"


def _data_uri(path):
    """A local image as a `data:<mime>;base64,...` URI (DashScope accepts these,
    so reference-image generation needs no object storage)."""
    ctype = mimetypes.guess_type(path)[0] or "image/png"
    try:
        with open(path, "rb") as fh:
            raw = fh.read()
    except OSError as exc:
        raise GenError(f"reference image not readable: {path} ({exc})") from exc
    return f"data:{ctype};base64,{base64.b64encode(raw).decode()}"


def _dashscope_mm(prompt, *, model, size, n, default_model, refs=None, extra_params=None):
    key = os.environ.get("DASHSCOPE_API_KEY")
    if not key:
        raise GenError(
            "DASHSCOPE_API_KEY not set in this environment. "
            "See references/providers.md for sandbox env injection."
        )
    base = os.environ.get("DASHSCOPE_BASE_URL", "https://dashscope.aliyuncs.com")
    # Tolerate a base that already includes the service path (FlipBook does this).
    base = base.split("/services/aigc")[0].rstrip("/")
    refs = refs or []
    if refs and not model:
        model = DEFAULT_DASHSCOPE_EDIT_MODEL
    model = model or default_model
    # generate.py speaks WxH; DashScope's multimodal endpoint wants W*H.
    ds_size = (size or "1280x720").replace("x", "*")
    params = {"size": ds_size}
    if extra_params:
        params.update(extra_params)
    if refs:
        # Don't let the edit model rewrite our style instruction or stamp a mark.
        params.setdefault("prompt_extend", False)
        params.setdefault("watermark", False)
    auth = {"Authorization": f"Bearer {key}"}
    # Reference image(s) first, then the text instruction (DashScope's order).
    content = [{"image": _data_uri(r)} for r in refs] + [{"text": prompt}]
    images = []
    for _ in range(max(1, n)):
        body = {
            "model": model,
            "input": {"messages": [{"role": "user", "content": content}]},
            "parameters": params,
        }
        out = _http_post_json(base + _DASHSCOPE_MM_PATH, body, auth)
        if out.get("code"):
            raise GenError(f"DashScope {out['code']}: {out.get('message')}")
        choices = (out.get("output") or {}).get("choices") or []
        reply = (choices[0].get("message") or {}).get("content") if choices else []
        url = next((c.get("image") for c in (reply or []) if c.get("image")), None)
        if not url:
            raise GenError("DashScope: no image URL in response")
        images.append(_http_get(url))
    return images


def dashscope_generate(prompt, *, model, size, n, refs=None, **_):
    return _dashscope_mm(prompt, model=model, size=size, n=n, refs=refs,
                         default_model=DEFAULT_DASHSCOPE_MODEL)


def wan_generate(prompt, *, model, size, n, refs=None, **_):
    # Wan defaults: no watermark, and keep OUR exact prompt (prompt_extend off)
    # so "no text / watercolor" instructions are honoured verbatim.
    return _dashscope_mm(prompt, model=model, size=size, n=n, refs=refs,
                         default_model=DEFAULT_WAN_MODEL,
                         extra_params={"watermark": False, "prompt_extend": False})


# ---- Provider registry (the single extension point) -----------------------

PROVIDERS = {
    # Alibaba z-image-turbo — the fast/light DEFAULT (auto-selected on the key).
    "dashscope": {
        "env": ["DASHSCOPE_API_KEY"],
        "default_model": DEFAULT_DASHSCOPE_MODEL,
        "fn": dashscope_generate,
    },
    # Alibaba 通义万相 Wan 2.x — higher fidelity, OPT-IN via `--provider wan`
    # (shares the same DASHSCOPE_API_KEY; default model wan2.7-image).
    "wan": {
        "env": ["DASHSCOPE_API_KEY"],
        "default_model": DEFAULT_WAN_MODEL,
        "fn": wan_generate,
    },
    "openai": {
        "env": ["OPENAI_API_KEY"],
        "default_model": DEFAULT_OPENAI_MODEL,
        "fn": openai_generate,
    },
}
# Order in which an unspecified provider is auto-selected (first with a key wins).
# z-image is the default; Wan 2.7 is opt-in — request it explicitly with
# `--provider wan` (e.g. when the user asks for Wan 2.7).
SELECTION_ORDER = ["dashscope", "wan", "openai"]


def pick_provider(explicit):
    if explicit:
        if explicit not in PROVIDERS:
            raise GenError(
                f"unknown provider '{explicit}'. Known: {', '.join(PROVIDERS)}"
            )
        return explicit
    for name in SELECTION_ORDER:
        if any(os.environ.get(e) for e in PROVIDERS[name]["env"]):
            return name
    all_envs = ", ".join(e for p in PROVIDERS.values() for e in p["env"])
    raise GenError(f"no provider API key found in environment. Set one of: {all_envs}")


def write_images(images, output):
    out = Path(output)
    if out.suffix == "":
        out = out.with_suffix(".png")
    out.parent.mkdir(parents=True, exist_ok=True)
    written = []
    if len(images) == 1:
        out.write_bytes(images[0])
        written.append(str(out))
    else:
        for i, img in enumerate(images, 1):
            part = out.with_name(f"{out.stem}-{i}{out.suffix}")
            part.write_bytes(img)
            written.append(str(part))
    return written


def main():
    ap = argparse.ArgumentParser(description="forge-image-gen: prompt -> image file(s).")
    ap.add_argument("--prompt", "-p", help="Text prompt.")
    ap.add_argument("--output", "-o", help="Output path (extension optional; .png default).")
    ap.add_argument("--provider", help=f"Force a provider. Known: {', '.join(PROVIDERS)}.")
    ap.add_argument("--model", "-m", help="Model id (provider default if omitted).")
    ap.add_argument("--size", help="Pixel size, e.g. 1024x1024.")
    ap.add_argument("--n", type=int, default=1, help="Images to generate (default 1).")
    ap.add_argument("--ref", action="append", help="Reference image for image-to-image (repeatable).")
    ap.add_argument("--list-providers", action="store_true", help="List registered providers and exit.")
    ap.add_argument("--json", action="store_true", help="Emit a machine-readable JSON result.")
    args = ap.parse_args()

    if args.list_providers:
        for name, prov in PROVIDERS.items():
            print(f"{name}\tenv={','.join(prov['env'])}\tdefault_model={prov['default_model']}")
        return

    if not args.prompt or not args.output:
        ap.error("--prompt and --output are required (unless --list-providers)")

    try:
        provider = pick_provider(args.provider)
        images = PROVIDERS[provider]["fn"](
            args.prompt, model=args.model, size=args.size, n=args.n, refs=args.ref
        )
        written = write_images(images, args.output)
    except GenError as exc:
        print(f"[forge-image-gen] error: {exc}", file=sys.stderr)
        sys.exit(1)

    if args.json:
        print(json.dumps({"provider": provider, "files": written}))
    else:
        for path in written:
            print(f"[forge-image-gen] wrote {path}")


if __name__ == "__main__":
    main()
