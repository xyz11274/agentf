"""Opt-in SMTP and allowlisted webhook connectors for real external effects."""
from __future__ import annotations

import ipaddress
import json
import os
import smtplib
import socket
import ssl
from email.message import EmailMessage
from email.utils import getaddresses, make_msgid
from urllib.parse import urlsplit
from urllib.error import HTTPError
from urllib.request import HTTPRedirectHandler, Request, build_opener, urlopen

from .live_information import result


def _bool(name: str) -> bool:
    return os.environ.get(name, "").lower() in ("1", "true", "yes", "on")


def _addresses(value) -> list[str]:
    raw = value if isinstance(value, list) else [value or ""]
    parsed = [address for _name, address in getaddresses(raw) if address and "@" in address]
    return parsed


def send_email(args: dict) -> dict:
    host = os.environ.get("BUSINESS_SMTP_HOST")
    sender = os.environ.get("BUSINESS_SMTP_FROM") or os.environ.get("BUSINESS_SMTP_USERNAME")
    if not host or not sender:
        return result("SMTP 尚未配置，没有发送邮件。", status="unsupported",
                      missing_capabilities=["BUSINESS_SMTP_HOST", "BUSINESS_SMTP_FROM"], execution="not_executed")
    recipients = _addresses(args.get("to"))
    cc = _addresses(args.get("cc"))
    if not recipients:
        return result("邮件需要有效收件人。", status="needs_input", missing_fields=["to"])
    if not args.get("subject") or not args.get("content"):
        return result("邮件需要主题和正文。", status="needs_input", missing_fields=[key for key in ("subject", "content") if not args.get(key)])
    if not args.get("confirmed"):
        return result("邮件内容已准备，请确认后发送。", {"to": recipients, "cc": cc, "subject": args["subject"]},
                      status="needs_confirmation", execution="not_executed")
    if not args.get("request_id"):
        return result("真实邮件发送需要 request_id，以便审计重试。", status="needs_input", missing_fields=["request_id"])
    security = os.environ.get("BUSINESS_SMTP_SECURITY", "starttls").lower()
    port = int(os.environ.get("BUSINESS_SMTP_PORT", "465" if security == "ssl" else "587"))
    if security not in ("ssl", "starttls", "none"):
        raise ValueError("BUSINESS_SMTP_SECURITY must be ssl, starttls, or none")
    try:
        addresses = {row[4][0] for row in socket.getaddrinfo(host, port)}
    except OSError as exc:
        return result("SMTP 主机无法解析。", {"error_type": type(exc).__name__}, status="failed", execution="not_executed")
    is_local = bool(addresses) and all(ipaddress.ip_address(value).is_loopback for value in addresses)
    if security == "none" and not (is_local or _bool("BUSINESS_SMTP_ALLOW_INSECURE")):
        return result("非本机 SMTP 必须启用 TLS。", status="failed", execution="not_executed")
    message = EmailMessage()
    message["From"] = sender
    message["To"] = ", ".join(recipients)
    if cc:
        message["Cc"] = ", ".join(cc)
    message["Subject"] = args["subject"]
    domain = sender.rsplit("@", 1)[-1] if "@" in sender else "agentflow.local"
    message["Message-ID"] = make_msgid(idstring=args["request_id"], domain=domain)
    message.set_content(args["content"])
    username = os.environ.get("BUSINESS_SMTP_USERNAME")
    password = os.environ.get("BUSINESS_SMTP_PASSWORD")
    client_class = smtplib.SMTP_SSL if security == "ssl" else smtplib.SMTP
    try:
        with client_class(host, port, timeout=30, context=ssl.create_default_context()) if security == "ssl" else client_class(host, port, timeout=30) as client:
            if security == "starttls":
                client.starttls(context=ssl.create_default_context())
            if username:
                client.login(username, password or "")
            refused = client.send_message(message, to_addrs=recipients + cc)
    except (OSError, smtplib.SMTPException) as exc:
        return result("SMTP 发送失败，未取得发送回执。", {"error_type": type(exc).__name__},
                      status="failed", source="smtp", execution="not_verified", retryable=True)
    if refused:
        return result("SMTP 拒绝了部分或全部收件人。", {"refused": sorted(refused)},
                      status="failed", source="smtp", execution="partially_sent" if len(refused) < len(recipients + cc) else "not_sent")
    return result("SMTP 服务器已接受邮件。", {"to": recipients, "cc": cc, "subject": args["subject"],
                  "message_id": message["Message-ID"], "accepted": True}, source="smtp", execution="accepted_by_server")


def _allowed_webhook(url: str) -> tuple[bool, str]:
    parsed = urlsplit(url)
    if not parsed.hostname or parsed.username or parsed.password or parsed.scheme not in ("https", "http"):
        return False, "invalid_url"
    origin = f"{parsed.scheme}://{parsed.netloc}"
    allowed = set(filter(None, os.environ.get("BUSINESS_WEBHOOK_ALLOWED_ORIGINS", "").split(",")))
    configured = os.environ.get("BUSINESS_WEBHOOK_URL")
    if configured:
        base = urlsplit(configured)
        allowed.add(f"{base.scheme}://{base.netloc}")
    if origin not in allowed:
        return False, "origin_not_allowlisted"
    if parsed.scheme != "https" and not _bool("BUSINESS_WEBHOOK_ALLOW_HTTP"):
        return False, "https_required"
    try:
        addresses = {row[4][0] for row in socket.getaddrinfo(parsed.hostname, parsed.port or (443 if parsed.scheme == "https" else 80))}
    except OSError:
        return False, "dns_failed"
    if not addresses:
        return False, "dns_failed"
    if any(not ipaddress.ip_address(value).is_global for value in addresses) and not _bool("BUSINESS_WEBHOOK_ALLOW_PRIVATE"):
        return False, "private_address_blocked"
    return True, ""


class _NoRedirect(HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        return None


def post_webhook(args: dict) -> dict:
    url = args.get("url") or os.environ.get("BUSINESS_WEBHOOK_URL", "")
    if not url:
        return result("Webhook 尚未配置。", status="unsupported",
                      missing_capabilities=["BUSINESS_WEBHOOK_URL or url", "BUSINESS_WEBHOOK_ALLOWED_ORIGINS"], execution="not_executed")
    if not args.get("confirmed"):
        return result("Webhook 请求已准备，请确认后提交。", {"url": url}, status="needs_confirmation", execution="not_executed")
    if not args.get("request_id"):
        return result("Webhook 提交需要 request_id。", status="needs_input", missing_fields=["request_id"])
    allowed, reason = _allowed_webhook(url)
    if not allowed:
        return result("Webhook 地址未通过管理员允许列表。", {"reason": reason}, status="failed", execution="not_executed")
    payload = args.get("payload")
    if payload is None:
        payload = {"content": args.get("content", "")}
    raw = json.dumps(payload, ensure_ascii=False).encode()
    if len(raw) > 1024 * 1024:
        return result("Webhook 请求体超过 1 MiB。", status="failed", execution="not_executed")
    headers = {"Content-Type": "application/json", "User-Agent": "AgentFlow-Business/1.0",
               "Idempotency-Key": args["request_id"]}
    if os.environ.get("BUSINESS_WEBHOOK_TOKEN"):
        headers["Authorization"] = "Bearer " + os.environ["BUSINESS_WEBHOOK_TOKEN"]
    try:
        with build_opener(_NoRedirect).open(Request(url, data=raw, headers=headers, method="POST"), timeout=30) as reply:
            body = reply.read(64 * 1024).decode(errors="replace")
            code = reply.status
    except HTTPError as exc:
        return result("Webhook 服务返回失败状态或重定向，未跟随到其他地址。",
                      {"http_status": exc.code}, status="failed", source="webhook", execution="response_observed")
    except OSError as exc:
        return result("Webhook 调用失败。", {"error_type": type(exc).__name__}, status="failed",
                      source="webhook", execution="not_verified", retryable=True)
    return result("Webhook 服务已接受请求并返回响应。", {"url": url, "http_status": code,
                  "response": body, "idempotency_key": args["request_id"]}, source="webhook", execution="response_observed")
