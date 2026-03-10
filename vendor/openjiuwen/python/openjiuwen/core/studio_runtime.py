import json
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Dict, List, Tuple


@dataclass
class StudioAgentBundle:
    manifest: Dict[str, Any]
    workflows: List[Dict[str, Any]] = field(default_factory=list)

    @property
    def agent_name(self) -> str:
        return str(self.manifest.get('agent_name') or self.manifest.get('name') or 'OpenJiuwen Studio Agent')

    @property
    def agent_id(self) -> str:
        return str(self.manifest.get('agent_id') or 'studio-agent')

    @property
    def version(self) -> str:
        return str(self.manifest.get('agent_version') or self.manifest.get('version') or '1.0.0')

    @property
    def opening_remarks(self) -> str:
        return str(self.manifest.get('opening_remarks') or '你好，我是 OpenJiuwen Studio Agent。')

    @property
    def default_response(self) -> str:
        return str(self.manifest.get('default_response') or 'OpenJiuwen Studio Agent 已收到你的消息。')

    def workflow_names(self) -> List[str]:
        names: List[str] = []
        for workflow in self.workflows:
            names.append(str(workflow.get('name') or workflow.get('workflow_id') or 'studio-workflow'))
        return names

    def plugin_names(self) -> List[str]:
        plugins = self.manifest.get('plugins') or []
        return [str(item.get('name') or item.get('id') or 'plugin') for item in plugins]

    def knowledge_names(self) -> List[str]:
        return [str(item) for item in (self.manifest.get('knowledge') or [])]

    def memory_strategy(self) -> str:
        memory = self.manifest.get('memory') or {}
        return str(memory.get('strategy') or 'none')

    def max_turns(self) -> int:
        memory = self.manifest.get('memory') or {}
        turns = memory.get('max_turns')
        return int(turns) if isinstance(turns, int) else 0


@dataclass
class StudioResponse:
    text: str
    metadata: Dict[str, str] = field(default_factory=dict)


class StudioAgentRuntime:
    def __init__(self, storage_root: Path, bundle_root: Path):
        self.storage_root = Path(storage_root)
        self.bundle_root = Path(bundle_root)
        self.history_dir = self.storage_root / 'history'
        self.state_dir = self.storage_root / 'state'
        self.history_dir.mkdir(parents=True, exist_ok=True)
        self.state_dir.mkdir(parents=True, exist_ok=True)
        self.bundle = self._load_bundle(self.bundle_root)

    def chat(self, session_id: str, user_text: str, mode: str = 'dispatcher', user_id: str = '', client_id: str = '') -> StudioResponse:
        history = self._load_history(session_id)
        bootstrap = self.bootstrap(session_id)
        reply = self._build_reply(session_id, user_text, history, mode)
        history.append({'role': 'user', 'text': user_text, 'user_id': user_id, 'client_id': client_id})
        history.append({'role': 'assistant', 'text': reply, 'mode': mode})
        self._save_history(session_id, history)
        metadata = {
            'provider': 'vendored-openjiuwen-studio',
            'session_id': session_id,
            'agent_id': self.bundle.agent_id,
            'agent_version': self.bundle.version,
            'workflow_count': str(len(self.bundle.workflows)),
            'plugin_count': str(len(self.bundle.plugin_names())),
            **bootstrap,
        }
        return StudioResponse(reply, metadata)

    def summarize_context(self, session_id: str) -> str:
        history = self._load_history(session_id)
        if not history:
            return f'{self.bundle.agent_name} 当前没有可汇总的上下文。'
        last_user = ''
        last_assistant = ''
        for item in reversed(history):
            role = item.get('role')
            if role == 'assistant' and not last_assistant:
                last_assistant = item.get('text', '')
            if role == 'user' and not last_user:
                last_user = item.get('text', '')
            if last_user and last_assistant:
                break
        return (
            f'会话 {session_id} 最近用户消息：{last_user or "<none>"}；'
            f'最近助手响应：{last_assistant or "<none>"}；'
            f'记忆策略：{self.bundle.memory_strategy()}。'
        )

    def export_history(self, session_id: str) -> List[Dict[str, str]]:
        return self._load_history(session_id)

    def describe_bundle(self) -> Dict[str, Any]:
        workflow_names = self.bundle.workflow_names()
        node_count = 0
        edge_count = 0
        for workflow in self.bundle.workflows:
            schema = workflow.get('schema') or {}
            node_count += len(schema.get('nodes') or [])
            edge_count += len(schema.get('edges') or [])
        return {
            'agent_id': self.bundle.agent_id,
            'agent_name': self.bundle.agent_name,
            'version': self.bundle.version,
            'workflow_names': workflow_names,
            'workflow_count': len(workflow_names),
            'plugin_names': self.bundle.plugin_names(),
            'plugin_count': len(self.bundle.plugin_names()),
            'knowledge': self.bundle.knowledge_names(),
            'memory_strategy': self.bundle.memory_strategy(),
            'max_turns': self.bundle.max_turns(),
            'node_count': node_count,
            'edge_count': edge_count,
        }

    def bootstrap(self, session_id: str) -> Dict[str, str]:
        state_path = self.state_dir / f'{self._sanitize_session_id(session_id)}.json'
        if state_path.exists():
            return {'bootstrapped': 'reused', 'state_path': str(state_path)}
        payload = {
            'session_id': session_id,
            'bundle': self.describe_bundle(),
            'bootstrapped_at_ms': int(time.time() * 1000),
        }
        state_path.write_text(json.dumps(payload, ensure_ascii=False, indent=2), encoding='utf-8')
        return {'bootstrapped': 'created', 'state_path': str(state_path)}

    def _build_reply(self, session_id: str, user_text: str, history: List[Dict[str, str]], mode: str) -> str:
        text = (user_text or '').strip()
        lowered = text.lower()
        bundle_info = self.describe_bundle()
        workflow_names = '、'.join(bundle_info['workflow_names']) or 'studio workflow'
        plugin_names = '、'.join(bundle_info['plugin_names']) or '无插件'
        knowledge_names = '、'.join(bundle_info['knowledge']) or '无知识库'
        if not text:
            return self.bundle.opening_remarks
        if lowered in {'/help', 'help'}:
            return '可直接输入问题；输入 /status 查看 bundle 状态；输入“总结上一条消息”验证 Studio bundle 的会话记忆。'
        if lowered in {'/status', 'status'}:
            return (
                f'{self.bundle.agent_name} 已通过适配器加载 Studio bundle，当前会话 {session_id}，'
                f'入口工作流 {workflow_names}，插件 {plugin_names}，记忆策略 {bundle_info["memory_strategy"]}。'
            )
        if any(token in text for token in ['你好', '您好', '嗨']) or any(token in lowered for token in ['hello', 'hi']):
            return (
                f'你好，我是 {self.bundle.agent_name}。当前示例把 agent-studio 编排结果整体打进 AgentFlow agent 包，'
                '由默认 dispatcher/coordinator/skill/contextupdater 壳和适配器 node 共同运行。'
            )
        if '总结上一条消息' in text or '总结上条消息' in text or '上一条消息' in text:
            previous_user = ''
            for item in reversed(history):
                if item.get('role') == 'user':
                    previous_user = item.get('text', '')
                    break
            if previous_user:
                return f'你上一条消息是：{previous_user}。Studio adapter 已将它保存在会话记忆中。'
            return '当前会话里还没有上一条用户消息可供总结。'
        if '介绍' in text or '编排' in text or 'studio' in lowered:
            return (
                f'Studio bundle 中包含 {bundle_info["workflow_count"]} 个工作流、{bundle_info["node_count"]} 个节点、'
                f'{bundle_info["edge_count"]} 条连接，插件为 {plugin_names}，知识域为 {knowledge_names}。'
            )
        if '插件' in text or 'plugin' in lowered or '工具' in text:
            return f'当前 bundled Studio agent 暴露的插件能力包括：{plugin_names}。这些能力通过包内适配器转换给 AgentFlow。'
        if 'memory' in lowered or '记忆' in text or '上下文' in text:
            return (
                f'{self.bundle.agent_name} 使用 {bundle_info["memory_strategy"]} 记忆策略，最多保留 {bundle_info["max_turns"]} 轮，'
                f'contextupdater 会输出会话摘要。'
            )
        prefix = 'Coordinator 已归一化 Studio 输出：' if mode == 'coordinator' else ''
        return f'{prefix}{self.bundle.default_response} 你的输入是：{text}'

    def _load_bundle(self, bundle_root: Path) -> StudioAgentBundle:
        manifest = json.loads((bundle_root / 'agent_manifest.json').read_text(encoding='utf-8'))
        workflows: List[Dict[str, Any]] = []
        workflow_dir = bundle_root / 'workflows'
        for path in sorted(workflow_dir.glob('*.json')):
            workflows.append(json.loads(path.read_text(encoding='utf-8')))
        return StudioAgentBundle(manifest=manifest, workflows=workflows)

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


_RUNTIME_CACHE: Dict[Tuple[str, str], StudioAgentRuntime] = {}


def get_studio_runtime(storage_root: str, bundle_root: str) -> StudioAgentRuntime:
    key = (str(Path(storage_root).resolve()), str(Path(bundle_root).resolve()))
    runtime = _RUNTIME_CACHE.get(key)
    if runtime is None:
        runtime = StudioAgentRuntime(Path(storage_root), Path(bundle_root))
        _RUNTIME_CACHE[key] = runtime
    return runtime
