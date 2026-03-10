#!/usr/bin/env python3
import json
from pathlib import Path
from typing import Dict, List

from .runtime_bridge import DataBusBridge, current_session_id, current_user_text, prompt_dir
from .skill_adapter import is_greeting
from .workflow_adapter import OpenJiuwenWorkflowAdapter


def read_prompt_template() -> str:
    prompt_file = prompt_dir() / "session_summary_prompt.txt"
    if not prompt_file.exists():
        return "请基于历史消息生成简洁的会话摘要。"
    return prompt_file.read_text(encoding="utf-8").strip()


def read_history_json(session_id: str, databus: DataBusBridge) -> str:
    return databus.get(f"session:{session_id}:history_json", "[]")


def history_tail(history_json: str, limit: int = 8) -> str:
    try:
        items = json.loads(history_json or "[]")
    except json.JSONDecodeError:
        return ""
    if not isinstance(items, list):
        return ""
    lines: List[str] = []
    for item in items[-limit:]:
        if not isinstance(item, dict):
            continue
        role = (item.get("role") or "unknown").strip()
        content = (item.get("content") or "").strip()
        if content:
            lines.append(f"{role}: {content}")
    return "\n".join(lines)


def summarize_history(prompt_template: str, history_tail_text: str) -> Dict[str, str]:
    adapter = OpenJiuwenWorkflowAdapter("session-context-summary", "Summarize session history")
    prompt = json.dumps(
        {
            "task": "总结当前会话历史，保留用户近期目标、上下文约束和已完成结果。",
            "prompt_template": prompt_template,
            "history_tail": history_tail_text,
        },
        ensure_ascii=False,
    )
    schema = {
        "type": "object",
        "properties": {"output": {"type": "string", "description": "session history summary"}},
        "required": ["output"],
    }
    result = adapter.query_reasoner(
        prompt,
        "你是一个会话上下文整理器。输出简洁、结构化的中文摘要，不要编造事实。",
        schema,
    )
    if result.text:
        return {"history_summary": result.text, "summary_source": result.source, "summary_error": result.error}
    if not history_tail_text.strip():
        return {"history_summary": "当前会话暂无可总结历史。", "summary_source": "fallback", "summary_error": result.error}
    first_line = history_tail_text.splitlines()[-1]
    return {"history_summary": f"最近对话焦点：{first_line}", "summary_source": "fallback", "summary_error": result.error}


def fill_context(session_id: str, history_summary: str, databus: DataBusBridge) -> str:
    payload = {
        "session_id": session_id,
        "history_summary": history_summary,
        "user_text": current_user_text(),
    }
    context_text = json.dumps(payload, ensure_ascii=False)
    databus.set(f"session:{session_id}:context", context_text)
    return context_text


def build_direct_response(user_text: str, history_json: str) -> str:
    if is_greeting(user_text):
        return "你好！我可以帮你做总结、检索、日期时间计算和任务规划。"
    try:
        history = json.loads(history_json or "[]")
    except json.JSONDecodeError:
        history = []
    if "总结" in user_text or "上条消息" in user_text or "上一条" in user_text:
        contents = []
        for item in history:
            if not isinstance(item, dict):
                continue
            content = (item.get("content") or "").strip()
            if content and content != user_text:
                contents.append(content)
        if contents:
            return f"上条消息主要内容是：{contents[-1]}"
    return ""


def session_history_bundle() -> Dict[str, str]:
    databus = DataBusBridge()
    session_id = current_session_id()
    history_json = read_history_json(session_id, databus)
    return {
        "session_id": session_id,
        "history_json": history_json,
        "history_tail": history_tail(history_json),
    }
