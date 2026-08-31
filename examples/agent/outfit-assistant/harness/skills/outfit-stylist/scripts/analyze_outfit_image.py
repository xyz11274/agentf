"""Trusted structured tool for outfit image analysis."""

from __future__ import annotations

import contextlib
import importlib.util
import io
import json
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
    validate_credential_url,
)


TOOL_NAME = "analyze_outfit_image"
TOOL_DESCRIPTION = (
    "Analyze a workspace outfit image as garment, person, or complete outfit. "
    "The vision provider credential is injected internally."
)
TOOL_PARAMS = {
    "type": "object",
    "properties": {
        "image": {"type": "string", "description": "Workspace image path or HTTPS image URL."},
        "mode": {"type": "string", "enum": ["garment", "person", "outfit"]},
        "model": {"type": "string"},
        "max_tokens": {"type": "integer", "minimum": 64, "maximum": 4096},
    },
    "required": ["image"],
    "additionalProperties": False,
}


def _load_backend():
    path = Path(__file__).with_name("outfit_vision.py")
    spec = importlib.util.spec_from_file_location("_agentflow_outfit_vision_backend", path)
    if spec is None or spec.loader is None:
        raise TrustedMediaError("vision backend could not be loaded")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    module._urlopen = direct_urlopen
    return module


def execute(args, sandbox_endpoint=None):
    register_environment_secrets()
    args = args if isinstance(args, dict) else {}
    image = str(args.get("image", "")).strip()
    mode = str(args.get("mode", "garment")).strip()
    if not image:
        return {"status": "failed", "error": "image is required"}
    if mode not in {"garment", "person", "outfit"}:
        return {"status": "failed", "error": "invalid analysis mode"}
    try:
        max_tokens = int(args.get("max_tokens", 1200))
    except (TypeError, ValueError):
        return {"status": "failed", "error": "max_tokens must be an integer"}
    if max_tokens < 64 or max_tokens > 4096:
        return {"status": "failed", "error": "max_tokens must be between 64 and 4096"}

    try:
        backend = _load_backend()
        validate_credential_url(backend.ENDPOINT)
        with materialize_media(
            image,
            sandbox_endpoint or "",
            allowed_suffixes=IMAGE_SUFFIXES,
        ) as local_image:
            argv = [
                "--image", local_image,
                "--mode", mode,
                "--max-tokens", str(max_tokens),
            ]
            if args.get("model"):
                argv.extend(["--model", str(args["model"])])
            stdout = io.StringIO()
            stderr = io.StringIO()
            with contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(stderr):
                exit_code = backend.main(argv)
        if exit_code:
            message = stderr.getvalue().strip() or "vision provider call failed"
            safe_message, _redaction = redact_secrets(message)
            return {"status": "failed", "error": safe_message}
        result = json.loads(stdout.getvalue())
        safe_result, _redaction = redact_secrets(result)
        return {"status": "succeeded", "analysis": safe_result}
    except Exception as exc:
        safe_error, _redaction = redact_secrets(str(exc))
        return {"status": "failed", "error": safe_error}
