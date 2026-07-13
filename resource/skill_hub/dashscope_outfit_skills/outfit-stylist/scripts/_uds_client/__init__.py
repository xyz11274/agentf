"""UDS LLM 客户端库 — outfit-stylist skill 自包含拷贝（UDS-only 子集）。

拷自 nodes/common/clients/，仅保留 UDS 通道（base.py + llm_uds_client.py），
不含 OpenAiCompatibleClient（避免拉进 `requests` 依赖，守 SKILL.md 的
"Zero pip dependencies" 约束）。

用法：
    from _uds_client import create_llm_client
    client = create_llm_client(model="aisf/qwen-vl-max")
    resp = client.chat_completion(messages=[{"role": "user", "content": [...]}])
"""

from .base import LlmClient, resolve_llm_config, create_llm_client, _ensure_aisf_model
from .llm_uds_client import LlmUdsClient, LlmUdsError

__all__ = [
    "LlmClient",
    "resolve_llm_config",
    "create_llm_client",
    "_ensure_aisf_model",
    "LlmUdsClient",
    "LlmUdsError",
]
