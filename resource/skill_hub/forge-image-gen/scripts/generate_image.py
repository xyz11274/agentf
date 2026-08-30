"""Trusted structured tool for provider-backed image generation."""

from __future__ import annotations

import importlib.util
import os
from contextlib import ExitStack
from pathlib import Path, PurePosixPath

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


TOOL_NAME = "generate_image"
TOOL_DESCRIPTION = (
    "Generate one or more images through the configured trusted provider. "
    "Provider credentials are injected internally and must never be supplied."
)
TOOL_PARAMS = {
    "type": "object",
    "properties": {
        "prompt": {"type": "string", "description": "Image generation prompt."},
        "output": {"type": "string", "description": "Workspace output path, normally assets/name.png."},
        "provider": {"type": "string", "enum": ["dashscope", "wan", "openai"]},
        "model": {"type": "string"},
        "size": {"type": "string"},
        "n": {"type": "integer", "minimum": 1, "maximum": 4},
        "refs": {
            "type": "array",
            "items": {"type": "string"},
            "description": "Optional workspace image paths for image-to-image.",
        },
    },
    "required": ["prompt", "output"],
    "additionalProperties": False,
}


def _load_backend():
    path = Path(__file__).with_name("generate.py")
    spec = importlib.util.spec_from_file_location("_agentflow_image_backend", path)
    if spec is None or spec.loader is None:
        raise TrustedMediaError("image backend could not be loaded")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    module._urlopen = direct_urlopen
    return module


def _logical_outputs(requested: str, count: int) -> list[str]:
    output = PurePosixPath(requested)
    if not output.suffix:
        output = output.with_suffix(".png")
    if count == 1:
        return [output.as_posix()]
    return [
        output.with_name(f"{output.stem}-{index}{output.suffix}").as_posix()
        for index in range(1, count + 1)
    ]


def execute(args, sandbox_endpoint=None):
    register_environment_secrets()
    args = args if isinstance(args, dict) else {}
    prompt = str(args.get("prompt", "")).strip()
    output = str(args.get("output", "")).strip()
    if not prompt or not output:
        return {"status": "failed", "error": "prompt and output are required"}
    if len(prompt) > 20000:
        return {"status": "failed", "error": "prompt exceeds 20000 characters"}

    try:
        count = int(args.get("n", 1))
    except (TypeError, ValueError):
        return {"status": "failed", "error": "n must be an integer"}
    if count < 1 or count > 4:
        return {"status": "failed", "error": "n must be between 1 and 4"}

    backend = _load_backend()
    try:
        provider = backend.pick_provider(args.get("provider") or None)
        if provider == "openai":
            base = os.environ.get("OPENAI_BASE_URL", "https://api.openai.com/v1").rstrip("/")
            validate_credential_url(base)
        else:
            base = os.environ.get("DASHSCOPE_BASE_URL", "https://dashscope.aliyuncs.com")
            validate_credential_url(base)

        refs = args.get("refs") or []
        if not isinstance(refs, list):
            raise TrustedMediaError("refs must be an array")
        with ExitStack() as stack:
            local_refs = []
            for reference in refs:
                value = str(reference)
                if value.startswith(("http://", "https://", "data:")):
                    raise TrustedMediaError("reference images must be workspace files")
                local_refs.append(stack.enter_context(materialize_media(
                    value,
                    sandbox_endpoint or "",
                    allowed_suffixes=IMAGE_SUFFIXES,
                )))
            logical_outputs = _logical_outputs(output, count)
            with temporary_media_output(logical_outputs[0]) as local_output:
                images = backend.PROVIDERS[provider]["fn"](
                    prompt,
                    model=args.get("model") or None,
                    size=args.get("size") or None,
                    n=count,
                    refs=local_refs or None,
                )
                local_files = backend.write_images(images, local_output)
                published = [
                    publish_media(path, logical, sandbox_endpoint or "")
                    for path, logical in zip(local_files, logical_outputs)
                ]
        return {
            "status": "succeeded",
            "provider": provider,
            "files": published,
        }
    except Exception as exc:
        safe_error, _redaction = redact_secrets(str(exc))
        return {"status": "failed", "error": safe_error}
