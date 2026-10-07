"""Real tool execution and trace preservation for the private AppWorld adapter."""
from __future__ import annotations

import ast
import copy
import inspect
import json
import os
import subprocess
import sys
from pathlib import Path

import pytest

HERE = Path(__file__).resolve().parent
ROOT = Path(__file__).resolve().parents[4]
sys.path.insert(0, str(ROOT / "packages/agentflow-runtime/src"))
sys.path.insert(0, str(ROOT / "examples/agent/business-simulator/appworld"))

from agentflow_runtime.security import SecretLeakBlocked
from agentflow_runtime.security.secrets import _reset_registry_for_tests, register_secret
from appworld_skill import (
    TOOL_NAME,
    TOOL_PARAMS,
    install_private_reply_transport,
    project_tool_response,
)


@pytest.fixture(autouse=True)
def isolated_registry():
    _reset_registry_for_tests()
    yield
    _reset_registry_for_tests()


def response(code: str, name: str = TOOL_NAME) -> dict:
    return {
        "choices": [{"message": {"role": "assistant", "content": "",
            "tool_calls": [{"type": "function", "id": "fixture-call",
                "function": {"name": name,
                    "arguments": json.dumps({"code": code})}}]},
            "finish_reason": "tool_calls"}],
        "usage": {"prompt_tokens": 3, "completion_tokens": 4, "total_tokens": 7},
    }


def code_from(result: dict) -> str:
    return json.loads(result["choices"][0]["message"]["tool_calls"][0]
                      ["function"]["arguments"])["code"]


def projected_twice(code: str) -> str:
    first, _ = project_tool_response(response(code))
    second, _ = project_tool_response(first)
    return code_from(second)


def test_smoke_login_short_reference_survives_two_boundaries_and_executes():
    """Smoke login used fs_pw, then an escaped newline swallowed the token assignment."""
    code = (
        'login_resp = apis.file_system.login(username=username, password=fs_pw)\n'
        'access_token = login_resp["access_token"]\n'
        'entries = apis.file_system.show_directory(access_token=access_token, '
        'directory_path="/")\n'
    )
    actual_code = projected_twice(code)
    ast.parse(actual_code)
    assert actual_code == code

    class FileSystem:
        def login(self, *, username, password):
            assert username == "benchmark-user"
            assert password == "fixture-only-password"
            return {"access_token": "fixture-only-session"}

        def show_directory(self, *, access_token, directory_path):
            assert access_token == "fixture-only-session"
            assert directory_path == "/"
            return ["example.txt"]

    class APIs:
        file_system = FileSystem()

    namespace = {"apis": APIs(), "username": "benchmark-user",
                 "fs_pw": "fixture-only-password"}
    exec(compile(actual_code, "<appworld-tool>", "exec"), namespace)
    assert namespace["entries"] == ["example.txt"]


@pytest.mark.parametrize("code", [
    'apis.login(password=password, access_token=session.access_token)\n',
    'apis.login(password=passwords["file_system"], access_token=get_token())\n',
    'password = supervisor_passwords["file_system"]\n'
    'result = apis.login(password=password)\n',
    'apis.login(password="synthetic-world-password")\n',
    'apis.show(access_token=session.access_token, message="跨应用任务")\n',
])
def test_public_benchmark_code_expressions_and_literals_are_preserved(code):
    assert projected_twice(code) == code


def test_registered_provider_credential_is_blocked_in_code():
    provider_key = "fixture-provider-credential-123456"
    register_secret("llm.api_key", provider_key)
    with pytest.raises(SecretLeakBlocked):
        project_tool_response(response(f'password = "{provider_key}"\n'))


def test_other_tool_arguments_still_use_generic_credential_redaction():
    result, _ = project_tool_response(response(
        'apis.login(password=synthetic_world_password)\n', name="other-tool"))
    assert "synthetic_world_password" not in code_from(result)


def test_private_adapter_install_stays_in_child_and_import_is_passive():
    """A benchmark child may patch its transport; business parent must stay intact."""
    from agentflow_runtime import recorder, tool_registry
    from agentflow_runtime.common.clients import openai_compatible_client
    from agentflow_runtime.hooks import engine

    originals = (engine.redact_secrets, recorder.redact_secrets,
                 openai_compatible_client.redact_secrets,
                 openai_compatible_client.OpenAiCompatibleClient.build_request_body,
                 recorder.TraceRecorder.trace_call,
                 tool_registry.find_secret_exposures, tool_registry.redact_secrets,
                 tool_registry._LocalToolRegistry.execute)
    child = r'''
import sys
from pathlib import Path
root = Path(sys.argv[1])
sys.path.insert(0, str(root / "packages/agentflow-runtime/src"))
sys.path.insert(0, str(root / "examples/agent/business-simulator/appworld"))
from agentflow_runtime import recorder, tool_registry
from agentflow_runtime.common.clients import openai_compatible_client
from agentflow_runtime.hooks import engine
from agentflow_runtime.hooks.registry import default_registry
before = (engine.redact_secrets, recorder.redact_secrets,
          openai_compatible_client.redact_secrets,
          recorder.TraceRecorder.trace_call,
                 tool_registry.find_secret_exposures, tool_registry.redact_secrets,
                 tool_registry._LocalToolRegistry.execute)
import appworld_skill
assert before == (engine.redact_secrets, recorder.redact_secrets,
                  openai_compatible_client.redact_secrets,
                  recorder.TraceRecorder.trace_call,
                 tool_registry.find_secret_exposures, tool_registry.redact_secrets,
                 tool_registry._LocalToolRegistry.execute)
install_observer = getattr(appworld_skill, "install_native_observer", None)
if install_observer is not None:
    assert install_observer() is None
    assert recorder.TraceRecorder.trace_call is before[3]
    assert not default_registry.handlers_for("llm.request.prepared")
appworld_skill.install_private_reply_transport()
assert engine.redact_secrets is not before[0]
assert recorder.redact_secrets is not before[1]
assert openai_compatible_client.redact_secrets is not before[2]
assert recorder.TraceRecorder.trace_call is before[3]
assert tool_registry.find_secret_exposures is not before[4]
assert tool_registry.redact_secrets is not before[5]
assert tool_registry._LocalToolRegistry.execute is not before[6]
print("private adapter isolated")
'''
    env = {key: value for key, value in os.environ.items()
           if not key.startswith(("APPWORLD_CAPTURE_", "TRAJECTORY_EVOLUTION_TRIAL_"))}
    result = subprocess.run([sys.executable, "-c", child, str(ROOT)], env=env,
                            capture_output=True, text=True, timeout=30)
    assert result.returncode == 0, result.stderr
    assert result.stdout.strip() == "private adapter isolated"
    assert originals == (engine.redact_secrets, recorder.redact_secrets,
                         openai_compatible_client.redact_secrets,
                         openai_compatible_client.OpenAiCompatibleClient.build_request_body,
                         recorder.TraceRecorder.trace_call,
                 tool_registry.find_secret_exposures, tool_registry.redact_secrets,
                 tool_registry._LocalToolRegistry.execute)


def test_non_tool_fields_still_redact_registered_provider_credential():
    provider_key = "fixture-provider-credential-123456"
    register_secret("llm.api_key", provider_key)
    raw = response('print("benchmark")\n')
    raw["choices"][0]["message"]["content"] = provider_key
    result, _ = project_tool_response(raw)
    assert provider_key not in json.dumps(result)
    assert code_from(result) == 'print("benchmark")\n'


def test_actual_http_client_and_recorder_return_executable_tool_code(monkeypatch):
    """Run the real transport and recorder rather than two helper calls only."""
    from agentflow_runtime import recorder as recorder_module
    from agentflow_runtime.common.clients import openai_compatible_client as client_module
    from agentflow_runtime.common.clients import OpenAiCompatibleClient
    from agentflow_runtime.recorder import TraceRecorder

    _install_isolated_projection(monkeypatch)

    code = (
        'login = apis.file_system.login(username=username, password=fs_pw)\n'
        'token = login["access_token"]\n'
        'apis.file_system.show_directory(access_token=token, directory_path="/")\n'
    )
    provider_response = response(code)

    class FakeResponse:
        status_code = 200
        headers = {"Content-Type": "application/json"}
        text = ""

        def json(self):
            return copy.deepcopy(provider_response)

        def close(self):
            pass

    submitted = []

    def post(_url, **kwargs):
        submitted.append(kwargs["json"])
        return FakeResponse()

    monkeypatch.setattr(client_module.requests, "post", post)
    tools = [{"type": "function", "function": {
        "name": TOOL_NAME, "description": "Execute benchmark code",
        "parameters": TOOL_PARAMS}}]
    stored = {}
    recorder = object.__new__(TraceRecorder)
    recorder._client = object()
    recorder._call_count = 0
    recorder._put = lambda key, value: stored.__setitem__(key, value)
    recorder._verify_meta_written = lambda _call_id: None
    recorder._append_to_index = lambda _call_id: None
    public_messages, _, _, _ = _public_history()
    expected_request = copy.deepcopy(public_messages)
    actual = recorder.trace_call(
        public_messages,
        model="fixture-model", base_url="https://fixture.invalid/v1",
        api_key="fixture-provider-api-key-123456", tools=tools,
        stream=False, source="skill_executor", skill="appworld-office")
    assert len(submitted) == 1
    assert submitted[0]["messages"] == expected_request
    assert submitted[0]["tool_choice"] == {
        "type": "function", "function": {"name": TOOL_NAME}}
    assert code_from(actual) == code
    assert code_from(stored[f'traces/{actual["_agentflow_call_id"]}/response']) == code
    ast.parse(code_from(actual))


def _public_history():
    """Public simulator accounts are task inputs, not provider credentials."""
    password = "synthetic-world-password-23"
    token = "synthetic-world-session-token-77"
    code = (
        'credentials = public_passwords["file_system"]\n'
        'login = apis.file_system.login(username=credentials["username"], '
        'password=credentials["password"])\n'
        'token = login["access_token"]\n'
        'entries = apis.file_system.show_directory(access_token=token, '
        'directory_path="/")\n'
    )
    calls = response(code)["choices"][0]["message"]["tool_calls"]
    credentials = {"file_system": {"username": "benchmark-user", "password": password}}
    messages = [
        {"role": "system", "content": "[AppWorld request] Complete the official public task."},
        {"role": "user", "content": "List my files using the public simulator account."},
        {"role": "tool", "tool_call_id": "public-passwords", "content": json.dumps(credentials)},
        {"role": "assistant", "content": "", "tool_calls": calls},
        {"role": "tool", "tool_call_id": "fixture-call", "content": json.dumps({
            "access_token": token, "token_type": "Bearer", "entries": ["example.txt"]})},
    ]
    return messages, code, credentials, token


def _install_isolated_projection(monkeypatch):
    """Exercise the real private installation, without leaking process patches."""
    from agentflow_runtime import recorder, tool_registry
    from agentflow_runtime.common.clients import OpenAiCompatibleClient
    from agentflow_runtime.common.clients import openai_compatible_client
    from agentflow_runtime.hooks import engine

    monkeypatch.setattr(tool_registry._LocalToolRegistry, "execute",
                        tool_registry._LocalToolRegistry.execute)
    for name in ("find_secret_exposures", "redact_secrets"):
        monkeypatch.setattr(tool_registry, name, getattr(tool_registry, name))
    monkeypatch.setattr(OpenAiCompatibleClient, "build_request_body",
                        OpenAiCompatibleClient.build_request_body)
    for module in (openai_compatible_client, recorder, engine):
        monkeypatch.setattr(module, "redact_secrets", module.redact_secrets)
        if hasattr(module, "find_secret_exposures"):
            monkeypatch.setattr(module, "find_secret_exposures", module.find_secret_exposures)
    install_private_reply_transport()
    return engine


@pytest.mark.parametrize("subscribed", [False, True])
def test_real_hook_engine_preserves_public_history_and_nested_request(monkeypatch, subscribed):
    """No-subscriber hooks also sanitize; subscribed hooks sanitize again in finally."""
    from agentflow_runtime.context import AgentContext
    from agentflow_runtime.hooks.registry import HookRegistry

    engine = _install_isolated_projection(monkeypatch)
    messages, code, credentials, token = _public_history()
    expected_messages = copy.deepcopy(messages)
    context = AgentContext(messages=messages)
    payload = {"request": {"messages": copy.deepcopy(messages),
                           "history": copy.deepcopy(messages),
                           "public_observation": {"passwords": credentials,
                                                  "access_token": token}},
               "messages": copy.deepcopy(messages), "round": 2}
    expected_payload = copy.deepcopy(payload)
    registry = HookRegistry()
    seen = []
    if subscribed:
        def handler(event, ctx):
            seen.append(copy.deepcopy(ctx.messages))
            assert ctx.messages == expected_messages
            assert event.payload == expected_payload
            # A handler return triggers its own projection and the finally
            # projections. Both must preserve the same official public data.
            return {"request": copy.deepcopy(expected_payload["request"])}
        registry.register("fixture-observer", ["fixture.before_model"], handler)
    actual = engine.trigger("fixture.before_model", payload, ctx=context, registry=registry,
                            can_intervene=True)
    assert actual == expected_payload
    assert context.messages == expected_messages
    assert seen == ([expected_messages] if subscribed else [])
    actual_code = json.loads(context.messages[3]["tool_calls"][0]
                             ["function"]["arguments"])["code"]
    assert actual_code == code
    ast.parse(actual_code)
    assert json.loads(context.messages[2]["content"]) == credentials
    assert json.loads(context.messages[4]["content"])["access_token"] == token

    class FileSystem:
        def login(self, *, username, password):
            assert username == credentials["file_system"]["username"]
            assert password == credentials["file_system"]["password"]
            return {"access_token": token}

        def show_directory(self, *, access_token, directory_path):
            assert access_token == token and directory_path == "/"
            return ["example.txt"]

    class APIs:
        file_system = FileSystem()

    namespace = {"apis": APIs(), "public_passwords": credentials}
    exec(compile(actual_code, "<public-appworld-history>", "exec"), namespace)
    assert namespace["entries"] == ["example.txt"]


@pytest.mark.parametrize("location", ["context", "nested_request"])
def test_real_hook_engine_blocks_registered_provider_key_in_executable_code(monkeypatch, location):
    from agentflow_runtime.context import AgentContext
    from agentflow_runtime.hooks.registry import HookRegistry

    engine = _install_isolated_projection(monkeypatch)
    provider_key = "fixture-provider-secret-do-not-submit-12345"
    register_secret("llm.api_key", provider_key)
    messages, _, _, _ = _public_history()
    messages[3]["tool_calls"][0]["function"]["arguments"] = json.dumps({
        "code": f'apis.file_system.login(password="{provider_key}")\n'})
    context = AgentContext(messages=messages if location == "context" else [])
    payload = {"request": {"messages": messages}} if location == "nested_request" else {"round": 1}
    with pytest.raises(SecretLeakBlocked):
        engine.trigger("fixture.before_model", payload, ctx=context, registry=HookRegistry())


@pytest.mark.parametrize("previous,current,expected", [
    ({"status": "failed", "observation": "Execution failed: 422 at line 1"},
     {"status": "failed", "observation": "Execution failed: 422 at line 9"}, 0),
    ({"status": "completed", "observation": "created document"},
     {"status": "rejected", "error_code": "policy_action_rejected"}, 0),
    ({"status": "completed", "observation": "directory: a.txt"},
     {"status": "completed", "observation": "directory: a.txt"}, 0),
    ({"status": "completed", "observation": "directory: a.txt"},
     {"status": "completed", "observation": "directory: a.txt, b.txt"}, 1),
    ({"status": "failed", "observation": "Execution failed: 422"},
     {"status": "completed", "observation": "saved"}, 1),
    ({"status": "completed", "observation": ""},
     {"status": "completed", "observation": "   "}, 1),
    ({}, {"status": "completed", "observation": "first observation"}, 1),
    (None, {"status": "completed", "observation": "first observation"}, 1),
    ({}, {"status": "completed"}, 0),
])
def test_task_progress_uses_successful_observations_not_failed_feedback(previous, current, expected):
    from appworld_host import appworld_progress_delta

    original_previous, original_current = copy.deepcopy(previous), copy.deepcopy(current)
    assert appworld_progress_delta(previous, current) == expected
    assert previous == original_previous and current == original_current


@pytest.mark.parametrize("previous,current,expected", [
    ({"status": "failed", "error_code": "appworld_validation_422",
      "observation": "Execution failed: invalid argument"},
     {"status": "failed", "error_code": "appworld_validation_422",
      "observation": "Execution failed: invalid argument"}, True),
    # A line number or target-specific result makes this a different factual
    # observation.  It must not be promoted to a repeated semantic event.
    ({"status": "failed", "error_code": "appworld_validation_422",
      "observation": "Execution failed at target A"},
     {"status": "failed", "error_code": "appworld_validation_422",
      "observation": "Execution failed at target B"}, False),
    # Identical ordinary reads are legitimate pagination/state inspection,
    # not evidence of a wasted iteration.
    ({"status": "completed", "observation": "directory: a.txt"},
     {"status": "completed", "observation": "directory: a.txt"}, False),
    ({"status": "completed", "observation": "No changes were made."},
     {"status": "completed", "observation": "No changes were made."}, True),
    ({"status": "completed", "observation": "already archived"},
     {"status": "completed", "observation": "already archived"}, True),
])
def test_semantic_no_progress_is_bounded_diagnostic_only(previous, current, expected):
    from appworld_host import appworld_semantic_no_progress

    original_previous, original_current = copy.deepcopy(previous), copy.deepcopy(current)
    assert appworld_semantic_no_progress(previous, current) is expected
    assert previous == original_previous and current == original_current


def test_host_diagnostics_do_not_publish_formal_invalid_loop_score():
    """Formal loop scoring remains the shared model-review contract.

    ``repeated_code_count`` and ``semantic_no_progress_count`` intentionally
    remain available for debugging.  They cannot be substituted for the
    reviewed ``loop_review.count`` because a result does not identify the
    target object or operation; the same observation can be valid for two
    separate targets or pagination pages.
    """
    from appworld_host import Trial

    source = inspect.getsource(Trial.receive)
    assert "invalid_loop_count = max(" not in source
    assert source.count('"invalid_loop_count": None') == 2
    assert source.count('"invalid_loop_basis": "model_review"') == 2


def _actual_reply_registry(tmp_path):
    """Use the nonce-checked generated reply module through original dispatch."""
    import importlib.util
    from agentflow_runtime import tool_registry
    from appworld_skill import prepare_skill, set_request

    meta = prepare_skill(tmp_path / "state", tmp_path / "skill")
    set_request(meta, "fixture public task", [], [{"role": "user", "content": "fixture"}], 0)
    spec = importlib.util.spec_from_file_location("appworld_reply_fixture", meta["module_path"])
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    registry = object.__new__(tool_registry._LocalToolRegistry)
    registry._modules = {TOOL_NAME: module}
    registry._mcp_bridges = {}
    return registry, meta


def test_real_registry_dispatch_preserves_code_and_nonce_checked_reply(tmp_path, monkeypatch):
    from agentflow_runtime import tool_registry
    from appworld_skill import read_reply

    code = 'apis.venmo.login(password=account_password_reference, access_token=session.access_token)\n'
    assert tool_registry.find_secret_exposures({"code": code}).applied
    _install_isolated_projection(monkeypatch)
    registry, meta = _actual_reply_registry(tmp_path)
    actual = json.loads(registry.execute(TOOL_NAME, {"code": code}))
    assert actual == {"status": "completed", "reply_saved": True}
    assert read_reply(meta) == {"code": code}
    # The original atomic submission rejects a different second reply.
    rejected = json.loads(registry.execute(TOOL_NAME, {"code": code + 'print("changed")\n'}))
    assert rejected["status"] == "failed" and rejected["error"] == "appworld_reply_already_submitted"
    assert read_reply(meta) == {"code": code}


def test_real_registry_blocks_environment_provider_key_before_reply_write(tmp_path, monkeypatch):
    provider_key = "fixture-environment-provider-credential-1234567"
    monkeypatch.setenv("APPWORLD_FIXTURE_API_KEY", provider_key)
    _install_isolated_projection(monkeypatch)
    registry, meta = _actual_reply_registry(tmp_path)
    blocked = json.loads(registry.execute(TOOL_NAME, {"code": f'print("{provider_key}")\n'}))
    assert blocked["status"] == "security_policy_blocked"
    assert provider_key not in json.dumps(blocked)
    assert not Path(meta["reply_path"]).exists()


def test_tool_scope_preserves_public_result_and_other_tool_guard(monkeypatch):
    import types
    from agentflow_runtime import tool_registry

    original_guard = tool_registry.find_secret_exposures
    original_redactor = tool_registry.redact_secrets
    _install_isolated_projection(monkeypatch)
    code = 'apis.login(password=account_password_reference)\n'
    result = {"code": code, "password": "synthetic-world-password", "access_token": "synthetic-world-session"}
    calls = []
    registry = object.__new__(tool_registry._LocalToolRegistry)
    def submit(args, **_kwargs):
        calls.append(args)
        # Nested dispatch must restore the parent tool's scope afterwards.
        nested = json.loads(registry.execute("other_tool", {"code": code}))
        assert nested["status"] == "security_policy_blocked"
        return result
    registry._modules = {TOOL_NAME: types.SimpleNamespace(execute=submit),
                         "other_tool": types.SimpleNamespace(execute=lambda args, **kwargs: result)}
    registry._mcp_bridges = {}
    assert json.loads(registry.execute(TOOL_NAME, {"code": code})) == result
    assert len(calls) == 1
    ordinary = json.loads(registry.execute("other_tool", {}))
    assert ordinary == original_redactor(result)[0]
    assert ordinary != result
    assert tool_registry.find_secret_exposures({"code": code}).applied == original_guard({"code": code}).applied


def test_tool_scope_never_returns_registered_provider_key(monkeypatch):
    import types
    from agentflow_runtime import tool_registry

    provider_key = "fixture-provider-result-secret-1234567"
    register_secret("fixture.provider", provider_key)
    _install_isolated_projection(monkeypatch)
    registry = object.__new__(tool_registry._LocalToolRegistry)
    registry._modules = {TOOL_NAME: types.SimpleNamespace(execute=lambda args, **kwargs: {"code": provider_key})}
    registry._mcp_bridges = {}
    actual = registry.execute(TOOL_NAME, {"code": 'print("safe")\n'})
    assert provider_key not in actual
    assert "error" in json.loads(actual)


def test_private_tool_scope_does_not_change_another_thread_guard(monkeypatch):
    import threading
    import types
    from concurrent.futures import ThreadPoolExecutor
    from agentflow_runtime import tool_registry

    _install_isolated_projection(monkeypatch)
    entered, release = threading.Event(), threading.Event()
    code = 'apis.login(password=account_password_reference)\n'
    registry = object.__new__(tool_registry._LocalToolRegistry)
    def submit(args, **_kwargs):
        entered.set()
        assert release.wait(5)
        return {"code": args["code"]}
    registry._modules = {TOOL_NAME: types.SimpleNamespace(execute=submit),
                         "other_tool": types.SimpleNamespace(execute=lambda args, **kwargs: {})}
    registry._mcp_bridges = {}
    with ThreadPoolExecutor(max_workers=1) as pool:
        pending = pool.submit(registry.execute, TOOL_NAME, {"code": code})
        try:
            assert entered.wait(5)
            assert tool_registry.find_secret_exposures({"code": code}).applied
            blocked = json.loads(registry.execute("other_tool", {"code": code}))
            assert blocked["status"] == "security_policy_blocked"
        finally:
            release.set()
        assert json.loads(pending.result(timeout=5)) == {"code": code}


def test_incremental_submit_guidance_preserves_complete_success_history(tmp_path):
    import types
    from appworld_skill import prepare_skill, set_request, compose_skill_messages

    meta = prepare_skill(tmp_path / "state", tmp_path / "skill")
    messages = [
        {"role": "user", "content": "Send the requested fixture payment."},
        {"role": "assistant", "content": "transaction = apis.venmo.create_transaction()"},
        {"role": "user", "content": json.dumps({"status": "completed", "transaction_id": "fixture-transaction"})},
        {"role": "assistant", "content": "The payment already succeeded; verify without creating another."},
    ]
    original = copy.deepcopy(messages)
    set_request(meta, "fixture public task", [], messages, 1)
    state = json.loads(Path(meta["state_path"]).read_text())
    assert state["messages"] == original == messages
    assert compose_skill_messages(messages, skill_name="appworld-office") == original
    doc = Path(meta["skill_doc_path"]).read_text()
    encoded_history = doc.split("## Complete recent public history\n```json\n", 1)[1].rsplit("\n```", 1)[0]
    assert json.loads(encoded_history) == original
    module = types.ModuleType("incremental_appworld_reply")
    exec(compile(Path(meta["module_path"]).read_text(), meta["module_path"], "exec"), module.__dict__)
    assert "world and Python variables persist" in module.TOOL_DESCRIPTION
    assert "not a full replay" in module.TOOL_DESCRIPTION
    assert "Never repeat an already successful non-idempotent change" in module.TOOL_DESCRIPTION
    assert "only necessary verification or complete_task" in module.TOOL_DESCRIPTION
    assert "not a full script replay" in doc
    assert "world state and Python variables persist" in doc
    assert "Never repeat an already successful non-idempotent change" in doc


def test_incremental_requests_keep_success_state_and_reject_old_submission(tmp_path):
    import types
    from appworld_skill import prepare_skill, set_request, read_reply

    class PublicAPIs:
        def __init__(self):
            self.created = 0
            self.completed = 0
            self.venmo = self
            self.supervisor = self

        def create_transaction(self):
            self.created += 1
            return {"transaction_id": "fixture-transaction"}

        def show_transaction(self, transaction_id):
            assert transaction_id == "fixture-transaction"
            return {"status": "completed"}

        def complete_task(self):
            self.completed += 1

    def reply_module(meta):
        module = types.ModuleType("incremental_appworld_reply")
        exec(compile(Path(meta["module_path"]).read_text(), meta["module_path"], "exec"), module.__dict__)
        return module

    meta = prepare_skill(tmp_path / "state", tmp_path / "skill")
    messages = [{"role": "user", "content": "Send one fixture payment."}]
    first_code = "transaction = apis.venmo.create_transaction()\n"
    set_request(meta, "fixture public task", [], messages, 0)
    first = reply_module(meta)
    assert json.loads(first.execute({"code": first_code}))["status"] == "completed"
    apis = PublicAPIs()
    namespace = {"apis": apis}
    exec(read_reply(meta)["code"], namespace)
    messages.extend([
        {"role": "assistant", "content": first_code},
        {"role": "user", "content": json.dumps(namespace["transaction"])},
    ])
    set_request(meta, "fixture public task", [], messages, 1)
    assert json.loads(first.execute({"code": first_code}))["error"] == "appworld_reply_stale_request"
    second_code = ("assert apis.venmo.show_transaction(transaction['transaction_id'])['status'] == 'completed'\n"
                   "apis.supervisor.complete_task()\n")
    second = reply_module(meta)
    assert json.loads(second.execute({"code": second_code}))["status"] == "completed"
    assert read_reply(meta) == {"code": second_code}
    exec(read_reply(meta)["code"], namespace)
    assert apis.created == 1 and apis.completed == 1
    assert json.loads(Path(meta["state_path"]).read_text())["messages"] == messages
