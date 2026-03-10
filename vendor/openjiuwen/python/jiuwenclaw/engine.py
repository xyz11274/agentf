import json
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Dict, List, Optional


@dataclass
class ClawResponse:
    text: str
    metadata: Dict[str, str] = field(default_factory=dict)


class JiuwenClawRuntime:
    def __init__(self, storage_root: Path):
        self.storage_root = Path(storage_root)
        self.history_dir = self.storage_root / 'history'
        self.state_dir = self.storage_root / 'state'
        self.history_dir.mkdir(parents=True, exist_ok=True)
        self.state_dir.mkdir(parents=True, exist_ok=True)

    def bootstrap(self, session_id: str) -> Dict[str, str]:
        state_path = self.state_dir / f'{self._sanitize_session_id(session_id)}.json'
        if state_path.exists():
            return {'bootstrapped': 'reused', 'state_path': str(state_path)}
        payload = {
            'session_id': session_id,
            'bootstrapped_at_ms': int(time.time() * 1000),
            'provider': 'vendored-jiuwenclaw',
        }
        state_path.write_text(json.dumps(payload, ensure_ascii=False, indent=2), encoding='utf-8')
        return {'bootstrapped': 'created', 'state_path': str(state_path)}

    def chat(self, session_id: str, user_text: str, user_id: str = '', client_id: str = '') -> ClawResponse:
        history = self._load_history(session_id)
        bootstrap = self.bootstrap(session_id)
        reply = self._build_reply(session_id, user_text, history)
        history.append({'role': 'user', 'text': user_text, 'user_id': user_id, 'client_id': client_id})
        history.append({'role': 'assistant', 'text': reply})
        self._save_history(session_id, history)
        metadata = {
            'provider': 'vendored-jiuwenclaw',
            'session_id': session_id,
            **bootstrap,
        }
        return ClawResponse(text=reply, metadata=metadata)

    def export_history(self, session_id: str) -> List[Dict[str, str]]:
        return self._load_history(session_id)

    def _build_reply(self, session_id: str, user_text: str, history: List[Dict[str, str]]) -> str:
        text = (user_text or '').strip()
        lowered = text.lower()
        if not text:
            return 'ClawAgent 已启动，请先输入你想交给 jiuwenclaw 的任务。'
        if lowered in {'/help', 'help'}:
            return '可直接输入问题；输入 /quit 退出；输入 “总结上一条消息” 可验证会话记忆。'
        if lowered in {'/status', 'status'}:
            return f'ClawAgent 正在使用 vendored jiuwenclaw 运行，当前会话为 {session_id}。'
        if any(token in lowered for token in ['hello', 'hi']) or any(token in text for token in ['你好', '嗨', '您好']):
            return '你好，我是 ClawAgent。当前示例展示 dispatcher 直连 jiuwenclaw、TUI 交互和会话记忆。'
        if '总结上一条消息' in text or '总结上条消息' in text or '上一条消息' in text:
            previous_user = ''
            for item in reversed(history):
                if item.get('role') == 'user':
                    previous_user = item.get('text', '')
                    break
            if previous_user:
                return f'你上一条消息是：{previous_user}。基于 vendored jiuwenclaw，我建议从这个意图继续细化任务。'
            return '当前会话里还没有上一条用户消息可供总结。'
        if '任务' in text or 'todo' in lowered:
            return f'ClawAgent 已记录你的任务意图：{text}。这是通过 dispatcher workflow 直连 jiuwenclaw 返回的示例回复。'
        return f'ClawAgent 通过 vendored jiuwenclaw 收到你的消息：{text}'

    def _history_path(self, session_id: str) -> Path:
        return self.history_dir / f'{self._sanitize_session_id(session_id)}.json'

    def _load_history(self, session_id: str) -> List[Dict[str, str]]:
        path = self._history_path(session_id)
        if not path.exists():
            return []
        try:
            return json.loads(path.read_text(encoding='utf-8'))
        except json.JSONDecodeError:
            return []

    def _save_history(self, session_id: str, history: List[Dict[str, str]]) -> None:
        self._history_path(session_id).write_text(json.dumps(history, ensure_ascii=False, indent=2), encoding='utf-8')

    def _sanitize_session_id(self, session_id: str) -> str:
        return ''.join(ch if ch.isalnum() or ch in {'-', '_'} else '_' for ch in session_id) or 'default'


_RUNTIME_CACHE: Dict[str, JiuwenClawRuntime] = {}


def get_runtime(storage_root: str) -> JiuwenClawRuntime:
    storage_root = str(Path(storage_root).resolve())
    runtime = _RUNTIME_CACHE.get(storage_root)
    if runtime is None:
        runtime = JiuwenClawRuntime(Path(storage_root))
        _RUNTIME_CACHE[storage_root] = runtime
    return runtime
