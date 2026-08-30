"""Fail-closed access to the Harness credential redaction policy."""

from __future__ import annotations

import logging
import traceback

from typing import Any

try:
    from agentflow_runtime.security import redact_secrets as _redact_secrets
except ImportError:  # Bridge chat remains available; sensitive persistence stops.
    _redact_secrets = None


class SecretSanitizerUnavailable(RuntimeError):
    """Raised when a persistence sink cannot load the shared DLP policy."""


def sanitize_for_storage(value: Any):
    """Return a sanitized value and report, or fail before persistence."""
    if _redact_secrets is None:
        raise SecretSanitizerUnavailable(
            "shared credential sanitizer is unavailable"
        )
    return _redact_secrets(value)


def safe_error_text(value: Any) -> str:
    """Format an exception without ever falling back to its raw value."""
    try:
        safe_value, _report = sanitize_for_storage(str(value))
        return str(safe_value)
    except SecretSanitizerUnavailable:
        return f"<{type(value).__name__}>"


class CredentialRedactionFilter(logging.Filter):
    """Sanitize a LogRecord before any Bridge handler persists it."""

    def filter(self, record: logging.LogRecord) -> bool:
        access_args = record.args
        is_uvicorn_access = (
            record.name == "uvicorn.access"
            and isinstance(access_args, tuple)
            and len(access_args) == 5
        )
        try:
            if is_uvicorn_access:
                safe_message, _report = sanitize_for_storage(record.msg)
                safe_args, _args_report = sanitize_for_storage(access_args)
                if not isinstance(safe_args, tuple) or len(safe_args) != 5:
                    raise ValueError("invalid sanitized uvicorn access arguments")
                record.msg = safe_message
                record.args = safe_args
            else:
                message = record.getMessage()
                if record.exc_info:
                    traceback_text = "".join(traceback.format_exception(*record.exc_info))
                    message = f"{message}\n{traceback_text}"
                safe_message, _report = sanitize_for_storage(message)
                record.msg = str(safe_message)
                record.args = ()
        except Exception:
            if is_uvicorn_access:
                record.msg = "%s - %s %s HTTP/%s %s"
                record.args = ("<redacted>", "<redacted>", "<redacted>", "0", 0)
            else:
                record.msg = "<redacted:log-record>"
                record.args = ()
        if record.name.startswith("uvicorn.") and not is_uvicorn_access:
            record.__dict__.pop("color_message", None)
        record.exc_info = None
        record.exc_text = None
        return True


def install_log_redaction() -> None:
    """Attach one fail-closed credential filter to every active handler."""
    loggers = [logging.getLogger()]
    loggers.extend(
        item for item in logging.root.manager.loggerDict.values()
        if isinstance(item, logging.Logger)
    )
    for active_logger in loggers:
        for handler in active_logger.handlers:
            if not any(isinstance(item, CredentialRedactionFilter)
                       for item in handler.filters):
                handler.addFilter(CredentialRedactionFilter())
