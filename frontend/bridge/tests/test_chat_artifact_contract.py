"""Contract tests for artifact URLs in REST and WebSocket chat content."""

from __future__ import annotations

import pytest

from routes import chat, chat_rest


RELATIVE = (
    "结果：![look](/api/artifacts/outfit-assistant/files/"
    "outfit_assistant/outputs/task-1/look.png)"
)


@pytest.mark.parametrize(
    "publicizer",
    [chat._publicize_artifact_urls, chat_rest._publicize_artifact_urls],
    ids=["websocket", "rest"],
)
def test_public_and_relative_artifact_url_modes(
    monkeypatch: pytest.MonkeyPatch,
    publicizer,
) -> None:
    """TC-brg-013: REST 与 WS 使用一致的 Artifact 公网 URL 规则。

    业务规则:
    - 未配置公网地址时保留相对 URL；配置后返回外部可访问的绝对 URL。
    - REST 与 WS 不能出现不同配置语义。

    错误模型:
    - 只修改一个聊天入口；尾部斜杠造成 ``//api``；重复改写绝对 URL。

    失败意味着:
    - 内置 UI 或外部服务至少有一个入口无法访问生成产物。
    """
    monkeypatch.setattr(chat.settings, "artifact_public_base_url", "")
    assert publicizer(RELATIVE) == RELATIVE

    monkeypatch.setattr(
        chat.settings,
        "artifact_public_base_url",
        "https://agent.example.com/",
    )
    absolute = publicizer(RELATIVE)
    assert "https://agent.example.com/api/artifacts/" in absolute
    assert "example.com//api" not in absolute
    assert "/Users/" not in absolute

    assert publicizer(absolute) == absolute
