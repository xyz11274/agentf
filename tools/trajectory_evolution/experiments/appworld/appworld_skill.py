"""Atomic Python intent submission; only the host invokes the official sandbox."""
from __future__ import annotations
from contextlib import contextmanager
import fcntl
import inspect
import json
import os
from pathlib import Path
import tempfile
import uuid
from typing import Any

SKILL_NAME = "appworld-office"
TOOL_NAME = "appworld_submit_code"
TOOL_PARAMS = {"type":"object", "properties":{"code":{"type":"string", "minLength":1}}, "required":["code"], "additionalProperties":False}

def _validate_payload(args, allowed_tools=None):
    if not isinstance(args, dict) or set(args) != {"code"}:
        raise ValueError("appworld_reply_schema_invalid")
    code=args["code"]
    if not isinstance(code, str) or not code.strip() or len(code)>65536:
        raise ValueError("appworld_reply_code_invalid")
    return {"code":code}


def _atomic_text(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, temporary = tempfile.mkstemp(prefix=f".{path.name}.", dir=str(path.parent))
    try:
        os.fchmod(fd, 0o600)
        with os.fdopen(fd, "w", encoding="utf-8") as stream:
            stream.write(text)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, path)
    finally:
        try:
            os.unlink(temporary)
        except FileNotFoundError:
            pass

@contextmanager
def _locked(meta: dict):
    fd = os.open(meta["lock_path"], os.O_CREAT | os.O_RDWR, 0o600)
    with os.fdopen(fd, "a+") as stream:
        fcntl.flock(stream.fileno(), fcntl.LOCK_EX)
        try:
            yield
        finally:
            fcntl.flock(stream.fileno(), fcntl.LOCK_UN)

_MODULE_BODY = '''
def execute(args, sandbox_endpoint=None):
    try:
        fd = os.open(_LOCK_PATH, os.O_CREAT | os.O_RDWR, 0o600)
        with os.fdopen(fd, "a+") as lock:
            fcntl.flock(lock.fileno(), fcntl.LOCK_EX)
            try:
                state = json.loads(Path(_STATE_PATH).read_text(encoding="utf-8"))
                if (not _REQUEST_ID or state.get("request_id") != _REQUEST_ID
                        or state.get("turn") != _TURN):
                    raise ValueError("appworld_reply_stale_request")
                payload = _validate_payload(args, set(state.get("allowed_tool_names", [])))
                reply = Path(state["reply_path"])
                envelope = {"schema_version": 1, "request_id": _REQUEST_ID,
                            "turn": _TURN, "payload": payload}
                if reply.exists():
                    previous = json.loads(reply.read_text(encoding="utf-8"))
                    if previous != envelope:
                        raise ValueError("appworld_reply_already_submitted")
                else:
                    fd, temporary = tempfile.mkstemp(prefix=".reply.", dir=str(reply.parent))
                    try:
                        os.fchmod(fd, 0o600)
                        with os.fdopen(fd, "w", encoding="utf-8") as stream:
                            json.dump(envelope, stream, ensure_ascii=False, allow_nan=False)
                            stream.flush()
                            os.fsync(stream.fileno())
                        os.replace(temporary, reply)
                    finally:
                        try:
                            os.unlink(temporary)
                        except FileNotFoundError:
                            pass
                return json.dumps({"status": "completed", "reply_saved": True})
            finally:
                fcntl.flock(lock.fileno(), fcntl.LOCK_UN)
    except ValueError as error:
        code = str(error)
        if not code.startswith("appworld_reply_"):
            code = "appworld_reply_state_invalid"
        return json.dumps({"status": "failed", "error": code})
    except (OSError, KeyError, TypeError):
        return json.dumps({"status": "failed", "error": "appworld_reply_unavailable"})
'''

def _module_source(meta: dict, request_id: str, turn: int) -> str:
    return (
        "import fcntl\nimport json\nimport os\nfrom pathlib import Path\nimport tempfile\n\n"
        f"TOOL_NAME = {TOOL_NAME!r}\n"
        "TOOL_DESCRIPTION = 'Submit Python code to the official AppWorld executor. '") + (
        "'The host executes it once against the official sandbox; stop after submission.'\n"
        f"TOOL_PARAMS = {TOOL_PARAMS!r}\n"
        f"_STATE_PATH = {meta['state_path']!r}\n"
        f"_LOCK_PATH = {meta['lock_path']!r}\n"
        f"_REQUEST_ID = {request_id!r}\n_TURN = {turn!r}\n\n"
        + inspect.getsource(_validate_payload) + "\n" + _MODULE_BODY
    )

def prepare_skill(workspace: str | os.PathLike[str],
                  package_skill_dir: str | os.PathLike[str]) -> dict[str, Any]:
    """Create a private skill and return paths plus daemon environment bindings."""
    root = Path(workspace).resolve()
    root.mkdir(parents=True, exist_ok=True)
    root.chmod(0o700)
    destination = Path(package_skill_dir)
    if destination.is_symlink() or (destination / "scripts").is_symlink():
        raise ValueError("appworld_skill_directory_must_be_private")
    destination = destination.resolve()
    scripts = destination / "scripts"
    scripts.mkdir(parents=True, exist_ok=True)
    meta = {
        "skill_name": SKILL_NAME, "tool_name": TOOL_NAME,
        "state_path": str(root / "appworld-runtime-state.json"),
        "reply_path": str(root / "reply.json"),
        "lock_path": str(root / "appworld-runtime.lock"),
        "scripts_dir": str(scripts),
        "skill_doc_path": str(destination / "SKILL.md"),
        "module_path": str(scripts / "appworld_submit_code.py"),
        "request_id": "", "turn": -1,
    }
    meta["env"] = {
        "BFCL_RUNTIME_STATE": meta["state_path"],
        "BFCL_RUNTIME_SKILL_DIR": meta["scripts_dir"],
        "APPWORLD_RUNTIME_STATE": meta["state_path"],
    }
    with _locked(meta):
        state = {"schema_version": 1, "request_id": "", "turn": -1,
                 "available_names": [TOOL_NAME], "allowed_tool_names": [],
                 "scripts_dir": str(scripts), "reply_path": meta["reply_path"]}
        _atomic_text(Path(meta["state_path"]), json.dumps(state, ensure_ascii=False))
        _atomic_text(Path(meta["module_path"]), _module_source(meta, "", -1))
        _atomic_text(scripts / "__init__.py", "")
        _atomic_text(Path(meta["skill_doc_path"]), "# AppWorld office\nAwait the current public request.\n")
        Path(meta["reply_path"]).unlink(missing_ok=True)
    return meta

def read_reply(meta: dict) -> dict:
    """Return only the current raw message payload; missing/stale replies are errors."""
    with _locked(meta):
        state = json.loads(Path(meta["state_path"]).read_text(encoding="utf-8"))
        reply_path = Path(meta["reply_path"])
        if not reply_path.is_file():
            raise RuntimeError("appworld_reply_missing")
        try:
            envelope = json.loads(reply_path.read_text(encoding="utf-8"))
            if (not isinstance(envelope, dict)
                    or set(envelope) != {"schema_version", "request_id", "turn", "payload"}
                    or envelope.get("schema_version") != 1):
                raise RuntimeError("appworld_reply_invalid")
            if (envelope["request_id"] != state.get("request_id")
                    or envelope["request_id"] != meta.get("request_id")
                    or envelope["turn"] != state.get("turn")
                    or envelope["turn"] != meta.get("turn")):
                raise RuntimeError("appworld_reply_stale")
            return _validate_payload(envelope["payload"], set(state.get("allowed_tool_names", [])))
        except (ValueError, TypeError, KeyError):
            raise RuntimeError("appworld_reply_invalid") from None

def install_private_reply_transport():
    """Require the one reply tool only in this disposable node process."""
    from agentflow_runtime.common.clients import OpenAiCompatibleClient
    original = OpenAiCompatibleClient.build_request_body
    if getattr(original, "_appworld_reply_transport", False):
        return

    def build(self, messages, *, tools=None, max_completion_tokens=8192,
              stream=False, extra_body=None):
        body = original(self, messages, tools=tools,
                        max_completion_tokens=max_completion_tokens,
                        stream=stream, extra_body=extra_body)
        if len(tools or []) == 1 and tools[0].get("function", {}).get("name") == TOOL_NAME:
            body["tool_choice"] = {"type": "function", "function": {"name": TOOL_NAME}}
        return body

    build._appworld_reply_transport = True
    OpenAiCompatibleClient.build_request_body = build


def set_request(meta, policy, tools_schema, messages, turn):
    if not isinstance(policy, str) or not isinstance(messages, list) or type(turn) is not int:
        raise ValueError("appworld_public_request_invalid")
    request_id = uuid.uuid4().hex
    state = {"schema_version": 1, "request_id": request_id, "turn": turn,
             "available_names": [TOOL_NAME], "allowed_tool_names": [TOOL_NAME],
             "scripts_dir": meta["scripts_dir"], "reply_path": meta["reply_path"],
             "policy": policy, "messages": messages}
    doc = ("# AppWorld cross-application office\n\nWrite the next Python code block for "
           "the official AppWorld environment. Call appworld_submit_code exactly once "
           "with {code:...} and stop. The host alone executes it once in the persistent "
           "official sandbox. Use the public apis interface and official api_docs. "
           "Do not access evaluation, ground_truth, host paths, database files, or hidden "
           "task data. Do not replace executing APIs with prose. Call the official "
           "apis.supervisor.complete_task only when the requested work is finished. "
           "The official database checks determine success.\n\n" + policy +
           "\n\n## Complete recent public history\n```json\n" +
           json.dumps(messages, ensure_ascii=False, indent=2) + "\n```\n")
    with _locked(meta):
        Path(meta["reply_path"]).unlink(missing_ok=True)
        _atomic_text(Path(meta["state_path"]), json.dumps(state, ensure_ascii=False))
        _atomic_text(Path(meta["module_path"]), _module_source(meta, request_id, turn))
        _atomic_text(Path(meta["skill_doc_path"]), doc)
        meta.update(request_id=request_id, turn=turn)


def current_reply():
    state_path = os.environ.get("APPWORLD_RUNTIME_STATE", "")
    if not state_path:
        return None
    try:
        state = json.loads(Path(state_path).read_text(encoding="utf-8"))
        return read_reply({"state_path": state_path, "reply_path": state["reply_path"],
            "lock_path": str(Path(state_path).parent / "appworld-runtime.lock"),
            "request_id": state["request_id"], "turn": state["turn"]})
    except (OSError, ValueError, KeyError, RuntimeError):
        return None
