import json
from dataclasses import dataclass, field
from types import SimpleNamespace
from typing import Any, Dict, List, Optional


def generate_workflow_key(workflow_id: str, version: str = '1.0') -> str:
    return f'{workflow_id}:{version}'


@dataclass
class WorkflowCard:
    id: str
    name: str = ''
    version: str = '1.0'
    description: str = ''
    input_params: Optional[Dict[str, Any]] = None


class Start:
    pass


class End:
    def __init__(self, config: Optional[Dict[str, Any]] = None):
        self.config = config or {}


@dataclass
class LLMCompConfig:
    model_client_config: Any
    model_config: Any
    template_content: List[Dict[str, str]] = field(default_factory=list)
    response_format: Optional[Dict[str, Any]] = None
    output_config: Optional[Dict[str, Any]] = None


class LLMComponent:
    def __init__(self, config: LLMCompConfig):
        self.config = config


class Workflow:
    def __init__(self, card: WorkflowCard):
        self.card = card
        self._components: Dict[str, Any] = {}
        self._inputs_schema: Dict[str, Dict[str, str]] = {}
        self._connections: List[tuple[str, str]] = []

    def set_start_comp(self, name: str, component: Any, inputs_schema: Optional[Dict[str, str]] = None):
        self._components[name] = component
        self._inputs_schema[name] = inputs_schema or {}

    def add_workflow_comp(self, name: str, component: Any, inputs_schema: Optional[Dict[str, str]] = None):
        self._components[name] = component
        self._inputs_schema[name] = inputs_schema or {}

    def set_end_comp(self, name: str, component: Any, inputs_schema: Optional[Dict[str, str]] = None):
        self._components[name] = component
        self._inputs_schema[name] = inputs_schema or {}

    def add_connection(self, src: str, dst: str):
        self._connections.append((src, dst))

    async def run(self, inputs: Dict[str, Any]):
        query = str(inputs.get('query', '')).strip()
        response_payload = {
            'response': f'vendored openJiuwen processed: {query[:160]}' if query else 'vendored openJiuwen received an empty query.'
        }
        return {'output': SimpleNamespace(result=response_payload)}
