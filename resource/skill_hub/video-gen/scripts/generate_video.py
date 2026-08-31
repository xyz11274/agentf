"""Trusted structured tool for provider-backed video generation."""

from __future__ import annotations

import contextlib
import importlib.util
import io
import urllib.parse
from pathlib import Path

from agentflow_runtime.security import (
    redact_secrets,
    register_environment_secrets,
)
from agentflow_runtime.security.trusted_media import (
    IMAGE_SUFFIXES,
    TrustedMediaError,
    direct_urlopen,
    materialize_media,
    publish_media,
    temporary_media_output,
    validate_credential_url,
)


TOOL_NAME = "generate_video"
TOOL_DESCRIPTION = (
    "Generate a video from text or animate a workspace image. "
    "The provider credential is injected internally and must never be supplied."
)
TOOL_PARAMS = {
    "type": "object",
    "properties": {
        "prompt": {"type": "string"},
        "image": {"type": "string", "description": "Optional workspace first-frame image."},
        "output": {"type": "string", "description": "Workspace .mp4 output path."},
        "model": {"type": "string"},
        "size": {"type": "string"},
        "resolution": {"type": "string"},
        "ratio": {"type": "string"},
        "duration": {"type": "integer", "minimum": 1, "maximum": 30},
        "audio_url": {"type": "string", "description": "Optional HTTPS audio URL."},
        "negative_prompt": {"type": "string"},
        "poll_timeout": {"type": "integer", "minimum": 30, "maximum": 1800},
    },
    "required": ["output"],
    "additionalProperties": False,
}


def _load_backend():
    path = Path(__file__).with_name("make_video.py")
    spec = importlib.util.spec_from_file_location("_agentflow_video_backend", path)
    if spec is None or spec.loader is None:
        raise TrustedMediaError("video backend could not be loaded")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    module._urlopen = direct_urlopen
    return module


def _optional_int(args: dict, name: str, default: int, low: int, high: int) -> int:
    try:
        value = int(args.get(name, default))
    except (TypeError, ValueError) as exc:
        raise TrustedMediaError(f"{name} must be an integer") from exc
    if value < low or value > high:
        raise TrustedMediaError(f"{name} must be between {low} and {high}")
    return value


def execute(args, sandbox_endpoint=None):
    register_environment_secrets()
    args = args if isinstance(args, dict) else {}
    prompt = str(args.get("prompt", "")).strip()
    image = str(args.get("image", "")).strip()
    output = str(args.get("output", "")).strip()
    if not output:
        return {"status": "failed", "error": "output is required"}
    if not image and not prompt:
        return {"status": "failed", "error": "prompt is required for text-to-video"}
    if len(prompt) > 20000:
        return {"status": "failed", "error": "prompt exceeds 20000 characters"}

    try:
        duration = _optional_int(args, "duration", 5, 1, 30)
        poll_timeout = _optional_int(args, "poll_timeout", 600, 30, 1800)
        audio_url = str(args.get("audio_url", "")).strip()
        if audio_url:
            parsed_audio = urllib.parse.urlsplit(audio_url)
            if parsed_audio.scheme != "https" or not parsed_audio.hostname:
                raise TrustedMediaError("audio_url must use HTTPS")

        backend = _load_backend()
        validate_credential_url(backend.SUBMIT_ENDPOINT)
        validate_credential_url(backend.TASK_ENDPOINT)
        image_context = (
            materialize_media(
                image,
                sandbox_endpoint or "",
                allowed_suffixes=IMAGE_SUFFIXES,
            )
            if image
            else contextlib.nullcontext("")
        )
        with image_context as local_image, temporary_media_output(output) as local_output:
            argv = ["--out", local_output, "--poll-timeout", str(poll_timeout)]
            if prompt:
                argv.extend(["--prompt", prompt])
            if local_image:
                argv.extend(["--image", local_image])
            for key, flag in (
                ("model", "--model"),
                ("size", "--size"),
                ("resolution", "--resolution"),
                ("ratio", "--ratio"),
                ("negative_prompt", "--negative-prompt"),
            ):
                if args.get(key):
                    argv.extend([flag, str(args[key])])
            argv.extend(["--duration", str(duration)])
            if audio_url:
                argv.extend(["--audio-url", audio_url])

            stdout = io.StringIO()
            stderr = io.StringIO()
            with contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(stderr):
                exit_code = backend.main(argv)
            if exit_code:
                message = stderr.getvalue().strip() or "video provider call failed"
                safe_message, _redaction = redact_secrets(message)
                return {"status": "failed", "error": safe_message}
            published = publish_media(local_output, output, sandbox_endpoint or "")
        return {"status": "succeeded", "files": [published]}
    except Exception as exc:
        safe_error, _redaction = redact_secrets(str(exc))
        return {"status": "failed", "error": safe_error}
