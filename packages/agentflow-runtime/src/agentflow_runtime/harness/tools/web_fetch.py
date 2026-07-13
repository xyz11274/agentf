"""web_fetch tool — fetch a URL and return its content as text."""

import json

TOOL_NAME = "web_fetch"
TOOL_DESCRIPTION = "Fetch a URL and return its content as plain text. Supports http and https URLs. Useful for reading documentation, API responses, or web pages."
TOOL_PARAMS = {
    "type": "object",
    "properties": {
        "url": {
            "type": "string",
            "description": "URL to fetch (http or https only)"
        },
        "max_chars": {
            "type": "integer",
            "description": "Maximum characters to return (default 5000, use -1 for no limit)",
            "default": 5000
        },
        "timeout": {
            "type": "integer",
            "description": "Timeout in seconds (default 15)",
            "default": 15
        }
    },
    "required": ["url"]
}


def execute(tool_input: dict) -> str:
    url = tool_input.get("url", "")
    max_chars = tool_input.get("max_chars", 5000)
    timeout = tool_input.get("timeout", 15)

    if not url:
        return json.dumps({"error": "url is required", "tool": "web_fetch"},
                          ensure_ascii=False)

    # Security: only allow http/https
    if not url.startswith("http://") and not url.startswith("https://"):
        return json.dumps({
            "error": "Only http and https URLs are allowed",
            "tool": "web_fetch"
        }, ensure_ascii=False)

    # Block private/internal addresses
    blocked_prefixes = [
        "http://localhost", "https://localhost",
        "http://127.0.0.1", "https://127.0.0.1",
        "http://10.", "https://10.",
        "http://172.16.", "https://172.16.",
        "http://192.168.", "https://192.168.",
        "http://169.254.", "https://169.254.",
        "http://[::1]", "https://[::1]",
        "http://0.0.0.0", "https://0.0.0.0",
    ]
    for prefix in blocked_prefixes:
        if url.startswith(prefix):
            return json.dumps({
                "error": f"Access denied: requests to private/internal addresses are not allowed",
                "tool": "web_fetch"
            }, ensure_ascii=False)

    # Metadata endpoints
    blocked_metadata = [
        "169.254.169.254",  # cloud metadata
        "metadata.google.internal",
        "metadata.aws.internal",
    ]
    for blocked in blocked_metadata:
        if blocked in url:
            return json.dumps({
                "error": f"Access denied: requests to metadata endpoints are not allowed",
                "tool": "web_fetch"
            }, ensure_ascii=False)

    try:
        import urllib.request
        import ssl

        # Create SSL context (skip certificate verification to avoid
        # certificate issues in various environments)
        ssl_ctx = ssl._create_unverified_context()

        req = urllib.request.Request(
            url,
            headers={
                "User-Agent": "AgentFlow-HarnessAgent/1.0",
                "Accept": "text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8",
            }
        )

        with urllib.request.urlopen(req, timeout=timeout, context=ssl_ctx) as resp:
            raw = resp.read()
            content_type = resp.headers.get("Content-Type", "")
            charset = "utf-8"

            # Try to extract charset from Content-Type
            if "charset=" in content_type:
                charset = content_type.split("charset=")[-1].split(";")[0].strip()

            try:
                text = raw.decode(charset, errors="replace")
            except (LookupError, UnicodeDecodeError):
                text = raw.decode("utf-8", errors="replace")

            # Strip HTML tags for cleaner output if it's HTML
            if "text/html" in content_type:
                import re
                # Remove script and style elements
                text = re.sub(r'<script[^>]*>.*?</script>', '', text, flags=re.DOTALL | re.IGNORECASE)
                text = re.sub(r'<style[^>]*>.*?</style>', '', text, flags=re.DOTALL | re.IGNORECASE)
                # Remove HTML tags
                text = re.sub(r'<[^>]+>', ' ', text)
                # Collapse whitespace
                text = re.sub(r'\s+', ' ', text).strip()
                # Restore newlines for readability
                text = text.replace('. ', '.\n').replace('? ', '?\n').replace('! ', '!\n')

            status = resp.status

        truncated = False
        if max_chars > 0 and len(text) > max_chars:
            text = text[:max_chars] + f"\n... (truncated at {max_chars} chars)"
            truncated = True

        return json.dumps({
            "content": text,
            "char_count": len(text),
            "content_type": content_type,
            "status": status,
            "url": url,
            "truncated": truncated,
            "tool": "web_fetch",
        }, ensure_ascii=False)

    except TimeoutError:
        return json.dumps({
            "error": f"Request timed out after {timeout}s",
            "tool": "web_fetch"
        }, ensure_ascii=False)
    except urllib.error.HTTPError as e:
        return json.dumps({
            "error": f"HTTP {e.code}: {e.reason}",
            "status": e.code,
            "tool": "web_fetch"
        }, ensure_ascii=False)
    except urllib.error.URLError as e:
        return json.dumps({
            "error": f"URL error: {e.reason}",
            "tool": "web_fetch"
        }, ensure_ascii=False)
    except ImportError:
        return json.dumps({
            "error": "urllib is not available",
            "tool": "web_fetch"
        }, ensure_ascii=False)
    except Exception as e:
        return json.dumps({
            "error": f"Failed to fetch URL: {e}",
            "tool": "web_fetch"
        }, ensure_ascii=False)
