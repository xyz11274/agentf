"""Atomic Python intent submission; only the host invokes the official sandbox."""
from __future__ import annotations
from contextlib import contextmanager
import copy
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
        "TOOL_DESCRIPTION = 'Submit only the next incremental Python code block to the official AppWorld executor. '") + (
        "'The world and Python variables persist between submissions. Continue from prior observations, not a full replay. '"
        "'Never repeat an already successful non-idempotent change, such as a payment, message, or deletion. '"
        "'After successful changes, submit only necessary verification or complete_task. '"
        "'The host executes this block once; stop after submission.'\n"
        f"TOOL_PARAMS = {TOOL_PARAMS!r}\n"
        f"_STATE_PATH = {meta['state_path']!r}\n"
        f"_LOCK_PATH = {meta['lock_path']!r}\n"
        f"_REQUEST_ID = {request_id!r}\n_TURN = {turn!r}\n\n"
        + inspect.getsource(_validate_payload) + "\n"
        + _MODULE_BODY
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

def submit_recovered_code(meta: dict, code: str) -> dict:
    """Persist one host-validated code block recovered from a normal model stop."""
    with _locked(meta):
        state = json.loads(Path(meta["state_path"]).read_text(encoding="utf-8"))
        payload = _validate_payload({"code": code}, set(state.get("allowed_tool_names", [])))
        envelope = {"schema_version": 1, "request_id": state.get("request_id"),
                    "turn": state.get("turn"), "payload": payload}
        reply = Path(state["reply_path"])
        if reply.exists():
            previous = json.loads(reply.read_text(encoding="utf-8"))
            if previous != envelope:
                raise RuntimeError("appworld_reply_already_submitted")
        else:
            _atomic_text(reply, json.dumps(envelope, ensure_ascii=False, allow_nan=False))
        return payload


def _policy_advice_cache_limit(value, default=None):
    """Read the native snapshot advice bound without exposing its contents.

    AppWorld receives the published policy through the trial ``version.json``
    rather than through the public skill prompt.  Keep the cache bound tied to
    that snapshot when it is available; L0 and lightweight fixtures have no
    snapshot and retain the historical bound of two cards.
    """
    def extract(source):
        if not isinstance(source, dict):
            return None
        direct = source.get("native_advice_cache_limit")
        if type(direct) is int and 1 <= direct <= 32:
            return direct
        candidates = []
        policy = source.get("policy")
        if isinstance(policy, dict):
            candidates.append(policy)
        artifact = source.get("artifact")
        if isinstance(artifact, dict):
            candidates.append(artifact)
            artifact_policy = artifact.get("policy")
            if isinstance(artifact_policy, dict):
                candidates.append(artifact_policy)
        candidates.append(source)
        for item in candidates:
            snapshot = item.get("snapshot") if isinstance(item, dict) else None
            options = snapshot.get("options") if isinstance(snapshot, dict) else None
            if isinstance(options, dict):
                value = options.get("max_advice_items")
                if type(value) is int and 1 <= value <= 32:
                    return value
            options = item.get("options") if isinstance(item, dict) else None
            if isinstance(options, dict):
                value = options.get("max_advice_items")
                if type(value) is int and 1 <= value <= 32:
                    return value
        return None

    value_found = extract(value)
    if value_found is not None:
        return value_found
    # The evaluator writes the complete immutable artifact next to the trial
    # config.  Reading only this bounded numeric option keeps runtime state
    # independent of the public prompt and preserves the L0 fallback.
    config_path = os.environ.get("TRAJECTORY_EVOLUTION_TRIAL_CONFIG", "")
    if config_path:
        try:
            version = Path(config_path).with_name("version.json")
            value_found = extract(json.loads(version.read_text(encoding="utf-8")))
        except (OSError, ValueError, TypeError):
            value_found = None
    if value_found is not None:
        return value_found
    return 2 if default is None else default


def appworld_l1_window_mode(artifact):
    """Read an AppWorld-only option from the frozen, versioned agent identity."""
    if not isinstance(artifact, dict) or artifact.get('level') not in ('L1', 'L2', 'L3'):
        return None
    agent = artifact.get('agent')
    options = agent.get('appworld_options') if isinstance(agent, dict) else None
    if options is None:
        return None
    if not isinstance(options, dict):
        raise ValueError('appworld_agent_options_invalid')
    mode = options.get('l1_window_mode')
    if mode is not None and mode not in ('observed_phase', 'observed_phase_v2', 'observed_phase_v3'):
        raise ValueError('appworld_l1_window_mode_invalid')
    return mode


def appworld_observed_phase_experiences(artifact):
    """Limit an opt-in success window to fixed, already approved sources."""
    mode = appworld_l1_window_mode(artifact)
    if mode is None:
        return None
    options = artifact.get('agent', {}).get('appworld_options') or {}
    if 'l1_observed_phase_experiences' not in options:
        return None
    refs = options['l1_observed_phase_experiences']
    approved = {(row.get('memory_id'), row.get('content_revision'))
                for row in (artifact.get('policy') or {}).get('options', {}).get('approved_experiences', [])}
    if not isinstance(refs, list) or not 1 <= len(refs) <= 16:
        raise ValueError('appworld_observed_phase_experiences_invalid')
    seen = set()
    for ref in refs:
        if (not isinstance(ref, dict) or set(ref) != {'memory_id', 'content_revision'}
                or not all(isinstance(value, str) and value for value in ref.values())):
            raise ValueError('appworld_observed_phase_experiences_invalid')
        pair = (ref['memory_id'], ref['content_revision'])
        if pair not in approved or pair in seen:
            raise ValueError('appworld_observed_phase_experiences_source_invalid')
        seen.add(pair)
    return [dict(ref) for ref in refs]

def appworld_auth_parameter_experiences(artifact):
    """Validate the optional, approved sources for causal token recovery."""
    if not isinstance(artifact, dict) or artifact.get('level') not in ('L1', 'L2', 'L3'):
        return None
    options = (artifact.get('agent') or {}).get('appworld_options') or {}
    refs = options.get('l1_auth_parameter_experiences')
    if refs is None:
        return None
    approved = {(row.get('memory_id'), row.get('content_revision'))
                for row in (artifact.get('policy') or {}).get('options', {}).get('approved_experiences', [])}
    if not isinstance(refs, list) or not 1 <= len(refs) <= 4:
        raise ValueError('appworld_auth_parameter_experiences_invalid')
    seen = set()
    phased = any(isinstance(ref, dict) and 'phase' in ref for ref in refs)
    completed_prefix = any(isinstance(ref, dict) and 'login_evidence' in ref for ref in refs)
    cards = {(row.get('memory_id'), row.get('content_revision')): row
             for row in (artifact.get('policy') or {}).get('snapshot', {}).get('experiences', [])}
    validated = []
    for ref in refs:
        expected = {'memory_id', 'content_revision', 'phase'} if phased else {'memory_id', 'content_revision'}
        if completed_prefix:
            expected.add('login_evidence')
        schema_source = isinstance(ref, dict) and 'parameter_evidence' in ref
        if schema_source:
            expected.add('parameter_evidence')
        if (not isinstance(ref, dict) or set(ref) != expected
                or not all(isinstance(v, str) and v for v in ref.values())):
            raise ValueError('appworld_auth_parameter_experiences_invalid')
        if completed_prefix and (not phased or ref['login_evidence'] != 'completed_prefix'):
            raise ValueError('appworld_auth_parameter_experiences_invalid')
        pair = (ref['memory_id'], ref['content_revision'])
        if pair not in approved or pair in seen:
            raise ValueError('appworld_auth_parameter_experiences_source_invalid')
        contracts = None
        if schema_source:
            if (not completed_prefix or ref.get('phase') != 'parameters'
                    or ref['parameter_evidence'] != 'source_schema'):
                raise ValueError('appworld_auth_parameter_experiences_invalid')
            contracts = auth_parameter_source_contracts(cards.get(pair))
            if not contracts:
                raise ValueError('appworld_auth_parameter_experiences_phase_source_invalid')
        if phased and (ref['phase'] not in ('login', 'parameters')
                       or not contracts and auth_recovery_source_phase(cards.get(pair)) != ref['phase']):
            raise ValueError('appworld_auth_parameter_experiences_phase_source_invalid')
        seen.add(pair)
        validated.append(dict(ref, schema_contracts=contracts) if contracts else dict(ref))
    if phased and {ref['phase'] for ref in refs} != {'login', 'parameters'}:
        raise ValueError('appworld_auth_parameter_experiences_phases_incomplete')
    return validated


def auth_parameter_source_contracts(experience):
    """Bind parameter lessons to their own API names and the public registry."""
    import re
    if not isinstance(experience, dict) or not isinstance(experience.get('content'), str):
        return None
    content = experience['content']
    calls = sorted(set(re.findall(r'\bapis\.([a-z_]+)\.([a-z_]+)\b', content)))
    contracts = []
    for app, method in calls:
        if app in ('supervisor', 'api_docs') or method == 'login':
            continue
        path = Path(__file__).parent / 'runtime/world/data/api_docs/standard' / (app + '.json')
        try:
            if path.stat().st_size > 1048576:
                return None
            doc = json.loads(path.read_text(encoding='utf-8')).get(method)
        except (OSError, ValueError, AttributeError):
            return None
        if (not isinstance(doc, dict) or doc.get('app_name') != app or doc.get('api_name') != method
                or not isinstance(doc.get('parameters'), list)):
            return None
        parameters = doc['parameters']
        names = sorted({row['name'] for row in parameters if isinstance(row, dict)
                        and isinstance(row.get('name'), str)})
        source_names = [name for name in names if name != 'access_token'
                        and re.search(r'(?<![a-zA-Z0-9_])' + re.escape(name) + r'(?![a-zA-Z0-9_])', content)]
        if source_names and any(isinstance(row, dict) and row.get('name') == 'access_token'
                                and row.get('required') is True for row in parameters):
            contracts.append({'app': app, 'method': method, 'supported_arguments': names,
                              'source_arguments': source_names})
    return contracts or None


def auth_parameter_schema_mismatch(ref, messages, causal):
    """Qualify only an unknown keyword on the exact source-supported failed API."""
    import ast
    import re
    if not isinstance(messages, list) or len(messages) < 2 or not isinstance(causal, dict):
        return None
    failure = causal.get('causal_auth_parameter_failure', '')
    pairs = set(re.findall(r'\b([a-z_]+)\.([a-z_]+) requires access_token;', failure))
    if len(pairs) != 1 or causal.get('causal_auth_login_failure'):
        return None
    pair = next(iter(pairs))
    contract = next((item for item in ref.get('schema_contracts') or []
                     if (item.get('app'), item.get('method')) == pair), None)
    if contract is None:
        return None
    action, observation = messages[-2:]
    if (not isinstance(action, dict) or action.get('role') != 'assistant'
            or not isinstance(observation, dict) or observation.get('role') != 'user'
            or not isinstance(observation.get('content'), str)):
        return None
    text = observation['content']
    if (not text.lstrip().startswith(('Execution failed', 'Traceback'))
            or 'Traceback' not in text or not re.search(r'\b401\b', text)):
        return None
    traceback = text[text.index('Traceback'):]
    lines = re.findall(r'\bline\s+([1-9][0-9]*)\b', traceback, re.I)
    if (set(re.findall(r'\bapis\.([a-z_]+)\.([a-z_]+)\s*\(', traceback)) != {pair}
            or len(lines) != 1):
        return None
    try:
        tree = ast.parse(messages[-2]['content'])
    except (SyntaxError, ValueError, TypeError, KeyError):
        return None
    calls = [node for node in ast.walk(tree) if isinstance(node, ast.Call)
             and isinstance(node.func, ast.Attribute) and isinstance(node.func.value, ast.Attribute)
             and isinstance(node.func.value.value, ast.Name) and node.func.value.value.id == 'apis'
             and (node.func.value.attr, node.func.attr) == pair]
    if (len(calls) != 1 or calls[0].lineno != int(lines[0]) or calls[0].args
            or any(item.arg is None for item in calls[0].keywords)):
        return None
    actual = {item.arg for item in calls[0].keywords}
    unknown = sorted(name for name in actual - set(contract['supported_arguments'])
                     if not re.search(r'token|auth|password|credential', name, re.I))
    omitted = sorted(set(contract['source_arguments']) - actual)
    if not unknown or not omitted:
        return None
    return {'app': pair[0], 'method': pair[1], 'unknown_arguments': unknown,
            'omitted_source_arguments': omitted}


def auth_recovery_source_phase(experience):
    """Classify only explicit generic steps in an approved source's own text."""
    import re
    if not isinstance(experience, dict):
        return None
    content = experience.get('content')
    if not isinstance(content, str) or 'access_token' not in content:
        return None
    # An application-specific lesson is not a generic auth recovery source.
    apps = re.findall(r'apis\.([a-z_]+)\.[a-z_]+', content)
    if any(app not in ('supervisor', 'api_docs') for app in apps):
        return None
    if re.search(r'\bapp_name\s*=', content):
        return None
    if 'show_account_passwords' in content and re.search(r'\blogin\b', content):
        return 'login'
    if not re.search(r'\blogin\b', content):
        return 'parameters'
    return None


def auth_parameter_phase_experiences(refs, messages, causal):
    """Select the new opt-in phase from prior observed login of this app only."""
    import re
    if not any('phase' in ref for ref in refs):
        return [dict(ref) for ref in refs]
    if not isinstance(causal, dict):
        return None
    parameter = causal.get('causal_auth_parameter_failure', '')
    login = causal.get('causal_auth_login_failure', '')
    pattern = r'\b([a-z_]+)\.[a-z_]+ (?:requires access_token|returned 401);'
    apps = set(re.findall(pattern, parameter + '\n' + login))
    if len(apps) != 1:
        return None
    details = {}
    public_retrieval_stage(messages[:-2], details)
    completed = (auth_completed_login_prefix(messages, causal)
                 if any(ref.get('login_evidence') == 'completed_prefix' for ref in refs) else None)
    if completed:
        causal['completed_authentication_state'] = (
            'Derived from the current executed failure traceback: '
            f"{completed['app']}.login completed at line {completed['login_line']}; "
            f"the access_token key was read at line {completed['token_line']} before "
            f"{completed['app']}.{completed['method']} failed at line {completed['failure_line']}. "
            'Only key presence and execution order were used; no token value was read.')
    phase = ('parameters' if not login and (completed or next(iter(apps)) in details.get('authenticated_apps', []))
             else 'login')
    schema = [(ref, auth_parameter_schema_mismatch(ref, messages, causal))
              for ref in refs if phase == 'parameters' and ref.get('parameter_evidence') == 'source_schema']
    eligible_schema = [(ref, mismatch) for ref, mismatch in schema if mismatch]
    if eligible_schema:
        causal['source_parameter_schema_mismatch'] = json.dumps(
            [mismatch for ref, mismatch in eligible_schema], sort_keys=True)
        return [{key: ref[key] for key in ('memory_id', 'content_revision')}
                for ref, mismatch in eligible_schema]
    return [{key: ref[key] for key in ('memory_id', 'content_revision')}
            for ref in refs if ref.get('phase') == phase and ref.get('parameter_evidence') != 'source_schema']


def auth_completed_login_prefix(messages, causal):
    """Prove a login/key read before the uniquely observed protected failure."""
    import ast
    import re
    if (not isinstance(causal, dict) or causal.get('causal_auth_login_failure')
            or not isinstance(messages, list) or len(messages) < 2):
        return None
    parameter_failure = causal.get('causal_auth_parameter_failure')
    if not isinstance(parameter_failure, str):
        return None
    pairs = re.findall(r'\b([a-z_]+)\.([a-z_]+) requires access_token;', parameter_failure)
    if len(set(pairs)) != 1:
        return None
    app, method = pairs[0]
    action, observation = messages[-2:]
    if (not isinstance(action, dict) or not isinstance(observation, dict)
            or action.get('role') != 'assistant' or observation.get('role') != 'user'
            or not isinstance(action.get('content'), str)
            or not isinstance(observation.get('content'), str)):
        return None
    text = observation['content']
    if (not text.lstrip().startswith(('Execution failed', 'Traceback'))
            or 'Traceback' not in text or not re.search(r'\b401\b', text)):
        return None
    traceback = text[text.index('Traceback'):]
    observed = set(re.findall(r'\bapis\.([a-z_]+)\.([a-z_]+)\s*\(', traceback))
    lines = re.findall(r'\bline\s+([1-9][0-9]*)\b', traceback, re.I)
    if observed != {(app, method)} or len(lines) != 1 or method == 'login':
        return None
    try:
        tree = ast.parse(action['content'])
    except (SyntaxError, ValueError):
        return None

    def api_call(value):
        if (isinstance(value, ast.Call) and isinstance(value.func, ast.Attribute)
                and isinstance(value.func.value, ast.Attribute)
                and isinstance(value.func.value.value, ast.Name)
                and value.func.value.value.id == 'apis'):
            return value.func.value.attr, value.func.attr
        return None

    failed = [node for node in ast.walk(tree) if api_call(node) == (app, method)]
    if (len(failed) != 1 or failed[0].lineno != int(lines[0]) or failed[0].args
            or any(keyword.arg in (None, 'access_token') for keyword in failed[0].keywords)):
        return None
    failure_index = next((i for i, statement in enumerate(tree.body)
                          if isinstance(statement, (ast.Assign, ast.AnnAssign, ast.Expr))
                          and statement.value is failed[0]), None)
    if failure_index is None:
        return None
    shadowed_print = any(
        isinstance(node, ast.Name) and isinstance(node.ctx, ast.Store) and node.id == 'print'
        or isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef, ast.ClassDef)) and node.name == 'print'
        or isinstance(node, ast.alias) and (node.asname or node.name) == 'print'
        for statement in tree.body[:failure_index] for node in ast.walk(statement))
    login_results = {}
    login_line = token_line = None
    for statement in tree.body[:failure_index]:
        if not isinstance(statement, (ast.Assign, ast.AnnAssign, ast.Expr)):
            login_results.clear()
            login_line = token_line = None
            continue
        value = statement.value
        targets = (statement.targets if isinstance(statement, ast.Assign)
                   else [statement.target] if isinstance(statement, ast.AnnAssign) else [])
        names = [target.id for target in targets if isinstance(target, ast.Name)]
        if any(not isinstance(target, ast.Name) for target in targets):
            login_results.clear()
            login_line = token_line = None
            continue
        direct = api_call(value)
        extracted = (isinstance(value, ast.Subscript) and isinstance(value.slice, ast.Constant)
                     and value.slice.value == 'access_token')
        source = value.value if extracted else None
        inline = api_call(source) if extracted else None
        source_line = (login_results.get(source.id) if isinstance(source, ast.Name) else None)
        if direct == (app, 'login') and len(names) == 1:
            login_results = {names[0]: statement.lineno}
            login_line, token_line = statement.lineno, None
        elif extracted and (inline == (app, 'login') or source_line is not None):
            login_line = statement.lineno if inline else source_line
            token_line = statement.lineno
        else:
            # Intervening arbitrary calls or control flow cannot prove an active session.
            calls = [node for node in ast.walk(value) if isinstance(node, ast.Call)]
            safe_print = (isinstance(statement, ast.Expr) and isinstance(value, ast.Call)
                          and isinstance(value.func, ast.Name) and value.func.id == 'print'
                          and calls == [value] and not shadowed_print)
            if calls and not safe_print:
                login_results.clear()
                login_line = token_line = None
        for name in names:
            if not (direct == (app, 'login') and len(names) == 1):
                login_results.pop(name, None)
    if login_line is None or token_line is None:
        return None
    return {'app': app, 'method': method, 'login_line': login_line,
            'token_line': token_line, 'failure_line': failed[0].lineno}


def auth_login_failure_state(policy, messages, instruction, project, *, public_contracts=None):
    """Opt-in login recovery: a unique executed 401 call bound to public schema."""
    import ast
    import re
    if not isinstance(messages, list) or len(messages) < 2:
        return None
    code, observation = messages[-2:]
    if (not isinstance(code, dict) or not isinstance(observation, dict)
            or code.get('role') != 'assistant' or observation.get('role') != 'user'
            or not isinstance(code.get('content'), str) or not isinstance(observation.get('content'), str)):
        return None
    text = observation['content']
    if not text.lstrip().startswith(('Execution failed', 'Traceback')) or not re.search(r'\b401\b', text):
        return None
    contracts = {tuple(pair) for pair in public_contracts or []
                 if isinstance(pair, (list, tuple)) and len(pair) == 2
                 and all(isinstance(part, str) and re.fullmatch(r'[a-z_]+', part) for part in pair)}
    try:
        tree = ast.parse(code['content'])
    except (SyntaxError, ValueError):
        return None
    observed = set(re.findall(r'\bapis\.([a-z_]+)\.([a-z_]+)\s*\(', text))
    eligible = set()
    for node in ast.walk(tree):
        if (not isinstance(node, ast.Call) or not isinstance(node.func, ast.Attribute)
                or not isinstance(node.func.value, ast.Attribute)
                or not isinstance(node.func.value.value, ast.Name) or node.func.value.value.id != 'apis'
                or node.args or any(kw.arg is None for kw in node.keywords)):
            continue
        pair = (node.func.value.attr, node.func.attr)
        if pair in observed and (pair in contracts or pair[1] == 'login'
                                and any(app == pair[0] for app, method in contracts)):
            eligible.add(pair)
    if len(eligible) != 1:
        return None
    app, method = next(iter(eligible))
    state = office_retrieval_state(policy, messages, instruction, project)
    state['causal_auth_login_failure'] = ('Derived from the current executed 401 and the public API '
        'schema registry: ' + app + '.' + method + ' returned 401; recover authentication for this app.')
    return state


def appworld_saved_result_experiences(artifact):
    """Bind saved-result advice to exact approved sources in a new artifact."""
    if not isinstance(artifact, dict) or artifact.get('level') not in ('L1', 'L2', 'L3'):
        return None
    refs = ((artifact.get('agent') or {}).get('appworld_options') or {}).get('l1_saved_result_experiences')
    if refs is None:
        return None
    policy = artifact.get('policy') or {}
    approved = {(ref.get('memory_id'), ref.get('content_revision'))
                for ref in policy.get('options', {}).get('approved_experiences', [])}
    entries = {(row.get('memory_id'), row.get('content_revision')): row
               for row in policy.get('snapshot', {}).get('experiences', [])}
    if not isinstance(refs, list) or not 1 <= len(refs) <= 4:
        raise ValueError('appworld_saved_result_experiences_invalid')
    seen, contents = set(), []
    for ref in refs:
        if (not isinstance(ref, dict) or set(ref) != {'memory_id', 'content_revision'}
                or not all(isinstance(value, str) and value for value in ref.values())):
            raise ValueError('appworld_saved_result_experiences_invalid')
        pair = (ref['memory_id'], ref['content_revision'])
        entry = entries.get(pair)
        if (pair not in approved or pair in seen or entry is None
                or not isinstance(entry.get('content'), str) or not entry['content'].strip()):
            raise ValueError('appworld_saved_result_experiences_source_invalid')
        seen.add(pair)
        contents.append(entry['content'])
    return {'refs': [dict(ref) for ref in refs], 'contents': contents}


def appworld_auth_structured_missing(artifact):
    if not isinstance(artifact, dict) or artifact.get('level') not in ('L1', 'L2', 'L3'):
        return False
    options = (artifact.get('agent') or {}).get('appworld_options') or {}
    enabled = options.get('l1_auth_structured_missing', False)
    if type(enabled) is not bool:
        raise ValueError('appworld_auth_structured_missing_invalid')
    if enabled and appworld_auth_parameter_experiences(artifact) is None:
        raise ValueError('appworld_auth_structured_missing_source_invalid')
    return enabled


def pure_authentication_experience(content):
    """Identify credential advice without independent task or schema guidance."""
    if not isinstance(content, str) or not content.strip():
        return False
    text = content.lower()
    authentication = re.search(r'access_token|password|credential|认证|登录|凭证', text)
    independent = re.search(
        r'api_docs|参数名|参数定义|签名|关键字|数据结构|返回类型|字典|索引|isinstance'
        r'|quantity|page_index|page_limit|duration|rating|release_date|cart|wish_list'
        r'|playlist|note_id|thread|分页|时长|数量|购物车|愿望清单|歌曲|播放|发送|删除|移动|归档', text)
    return bool(authentication and not independent)


def appworld_shape_error_auth_experiences(artifact):
    """Bind an opt-in exclusion to approved, unchanged credential-only sources."""
    if not isinstance(artifact, dict) or artifact.get('level') not in ('L1', 'L2', 'L3'):
        return None
    options = (artifact.get('agent') or {}).get('appworld_options') or {}
    refs = options.get('l1_shape_error_auth_experiences')
    if refs is None:
        return None
    policy = artifact.get('policy') or {}
    approved = policy.get('options', {}).get('approved_experiences', [])
    pairs = {(row.get('memory_id'), row.get('content_revision')) for row in approved}
    experiences = {(row.get('memory_id'), row.get('content_revision')): row
                   for row in policy.get('snapshot', {}).get('experiences', [])}
    if not isinstance(refs, list) or not 1 <= len(refs) <= len(pairs):
        raise ValueError('appworld_shape_error_auth_experiences_invalid')
    seen, contents = set(), []
    for ref in refs:
        if (not isinstance(ref, dict) or set(ref) != {'memory_id', 'content_revision'}
                or not all(isinstance(v, str) and v for v in ref.values())):
            raise ValueError('appworld_shape_error_auth_experiences_invalid')
        pair = (ref['memory_id'], ref['content_revision'])
        experience = experiences.get(pair)
        if (pair not in pairs or pair in seen or experience is None
                or not pure_authentication_experience(experience.get('content'))):
            raise ValueError('appworld_shape_error_auth_experiences_source_invalid')
        seen.add(pair)
        contents.append(experience['content'])
    return {'excluded_refs': [dict(ref) for ref in refs],
            'approved_refs': [dict(ref) for ref in approved], 'excluded_contents': contents}


def non_authentication_type_error(messages):
    """Use only the current executed-code observation, never earlier failures."""
    if not isinstance(messages, list) or len(messages) < 2:
        return False
    previous, current = messages[-2:]
    if (not isinstance(previous, dict) or not isinstance(current, dict)
            or previous.get('role') != 'assistant' or current.get('role') != 'user'
            or not isinstance(previous.get('content'), str)
            or not re.search(r'\bapis\.\w+\.\w+\s*\(', previous['content'])
            or not isinstance(current.get('content'), str)):
        return False
    observation = current['content']
    terminal_errors = re.findall(r'\bTypeError\s*:[^\r\n]*',
                                 observation.replace('\\n', '\n'))
    # A traceback may echo the correctly supplied token in a source line.
    # Only the terminal exception establishes an authentication parameter
    # error; factual HTTP/host authentication failures still veto the window.
    return bool(terminal_errors
                and not re.search(r'\b401\b|Unauthorized|Invalid credentials'
                                  r'|appworld_missing_access_token',
                                  observation, re.IGNORECASE)
                and not re.search(r'access_token|\btoken\b|authentication|credential'
                                  r'|password|认证|登录',
                                  '\n'.join(terminal_errors), re.IGNORECASE))


def _runtime_shape_error_auth_experiences(state=None):
    # The immutable version file is also used for the existing advice bound.
    # Load this option once per observer rather than rereading it per request.
    # The nested runtime removes trial-entry environment variables. The
    # outer host validates the immutable source and passes this private
    # payload through the same state used by the other L1 options.
    if isinstance(state, dict) and state.get('appworld_shape_error_auth_experiences') is not None:
        return copy.deepcopy(state['appworld_shape_error_auth_experiences'])
    config_path = os.environ.get('TRAJECTORY_EVOLUTION_TRIAL_CONFIG', '')
    if not config_path:
        return None
    try:
        version = json.loads(Path(config_path).with_name('version.json').read_text(encoding='utf-8'))
    except (OSError, ValueError, TypeError):
        return None
    return appworld_shape_error_auth_experiences(version.get('artifact'))


def project_public_context(value):
    """Preserve public world data inside this private benchmark process.

    Real credentials registered by the provider/environment loaders remain
    blocked. Fixture passwords and Python expressions are executable task
    inputs and must survive hooks and saved conversation history unchanged.
    """
    from agentflow_runtime.security.secrets import (
        SecretLeakBlocked, find_registered_secret_exposures,
    )
    report = find_registered_secret_exposures(value)
    if report.applied:
        raise SecretLeakBlocked(report)
    return copy.deepcopy(value), report


def project_tool_response(value, redact=None):
    """Keep executable AppWorld code intact at private response boundaries.

    Synthetic credentials returned by the official world are task data. Real
    provider/environment credentials must never enter submitted code. Other
    tools and response fields retain the production redaction policy.
    """
    from agentflow_runtime.security import redact_secrets
    from agentflow_runtime.security.secrets import (
        SecretLeakBlocked, find_registered_secret_exposures,
    )
    if isinstance(value, list) and all(isinstance(item, dict) and "role" in item
                                       for item in value):
        return project_public_context(value)
    safe, report = (redact or redact_secrets)(value)
    if not isinstance(value, dict) or not isinstance(safe, dict):
        return safe, report
    def restore(original, projected):
        if isinstance(original, list) and isinstance(projected, list):
            for item, safe_item in zip(original, projected):
                restore(item, safe_item)
            return
        if not isinstance(original, dict) or not isinstance(projected, dict):
            return
        for key, item in original.items():
            if key in {"messages", "history"} and isinstance(item, list):
                projected[key], _ = project_public_context(item)
            elif key != "tool_calls" and key in projected:
                restore(item, projected[key])
        for original_call, safe_call in zip(original.get("tool_calls", []),
                                            projected.get("tool_calls", [])):
            if not isinstance(original_call, dict) or not isinstance(safe_call, dict):
                continue
            original_function = original_call.get("function") or {}
            safe_function = safe_call.get("function") or {}
            if original_function.get("name") != TOOL_NAME:
                continue
            try:
                arguments = json.loads(original_function.get("arguments", ""))
            except (TypeError, ValueError):
                continue
            if not isinstance(arguments, dict) or not isinstance(arguments.get("code"), str):
                continue
            exposure = find_registered_secret_exposures(arguments["code"])
            if exposure.applied:
                raise SecretLeakBlocked(exposure)
            safe_arguments, _ = (redact or redact_secrets)(arguments)
            safe_arguments["code"] = arguments["code"]
            safe_function["arguments"] = json.dumps(safe_arguments, ensure_ascii=False)
    restore(value, safe)
    return safe, report



def _install_private_tool_boundary():
    """Keep the original registry dispatch, with a scope for this reply tool."""
    from contextvars import ContextVar
    from agentflow_runtime import tool_registry
    from agentflow_runtime.security.secrets import find_registered_secret_exposures
    original_execute = tool_registry._LocalToolRegistry.execute
    if getattr(original_execute, "_appworld_reply_boundary", False):
        return
    original_guard = tool_registry.find_secret_exposures
    original_redactor = tool_registry.redact_secrets
    current_tool = ContextVar("appworld_private_reply_tool", default=None)

    def inspect_arguments(value):
        if current_tool.get() == TOOL_NAME:
            return find_registered_secret_exposures(value)
        return original_guard(value)

    def project_result(value):
        if current_tool.get() == TOOL_NAME:
            return project_public_context(value)
        return original_redactor(value)

    def execute(self, name, args):
        token = current_tool.set(name)
        try:
            return original_execute(self, name, args)
        finally:
            current_tool.reset(token)

    execute._appworld_reply_boundary = True
    tool_registry.find_secret_exposures = inspect_arguments
    tool_registry.redact_secrets = project_result
    tool_registry._LocalToolRegistry.execute = execute


def install_private_reply_transport():
    """Require the one reply tool only in this disposable node process."""
    from agentflow_runtime.common.clients import OpenAiCompatibleClient
    from agentflow_runtime.common.clients import openai_compatible_client
    from agentflow_runtime import recorder
    from agentflow_runtime.hooks import engine
    from agentflow_runtime.security.secrets import find_registered_secret_exposures
    _install_private_tool_boundary()
    for module in (openai_compatible_client, recorder, engine):
        # This private benchmark process handles official synthetic account
        # data. Check actual environment/provider values, not code variable
        # names or public fixture passwords, before sending model context.
        if hasattr(module, "find_secret_exposures"):
            module.find_secret_exposures = find_registered_secret_exposures
        redactor = module.redact_secrets
        if not getattr(redactor, "_appworld_code_projection", False):
            def project(value):
                return project_public_context(value)
            project._appworld_code_projection = True
            module.redact_secrets = project
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


def public_instruction_from_policy(policy):
    """Read the exact public_context object emitted by the private host."""
    decoder = json.JSONDecoder()
    for index, char in enumerate(policy):
        if char != '{' or (index and policy[index - 1] not in '\n\r'):
            continue
        try:
            value, _ = decoder.raw_decode(policy[index:])
        except ValueError:
            continue
        if (isinstance(value, dict) and {'instruction', 'supervisor', 'datetime',
                'app_descriptions', 'bootstrap_api_docs'} <= set(value)
                and isinstance(value.get('instruction'), str)):
            return value['instruction']
    return ''



def _public_access_token_contracts(text):
    """Read required token names from actual returned public API documents."""
    import ast
    contracts, offset = set(), 0
    while (start := text.find('{', offset)) >= 0:
        depth, quote, escaped = 0, None, False
        end = start
        for end in range(start, len(text)):
            char = text[end]
            if quote:
                if escaped:
                    escaped = False
                elif char == '\\':
                    escaped = True
                elif char == quote:
                    quote = None
            elif char in ("'", '"'):
                quote = char
            elif char == '{':
                depth += 1
            elif char == '}':
                depth -= 1
                if not depth:
                    break
        offset = end + 1
        if depth:
            break
        body = text[start:offset]
        try:
            value = json.loads(body)
        except ValueError:
            try:
                value = ast.literal_eval(body)
            except (ValueError, SyntaxError, MemoryError, RecursionError):
                continue
        parameters = value.get('parameters') if isinstance(value, dict) else None
        if (isinstance(value, dict) and isinstance(value.get('app_name'), str)
                and isinstance(value.get('api_name'), str)
                and isinstance(parameters, list)
                and any(isinstance(item, dict) and item.get('name') == 'access_token'
                        and item.get('required') is True for item in parameters)):
            contracts.add((value['app_name'], value['api_name']))
    return contracts


def auth_parameter_failure_state(policy, messages, instruction, project, *, public_contracts=None,
                                 allow_structured_missing=False):
    """Associate a real auth failure or opt-in Host rejection with a contract.

    The diagnostic is retrieval metadata, not a substituted observation.
    Ambiguous calls and missing contracts do not qualify an experience.
    """
    import ast
    import re
    if not isinstance(messages, list) or len(messages) < 2:
        return None
    code_row, observation = messages[-2:]
    if (not isinstance(code_row, dict) or not isinstance(observation, dict)
            or code_row.get('role') != 'assistant' or observation.get('role') != 'user'):
        return None
    code, text = code_row.get('content'), observation.get('content')
    if not isinstance(code, str) or not isinstance(text, str):
        return None
    structured = False
    if allow_structured_missing is True:
        try:
            guard = json.loads(text)
        except (ValueError, TypeError):
            guard = None
        structured = (isinstance(guard, dict) and set(guard) == {'status', 'error_code', 'message'}
                      and guard.get('status') == 'rejected'
                      and guard.get('error_code') == 'appworld_missing_access_token'
                      and guard.get('message') == 'The host blocked a protected call without access_token. '
                          'Reuse the token variable returned by the successful login; do not login again.')
    if not structured and (not text.lstrip().startswith(('Execution failed', 'Traceback'))
                           or re.search(r'\b401\b', text) is None):
        return None
    contracts = set()
    registry_contracts = set()
    if isinstance(public_contracts, list):
        for pair in public_contracts:
            if (isinstance(pair, (list, tuple)) and len(pair) == 2
                    and all(isinstance(part, str) and re.fullmatch(r'[a-z_]+', part) for part in pair)):
                registry_contracts.add(tuple(pair))
        contracts.update(registry_contracts)
    for index in range(1, len(messages) - 1):
        request, response = messages[index - 1], messages[index]
        if (not isinstance(request, dict) or not isinstance(response, dict)
                or request.get('role') != 'assistant' or response.get('role') != 'user'
                or not isinstance(request.get('content'), str)
                or not isinstance(response.get('content'), str)
                or response['content'].lstrip().startswith(('Execution failed', 'Traceback'))):
            continue
        try:
            tree = ast.parse(request['content'])
        except (SyntaxError, ValueError):
            continue
        requested = set()
        # Only unconditional returned doc calls count; unused branch contents
        # and fabricated document-like strings never establish a contract.
        values = [node.value for node in tree.body
                  if isinstance(node, (ast.Assign, ast.AnnAssign, ast.Expr))]
        while values:
            node = values.pop()
            if not isinstance(node, ast.Call):
                continue
            if (isinstance(node.func, ast.Name) and node.func.id in ('print', 'str', 'repr')
                    or isinstance(node.func, ast.Attribute) and node.func.attr == 'dumps'
                    and isinstance(node.func.value, ast.Name) and node.func.value.id == 'json'):
                values.extend(node.args)
            if (isinstance(node.func, ast.Attribute) and node.func.attr == 'show_api_doc'
                    and isinstance(node.func.value, ast.Attribute) and node.func.value.attr == 'api_docs'
                    and isinstance(node.func.value.value, ast.Name) and node.func.value.value.id == 'apis'):
                params = {kw.arg: kw.value for kw in node.keywords if kw.arg is not None}
                app, method = params.get('app_name'), params.get('api_name')
                if (isinstance(app, ast.Constant) and isinstance(method, ast.Constant)
                        and isinstance(app.value, str) and isinstance(method.value, str)):
                    requested.add((app.value, method.value))
        contracts.update(requested & _public_access_token_contracts(response['content']))
    try:
        tree = ast.parse(code)
    except (SyntaxError, ValueError):
        return None
    traceback_calls = set(re.findall(r'\bapis\.([a-z_]+)\.([a-z_]+)\s*\(', text))
    eligible = set()
    for node in ast.walk(tree):
        if (not isinstance(node, ast.Call) or not isinstance(node.func, ast.Attribute)
                or not isinstance(node.func.value, ast.Attribute)
                or not isinstance(node.func.value.value, ast.Name)
                or node.func.value.value.id != 'apis'):
            continue
        pair = (node.func.value.attr, node.func.attr)
        keywords = {kw.arg for kw in node.keywords}
        if (pair in contracts and (structured or pair in traceback_calls) and pair[1] != 'login'
                and not node.args and None not in keywords and 'access_token' not in keywords):
            eligible.add(pair)
    if len(eligible) != 1:
        return None
    app, method = next(iter(eligible))
    state = office_retrieval_state(policy, messages, instruction, project)
    state['causal_auth_parameter_failure'] = (
        ('Derived from the current host_missing_access_token rejection and ' if structured
         else 'Derived from the current executed 401 and ')
        + ('the public API schema registry: ' if (app, method) in registry_contracts
           else 'the previously returned public API contract: ')
        + app + '.' + method + ' requires access_token; '
        + ('the submitted call omitted that parameter.' if structured
           else 'the executed call omitted that parameter.'))
    return state


def public_retrieval_stage(messages, details=None):
    """Derive per-application phases from actual public executed feedback."""
    import ast
    import re
    authenticated = set()
    token_contracts = set()
    active = set()
    stage = ''
    for index in range(1, len(messages)):
        observation, action = messages[index], messages[index - 1]
        if (not isinstance(action, dict) or not isinstance(observation, dict)
                or action.get('role') != 'assistant' or observation.get('role') != 'user'
                or not isinstance(action.get('content'), str)
                or not isinstance(observation.get('content'), str)):
            continue
        code, text = action['content'], observation['content']
        try:
            tree = ast.parse(code)
        except SyntaxError:
            stage = ''
            continue
        calls, docs, requested_docs, token_aliases = [], set(), set(), set()
        direct_nodes = set()
        pending_values = [statement.value for statement in tree.body
                          if isinstance(statement, (ast.Assign, ast.AnnAssign, ast.Expr))]
        while pending_values:
            value = pending_values.pop()
            if not isinstance(value, ast.Call):
                continue
            direct_nodes.add(id(value))
            if (isinstance(value.func, ast.Name) and value.func.id in ('print', 'str', 'repr')
                    or isinstance(value.func, ast.Attribute) and value.func.attr == 'dumps'
                    and isinstance(value.func.value, ast.Name) and value.func.value.id == 'json'):
                pending_values.extend(value.args)
        for node in ast.walk(tree):
            if (not isinstance(node, ast.Call) or not isinstance(node.func, ast.Attribute)
                    or not isinstance(node.func.value, ast.Attribute)
                    or not isinstance(node.func.value.value, ast.Name)
                    or node.func.value.value.id != 'apis'):
                continue
            app, method = node.func.value.attr, node.func.attr
            calls.append((app, method))
            keywords = {kw.arg for kw in node.keywords}
            if 'token' in keywords and 'access_token' not in keywords and method != 'login':
                token_aliases.add((app, method))
            if app == 'api_docs':
                value = next((kw.value for kw in node.keywords if kw.arg == 'app_name'),
                             node.args[0] if node.args else None)
                if isinstance(value, ast.Constant) and isinstance(value.value, str):
                    docs.add(value.value)
                    api = next((kw.value for kw in node.keywords if kw.arg == 'api_name'), None)
                    if (method == 'show_api_doc' and id(node) in direct_nodes
                            and isinstance(api, ast.Constant) and isinstance(api.value, str)):
                        requested_docs.add((value.value, api.value))
        current = {app for app, _ in calls if app not in ('supervisor', 'api_docs')} or docs
        if current:
            active = current
        failed = text.lstrip().startswith(('Execution failed', 'Traceback'))
        auth_failure = failed and bool(re.search(r'\b401\b|Unauthorized|Invalid credentials', text, re.I))
        if not failed and requested_docs:
            token_contracts.update(_public_access_token_contracts(text) & requested_docs)
        # A protected-call argument mismatch is not evidence that login failed.
        # Require its actual traceback, returned contract, and executed code.
        traceback = text.split('Traceback:', 1)[1] if auth_failure and 'Traceback:' in text else ''
        failed_calls = set(re.findall(r'\bapis\.([a-z_]+)\.([a-z_]+)\s*\([^\n]*\btoken\s*=', traceback))
        token_parameter_failure = bool(failed_calls & token_aliases & token_contracts)
        # Authenticate only from unconditional top-level evaluation. Calls
        # inside functions, lambdas, branches or swallowed try blocks are not
        # evidence that a login or token extraction actually happened.
        login_results = {}
        extracted_apps, printed_apps = set(), set()
        def direct_login(value):
            if (isinstance(value, ast.Call) and isinstance(value.func, ast.Attribute)
                    and value.func.attr == 'login' and isinstance(value.func.value, ast.Attribute)
                    and isinstance(value.func.value.value, ast.Name)
                    and value.func.value.value.id == 'apis'):
                return value.func.value.attr
            return None
        def token_source(value):
            if (not isinstance(value, ast.Subscript) or not isinstance(value.slice, ast.Constant)
                    or value.slice.value != 'access_token'):
                return None
            return direct_login(value.value) or (login_results.get(value.value.id)
                if isinstance(value.value, ast.Name) else None)
        for statement in tree.body:
            if isinstance(statement, (ast.Assign, ast.AnnAssign, ast.Expr)):
                value = statement.value
                extracted_app = token_source(value)
                if extracted_app:
                    extracted_apps.add(extracted_app)
                if (isinstance(value, ast.Call) and isinstance(value.func, ast.Name)
                        and value.func.id == 'print'):
                    arguments = list(value.args)
                    while arguments:
                        argument = arguments.pop()
                        if (isinstance(argument, ast.Call) and (
                                isinstance(argument.func, ast.Name)
                                and argument.func.id in ('str', 'repr')
                                or isinstance(argument.func, ast.Attribute)
                                and argument.func.attr == 'dumps'
                                and isinstance(argument.func.value, ast.Name)
                                and argument.func.value.id == 'json')):
                            arguments.extend(argument.args)
                            continue
                        extracted_app = token_source(argument)
                        if extracted_app:
                            extracted_apps.add(extracted_app)
                        printed_app = direct_login(argument) or (login_results.get(argument.id)
                            if isinstance(argument, ast.Name) else None)
                        if printed_app:
                            printed_apps.add(printed_app)
                if isinstance(statement, (ast.Assign, ast.AnnAssign)):
                    targets = statement.targets if isinstance(statement, ast.Assign) else [statement.target]
                    for target in targets:
                        if isinstance(target, ast.Name):
                            login_results.pop(target.id, None)
                            app = direct_login(value)
                            if app:
                                login_results[target.id] = app
        token_response = bool(re.search(r"(?:redacted_)?access_token['\"]?\s*[:=]", text))
        if not failed:
            authenticated.update(extracted_apps)
            if token_response:
                authenticated.update(printed_apps)
        if not failed:
            authenticated.difference_update(app for app, method in calls if method == 'logout')
        if auth_failure and not token_parameter_failure:
            authenticated.difference_update(active)
        stage = ''
        fetched = ('supervisor', 'show_account_passwords') in calls
        if token_parameter_failure:
            stage = 'authentication_parameter_failure access_token'
        elif auth_failure or (fetched and not failed and active - authenticated):
            stage = 'authentication_needed login'
        elif failed and 'TypeError' in text and re.search(r'positional', text, re.I):
            stage = 'positional_type_error api_failure'
        elif failed and re.search(r'No API named|has no attribute', text):
            stage = 'unknown_api api_documentation'
    if details is not None:
        details['authenticated_apps'] = sorted(authenticated)
        details['active_apps'] = sorted(active)
    return stage


def public_playlist_downloaded_duration_playback(instruction):
    """Recognize only the present public request, never a tool's documentation."""
    import re
    if not isinstance(instruction, str):
        return False
    playlist = bool(re.search(r'\bplaylists?\b', instruction, re.I))
    album = bool(re.search(r'\balbums?\b', instruction, re.I))
    playback = bool(re.search(r'\b(?:play|playing|listen|listening)\b', instruction, re.I))
    negated = bool(re.search(r"\b(?:do not|don't|not|without)\s+(?:start\s+)?(?:play|playing|listen|listening)\b", instruction, re.I))
    duration = bool(re.search(r'\b(?:duration|minutes?|seconds?|hours?)\b', instruction, re.I))
    offline = bool(re.search(r'\b(?:downloaded|offline)\b|\bwithout\b[^.\n]{0,40}\b(?:network|internet|connection)\b|\bno\s+(?:network|internet)\b', instruction, re.I))
    all_downloaded = bool(re.search(
        r'\b(?:all|every|each|entire)\b[^.\n]{0,50}\b(?:songs?|tracks?)\b'
        r'[^.\n]{0,30}\b(?:downloaded|offline)\b', instruction, re.I))
    return playlist and not album and playback and not negated and duration and offline and not all_downloaded


def _public_downloaded_duration_playback(instruction, object_kind='playlist', *, scoped_quantifier=False):
    """Distinguish a requested all-download condition from a rejected filter."""
    import re
    if not isinstance(instruction, str) or object_kind not in ('playlist', 'album'):
        return False
    requested = bool(re.search(r'\b' + object_kind + r's?\b', instruction, re.I))
    other = 'album' if object_kind == 'playlist' else 'playlist'
    if not requested or re.search(r'\b' + other + r's?\b', instruction, re.I):
        return False
    if not re.search(r'\b(?:play|playing|listen|listening)\b', instruction, re.I):
        return False
    if re.search(r"\b(?:do not|don't|not|without)\s+(?:start\s+)?(?:play|playing|listen|listening)\b", instruction, re.I):
        return False
    if not re.search(r'\b(?:duration|minutes?|seconds?|hours?)\b', instruction, re.I):
        return False
    if not re.search(r'\b(?:downloaded|offline)\b|\bwithout\b[^.\n]{0,40}\b(?:network|internet|connection)\b|\bno\s+(?:network|internet)\b', instruction, re.I):
        return False
    all_pattern = (r'\b(?:all|every|each|entire)\b[^.;\n]{0,50}\b(?:songs?|tracks?)\b'
                   r'[^.;\n]{0,30}\b(?:downloaded|offline)\b'
                   r'|\b(?:fully|entirely|completely)\s+downloaded\s+(?:playlists?|albums?)\b')
    if scoped_quantifier:
        # "All album-library and downloaded-song pages" quantifies pages,
        # not every song's downloaded state. Preserve the requested noun's
        # scope instead of joining words across an unrelated page clause.
        all_pattern = (r'\b(?:all|every|each|entire)(?:[\s-]+(?:of|the|my|its|their|these|those))*'
                       r'[\s-]+(?:songs?|tracks?)\b[^.;\n]{0,30}\b(?:downloaded|offline)\b'
                       r'|\b(?:fully|entirely|completely)\s+downloaded\s+(?:playlists?|albums?)\b')
    for sentence in re.split(r'[.;\n]', instruction):
        for match in re.finditer(all_pattern, sentence, re.I):
            before, after = sentence[:match.start()], sentence[match.end():]
            rejected = (re.search(r"\b(?:do not|don't|not)\s+(?:add|require|impose|assume|apply|enforce|use)\b[^.;\n]{0,90}$", before, re.I)
                        or re.search(r'\b(?:not|without requiring|without needing)\s*$', before, re.I)
                        or re.search(r'\b(?:not required|not necessary|does not|do not|are allowed|may be undownloaded)\b', after, re.I)
                        or (re.search(r'\b(?:empty|no)\s+results?\b', sentence, re.I)
                            and re.search(r'\b(?:filter|filtering|requirement)\b', sentence, re.I)
                            and re.search(r'\b(?:reconsider|compare|actual|subset)\b', sentence, re.I)))
            if not rejected:
                return False
    return True


def public_downloaded_duration_playback_v2(instruction, object_kind='playlist'):
    return _public_downloaded_duration_playback(instruction, object_kind)


def public_downloaded_duration_playback_v3(instruction, object_kind='playlist'):
    return _public_downloaded_duration_playback(instruction, object_kind, scoped_quantifier=True)


def public_spotify_downloaded_subset_observed(messages):
    """Require a real successful Spotify downloaded-song read for the phase."""
    import ast
    import re
    if not isinstance(messages, list):
        return False
    for index in range(1, len(messages)):
        action, observation = messages[index - 1], messages[index]
        if (not isinstance(action, dict) or not isinstance(observation, dict)
                or action.get('role') != 'assistant' or observation.get('role') != 'user'
                or not isinstance(action.get('content'), str)
                or not isinstance(observation.get('content'), str)):
            continue
        text = observation['content']
        if re.search(r'Traceback|Execution failed|\b(?:401|422)\b|Unauthorized', text, re.I):
            continue
        try:
            tree = ast.parse(action['content'])
        except SyntaxError:
            continue
        if any(isinstance(node, ast.Call)
               and isinstance(node.func, ast.Attribute)
               and node.func.attr == 'show_downloaded_songs'
               and isinstance(node.func.value, ast.Attribute)
               and node.func.value.attr == 'spotify'
               and isinstance(node.func.value.value, ast.Name)
               and node.func.value.value.id == 'apis'
               for node in ast.walk(tree)):
            return True
    return False


def public_spotify_playlist_library_complete_enumeration(instruction):
    """A public whole-library read requires collecting the available pages."""
    import re
    if not isinstance(instruction, str):
        return False
    if not (re.search(r'\bspotify\b', instruction, re.I)
            and re.search(r'\bplaylists\b|\bplaylist\s+library\b', instruction, re.I)):
        return False
    operation = r'(?:give|count|list|find|choose|select|inspect|show|enumerate|collect|read|which|determine)'
    positive = re.search(r'\b' + operation + r'\b', instruction, re.I)
    negated = re.search(r"\b(?:do not|don't|never|not)\s+(?:ever\s+)?" + operation + r'\b', instruction, re.I)
    if not positive or (negated and negated.start() <= positive.start()):
        return False
    complete = re.search(r'\b(?:all|every|entire|complete)\b[^.\n]{0,40}\b(?:pages?|playlists?|library|collections?)\b', instruction, re.I)
    aggregate = re.search(r'\b(?:count|least|most|fewest|highest|lowest|total|number)\b', instruction, re.I)
    return bool(complete or aggregate)


def public_gmail_inbox_complete_enumeration(instruction):
    """Recognize a complete non-archived Gmail bulk request.

    A number of public tasks say “all Gmail threads ... ignore archived” instead
    of spelling out ``inbox``.  The archived qualifier is an explicit scope
    equivalent for this retrieval phase; it is not inferred from a generic
    mention of Gmail.  Keep the request and qualifier together while avoiding
    matches from unrelated trailing prose.
    """
    import re
    if not isinstance(instruction, str):
        return False
    text = instruction.strip()
    if not text:
        return False
    # The first sentence contains the operation and target.  A later sentence
    # may supply the narrow, explicit non-archived scope (e.g. “Ignore the
    # archived threads.”); no other trailing sentence broadens the match.
    primary = re.split(r'[.!?\n](?=\s|$)', text, maxsplit=1)[0]
    archived_scope = bool(re.search(
        r'\b(?:ignore|excluding|exclude|skip|without|except)\s+'
        r'(?:the\s+)?archived\b', text, re.I))
    if not (re.search(r'\bgmail\b', primary, re.I)
            and re.search(r'\b(?:all|every|complete\s+set)\b', primary, re.I)
            and re.search(r'\b(?:threads?|conversations?|emails?|messages?)\b', primary, re.I)
            and (re.search(r'\binbox\b', primary, re.I) or archived_scope)):
        return False
    operation = (r'(?:archiv(?:e|ing)|delet(?:e|ing)|remov(?:e|ing)|'
                 r'collect(?:ing)?|find|list|count|mark|read|'
                 r'(?:un)?star(?:ring)?|label)')
    positive = re.search(r'\b' + operation + r'\b', primary, re.I)
    negated = re.search(
        r"\b(?:do not|don't|never|not)\s+(?:ever\s+)?" + operation + r'\b',
        primary, re.I)
    return bool(positive and (not negated or positive.start() < negated.start()))


def public_wishlist_target_purchase(instruction):
    """Recognize purchasing a requested subset from the public Amazon wish list."""
    import re
    if not isinstance(instruction, str):
        return False
    instruction = re.split(r'[.!?\n](?=\s|$)', instruction, maxsplit=1)[0]
    if not (re.search(r'\bamazon\b', instruction, re.I)
            and re.search(r'\bwish\s*list\b', instruction, re.I)):
        return False
    source = r'(?:from|in|on)\s+(?:(?:my|the|our|your|current|existing|amazon)\s+){0,5}wish\s*list'
    if (not re.search(r'\b' + source + r'\b', instruction, re.I)
            or re.search(r'\b(?:not|without|excluding|except)\s+' + source + r'\b', instruction, re.I)):
        return False
    operation = (r'(?:buy|purchase|purchasing|place\s+(?:(?:a|an|one|single|new|the)\s+)?(?:amazon\s+)?order)')
    positive = re.search(r'\b' + operation + r'\b', instruction, re.I)
    if positive is None:
        positive = re.match(r'\s*order\b', instruction, re.I)
    negated = re.search(r"\b(?:do not|don't|never|not)\s+(?:ever\s+)?" + operation + r'\b', instruction, re.I)
    return bool(positive and (not negated or positive.start() < negated.start()))


def public_cart_to_wishlist(instruction):
    """Recognize an instructed forward transfer, including a selected subset."""
    import re
    if not isinstance(instruction, str):
        return False
    instruction = re.split(r'[.!?\n](?=\s|$)', instruction, maxsplit=1)[0]
    cart = re.search(r'\bcart\b', instruction, re.I)
    wish = re.search(r'\bwish\s*list\b', instruction, re.I)
    move = re.search(r'\b(?:mov(?:e|ing)|transfer(?:ring)?)\b', instruction, re.I)
    negated = re.search(r"\b(?:do not|don't|not|never|without)\s+(?:ever\s+)?(?:mov(?:e|ing)|transfer(?:ring)?)\b", instruction, re.I)
    return bool(cart and wish and move and not negated and cart.start() < wish.start())


def public_whole_cart_to_wishlist(instruction):
    """Whole-cart completion advice cannot authorize a filtered transfer."""
    import re
    if not public_cart_to_wishlist(instruction):
        return False
    instruction = re.split(r'[.!?\n](?=\s|$)', instruction, maxsplit=1)[0]
    cart = re.search(r'\bcart\b', instruction, re.I)
    wish = re.search(r'\bwish\s*list\b', instruction, re.I)
    if not cart or not wish or not re.search(r'\bmov(?:e|ing)\b', instruction, re.I):
        return False
    if re.search(r'\b(?:under|over|rating|rated|except|only|selected|target|if|less|more)\b', instruction, re.I):
        return False
    whole = re.search(r'\b(?:all\s+(?:the\s+)?(?:products|items)|every\s+(?:unit|product|item)|entire\s+cart|everything)\b', instruction, re.I)
    return bool(whole and cart.start() < wish.start())


def public_cart_quantity_observed(messages):
    """A real successful cart read, not a response schema, authorizes the phase."""
    import ast
    import re
    for index in range(len(messages) - 1, 0, -1):
        observation, action = messages[index], messages[index - 1]
        if (not isinstance(action, dict) or not isinstance(observation, dict)
                or action.get('role') != 'assistant' or observation.get('role') != 'user'
                or not isinstance(action.get('content'), str)
                or not isinstance(observation.get('content'), str)):
            continue
        code, text = action['content'], observation['content']
        if re.search(r'Traceback|appworld_[a-z_]*(?:error|401|422)|Execution failed', text):
            continue
        try:
            tree = ast.parse(code)
        except SyntaxError:
            continue
        direct = [statement.value for statement in tree.body
                  if isinstance(statement, (ast.Assign, ast.AnnAssign, ast.Expr))]
        actual_cart_call = False
        while direct:
            value = direct.pop()
            if not isinstance(value, ast.Call):
                continue
            if (isinstance(value.func, ast.Name) and value.func.id in ('print', 'str', 'repr')
                    or isinstance(value.func, ast.Attribute) and value.func.attr == 'dumps'
                    and isinstance(value.func.value, ast.Name) and value.func.value.id == 'json'):
                direct.extend(value.args)
            method = value.func
            if (isinstance(method, ast.Attribute) and method.attr == 'show_cart'
                    and isinstance(method.value, ast.Attribute) and method.value.attr == 'amazon'
                    and isinstance(method.value.value, ast.Name) and method.value.value.id == 'apis'):
                actual_cart_call = True
        if not actual_cart_call:
            continue
        # AppWorld print() returns Python dictionary repr; JSON-formatted
        # observations remain valid. Neither parser executes model code.
        # Documentation schema dictionaries cannot satisfy the item predicate.
        decoder = json.JSONDecoder()
        returned = []
        for offset, char in enumerate(text):
            if char != '{':
                continue
            try:
                value, _ = decoder.raw_decode(text[offset:])
            except ValueError:
                continue
            returned.append(value)
        for line in text.splitlines():
            start, end = line.find('{'), line.rfind('}')
            if start < 0 or end < start or end - start > 32768:
                continue
            try:
                value = ast.literal_eval(line[start:end + 1])
            except (ValueError, SyntaxError, RecursionError):
                continue
            returned.append(value)
        for value in returned:
            items = value.get('cart_items') if isinstance(value, dict) else None
            if isinstance(items, list) and all(isinstance(item, dict)
                    and type(item.get('product_id')) is int and type(item.get('quantity')) is int
                    and item['quantity'] > 0 for item in items):
                return True
    return False


def office_retrieval_state(policy, messages, instruction=None, project=None, *, l1_window_mode=None):
    """Expose public task and the latest executed code/feedback, not model prose."""
    import re
    if project is None:
        from appworld_host import native_view
        project = native_view
    if instruction is None:
        instruction = public_instruction_from_policy(policy)
    result = {'instruction': instruction if isinstance(instruction, str) else ''}
    objects = [name for name in ('playlist', 'album')
               if re.search(r'\b' + name + r's?\b', result['instruction'], re.I)]
    if len(objects) == 1:
        # A public request fact used solely by retrieval. It is not injected
        # into model messages or inferred from unrelated API documentation.
        # An explicit delimiter prevents projected Bearer text from joining
        # this public marker into a credential-shaped value after projection.
        result['task_object'] = '[task_object_' + objects[0] + ']'
    downloaded_kind = None
    if l1_window_mode in ('observed_phase_v2', 'observed_phase_v3'):
        purpose = (public_downloaded_duration_playback_v3 if l1_window_mode == 'observed_phase_v3'
                   else public_downloaded_duration_playback_v2)
        downloaded_kind = next((kind for kind in ('playlist', 'album')
            if purpose(result['instruction'], kind)), None)
    elif public_playlist_downloaded_duration_playback(result['instruction']):
        downloaded_kind = 'playlist'
    if downloaded_kind:
        result['task_purpose'] = '[task_object_' + downloaded_kind + '_downloaded_duration_playback]'
    if public_whole_cart_to_wishlist(result['instruction']):
        result['task_transfer'] = '[task_cart_to_wishlist_full_quantity]'
    if public_cart_to_wishlist(result['instruction']):
        result['task_quantity_transfer'] = '[task_cart_to_wishlist_quantity]'
        if public_cart_quantity_observed(messages):
            result['cart_phase'] = '[cart_quantity_state_ready]'
    # receive() records each executed AppWorld code/observation as an adjacent
    # assistant/user pair. Parser retry instructions alone are not observations.
    for index in range(len(messages) - 1, 0, -1):
        current, previous = messages[index], messages[index - 1]
        if (isinstance(current, dict) and isinstance(previous, dict)
                and current.get('role') == 'user' and previous.get('role') == 'assistant'
                and isinstance(current.get('content'), str)):
            executed = {'observation': current['content']}
            if isinstance(previous.get('content'), str):
                executed['code'] = previous['content']
            safe = project(executed)
            for name, limit in (('code', 1024), ('observation', 2048)):
                if name not in safe:
                    continue
                encoded = safe[name].encode('utf-8')
                if len(encoded) <= limit:
                    result[name] = safe[name]
                    continue
                marker = ('\n[' + name + ' excerpt]\n').encode()
                head = (limit - len(marker)) // 2
                tail = len(encoded) - (limit - len(marker) - head)
                while head and encoded[head] & 0xc0 == 0x80:
                    head -= 1
                while tail < len(encoded) and encoded[tail] & 0xc0 == 0x80:
                    tail += 1
                result[name] = (encoded[:head] + marker + encoded[tail:]).decode('utf-8')
            break
    phase = {}
    stage = public_retrieval_stage(messages, phase)
    if (downloaded_kind is not None
            and phase.get('authenticated_apps') == ['spotify']
            and phase.get('active_apps') == ['spotify']
            and public_spotify_downloaded_subset_observed(messages)):
        result['playlist_downloaded_phase'] = '[spotify_downloaded_subset_state_ready]'
    if public_wishlist_target_purchase(result['instruction']):
        result['purchase_purpose'] = '[task_wishlist_target_purchase]'
        if ('amazon' in phase.get('authenticated_apps', [])
                and phase.get('active_apps') == ['amazon']
                and public_cart_quantity_observed(messages)):
            result['purchase_phase'] = '[cart_purchase_state_ready]'
    if public_spotify_playlist_library_complete_enumeration(result['instruction']):
        result['playlist_collection'] = '[task_spotify_playlist_library_complete_enumeration]'
        if ('spotify' in phase.get('authenticated_apps', [])
                and phase.get('active_apps') == ['spotify']):
            result['playlist_collection_phase'] = '[spotify_authenticated_state_ready]'
    if public_gmail_inbox_complete_enumeration(result['instruction']):
        result['task_collection'] = '[task_gmail_inbox_complete_enumeration]'
        if ('gmail' in phase.get('authenticated_apps', [])
                and phase.get('active_apps') == ['gmail']):
            result['collection_phase'] = '[gmail_authenticated_state_ready]'
    if stage:
        result['retrieval_stage'] = stage
    return project(result)


def office_observed_phase_state(policy, messages, instruction=None, project=None, *, l1_window_mode=None):
    """Allow prevention only after execution proves an existing public task phase."""
    import ast
    import re
    if not isinstance(messages, list) or len(messages) < 2:
        return None
    action, observation = messages[-2:]
    if (not isinstance(action, dict) or not isinstance(observation, dict)
            or action.get('role') != 'assistant' or observation.get('role') != 'user'
            or not isinstance(action.get('content'), str) or not action['content'].strip()
            or not isinstance(observation.get('content'), str) or not observation['content'].strip()
            or observation['content'].lstrip().startswith(('<system-reminder>', '[AppWorld request]'))):
        return None
    try:
        tree = ast.parse(action['content'])
    except (SyntaxError, ValueError, RecursionError):
        return None
    if re.search(
            r'Traceback|Execution failed|(?:TypeError|NameError|AttributeError|SyntaxError|ValueError|KeyError|RuntimeError|Exception):'
            r'|appworld_[a-z_]*(?:error|401|422)|\b(?:401|422)\b|Unauthorized|Invalid credentials'
            r'|Policy rejected|No API named|has no attribute', observation['content'], re.IGNORECASE):
        return None
    state = office_retrieval_state(policy, messages, instruction, project, l1_window_mode=l1_window_mode)
    pairs = (
        ('task_purpose', '[task_object_playlist_downloaded_duration_playback]',
         'playlist_downloaded_phase', '[spotify_downloaded_subset_state_ready]'),
        ('playlist_collection', '[task_spotify_playlist_library_complete_enumeration]',
         'playlist_collection_phase', '[spotify_authenticated_state_ready]'),
        ('task_collection', '[task_gmail_inbox_complete_enumeration]',
         'collection_phase', '[gmail_authenticated_state_ready]'),
        ('task_quantity_transfer', '[task_cart_to_wishlist_quantity]',
         'cart_phase', '[cart_quantity_state_ready]'),
        ('purchase_purpose', '[task_wishlist_target_purchase]',
         'purchase_phase', '[cart_purchase_state_ready]'),
    )
    if l1_window_mode in ('observed_phase_v2', 'observed_phase_v3'):
        pairs += (('task_purpose', '[task_object_album_downloaded_duration_playback]',
                   'playlist_downloaded_phase', '[spotify_downloaded_subset_state_ready]'),)
        # A successful, unconditional public API evaluation also supplies a
        # real matching window. Card-specific task/API/field conditions are
        # still validated by the native compiler; no extra marker is added.
        def evaluated_api(value):
            if isinstance(value, (ast.Lambda, ast.IfExp, ast.BoolOp, ast.ListComp,
                                  ast.SetComp, ast.DictComp, ast.GeneratorExp)):
                return False
            if (isinstance(value, ast.Call) and isinstance(value.func, ast.Attribute)
                    and isinstance(value.func.value, ast.Attribute)
                    and isinstance(value.func.value.value, ast.Name)
                    and value.func.value.value.id == 'apis'):
                return True
            return any(evaluated_api(child) for child in ast.iter_child_nodes(value))
        if any(evaluated_api(statement.value) for statement in tree.body
               if isinstance(statement, (ast.Assign, ast.AnnAssign, ast.Expr))
               and statement.value is not None):
            return state
    return state if any(state.get(task_key) == task_value and state.get(phase_key) == phase_value
                        for task_key, task_value, phase_key, phase_value in pairs) else None


def saved_result_failure_prevention_state(policy, messages, instruction=None, project=None,
                                          *, public_contracts=None):
    """A real silent, successful read saved a public collection in a new Name.

    The collection's value has not been observed. This predicate establishes
    only the successful assignment and its public return contract, never the
    contents, a future model action, or success from the absence of an error.
    """
    import ast
    if not isinstance(messages, list) or len(messages) < 2:
        return None
    previous, current = messages[-2:]
    if (not isinstance(previous, dict) or not isinstance(current, dict)
            or previous.get('role') != 'assistant' or current.get('role') != 'user'
            or not isinstance(previous.get('content'), str)
            or not isinstance(current.get('content'), str)
            or current.get('content', '').strip() != 'Execution successful.'):
        return None
    try:
        tree = ast.parse(previous['content'])
    except (SyntaxError, ValueError, RecursionError):
        return None
    if len(tree.body) != 1 or not isinstance(tree.body[0], ast.Assign):
        return None
    assignment = tree.body[0]
    if len(assignment.targets) != 1 or not isinstance(assignment.targets[0], ast.Name):
        return None
    target, call = assignment.targets[0].id, assignment.value
    if (re.search(r'token|password|credential|secret|login|auth', target, re.I)
            or not isinstance(call, ast.Call) or not isinstance(call.func, ast.Attribute)
            or not isinstance(call.func.value, ast.Attribute)
            or not isinstance(call.func.value.value, ast.Name)
            or call.func.value.value.id != 'apis'):
        return None
    app, method = call.func.value.attr, call.func.attr
    contracts = {tuple(pair) for pair in public_contracts or []
                 if isinstance(pair, (list, tuple)) and len(pair) == 2
                 and all(isinstance(part, str) for part in pair)}
    if (app in ('api_docs', 'supervisor') or (app, method) not in contracts
            or re.search(r'login|logout|token|password|credential|secret|auth', method, re.I)
            or call.args or any(keyword.arg is None for keyword in call.keywords)):
        return None
    bound, written = set(), set()
    for index in range(1, len(messages) - 2):
        action, observation = messages[index - 1], messages[index]
        if (not isinstance(action, dict) or not isinstance(observation, dict)
                or action.get('role') != 'assistant' or observation.get('role') != 'user'
                or not isinstance(action.get('content'), str)):
            continue
        try:
            earlier = ast.parse(action['content'])
        except (SyntaxError, ValueError, RecursionError):
            continue
        stores = {node.id for node in ast.walk(earlier)
                  if isinstance(node, ast.Name) and isinstance(node.ctx, (ast.Store, ast.Del))}
        written.update(stores)
        bound.difference_update(stores)
        feedback = observation.get('content')
        if (not isinstance(feedback, str) or not feedback.strip()
                or feedback.lstrip().startswith(('<system-reminder>', '[AppWorld request]'))
                or re.search(r'Traceback|Execution failed|\b\w*Error:|Unauthorized|Invalid credentials'
                             r'|Policy rejected|appworld_\w*(?:error|401|422|missing_access_token)', feedback, re.I)):
            continue
        # Only unconditional top-level assignments establish a persistent
        # binding. Conditional/loop/function-local or deleted names do not.
        for statement in earlier.body:
            if isinstance(statement, ast.Assign):
                bound.update(item.id for item in statement.targets if isinstance(item, ast.Name))
    if target in written or 'apis' in written:
        return None
    for keyword in call.keywords:
        value = keyword.value
        if isinstance(value, ast.Name):
            if value.id not in bound:
                return None
        elif not isinstance(value, ast.Constant):
            return None
    state = office_retrieval_state(policy, messages, instruction, project)
    state['saved_result_contract'] = '[saved_public_collection_result]'
    state['saved_result_phase'] = '[undisplayed_api_assignment]'
    state['saved_result_evidence'] = {'api': 'apis.' + app + '.' + method,
                                    'assigned_name': target, 'value_observed': False,
                                    'contract_source': 'public_api_registry'}
    return (project or (lambda value: value))(state)


def set_request(meta, policy, tools_schema, messages, turn, *, l1_window_mode=None,
                native_run_id=None, observed_phase_experiences=None, auth_parameter_experiences=None,
                auth_fallback_experiences=None, auth_parameter_contracts=None,
                auth_structured_missing=False, shape_error_auth_experiences=None,
                saved_result_experiences=None, saved_result_contracts=None):
    if not isinstance(policy, str) or not isinstance(messages, list) or type(turn) is not int:
        raise ValueError("appworld_public_request_invalid")
    request_id = uuid.uuid4().hex
    advice_cache_limit = _policy_advice_cache_limit(policy)
    state = {"schema_version": 1, "request_id": request_id, "turn": turn,
             "available_names": [TOOL_NAME], "allowed_tool_names": [TOOL_NAME],
             "scripts_dir": meta["scripts_dir"], "reply_path": meta["reply_path"],
             "policy": policy, "messages": messages,
             "public_instruction": public_instruction_from_policy(policy),
             "native_advice_cache_limit": advice_cache_limit}
    if shape_error_auth_experiences is not None:
        state['appworld_shape_error_auth_experiences'] = copy.deepcopy(shape_error_auth_experiences)
    if saved_result_experiences is not None:
        state['appworld_saved_result_experiences'] = copy.deepcopy(saved_result_experiences)
        state['appworld_saved_result_contracts'] = copy.deepcopy(saved_result_contracts)
    if auth_parameter_experiences is not None:
        state['appworld_auth_parameter_experiences'] = auth_parameter_experiences
        state['appworld_auth_fallback_experiences'] = auth_fallback_experiences
        state['appworld_auth_parameter_contracts'] = auth_parameter_contracts
        if auth_structured_missing is True:
            state['appworld_auth_structured_missing'] = True
    if l1_window_mode is not None:
        if l1_window_mode not in ('observed_phase', 'observed_phase_v2', 'observed_phase_v3') or not isinstance(native_run_id, str) or not native_run_id:
            raise ValueError('appworld_l1_window_mode_invalid')
        state.update(appworld_l1_window_mode=l1_window_mode,
                     appworld_l1_run_id=native_run_id, appworld_l1_count=0)
        if observed_phase_experiences is not None:
            state['appworld_observed_phase_experiences'] = observed_phase_experiences
    doc = ("# AppWorld cross-application office\n\nWrite the next Python code block for "
           "the official AppWorld environment. Call appworld_submit_code exactly once "
           "with {code:...} and stop. The host alone executes it once in the persistent "
           "official sandbox; world state and Python variables persist between submissions. "
           "Continue from the executed code and observations in the complete history. "
           "Submit only the next incremental step, not a full script replay. Never repeat "
           "an already successful non-idempotent change, such as creating a payment, "
           "sending a message, or deleting a resource. After successful changes, submit "
           "only necessary verification or apis.supervisor.complete_task. "
           "Use the public apis interface and official api_docs. "
           "Do not access evaluation, ground_truth, host paths, database files, or hidden "
           "task data. Do not replace executing APIs with prose. Call the official "
           "apis.supervisor.complete_task only when the requested work is finished. "
           "The official database checks determine success.\n\n" + policy +
           "\n\n## Complete recent public history\n```json\n" +
           json.dumps(messages, ensure_ascii=False, indent=2) + "\n```\n")
    with _locked(meta):
        # Carry only the bounded private native advice cache across skill rebuilds.
        try:
            previous = json.loads(Path(meta["state_path"]).read_text(encoding="utf-8"))
            cached = previous.get("native_advice_cache")
            if isinstance(cached, list) and cached:
                state["native_advice_cache"] = cached[:advice_cache_limit]
            if (l1_window_mode in ('observed_phase', 'observed_phase_v2', 'observed_phase_v3')
                    and previous.get('appworld_l1_window_mode') == l1_window_mode
                    and previous.get('appworld_l1_run_id') == native_run_id):
                count = previous.get('appworld_l1_count', 0)
                if type(count) is int and count >= 0:
                    state['appworld_l1_count'] = min(3, count)
                if previous.get('appworld_l1_applied') is True:
                    state['appworld_l1_applied'] = True
        except (OSError, ValueError, TypeError):
            pass
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


def compose_skill_messages(messages: list[dict[str, Any]], *, skill_name: str) -> list[dict[str, Any]]:
    """Return the current public request without rewriting executable history."""
    if not isinstance(messages, list):
        raise TypeError("appworld_skill_messages_invalid")
    if not isinstance(skill_name, str) or not skill_name.strip():
        raise ValueError("appworld_skill_name_invalid")
    result: list[dict[str, Any]] = []
    for message in messages:
        if not isinstance(message, dict):
            raise TypeError("appworld_skill_message_invalid")
        result.append(dict(message))
    return result


import atexit
import fcntl
import json
import inspect
import os
import re
from pathlib import Path
import time


def native_capture_point(client, packet):
    """Recover only a private telemetry RPC, with one stable request identity."""
    # Lightweight injected lifecycle clients need not implement HTTP internals.
    # Production uses the normal Evolution transport and preserves its fields.
    if not all(callable(getattr(client, name, None)) for name in ('_invoke', '_point_fields', '_point_observed')):
        return client.point(packet, timeout_ms=1000)
    from evolution import EvolutionError
    fields = client._point_fields(packet)
    request_id = uuid.uuid4().hex
    for attempt in (1, 2):
        try:
            result = client._invoke('point', fields, request_id, 5.0, 0)
            return client._point_observed(fields, result)
        except EvolutionError as error:
            if attempt == 1 and (error.status == 0 or error.status >= 500):
                continue
            raise


def _advice_cards(original, selected, *, remove_text=""):
    """Extract complete native-selected experience cards without truncation."""
    if not isinstance(original, list) or not isinstance(selected, list):
        return []
    cards = []
    for old, chosen in zip(original, selected):
        if not (isinstance(old, dict) and isinstance(chosen, dict)
                and old.get("role") == chosen.get("role") == "system"):
            continue
        old_text, chosen_text = old.get("content"), chosen.get("content")
        if not (isinstance(old_text, str) and isinstance(chosen_text, str)
                and chosen_text.startswith(old_text)):
            continue
        suffix = chosen_text[len(old_text):]
        if remove_text and remove_text in suffix:
            suffix = suffix.replace(remove_text, "")
        if suffix and suffix not in cards:
            cards.append(suffix)
    if len(selected) == len(original) + 1 and selected and isinstance(selected[0], dict):
        item = selected[0]
        if item.get("role") == "system" and isinstance(item.get("content"), str):
            content = item["content"]
            if remove_text and remove_text in content:
                content = content.replace(remove_text, "")
            if content and content not in cards:
                cards.insert(0, content)
    result, total = [], 0
    for card in cards:
        size = len(card.encode("utf-8"))
        # Cards are atomic: oversized cards are skipped, never truncated.
        if size > 2048 or total + size > 2048 or len(result) >= 2:
            continue
        result.append(card); total += size
    return result


def _native_application_verified(result, expected_context=None):
    """Require the native after-boundary receipt, never a caller claim."""
    if not isinstance(result, dict):
        return False
    receipt = result.get("application_receipt")
    if not isinstance(receipt, dict):
        receipt = result.get("receipt")
    app = result.get("application")
    if not isinstance(app, dict) and isinstance(receipt, dict):
        app = receipt.get("application")
    experiences = receipt.get("experiences") if isinstance(receipt, dict) else None
    if not (isinstance(app, dict) and app.get("status") == "applied"
            and app.get("reason") == "host_boundary_verified"
            and isinstance(experiences, list) and bool(experiences)):
        return False
    if isinstance(expected_context, dict):
        if receipt.get("boundary") != "llm":
            return False
        if receipt.get("call_id") != expected_context.get("activity_id"):
            return False
        identity = receipt.get("identity")
        if isinstance(identity, dict) and identity.get("run_id") != expected_context.get("run_id"):
            return False
    return True


class NativeFrameworkObserver:
    def __init__(self, client=None, state_reader=None, project=None, model_kind='model'):
        if client is None:
            from evolution import Evolution
            client = Evolution(os.environ['APPWORLD_CAPTURE_URL'],
                               os.environ['APPWORLD_CAPTURE_TOKEN'], timeout_ms=5000, retries=1)
        self.client = client
        self.state_reader = state_reader or self._read_state
        if project is None:
            from appworld_host import native_view
            project = native_view
        self.project = project
        self.model_kind = model_kind
        self.models = {}
        self.tools = {}
        self.current_loop = None
        self.failures = 0
        self.closed_model_calls = 0
        self.transport_attempts = 0
        self.runtime_level = str(os.environ.get('APPWORLD_POLICY_LEVEL', 'L0')).upper()
        self._diagnostic_path = None
        state_path = os.environ.get('APPWORLD_RUNTIME_STATE', '')
        if state_path:
            self._diagnostic_path = Path(state_path).with_name('observer-diagnostic.json')
        self._diagnostics = []
        self.last_model_call_id = None
        self.preparing_model = None
        self.loop_contexts = {}
        self.native_advice_cache = []
        self.native_advice_cache_limit = 2
        self.shape_error_auth_experiences = None
        try:
            current_state = self.state_reader() or {}
            self.shape_error_auth_experiences = _runtime_shape_error_auth_experiences(current_state)
            self.native_advice_cache_limit = _policy_advice_cache_limit(current_state)
            raw_cache = current_state.get("native_advice_cache", [])
            if isinstance(raw_cache, list):
                self.native_advice_cache = [entry for entry in raw_cache[:self.native_advice_cache_limit]
                    if isinstance(entry, dict)]
        except Exception:
            self.native_advice_cache = []

    def _read_state(self):
        return json.loads(Path(os.environ['APPWORLD_RUNTIME_STATE']).read_text())

    def _stat(self, key, delta=1):
        filename = os.environ.get('APPWORLD_RUNTIME_STATE')
        if not filename:
            return
        try:
            lock = Path(filename).with_name('appworld-runtime.lock')
            with lock.open('a+') as stream:
                fcntl.flock(stream.fileno(), fcntl.LOCK_EX)
                path = Path(filename)
                state = json.loads(path.read_text())
                stats = state.setdefault('framework_capture', {'calls': 0, 'transport_attempts': 0, 'failures': 0})
                stats[key] = stats.get(key, 0) + delta
                from appworld_skill import _atomic_text
                _atomic_text(path, json.dumps(state, ensure_ascii=False))
        except Exception:
            self.failures += 1

    def _gap(self, reason='observer_gap'):
        self.failures += 1
        self._record_diagnostic(reason, {})
        self._stat('failures')

    def _record_diagnostic(self, phase, fields, *, error=None, result=None):
        """Keep bounded, identity-only observer diagnostics for contract failures."""
        context = fields.get('context') if isinstance(fields, dict) else {}
        extensions = fields.get('extensions') if isinstance(fields, dict) else {}
        if not isinstance(context, dict):
            context = {}
        if not isinstance(extensions, dict):
            extensions = {}
        entry = {
            'phase': str(phase), 'kind': fields.get('kind') if isinstance(fields, dict) else None,
            'run_id': context.get('run_id'), 'activity_id': context.get('activity_id'),
            'control_id': context.get('control_id'),
            'source_call_id': extensions.get('source_call_id'),
        }
        if error is not None:
            entry['error_type'] = type(error).__name__
            entry['error_code'] = getattr(error, 'code', None) or str(error)[:96]
            entry['status'] = getattr(error, 'status', None)
        if isinstance(result, dict):
            for key in ('reason', 'closed', 'capture_complete', 'degraded'):
                if key in result:
                    entry[key] = result[key]
        self._diagnostics.append(entry)
        self._diagnostics = self._diagnostics[-12:]
        if self._diagnostic_path is not None:
            try:
                _atomic_text(self._diagnostic_path,
                             json.dumps({'entries': self._diagnostics}, ensure_ascii=False))
            except (OSError, TypeError, ValueError):
                pass

    def point(self, packet):
        try:
            safe = self.project(packet)
            # AF_EXEC_SKILL_KEY is a framework execution identity, not an API
            # credential. The host's conservative environment-value filter
            # must not turn native loop/causal identifiers into placeholders.
            for field in ('loop_id', 'actor_id'):
                if field in packet:
                    safe[field] = packet[field]
            if isinstance(packet.get('extensions'), dict):
                for field in ('exec_skill_key', 'task_id', 'node_id', 'source_call_id', 'source_tool_call_id'):
                    if field in packet['extensions']:
                        safe.setdefault('extensions', {})[field] = packet['extensions'][field]
            result = native_capture_point(self.client, safe)
            if result.get('degraded'):
                self._record_diagnostic('point_degraded', packet, result=result)
                self._gap('point_degraded')
                return None
            return result
        except Exception as error:
            self._record_diagnostic('point_error', packet, error=error)
            self._gap('point_error')
            return None

    def _source(self, name, payload):
        meta = payload.get('meta') or {}
        return {'source_event_name': name,
                'source_call_id': payload.get('call_id', '' if name == 'agentflow.trace_call.enter' else self.last_model_call_id or ''),
                'source_timestamp_us': int(time.clock_gettime(time.CLOCK_REALTIME) * 1000000),
                'node_id': meta.get('node_id', os.environ.get('AF_NODE_ID', '')),
                'task_id': meta.get('task_id', os.environ.get('AF_TASK_ID', '')),
                'exec_skill_key': meta.get('exec_skill_key', os.environ.get('AF_EXEC_SKILL_KEY', '')),
                'framework_span_id': meta.get('framework_span_id', '')}

    @staticmethod
    def _response(response):
        # Decode structured arguments so credential projection never treats
        # an entire JSON/Python code string as one opaque credential value.
        choices = []
        for choice in response.get('choices') or []:
            message = choice.get('message') or {}
            calls = []
            for call in message.get('tool_calls') or []:
                fn = call.get('function') or {}
                raw = fn.get('arguments', '{}')
                try:
                    arguments = json.loads(raw)
                except (ValueError, TypeError):
                    arguments = raw
                calls.append({'tool_call_id': call.get('id', ''),
                              'tool_name': fn.get('name', ''), 'arguments': arguments})
            choices.append({'finish_reason': choice.get('finish_reason'),
                            'content': message.get('content'), 'tool_calls': calls})
        result = {'choices': choices}
        if response.get('_llm_error') or response.get('error'):
            result['error'] = response.get('error') or 'framework_model_call_failed'
        return result

    @staticmethod
    def _cache_scope(state, meta):
        outer = state.get("native_context") if isinstance(state, dict) else None
        run_id = outer.get("run_id") if isinstance(outer, dict) else None
        skill, source = meta.get("skill_name"), meta.get("source")
        actor = meta.get("node_id") or "skill_executor"
        task = meta.get("task_id") or os.environ.get("AF_TASK_ID", "")
        if (not isinstance(run_id, str) or not run_id or source != "skill_executor"
                or skill != "appworld-office" or actor != "skill_executor"):
            return None
        return {"run_id": run_id, "skill": skill, "task_id": str(task), "actor": actor}

    def _cached_cards(self, scope):
        if not isinstance(scope, dict): return []
        return [item["content"] for item in self.native_advice_cache
                if isinstance(item, dict) and item.get("scope") == scope
                and isinstance(item.get("content"), str)][:self.native_advice_cache_limit]

    def _remember_verified(self, scope, cards):
        if not isinstance(scope, dict) or not cards: return
        kept = [item for item in self.native_advice_cache
                if not (isinstance(item, dict) and item.get("scope") == scope)]
        for card in cards[:self.native_advice_cache_limit]:
            kept.append({"scope": dict(scope), "content": card})
        self.native_advice_cache = kept[-self.native_advice_cache_limit:]
        filename = os.environ.get("APPWORLD_RUNTIME_STATE")
        if not filename: return
        try:
            path = Path(filename)
            with path.with_name("appworld-runtime.lock").open("a+") as stream:
                fcntl.flock(stream.fileno(), fcntl.LOCK_EX)
                state = json.loads(path.read_text(encoding="utf-8"))
                state["native_advice_cache"] = self.native_advice_cache[-self.native_advice_cache_limit:]
                _atomic_text(path, json.dumps(state, ensure_ascii=False))
        except Exception:
            self.failures += 1

    def _mark_l1_applied(self):
        """Persist a bounded recovery budget for repeated execution failures.

        One global boolean made the first ineffective advice consume the only
        correction opportunity for a task.  Keep the compact legacy marker,
        but count up to three real failure-triggered advice applications so a
        token/argument correction can be retried after a changed observation.
        """
        filename = os.environ.get("APPWORLD_RUNTIME_STATE")
        if not filename:
            return
        try:
            path = Path(filename)
            with path.with_name("appworld-runtime.lock").open("a+") as stream:
                fcntl.flock(stream.fileno(), fcntl.LOCK_EX)
                state = json.loads(path.read_text(encoding="utf-8"))
                count = int(state.get("appworld_l1_count", 0) or 0)
                state["appworld_l1_count"] = min(3, count + 1)
                state["appworld_l1_applied"] = True
                _atomic_text(path, json.dumps(state, ensure_ascii=False))
        except Exception:
            self.failures += 1

    def before_model(self, messages, meta):
        """Begin the real recorder call and inject L1 into its real messages."""
        state = self.state_reader()
        if isinstance(state, dict):
            # The observer may be installed when the daemon starts, before
            # Host.configure writes the first request. Consume only the
            # current host-validated private opt-in payload at this boundary.
            if state.get('appworld_shape_error_auth_experiences') is not None:
                self.shape_error_auth_experiences = _runtime_shape_error_auth_experiences(state)
            self.native_advice_cache_limit = _policy_advice_cache_limit(
                state, default=self.native_advice_cache_limit)
            if isinstance(state.get("native_advice_cache"), list):
                self.native_advice_cache = [entry for entry in state["native_advice_cache"][:self.native_advice_cache_limit]
                    if isinstance(entry, dict)]
        outer = state.get('native_context')
        if not isinstance(outer, dict) or not outer.get('run_id'):
            self._gap('before_model_context_missing')
            return None
        skill_key = meta.get('exec_skill_key') or 'coordinator'
        actor = meta.get('node_id') or ('skill_executor' if skill_key != 'coordinator' else 'coordinator_decide')
        source = self._source('agentflow.trace_call.enter', {'meta': meta})
        loop_id = 'framework:' + str(state.get('turn', 0)) + ':' + actor + ':' + skill_key
        round_number = int(meta.get('decision_index', meta.get('round', 0)) or 0)
        loop_key = (loop_id, str(meta.get('task_id', '')), meta.get('framework_span_id') or round_number)
        # Every framework model turn gets a child loop receipt.  L0 still
        # disables policy application and action instrumentation below, but it
        # needs the same causal model-turn boundary so loop/cost accounting is
        # comparable with candidate levels.
        if round_number > 0:
            loop_ctx = self.loop_contexts.get(loop_key)
            if loop_ctx is None:
                loop = self.point({'phase': 'before', 'kind': 'loop', 'context': outer,
                                   'loop_id': loop_id, 'actor_id': actor, 'scope_id': 'default',
                                   'state': {key: meta[key] for key in ('round', 'activation_index', 'decision_index',
                                              'internal_tool_correction_index') if key in meta},
                                   'apply_policy': False, 'extensions': source})
                if loop is None:
                    return None
                loop_ctx = loop['context']
                if self.point({'phase': 'after', 'kind': 'loop', 'context': loop_ctx,
                               'decision': 'continue', 'extensions': source}) is None:
                    return None
                self.loop_contexts[loop_key] = loop_ctx
        else:
            # Compaction/helper calls with no authoritative framework round
            # stay in the real outer loop. A model call is not a new loop.
            loop_ctx = outer
        # Keep an immutable pre-injection request for native receipt comparison.
        request = {'messages': copy.deepcopy(messages)}
        # AppWorld task experience belongs to the execution skill. Route
        # planning and compaction are observed without strategy injection.
        # Do not inject a card on the first skill request merely because the
        # task text contains a matching word such as ``login`` or ``playlist``.
        # The model already receives the complete public task and API policy
        # there; L1 is a recovery aid and is actionable only after the latest
        # real code/observation pair reports a factual execution failure. A
        # guard over any old pair made every later successful turn pay for
        # advice, increasing calls/tokens without improving task results.
        # ``messages`` is the fresh skill request (system/task/reminder), so it
        # cannot prove that code has executed.  The host's persisted state is
        # the authoritative history: it contains the assistant code followed
        # by the native user observation.  Checking that history keeps the
        # first planning call clean while allowing L1 after a real failure.
        history = state.get('messages') if isinstance(state, dict) else []
        if not isinstance(history, list):
            history = []
        has_execution_observation = False
        if len(history) >= 2:
            previous, current = history[-2], history[-1]
            if (isinstance(current, dict) and isinstance(previous, dict)
                    and current.get('role') == 'user'
                    and previous.get('role') == 'assistant'
                    and isinstance(previous.get('content'), str)
                    and previous.get('content', '').strip()
                    and isinstance(current.get('content'), str)
                    and current.get('content', '').strip()
                    and not current.get('content', '').lstrip().startswith(('<system-reminder>', '[AppWorld request]'))):
                has_execution_observation = bool(re.search(
                    r'Traceback|Execution failed|(?:TypeError|NameError|AttributeError|SyntaxError|ValueError|KeyError|RuntimeError|Exception):|appworld_[a-z_]*(?:error|401|422)'
                    r'|\b(?:401|422)\b|Unauthorized|Invalid credentials'
                    r'|Policy rejected|No API named|has no attribute',
                    current['content'], re.IGNORECASE))
        replan = state.get('native_replan_context')
        eligible_main_replan = (isinstance(replan, str) and bool(replan.strip())
                                and outer.get('loop_id') == 'main'
                                and bool(outer.get('control_id')))
        l1_count = int(state.get('appworld_l1_count', 0) or 0)
        observed_phase = None
        saved_result_phase = None
        saved_result_sources = state.get('appworld_saved_result_experiences')
        structured_auth = None
        if (state.get('appworld_auth_structured_missing') is True
                and self.runtime_level in ('L1', 'L2', 'L3') and round_number > 0
                and actor == 'skill_executor' and meta.get('source') == 'skill_executor'
                and meta.get('skill_name') == 'appworld-office' and l1_count < 3):
            structured_auth = auth_parameter_failure_state(
                state.get('policy', ''), history, state.get('public_instruction'), self.project,
                public_contracts=state.get('appworld_auth_parameter_contracts'),
                allow_structured_missing=True)
        if (state.get('appworld_l1_window_mode') in ('observed_phase', 'observed_phase_v2', 'observed_phase_v3')
                and self.runtime_level in ('L1', 'L2', 'L3')
                and round_number > 0 and actor == 'skill_executor'
                and meta.get('source') == 'skill_executor'
                and meta.get('skill_name') == 'appworld-office'
                and l1_count < 3 and not has_execution_observation
                and not eligible_main_replan):
            observed_phase = office_observed_phase_state(
                state.get('policy', ''), history, state.get('public_instruction'), self.project,
                l1_window_mode=state.get('appworld_l1_window_mode'))
        if (saved_result_sources is not None and self.runtime_level in ('L1', 'L2', 'L3')
                and round_number > 0 and actor == 'skill_executor'
                and meta.get('source') == 'skill_executor' and meta.get('skill_name') == 'appworld-office'
                and l1_count < 3 and not has_execution_observation and not eligible_main_replan):
            saved_result_phase = saved_result_failure_prevention_state(
                state.get('policy', ''), history, state.get('public_instruction'), self.project,
                public_contracts=state.get('appworld_saved_result_contracts'))
        apply_l1 = (meta.get('source') == 'skill_executor'
                    and meta.get('skill_name') == 'appworld-office'
                    and (has_execution_observation or eligible_main_replan or observed_phase is not None
                         or structured_auth is not None or saved_result_phase is not None)
                    and l1_count < 3)
        # A main-loop replan is consumed by the real office model, not by the
        # custom agent-turn wrapper. Keep its control identity for the native
        # request receipt while retaining the internal loop as a causal parent.
        main_replan = apply_l1 and eligible_main_replan
        packet = {'phase': 'before', 'kind': self.model_kind,
                               'name': 'agentflow.llm', 'context': outer if main_replan else loop_ctx,
                               'actor_id': actor, 'input': request,
                               'binding': {'pointer': '/messages', 'mode': 'system_message'},
                               'apply_policy': apply_l1,
                               'extensions': {**source, 'metrics': {'tool_call': False}}}
        restricted = state.get('appworld_observed_phase_experiences')
        if observed_phase is not None and restricted is not None:
            packet['extensions']['runtime_experience_allowlist'] = restricted
        if saved_result_phase is not None:
            refs = list(restricted or []) if observed_phase is not None and restricted is not None else []
            refs += [ref for ref in saved_result_sources['refs'] if ref not in refs]
            packet['extensions']['runtime_experience_allowlist'] = refs
        if apply_l1:
            packet['state'] = saved_result_phase if saved_result_phase is not None else observed_phase if observed_phase is not None else office_retrieval_state(
                state.get('policy', ''), state.get('messages') or [],
                state.get('public_instruction'), self.project,
                l1_window_mode=(None if restricted is not None else state.get('appworld_l1_window_mode')))
            auth_refs = state.get('appworld_auth_parameter_experiences')
            if auth_refs is not None:
                causal_auth = auth_parameter_failure_state(
                    state.get('policy', ''), history, state.get('public_instruction'), self.project,
                    public_contracts=state.get('appworld_auth_parameter_contracts'),
                    allow_structured_missing=state.get('appworld_auth_structured_missing', False))
                if causal_auth is None and any('phase' in ref for ref in auth_refs):
                    causal_auth = auth_login_failure_state(
                        state.get('policy', ''), history, state.get('public_instruction'), self.project,
                        public_contracts=state.get('appworld_auth_parameter_contracts'))
                if causal_auth is not None and (has_execution_observation or structured_auth is not None):
                    selected_refs = auth_parameter_phase_experiences(auth_refs, history, causal_auth)
                    if not selected_refs:
                        self._gap('auth_parameter_phase_unknown')
                        return None
                    packet['state'] = causal_auth
                    packet['extensions']['runtime_experience_allowlist'] = selected_refs
                elif observed_phase is None and saved_result_phase is None:
                    fallback = state.get('appworld_auth_fallback_experiences')
                    if not isinstance(fallback, list):
                        self._gap('auth_parameter_fallback_missing')
                        return None
                    packet['extensions']['runtime_experience_allowlist'] = fallback
            # The new source is never eligible in an ordinary error/read/write
            # window. Do not let fallback or a previously cached card bypass
            # the causal qualification.
            if saved_result_sources is not None and saved_result_phase is None:
                excluded = {(ref['memory_id'], ref['content_revision']) for ref in saved_result_sources['refs']}
                eligible = packet['extensions'].get('runtime_experience_allowlist')
                if isinstance(eligible, list):
                    packet['extensions']['runtime_experience_allowlist'] = [ref for ref in eligible
                        if (ref['memory_id'], ref['content_revision']) not in excluded]
        shape_filter = (self.shape_error_auth_experiences if apply_l1
                        and non_authentication_type_error(history) else None)
        if shape_filter is not None:
            excluded = {(ref['memory_id'], ref['content_revision'])
                        for ref in shape_filter['excluded_refs']}
            eligible = packet['extensions'].get('runtime_experience_allowlist',
                                                shape_filter['approved_refs'])
            packet['extensions']['runtime_experience_allowlist'] = [ref for ref in eligible
                if (ref['memory_id'], ref['content_revision']) not in excluded]
        if main_replan and loop_ctx is not outer:
            packet['causes'] = [loop_ctx]
        activity = self.point(packet)
        if activity is None:
            return None
        if apply_l1:
            selected = (activity.get('input') or {}).get('messages')
            original = self.project(request).get('messages', [])
            if isinstance(selected, list) and selected != original:
                # Reuse the raw fixture values and append only the actual
                # native advice; a persisted credential projection is never
                # substituted for the real executable public request.
                if len(selected) == len(original):
                    for real, old, chosen in zip(messages, original, selected):
                        if old.get('role') != 'system' or chosen.get('role') != 'system':
                            continue
                        old_text, chosen_text = old.get('content'), chosen.get('content')
                        if isinstance(old_text, str) and isinstance(chosen_text, str) and chosen_text.startswith(old_text):
                            suffix = chosen_text[len(old_text):]
                            # The host skill document can already carry this
                            # exact main-loop advice. Native verification needs
                            # one occurrence, so trim only the duplicate from
                            # the newly added advice, leaving raw text intact.
                            if main_replan and any(item.get('role') == 'system' and
                                    isinstance(item.get('content'), str) and
                                    replan in item['content'] for item in original):
                                suffix = suffix.replace(replan, '')
                            if suffix and suffix not in real['content']:
                                real['content'] += suffix
                            break
                elif len(selected) == len(original) + 1 and selected[0].get('role') == 'system':
                    advice = dict(selected[0])
                    if main_replan and isinstance(advice.get('content'), str) and any(
                            item.get('role') == 'system' and isinstance(item.get('content'), str)
                            and replan in item['content'] for item in original):
                        advice['content'] = advice['content'].replace(replan, '')
                    if advice.get('content') and not any(item.get('role') == 'system' and
                               item.get('content') == advice.get('content') for item in messages):
                        messages.insert(0, advice)
            scope = self._cache_scope(state, meta)
            selected = (activity.get('input') or {}).get('messages')
            original = self.project(request).get('messages', [])
            candidate_cards = _advice_cards(original, selected,
                                             remove_text=replan if main_replan else '')
            if not candidate_cards:
                candidate_cards = self._cached_cards(scope)
                if shape_filter is not None:
                    candidate_cards = [card for card in candidate_cards if not any(
                        content in card for content in shape_filter['excluded_contents'])]
                if saved_result_sources is not None and saved_result_phase is None:
                    candidate_cards = [card for card in candidate_cards if not any(
                        content in card for content in saved_result_sources['contents'])]
                for card in reversed(candidate_cards):
                    if not any(item.get('role') == 'system' and item.get('content') == card
                               for item in messages if isinstance(item, dict)):
                        messages.insert(0, {'role': 'system', 'content': card})
            if candidate_cards:
                self._mark_l1_applied()
        else:
            scope, candidate_cards = None, []
        pending = {'context': activity['context'], 'request': request,
                   'loop_context': loop_ctx, 'source': source,
                   'advice_scope': scope, 'advice_candidates': candidate_cards}
        self.current_loop = loop_ctx
        self.preparing_model = pending
        return pending

    def handle(self, event, ctx):
        payload = event.payload
        if event.name == 'llm.request.prepared':
            call_id = payload.get('call_id')
            if not isinstance(call_id, str) or not call_id or call_id in self.models:
                return
            pending = self.preparing_model
            if pending is None:
                # Useful for direct lifecycle fixtures; production enters
                # before_model via the real recorder wrapper below.
                meta = payload.get('meta') or {}
                pending = self.before_model((payload.get('request') or {}).get('messages', []), meta)
            if pending is None:
                return
            pending['request'] = payload.get('request') or {}
            pending['source'] = self._source(event.name, payload)
            self.models[call_id] = pending
            self.preparing_model = None
            self.last_model_call_id = call_id
        elif event.name == 'skill_executor.before_tool_call':
            if self.runtime_level == 'L0':
                return
            if self.current_loop is None:
                self._gap('skill_before_tool_no_loop')
                return
            for record in payload.get('tool_calls') or []:
                call_id = record.get('tool_call_id')
                if not call_id or call_id in self.tools:
                    continue
                raw = record.get('arguments_json', '{}')
                try:
                    args = json.loads(raw)
                except (ValueError, TypeError):
                    args = {'raw_arguments': raw}
                source = self._source(event.name, payload)
                source.update(source_tool_call_id=call_id, ordinal=record.get('ordinal'))
                activity = self.point({'phase': 'before', 'kind': 'action',
                                       'name': record.get('tool_name') or 'framework.tool',
                                       'context': self.current_loop, 'actor_id': 'skill_executor',
                                       'input': args, 'apply_policy': False, 'extensions': source})
                if activity is not None:
                    self.tools[call_id] = {'context': activity['context'], 'input': args, 'source': source}
        elif event.name == 'skill_executor.tool_result':
            if self.runtime_level == 'L0':
                return
            for record in payload.get('tool_results') or []:
                pending = self.tools.pop(record.get('tool_call_id'), None)
                if pending is None:
                    self._gap('skill_tool_result_unknown')
                    continue
                failed = record.get('status') in {'error', 'failed'}
                self.point({'phase': 'after', 'kind': 'action', 'context': pending['context'],
                            'actual_input': record.get('arguments', pending['input']),
                            'executed': True, 'status': 'failed' if failed else 'completed',
                            'output': {'tool_result': record.get('result'), 'status': record.get('status')},
                            'extensions': {**pending['source'], **self._source(event.name, payload)}})

    def finish_model(self, recorder, response):
        call_id = response.get('_agentflow_call_id')
        pending = self.models.pop(call_id, None)
        if pending is None:
            self._gap('finish_model_unknown_call')
            return
        try:
            meta = recorder._client.llm().get('traces/' + call_id + '/meta') if recorder._client else None
            if isinstance(meta, bytes):
                meta = meta.decode('utf-8')
            if isinstance(meta, str):
                meta = json.loads(meta)
        except Exception:
            meta = None
        # The recorder's actual attempt count distinguishes a blocked call
        # from real provider transport, including an HTTP/provider failure.
        attempted = bool(meta and meta.get('transport_attempts', 0) > 0)
        if meta is None:
            self._gap('finish_model_meta_missing')
        failed = bool(response.get('_llm_error') or response.get('error'))
        packet = {'phase': 'after', 'kind': self.model_kind, 'context': pending['context'],
                  'actual_input': pending['request'], 'executed': attempted,
                  'status': 'failed' if failed else 'completed',
                  'output': self._response(response),
                  'extensions': {**pending['source'], 'source_timestamp_us':
                                 int(time.clock_gettime(time.CLOCK_REALTIME) * 1000000)}}
        usage = response.get('usage')
        if self.model_kind == 'model' and isinstance(usage, dict):
            metrics = {k: usage[v] for k, v in [('input_tokens', 'prompt_tokens'),
                       ('output_tokens', 'completion_tokens'), ('tokens', 'total_tokens')]
                       if isinstance(usage.get(v), int) and not isinstance(usage[v], bool) and usage[v] >= 0}
            if metrics:
                packet['metrics'] = metrics
        result = self.point(packet)
        if (_native_application_verified(result, pending.get('context'))
                and pending.get('advice_scope') and pending.get('advice_candidates')):
            self._remember_verified(pending['advice_scope'], pending['advice_candidates'])
        attempts = int(meta.get('transport_attempts', 0)) if isinstance(meta, dict) else 0
        self.transport_attempts += attempts
        self._stat('transport_attempts', attempts)
        if result is not None:
            self.closed_model_calls += 1
            self._stat('calls')

    def close(self):
        # Planned/skipped tool calls and interrupted requests are aborted,
        # never counted as successful execution or left to block run.end.
        if self.preparing_model is not None:
            self.point({'phase': 'abort', 'kind': self.model_kind,
                        'context': self.preparing_model['context'],
                        'extensions': self.preparing_model['source']})
            self.preparing_model = None
        for kind, items in [(self.model_kind, self.models), ('action', self.tools)]:
            for pending in list(items.values()):
                self.point({'phase': 'abort', 'kind': kind, 'context': pending['context'],
                            'extensions': pending['source']})
            items.clear()


def install_native_observer():
    if not os.environ.get('APPWORLD_CAPTURE_URL') or not os.environ.get('APPWORLD_CAPTURE_TOKEN'):
        return None
    from agentflow_runtime.hooks.registry import default_registry
    from agentflow_runtime.recorder import TraceRecorder
    observer = NativeFrameworkObserver()
    default_registry.register('appworld_native_framework_observer',
        ['llm.request.prepared', 'skill_executor.before_tool_call', 'skill_executor.tool_result'],
        observer.handle, order=1000)
    original = TraceRecorder.trace_call
    signature = inspect.signature(original)
    if not getattr(original, '_appworld_native_observer', False):
        def trace(self, *args, **kwargs):
            bound = signature.bind_partial(self, *args, **kwargs)
            values = bound.arguments
            import copy
            values['messages'] = copy.deepcopy(values.get('messages', []))
            from agentflow_runtime.recorder import _task_context
            meta = _task_context()
            meta.update(source=values.get('source', ''),
                        skill_name=values.get('skill', ''), round=values.get('round_i', 0))
            # An actual framework span defines its loop iteration. Compaction
            # inside a skill round shares that span; coordinator correction
            # decisions carry their actual decision_index even in one round.
            try:
                import importlib
                from agentflow_runtime import events
                from agentflow_runtime.trace.span_context import SpanContext
                span_module = importlib.import_module('agentflow_runtime.trace.emit')
                span_id = SpanContext.current().current_span_id()
                with span_module._active_spans_lock:
                    active = span_module._active_spans.get(span_id)
                    if active and active.name in {events.SPAN_SKILL_EXECUTOR_LLM_ROUND,
                                                  events.SPAN_COORDINATOR_LLM_ROUND}:
                        meta['framework_span_id'] = active.span_id
                        for key in ('round', 'activation_index', 'decision_index',
                                    'internal_tool_correction_index'):
                            if key in active.tags:
                                meta[key] = int(active.tags[key])
            except (ImportError, AttributeError, TypeError, ValueError):
                observer._gap('trace_span_context')
            try:
                pending = observer.before_model(values.get('messages', []), meta)
            except Exception as error:
                observer._record_diagnostic('trace_before_model_error', {}, error=error)
                observer._gap('trace_before_model')
                pending = None
            try:
                response = original(*bound.args, **bound.kwargs)
            except Exception:
                if pending is not None:
                    observer.point({'phase': 'abort', 'kind': observer.model_kind,
                                    'context': pending['context'], 'extensions': pending['source']})
                    for call_id, candidate in list(observer.models.items()):
                        if candidate is pending:
                            observer.models.pop(call_id, None)
                observer.preparing_model = None
                raise
            try:
                observer.finish_model(self, response)
            except Exception as error:
                observer._record_diagnostic('trace_finish_model_error', {}, error=error)
                observer._gap('trace_finish_model')
            return response
        trace._appworld_native_observer = True
        TraceRecorder.trace_call = trace
    atexit.register(observer.close)
    return observer
