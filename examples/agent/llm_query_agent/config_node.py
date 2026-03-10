#!/usr/bin/env python3
"""
Config Node - Reads LLM configuration from environment variables

Input:  Environment variables LLM_API_KEY, LLM_MODEL, LLM_BASE_URL
Output: JSON with api_key, model, base_url

Usage:
    python3 config_node.py

Environment Variables:
    LLM_API_KEY   - API key for LLM service (required)
    LLM_MODEL     - Model name (default: qwen3.5-plus)
    LLM_BASE_URL  - API base URL (default: Alibaba Cloud DashScope)
"""

import os
import sys
import json


def main():
    # Read configuration from environment variables
    api_key = os.environ.get("LLM_API_KEY", "")
    model = os.environ.get("LLM_MODEL", "qwen3.5-plus")
    base_url = os.environ.get(
        "LLM_BASE_URL",
        "https://dashscope.aliyuncs.com/compatible-mode/v1"
    )

    # Debug output to stderr
    print(f"[CONFIG_NODE] Model: {model}, Base URL: {base_url}", file=sys.stderr)

    if not api_key:
        print("[CONFIG_NODE] Warning: LLM_API_KEY not set", file=sys.stderr)

    # Output JSON configuration
    config = {
        "api_key": api_key,
        "model": model,
        "base_url": base_url
    }

    print(json.dumps(config))
    sys.exit(0)


if __name__ == "__main__":
    main()