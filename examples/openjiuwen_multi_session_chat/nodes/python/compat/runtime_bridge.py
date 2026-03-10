#!/usr/bin/env python3
import importlib
import json
import os
import sys
import time
import uuid
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Any, Dict, Optional


def _project_root() -> Path:
    return Path(__file__).resolve().parents[5]


def _load_python_databus():
    sdk_path = _project_root() / "publish" / "sdk" / "python"
    if str(sdk_path) not in sys.path:
        sys.path.insert(0, str(sdk_path))
    module = importlib.import_module("agentflow_databus")
    return module.FileDataBus


FileDataBus = _load_python_databus()


@dataclass
class OpenJiuwenModules:
    available: bool
    reason: str
    workflow: Optional[Any] = None
    llm: Optional[Any] = None
    runner: Optional[Any] = None
    single_agent: Optional[Any] = None
    application: Optional[Any] = None


@dataclass
class RuntimeBinding:
    request_id: str = ""
    user_id: str = ""
    client_id: str = ""
    agent_id: str = ""
    session_id: str = ""
    task_id: str = ""
    workflow_id: str = ""
    workflow_instance_id: str = ""
    node_id: str = ""
    master_id: str = ""
    worker_id: str = ""
    output_format: str = ""

    def to_dict(self) -> Dict[str, str]:
        return {key: value for key, value in asdict(self).items() if value}


def load_openjiuwen_modules() -> OpenJiuwenModules:
    try:
        workflow = importlib.import_module("openjiuwen.core.workflow")
        llm = importlib.import_module("openjiuwen.core.foundation.llm")
        runner = importlib.import_module("openjiuwen.core.runner.runner")
        single_agent = importlib.import_module("openjiuwen.core.single_agent.legacy")
        application = importlib.import_module("openjiuwen.core.application.workflow_agent")
        return OpenJiuwenModules(
            available=True,
            reason="openjiuwen import succeeded",
            workflow=workflow,
            llm=llm,
            runner=runner,
            single_agent=single_agent,
            application=application,
        )
    except ModuleNotFoundError as exc:
        return OpenJiuwenModules(available=False, reason=f"openjiuwen not installed: {exc}")
    except ImportError as exc:
        return OpenJiuwenModules(available=False, reason=f"openjiuwen import failed: {exc}")


def package_root() -> Path:
    return Path(__file__).resolve().parents[2] / "package"


def prompt_dir() -> Path:
    return package_root() / "prompts"


def runtime_dir() -> Path:
    return package_root() / "runtime"


def env_text(name: str, default: str = "") -> str:
    return os.environ.get(name, default).strip()


def env_bool(name: str, default: bool = False) -> bool:
    value = os.environ.get(name)
    if value is None:
        return default
    return value.strip().lower() in {"1", "true", "yes", "on"}


def parse_json_blob(value: str, default: Any) -> Any:
    value = (value or "").strip()
    if not value:
        return default
    try:
        return json.loads(value)
    except json.JSONDecodeError:
        return default


def emit_json(payload: Dict[str, Any]) -> None:
    print(json.dumps(payload, ensure_ascii=False))


def databus_root() -> Optional[str]:
    return os.environ.get("AF_DATABUS_DIR") or None


class DataBusBridge:
    def __init__(self) -> None:
        self._bus = FileDataBus(databus_root()) if databus_root() else None

    def available(self) -> bool:
        return self._bus is not None

    def get(self, key: str, default: str = "") -> str:
        if not self._bus:
            return default
        value = self._bus.get(key)
        return default if value is None else str(value)

    def set(self, key: str, value: str, value_type: str = "string") -> None:
        if self._bus:
            self._bus.set(key, value, value_type=value_type)

    def publish(self, topic: str, value: str, value_type: str = "string") -> None:
        if self._bus:
            self._bus.publish(topic, value, value_type=value_type)

    def request_llm(self, request_id: str, prompt: str, channel: Optional[str] = None) -> None:
        if self._bus:
            self._bus.request_llm(request_id, prompt, channel=channel)

    def next_event(self, topic: str, timeout_ms: int = 2500) -> str:
        if not self._bus:
            return ""
        value = self._bus.next_event(topic, timeout_ms=timeout_ms)
        return "" if value is None else str(value)

    def set_json(self, key: str, payload: Dict[str, Any]) -> None:
        self.set(key, json.dumps(payload, ensure_ascii=False, separators=(",", ":")))

    def publish_json(self, topic: str, payload: Dict[str, Any]) -> None:
        self.publish(topic, json.dumps(payload, ensure_ascii=False, separators=(",", ":")))

    def bind_runtime_state(self, namespace: str, payload: Dict[str, Any], binding: Optional["RuntimeBinding"] = None) -> None:
        runtime_binding = binding or current_runtime_binding()
        merged = {"namespace": namespace, **runtime_binding.to_dict(), **payload}
        self.set_json(f"agentflow:openjiuwen:{namespace}", merged)
        if runtime_binding.session_id:
            self.set_json(f"session:{runtime_binding.session_id}:openjiuwen:{namespace}", merged)
        if runtime_binding.workflow_instance_id:
            self.set_json(f"workflow_instance:{runtime_binding.workflow_instance_id}:openjiuwen:{namespace}", merged)
        if runtime_binding.worker_id:
            self.set_json(f"worker:{runtime_binding.worker_id}:openjiuwen:{namespace}", merged)

    def publish_lifecycle(self, stage: str, payload: Dict[str, Any], binding: Optional["RuntimeBinding"] = None) -> None:
        runtime_binding = binding or current_runtime_binding()
        merged = {"stage": stage, **runtime_binding.to_dict(), **payload}
        self.publish_json(f"agentflow.openjiuwen.{stage}", merged)

    def store_reasoner_result(self, request_id: str, payload: Dict[str, Any], binding: Optional["RuntimeBinding"] = None) -> None:
        runtime_binding = binding or current_runtime_binding()
        merged = {"request_id": request_id, **runtime_binding.to_dict(), **payload}
        self.set_json(f"agentflow.openjiuwen.response.{request_id}", merged)
        self.bind_runtime_state("last_response", merged, runtime_binding)


def llm_env() -> Dict[str, str]:
    return {
        "api_base": os.environ.get("AF_LLM_BASE_URL", "").strip(),
        "api_key": os.environ.get("AF_LLM_API_KEY", "").strip(),
        "model": os.environ.get("AF_LLM_MODEL", "").strip(),
        "provider": os.environ.get("MODEL_PROVIDER", "").strip(),
        "ssl_verify": os.environ.get("LLM_SSL_VERIFY", "false").strip(),
        "timeout_sec": os.environ.get("AF_LLM_TIMEOUT_SEC", "3").strip(),
    }


def build_request_id(prefix: str) -> str:
    return f"{prefix}_{int(time.time() * 1000)}_{uuid.uuid4().hex[:8]}"


def current_session_id() -> str:
    return env_text("AF_SESSION_ID", "default")


def current_user_text() -> str:
    return env_text("AF_USER_TEXT") or env_text("AF_INPUT")


def current_runtime_binding() -> RuntimeBinding:
    return RuntimeBinding(
        request_id=env_text("AF_REQUEST_ID"),
        user_id=env_text("AF_USER_ID"),
        client_id=env_text("AF_CLIENT_ID"),
        agent_id=env_text("AF_AGENT_ID"),
        session_id=current_session_id(),
        task_id=env_text("AF_TASK_ID"),
        workflow_id=env_text("AF_WORKFLOW_ID"),
        workflow_instance_id=env_text("AF_WORKFLOW_INSTANCE_ID"),
        node_id=env_text("AF_NODE_ID"),
        master_id=env_text("AF_MASTER_ID"),
        worker_id=env_text("AF_WORKER_ID"),
        output_format=env_text("AF_OUTPUT_FORMAT"),
    )
