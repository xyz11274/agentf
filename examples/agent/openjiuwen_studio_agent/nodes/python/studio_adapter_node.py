#!/usr/bin/env python3
import argparse
import importlib
import json
import os
import sys
from pathlib import Path
from typing import Any, Dict


PROJECT_ROOT = Path(__file__).resolve().parents[5]
SDK_PATH = PROJECT_ROOT / 'publish' / 'sdk' / 'python'
VENDOR_PATH = PROJECT_ROOT / 'vendor' / 'openjiuwen' / 'python'
for path in (SDK_PATH, VENDOR_PATH):
    if str(path) not in sys.path:
        sys.path.insert(0, str(path))


FileDataBus = importlib.import_module('agentflow_databus').FileDataBus
from openjiuwen.core.studio_runtime import get_studio_runtime  # noqa: E402


class DataBusBridge:
    def __init__(self) -> None:
        databus_dir = Path(os.environ.get('AF_DATABUS_DIR', Path.cwd() / '.agentflow_databus')).resolve()
        databus_dir.mkdir(parents=True, exist_ok=True)
        self._bus = FileDataBus(str(databus_dir))

    def get(self, key: str, default: str = '') -> str:
        value = self._bus.get(key)
        return default if value is None else str(value)

    def set(self, key: str, value: str) -> None:
        self._bus.set(key, value)


def env_text(name: str, default: str = '') -> str:
    return os.environ.get(name, '').strip() or default


def package_root() -> Path:
    return Path(__file__).resolve().parents[2]


def bundle_root() -> Path:
    return package_root() / 'studio_bundle'


def storage_root() -> Path:
    root = package_root() / 'runtime'
    root.mkdir(parents=True, exist_ok=True)
    return root


def current_session_id() -> str:
    return env_text('AF_SESSION_ID', 'studio-default')


def current_user_text() -> str:
    return env_text('AF_USER_TEXT') or env_text('AF_INPUT')


def emit_json(payload: Dict[str, Any]) -> None:
    print(json.dumps(payload, ensure_ascii=False))


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument('--mode', default='dispatcher', choices=['dispatcher', 'coordinator', 'skill', 'contextupdater'])
    args = parser.parse_args()

    session_id = current_session_id()
    user_text = current_user_text()
    user_id = env_text('AF_USER_ID', 'studio-user')
    client_id = env_text('AF_CLIENT_ID', 'studio-tui')
    databus = DataBusBridge()
    runtime = get_studio_runtime(str(storage_root()), str(bundle_root()))
    bundle_info = runtime.describe_bundle()

    trace = {
        'mode': args.mode,
        'provider': 'vendored-openjiuwen-studio',
        'session_id': session_id,
        'agent_id': bundle_info['agent_id'],
        'workflow_count': bundle_info['workflow_count'],
        'plugin_count': bundle_info['plugin_count'],
        'memory_strategy': bundle_info['memory_strategy'],
    }

    if args.mode == 'contextupdater':
        summary = runtime.summarize_context(session_id)
        databus.set('context_summary', summary)
        databus.set(f'studio:session:{session_id}:context_summary', summary)
        emit_json({'context_summary': summary, 'studio_trace': json.dumps(trace, ensure_ascii=False)})
        return

    response = runtime.chat(session_id, user_text, mode=args.mode, user_id=user_id, client_id=client_id)
    history = runtime.export_history(session_id)
    databus.set('response', response.text)
    databus.set(f'studio:session:{session_id}:last_response', response.text)
    databus.set(f'studio:session:{session_id}:history', json.dumps(history, ensure_ascii=False))
    databus.set(f'studio:session:{session_id}:bundle', json.dumps(bundle_info, ensure_ascii=False))
    trace.update({
        'history_length': len(history),
        'bootstrapped': response.metadata.get('bootstrapped', ''),
    })
    emit_json({'response': response.text, 'studio_trace': json.dumps(trace, ensure_ascii=False)})


if __name__ == '__main__':
    main()
