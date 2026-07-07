#!/usr/bin/env python3
"""
Abstract LLM client interface — 所有 LLM 调用方式的抽象基类。

模型名前缀决定路由：
  ``aisf/<model>`` → UDS 通道（LlmUdsClient）
  ``<model>``      → HTTP 通道（OpenAiCompatibleClient）

节点代码只依赖 ``LlmClient``，不依赖具体实现。
"""

from __future__ import annotations

import logging
import os
from abc import ABC, abstractmethod
from typing import Any, Callable, Optional

logger = logging.getLogger("agentflow.node.llm_client")

_AISF_MODEL_PREFIX = "aisf/"


def _ensure_aisf_model(model: str) -> str:
    """确保模型名带 ``aisf/`` 前缀。"""
    m = model.strip()
    if not m.startswith(_AISF_MODEL_PREFIX):
        m = f"{_AISF_MODEL_PREFIX}{m}"
    return m


class LlmClient(ABC):
    """LLM 客户端抽象接口。

    用法：
        client = create_llm_client()
        resp = client.chat_completion(messages, tools=[...])
    """

    @abstractmethod
    def chat_completion(
        self,
        messages: list[dict[str, Any]],
        *,
        tools: list[dict[str, Any]] | None = None,
        max_completion_tokens: int = 8192,
        stream: bool = False,
        on_chunk: Callable[[str], None] | None = None,
    ) -> dict[str, Any]:
        """调用 LLM chat completion。

        Args:
            messages: OpenAI 格式的消息列表 [{role, content}, ...]
            tools: 可用的工具定义列表（OpenAI function calling 格式）
            max_completion_tokens: 最大输出 token 数
            stream: 是否启用流式输出
            on_chunk: 流式回调，每收到一个 token 时触发（仅在 stream=True 时有效）

        Returns:
            OpenAI 兼容响应字典，包含 ``choices[0].message``，
            或 ``{"error": ..., "_llm_error": true}``。
        """
        ...


def resolve_llm_config(agent_md_model: str = "") -> dict[str, str]:
    """解析 LLM 配置 (model, base_url, api_key)，含 fallback 链。

    Model:  agent.md → AF_LLM_MODEL → AF_FALLBACK_LLM_MODEL
    API key: AF_LLM_API_KEY → API_KEY → AF_FALLBACK_LLM_API_KEY
    Base URL: AF_LLM_BASE_URL → AF_FALLBACK_LLM_BASE_URL
    """
    import os

    _PLACEHOLDER_PREFIXES = ("${", "sk-your-", "your-")

    def _is_placeholder(val: str) -> bool:
        if not val:
            return True
        return any(val.startswith(p) for p in _PLACEHOLDER_PREFIXES)

    # Model
    model = agent_md_model
    if _is_placeholder(model):
        model = os.environ.get("AF_LLM_MODEL", "")
    if _is_placeholder(model):
        model = os.environ.get("AF_FALLBACK_LLM_MODEL", "")

    # API key
    api_key = os.environ.get("AF_LLM_API_KEY", "")
    if _is_placeholder(api_key):
        api_key = os.environ.get("API_KEY", "")
    if _is_placeholder(api_key):
        api_key = os.environ.get("AF_FALLBACK_LLM_API_KEY", "")

    # Base URL
    base_url = os.environ.get("AF_LLM_BASE_URL", "")
    if _is_placeholder(base_url):
        base_url = os.environ.get("AF_FALLBACK_LLM_BASE_URL", "")

    # Fallback model if empty after all resolution
    if not model:
        model = "gpt-4o-mini"

    return {"model": model, "base_url": base_url, "api_key": api_key}


def create_llm_client(
    model: str = "",
    base_url: str = "",
    api_key: str = "",
) -> LlmClient:
    """工厂方法：创建 LLM 客户端（UDS-only 子集）。

    与 nodes/common/clients/base.py 的差异：本 skill 拷贝只保留 UDS 通道，
    不含 OpenAiCompatibleClient（避免拉进 `requests` 依赖，守 SKILL.md 的
    "Zero pip dependencies" 约束）。HTTP 路径仍可由脚本自行用 urllib 走。

    路由规则：
      ``aisf/<model>`` → UDS 通道（LlmUdsClient）
      ``<model>``      → 自动补 ``aisf/`` 前缀走 UDS（不 fallback HTTP）

    环境变量 ``AF_USE_AISF_MODELS=true`` 时自动为模型名补 ``aisf/`` 前缀，
    实现一键 UDS 切换，无需修改代码。

    Args:
        model: 模型名。以 ``aisf/`` 开头走 UDS，否则自动补前缀走 UDS。
        base_url: API 地址（保留参数为兼容上游签名，本子集忽略）
        api_key: API key（保留参数为兼容上游签名，本子集忽略）

    Returns:
        LlmUdsClient
    """
    cfg = resolve_llm_config()
    model = model or cfg["model"]

    # AF_USE_AISF_MODELS=true 时自动补 aisf/ 前缀，一键切换到 UDS 通道
    if os.environ.get("AF_USE_AISF_MODELS", "").lower() in ("true", "1"):
        if not model.startswith("aisf/"):
            model = f"aisf/{model}"
            logger.info("AF_USE_AISF_MODELS=true → auto-prepended aisf/ prefix: model=%s", model)

    # 不带 aisf/ 前缀也自动补（本子集只支持 UDS，不 fallback HTTP）
    if not model.startswith("aisf/"):
        model = f"aisf/{model}"
        logger.info("auto-prepended aisf/ prefix for UDS-only subset: model=%s", model)

    from .llm_uds_client import LlmUdsClient

    logger.info("Created LlmUdsClient: model=%s", model)
    return LlmUdsClient(model=model)


__all__ = ["LlmClient", "resolve_llm_config", "create_llm_client", "_ensure_aisf_model"]
