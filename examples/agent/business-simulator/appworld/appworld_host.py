"""Real AgentFlow decisions executed once in the official AppWorld sandbox.

Each native task owns an independent AppWorld and private AgentFlow daemon.
Only public instruction, profile, API docs and observations reach the model.
The official final database tests run after interaction, never during planning.
"""
from __future__ import annotations

import argparse
import ast
import json
import io
import os
from pathlib import Path
import re
import sys
import time
import tokenize
import uuid

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[3]
# The completion-stop contract changed the host's externally observable
# execution behavior.  Bump the revision so cached captures from v7 cannot be
# silently mixed into v8 comparisons.
HOST_VERSION = "appworld-agentflow-v22"
REDACTION = "<redacted:credential_field>"
_SECRET_FIELDS = {"authorization", "apikey", "password", "passwd", "accesstoken",
                  "refreshtoken", "sessiontoken", "token", "clientsecret", "privatekey",
                  "cookie", "setcookie", "secret", "credentials"}
_NATIVE_CREDENTIAL_PATTERNS = [
    (r"\bsk-[A-Za-z0-9_-]{16,}\b", 0), (r"\bark-[A-Za-z0-9_-]{16,}\b", 0),
    (r"\bAKIA[A-Z0-9]{16}\b", 0), (r"\bAIza[A-Za-z0-9_-]{20,}\b", 0),
    (r"\bgh[pousr]_[A-Za-z0-9_]{20,}\b", 0), (r"\bgithub_pat_[A-Za-z0-9_]{20,}\b", 0),
    (r"\bxox[a-z]-[A-Za-z0-9-]{16,}\b", 0),
    (r"\bBearer\s+[A-Za-z0-9._~+/-]{16,}=*\b", re.I),
    (r"(api[_ -]?key|access[_ -]?token|refresh[_ -]?token|session[_ -]?token|token|secret|password|passwd|authorization|client[_ -]?secret|private[_ -]?key|set[_ -]?cookie|cookie|credentials)\s*[:=]\s*[\"']?[A-Za-z0-9._~+/-]{12,}", re.I),
    (r"([?&](api[_-]?key|access[_-]?token|token|signature|sig|key)=)[^&\s\"']{8,}", re.I),
    (r"\beyJ[A-Za-z0-9_-]{16,}\.[A-Za-z0-9_-]{8,}\.[A-Za-z0-9_-]{8,}\b", 0),
    (r"-----BEGIN [A-Z ]*PRIVATE KEY-----", 0),
]
_OBSERVATION_CREDENTIAL_FIELDS = {
    "observation", "error_detail", "message", "stderr", "stdout", "response",
}
_OBSERVATION_KEY_VALUE = re.compile(
    r"(?i)((?:[\"']?(?:password|passwd|token|access[_-]?token|refresh[_-]?token|"
    r"session[_-]?token|api[_-]?key|secret|authorization)[\"']?)\s*[:=]\s*)"
    r"([A-Za-z0-9._~+/-]{8,})"
)


def native_contains_secret(text, depth=0):
    if depth > 16:
        return True
    for name, secret in os.environ.items():
        if len(secret) >= 8 and any(part in name.upper() for part in
                                  ["KEY", "TOKEN", "SECRET", "PASSWORD", "CREDENTIAL", "AUTH"]):
            if secret in text:
                return True
    try:
        decoded = json.loads(text)
    except (ValueError, TypeError):
        decoded = None
    if isinstance(decoded, (dict, list)):
        pending = [decoded]
        while pending:
            item = pending.pop()
            if isinstance(item, dict):
                pending.extend(item.keys())
                pending.extend(item.values())
            elif isinstance(item, list):
                pending.extend(item)
            elif isinstance(item, str) and native_contains_secret(item, depth + 1):
                return True
        return False
    if isinstance(decoded, str):
        return native_contains_secret(decoded, depth + 1)
    inspected = text.replace(REDACTION, "")
    return any(re.search(pattern, inspected, flags) for pattern, flags in _NATIVE_CREDENTIAL_PATTERNS)


def _mask_native_spans(value, *, python=False):
    """Mask only credential values; retain their surrounding code or prose."""
    value = re.sub(r"-----BEGIN [A-Z ]*PRIVATE KEY-----.*?-----END [A-Z ]*PRIVATE KEY-----",
                   REDACTION, value, flags=re.S)
    for name, secret in os.environ.items():
        if len(secret) >= 8 and any(part in name.upper() for part in
                                  ["KEY", "TOKEN", "SECRET", "PASSWORD", "CREDENTIAL", "AUTH"]):
            value = value.replace(secret, repr(REDACTION) if python else REDACTION)
    for index, (pattern, flags) in enumerate(_NATIVE_CREDENTIAL_PATTERNS):
        def replace(match):
            hit = match.group(0)
            if index == 8:
                prefix = re.match(r"(.*?[:=]\s*)([\"']?)", hit, re.S)
                marker = REDACTION if prefix.group(2) or not python else repr(REDACTION)
                return prefix.group(1) + prefix.group(2) + marker
            if index == 9:
                return match.group(1) + REDACTION
            if index == 7:
                return re.match(r"(?i)Bearer\s+", hit).group(0) + REDACTION
            return repr(REDACTION) if python else REDACTION
        value = re.sub(pattern, replace, value, flags=flags)
    return value


def _redact_python_literals(value, *, preserve_credential_call_structure=False):
    """Keep string/code bodies intact, replacing only credential spans."""
    try:
        tokens = list(tokenize.generate_tokens(io.StringIO(value).readline))
    except (IndentationError, tokenize.TokenError):
        return value
    replacements = []
    for token in tokens:
        if token.type != tokenize.STRING:
            continue
        try:
            # AppWorld stdout commonly contains JSON string literals. Its
            # decoder correctly combines escaped UTF-16 surrogate pairs;
            # ast.literal_eval would create unencodable surrogate characters.
            content = json.loads(token.string)
        except (ValueError, TypeError):
            try:
                content = ast.literal_eval(token.string)
            except (SyntaxError, ValueError):
                content = None
        if isinstance(content, str):
            try:
                structured = isinstance(json.loads(content), (dict, list))
            except (ValueError, TypeError):
                structured = False
            # A Python string literal is data rather than an executable
            # reference. Mask credential spans within it, retaining its body.
            safe = native_view(content if structured else _mask_native_spans(content),
                               preserve_credential_call_structure=preserve_credential_call_structure)
            if safe != content:
                replacements.append((token.start, token.end, repr(safe)))
        elif _mask_native_spans(token.string) != token.string:
            replacements.append((token.start, token.end, _mask_native_spans(token.string)))
    if not replacements:
        return value
    lines = value.splitlines(keepends=True)
    offsets, offset = [], 0
    for line in lines:
        offsets.append(offset)
        offset += len(line)
    def absolute(point):
        row, column = point
        return offsets[row - 1] + column
    for start, end, replacement in reversed(replacements):
        value = value[:absolute(start)] + replacement + value[absolute(end):]
    return value


def _python_tokens(value):
    """Return tokens when a value can be treated as a code-shaped string."""
    try:
        return list(tokenize.generate_tokens(io.StringIO(value).readline))
    except (IndentationError, tokenize.TokenError):
        return None


def _display_reference_rhs(value, *, preserve_credential_call_structure=False):
    """Parenthesize genuine Python references in the persisted view only.

    The shared native guard lexically treats a long identifier after a
    credential label as a credential. Parentheses leave Python semantics
    intact while keeping that public code reference distinct from a literal.
    """
    try:
        tree = ast.parse(value)
    except (SyntaxError, ValueError):
        return value
    candidates = []
    fields = _SECRET_FIELDS
    for node in ast.walk(tree):
        def target_name(target):
            if isinstance(target, ast.Name):
                return target.id
            if isinstance(target, ast.Attribute):
                return target.attr
            if isinstance(target, ast.Subscript) and isinstance(target.slice, ast.Constant) and isinstance(target.slice.value, str):
                return target.slice.value
            return None
        if isinstance(node, ast.keyword):
            names, expression = [node.arg], node.value
        elif isinstance(node, ast.Assign):
            names, expression = [target_name(target) for target in node.targets], node.value
        elif isinstance(node, ast.AnnAssign):
            names, expression = [target_name(node.target)], node.value
        else:
            continue
        if expression is None:
            continue
        if not isinstance(expression, (ast.Name, ast.Attribute, ast.Subscript, ast.Call)):
            expression = next((child for child in ast.walk(expression)
                if isinstance(child, (ast.Name, ast.Attribute, ast.Subscript, ast.Call))
                and child.lineno == expression.lineno and child.col_offset == expression.col_offset), None)
            if expression is None:
                continue
        # Legacy captures keep their original projection. An explicit fixed
        # projection also protects credential suffixes such as phone_token.
        if not any(name and (re.sub(r"[-_]", "", name).lower() in fields
                or preserve_credential_call_structure is True
                and any(re.sub(r"[-_]", "", name).lower().endswith(field)
                        for field in fields)) for name in names):
            continue
        segment = ast.get_source_segment(value, expression)
        if segment and re.match(r"[A-Za-z0-9._~+/-]{12,}", segment):
            candidates.append(expression)
    if not candidates:
        return value
    lines = value.splitlines(keepends=True)
    offsets = []
    total = 0
    for line in lines:
        offsets.append(total)
        total += len(line)
    def position(row, column):
        # AST columns are UTF-8 byte offsets; native persisted code may
        # contain non-ASCII identifiers and comments on the same line.
        return offsets[row - 1] + len(lines[row - 1].encode("utf-8")[:column].decode("utf-8"))
    insertions = {}
    for expression in candidates:
        start = position(expression.lineno, expression.col_offset)
        end = position(expression.end_lineno, expression.end_col_offset)
        # Preserve idempotence when native_view is called more than once.
        if start > 0 and end < len(value) and value[start - 1] == "(" and value[end] == ")":
            continue
        insertions[start] = insertions.get(start, "") + "("
        insertions[end] = ")" + insertions.get(end, "")
    for offset in sorted(insertions, reverse=True):
        value = value[:offset] + insertions[offset] + value[offset:]
    return value


def native_view(value, *, preserve_credential_call_structure=False):
    """Redact the persisted view, preserving actual model/environment values."""
    if isinstance(value, dict):
        result = {}
        for key, item in value.items():
            normalized = re.sub(r"[-_]", "", str(key)).lower()
            output_key = f"redacted_{key}" if normalized in _SECRET_FIELDS else key
            if isinstance(output_key, str) and native_contains_secret(output_key):
                output_key = _mask_native_spans(output_key)
            if normalized in _SECRET_FIELDS:
                result[output_key] = REDACTION
            elif normalized in _OBSERVATION_CREDENTIAL_FIELDS and isinstance(item, str):
                # Runtime observations are plain text, not executable code.
                # Treat an opaque bare RHS as a credential too; otherwise a
                # value such as ``access_token: opaqueName`` is mistaken for
                # a Python identifier and reaches the native boundary.
                observed = native_view(_mask_native_spans(_OBSERVATION_KEY_VALUE.sub(lambda match: match.group(1) + REDACTION, item)),
                    preserve_credential_call_structure=preserve_credential_call_structure)
                result[output_key] = _OBSERVATION_KEY_VALUE.sub(
                    lambda match: match.group(1) + REDACTION, observed)
            else:
                result[output_key] = native_view(item,
                    preserve_credential_call_structure=preserve_credential_call_structure)
        return result
    if isinstance(value, list):
        return [native_view(item, preserve_credential_call_structure=preserve_credential_call_structure)
                for item in value]
    if not isinstance(value, str):
        return value
    # Decode every serialized layer before inspecting individual code and
    # history values. Mixed prose is projected below without erasing it.
    try:
        decoded = json.loads(value)
    except (ValueError, TypeError):
        decoded = None
    if isinstance(decoded, (dict, list, str)):
        projected = json.dumps(native_view(decoded,
            preserve_credential_call_structure=preserve_credential_call_structure), ensure_ascii=False)
        if not native_contains_secret(projected):
            return projected
        raise RuntimeError("appworld_native_projection_unmeasurable")
    value = _display_reference_rhs(value,
        preserve_credential_call_structure=preserve_credential_call_structure)
    value = _redact_python_literals(value,
        preserve_credential_call_structure=preserve_credential_call_structure)
    try:
        ast.parse(value)
        valid_python = True
    except (SyntaxError, ValueError):
        valid_python = False
    value = _mask_native_spans(value, python=valid_python)
    if native_contains_secret(value):
        # Preserve normal Agent execution; the caller records an explicit
        # capture gap rather than sending rejected data or inventing a trace.
        raise RuntimeError("appworld_native_projection_unmeasurable")
    return value


def _official_io_blocks(text):
    """Read public environment log fences without executing their contents."""
    blocks, current, language = [], None, None
    for line in text.splitlines(keepends=True):
        marker = line.rstrip('\r\n')
        if current is None:
            if marker in ('```python', '```'):
                current, language = [], marker.removeprefix('```')
            elif marker.startswith('```'):
                raise ValueError('appworld_projection_io_fence_invalid')
        elif marker == '```':
            blocks.append((language, ''.join(current)))
            current, language = None, None
        else:
            current.append(line)
    if current is not None or len(blocks) % 2:
        raise ValueError('appworld_projection_io_fence_invalid')
    if any(blocks[index][0] != 'python' or blocks[index + 1][0] != ''
           for index in range(0, len(blocks), 2)):
        raise ValueError('appworld_projection_io_fence_invalid')
    return [(blocks[index][1], blocks[index + 1][1])
            for index in range(0, len(blocks), 2)]


def _official_unlogged_safety_rejection(event):
    """Verify an official early return using its pure syntax guard only."""
    code = event.get('input', {}).get('code')
    observation = event.get('output', {}).get('observation')
    if (event.get('kind') != 'action.end' or event.get('loop_id') != 'main'
            or event.get('status') != 'failed'
            or event.get('error', {}).get('code') != 'appworld_code_error'
            or event.get('attributes', {}).get('executed') is not True
            or event.get('attributes', {}).get('observed_failure') is not True
            or event.get('attributes', {}).get('observed_error_code') != 'appworld_code_error'
            or not isinstance(code, str) or not isinstance(observation, str)
            or 'redacted_' in code
            or native_contains_secret(code) or native_view(code) != code
            or native_view(code, preserve_credential_call_structure=True) != code):
        return False
    from appworld.common.safety_guard import SafetyGuard
    # The official method reads only the syntax tree and static allowlists.
    # Avoid the constructor, which also changes process-wide SystemExit.
    safe, message = SafetyGuard.is_syntax_safe(None, code)
    if safe is not False or observation != 'Execution failed. Traceback:\n' + message:
        return False
    if REDACTION not in code:
        return True
    # A redacted string does not hide an import or a static call path. Require
    # this exact official rejection to be structural, and prove the same
    # rejection with independent literal replacements. Never execute code or
    # infer a credential's original value from its display placeholder.
    if not re.fullmatch(r'Usage of the following (?:modules?|functions?) is not allowed: '
                        r'[A-Za-z0-9_., ]+\.', message):
        return False
    try:
        tree = ast.parse(code)
        literals = [node for node in ast.walk(tree)
                    if isinstance(node, ast.Constant) and isinstance(node.value, str)
                    and REDACTION in node.value]
        if sum(node.value.count(REDACTION) for node in literals) != code.count(REDACTION):
            return False
        for replacement in ('public_literal_probe_a', 'public_literal_probe_b'):
            class ReplaceLiteral(ast.NodeTransformer):
                def visit_Constant(self, node):
                    if isinstance(node.value, str) and REDACTION in node.value:
                        return ast.copy_location(ast.Constant(value=replacement), node)
                    return node
            projected = ast.unparse(ReplaceLiteral().visit(ast.parse(code)))
            alternative_safe, alternative_message = SafetyGuard.is_syntax_safe(None, projected)
            if alternative_safe is not False or alternative_message != message:
                return False
    except (SyntaxError, ValueError, TypeError):
        return False
    return True


def project_official_code_io(request, *, evidence_root=None):
    """Reconstruct only strictly bound public code in a temporary judge view.

    The native caller validates its report first. This adapter independently
    checks trial/run/task identity and every executed input/observation against
    the same run's public IO log before returning any sanitized replacement.
    It neither evaluates Python nor reads task answers or ground truth.
    """
    trial_id = request.get('trial_id') if isinstance(request, dict) else None
    run_id = (request.get('evidence', {}).get('execution_run_id')
              if isinstance(request, dict) and isinstance(request.get('evidence'), dict) else None)
    result = {'schema_version': 1,
              'trial_id': trial_id if isinstance(trial_id, str) and re.fullmatch(r'[a-f0-9]{32}', trial_id) else None,
              'execution_run_id': run_id if isinstance(run_id, str) and re.fullmatch(r'[a-f0-9]{40}', run_id) else None,
              'status': 'unknown', 'patches': []}
    try:
        if (not isinstance(request, dict) or type(request.get('schema_version')) is not int
                or request.get('schema_version') != 1):
            raise ValueError('appworld_projection_request_invalid')
        trial_id, task_id = request.get('trial_id'), request.get('task_id')
        evidence = request.get('evidence')
        initial = request.get('task_initial_state')
        if (not isinstance(trial_id, str) or not re.fullmatch(r'[a-f0-9]{32}', trial_id)
                or not isinstance(task_id, str) or not re.fullmatch(r'appworld-[A-Za-z0-9_-]+', task_id)
                or not isinstance(evidence, dict) or not isinstance(initial, dict)):
            raise ValueError('appworld_projection_identity_invalid')
        run_id = evidence.get('execution_run_id')
        if not isinstance(run_id, str) or not re.fullmatch(r'[a-f0-9]{40}', run_id):
            raise ValueError('appworld_projection_identity_invalid')
        trace = evidence.get('trace')
        if trace is not None and evidence.get('traces'):
            raise ValueError('appworld_projection_trace_ambiguous')
        if trace is None:
            traces = evidence.get('traces')
            if not isinstance(traces, list) or len(traces) != 1:
                raise ValueError('appworld_projection_trace_ambiguous')
            trace = traces[0]
        if not isinstance(trace, dict):
            raise ValueError('appworld_projection_trace_invalid')
        identity = trace.get('identity') or {}
        if identity.get('run_id') != run_id or identity.get('conversation_id') != trial_id:
            raise ValueError('appworld_projection_run_mismatch')
        events = trace.get('events')
        if not isinstance(events, list) or not events:
            raise ValueError('appworld_projection_trace_invalid')
        source_task = initial.get('source_task_id')
        if not isinstance(source_task, str) or not re.fullmatch(r'[a-z0-9]+_[0-9]+', source_task):
            raise ValueError('appworld_projection_task_invalid')
        if not any(event.get('kind') == 'run.start' and
                   event.get('input', {}).get('task_id') == task_id for event in events):
            raise ValueError('appworld_projection_task_mismatch')
        output = evidence.get('output') or {}
        experiment = output.get('official_experiment_name')
        directory = output.get('official_evidence_dir')
        if (not isinstance(experiment, str) or not re.fullmatch(r'agentflow-[a-f0-9]{32}', experiment)
                or not isinstance(directory, str) or not Path(directory).is_absolute()):
            raise ValueError('appworld_projection_sidecar_identity_invalid')
        root = Path(os.path.abspath(evidence_root or ROOT / 'trace_output/appworld'))
        folder = Path(directory).resolve()
        source_kind = request.get('source_kind')
        if source_kind is None:
            source = request.get('source_report')
        elif source_kind in ('completed_capture', 'historical_snapshot'):
            source = request.get('source_trial')
        else:
            raise ValueError('appworld_projection_source_kind_invalid')
        if (not isinstance(source, str) or not Path(source).is_absolute()
                or not Path(source).is_file()
                or not Path(os.path.abspath(source)).is_relative_to(root)
                or not Path(os.path.abspath(directory)).is_relative_to(root) or folder.name != experiment):
            raise ValueError('appworld_projection_sidecar_scope_invalid')
        path = (folder / 'tasks' / source_task / 'logs/environment_io.md').resolve()
        if not path.is_relative_to(folder) or not path.is_file() or path.stat().st_size > 64 * 1024 * 1024:
            raise ValueError('appworld_projection_sidecar_missing')
        blocks = _official_io_blocks(path.read_text(encoding='utf-8'))
        seen_ids, ends, dispatches, failed_dispatches, world_actions = set(), [], [], [], []
        for event in events:
            event_id = event.get('event_id')
            sequence = event.get('sequence')
            if (not isinstance(event_id, str) or event_id in seen_ids
                    or not isinstance(sequence, (str, int)) or isinstance(sequence, bool)
                    or not re.fullmatch(r'[0-9]+', str(sequence))):
                raise ValueError('appworld_projection_event_ambiguous')
            seen_ids.add(event_id)
            if event.get('kind') == 'action.end' and isinstance(event.get('input', {}).get('code'), str):
                executed = event.get('attributes', {}).get('executed')
                extensions = event.get('attributes', {}).get('extensions', {})
                if (event.get('actor_id') == 'skill_executor'
                        and extensions.get('source_event_name') == 'skill_executor.tool_result'
                        and isinstance(event.get('output', {}).get('tool_result'), str)):
                    saved = json.loads(event['output']['tool_result'])
                    # Executing this framework tool can fail before it saves a
                    # World proposal. Keep that real failure unchanged; only a
                    # verified saved reply may be projected as executed code.
                    if (executed is True and event.get('output', {}).get('status') == 'error'
                            and isinstance(saved, dict) and set(saved) == {'error'}
                            and isinstance(saved['error'], str) and saved['error']
                            and 'observation' not in event.get('output', {})):
                        failed_dispatches.append(event)
                        continue
                    if (executed is not True or event.get('output', {}).get('status') != 'completed'
                            or not isinstance(saved, dict) or saved.get('status') != 'completed'
                            or saved.get('reply_saved') is not True
                            or 'observation' in event.get('output', {})):
                        raise ValueError('appworld_projection_dispatch_invalid')
                    dispatches.append(event)
                    continue
                if executed is True:
                    if (not isinstance(event.get('output', {}).get('observation'), str)
                            or not isinstance(event.get('action_id'), str)):
                        raise ValueError('appworld_projection_execution_invalid')
                    ends.append(event)
                elif executed is not False:
                    raise ValueError('appworld_projection_execution_unknown')
                world_actions.append(event)
        # A skill dispatch saves proposed code; the public World call later
        # produces its observation. Bind the two by their real main decision
        # ancestry and exact code bytes, rather than counting the dispatch as
        # a second execution or matching repeated code from another round.
        by_id = {event['event_id']: event for event in events}
        positions = {event['event_id']: index for index, event in enumerate(events)}
        decision_cache = {}
        def main_decisions(event):
            if event['event_id'] in decision_cache:
                return decision_cache[event['event_id']]
            pending, visited, decisions = list(event.get('caused_by') or []), set(), set()
            while pending:
                parent_id = pending.pop()
                if parent_id in visited: continue
                visited.add(parent_id)
                parent = by_id.get(parent_id)
                if parent is None: continue
                if parent.get('kind') == 'loop.decision' and parent.get('loop_id') == 'main':
                    decisions.add(parent_id)
                    continue
                pending.extend(parent.get('caused_by') or [])
            decision_cache[event['event_id']] = decisions
            return decisions
        def latest_decision(event):
            roots = main_decisions(event)
            return max(roots, key=positions.__getitem__) if roots else None
        worlds_by_decision_code = {}
        for event in world_actions:
            if event.get('loop_id') == 'main':
                key = (latest_decision(event), event['input']['code'])
                worlds_by_decision_code.setdefault(key, []).append(event)
        for dispatch in failed_dispatches:
            starts = [event for event in events if event.get('kind') == 'action.start'
                      and event.get('action_id') == dispatch.get('action_id')]
            closes = [event for event in events if event.get('kind') == 'action.end'
                      and event.get('action_id') == dispatch.get('action_id')]
            root = latest_decision(dispatch)
            proposed, _ = normalize_appworld_code(dispatch['input']['code'])
            if (len(starts) != 1 or len(closes) != 1
                    or starts[0].get('actor_id') != 'skill_executor'
                    or starts[0].get('input', {}).get('code') != dispatch['input']['code']
                    or positions[starts[0]['event_id']] >= positions[dispatch['event_id']]
                    or root is None or latest_decision(starts[0]) != root
                    or worlds_by_decision_code.get((root, proposed))):
                raise ValueError('appworld_projection_dispatch_invalid')
        dispatch_by_action = {}
        for dispatch in dispatches:
            root = latest_decision(dispatch)
            proposed, _ = normalize_appworld_code(dispatch['input']['code'])
            candidates = [event for event in worlds_by_decision_code.get((root, proposed), [])
                          if root is not None and positions[event['event_id']] > positions[dispatch['event_id']]]
            if len(candidates) != 1:
                raise ValueError('appworld_projection_dispatch_ambiguous')
            target = candidates[0]
            dispatch_by_action.setdefault(target['action_id'], []).append(dispatch)
        # Sequence is local to each native activity, not a global event
        # counter. Keep the verified trace's append order for the IO pairing.
        if len(ends) != len(blocks):
            # Official syntax-safety rejection appends in-memory IO, then
            # returns before save_logs(). Only a final rejected suffix can
            # therefore be absent from the persisted log. Keep these real
            # attempts unchanged in the trace and verify every rejection;
            # never manufacture IO or treat the attempts as successful.
            closes = [event for event in events if event.get('kind') == 'run.end']
            unlogged = ends[len(blocks):]
            end_counts = {}
            for event in world_actions:
                action_id = event.get('action_id')
                end_counts[action_id] = end_counts.get(action_id, 0) + 1
            def bound_rejection(event):
                starts = [start for start in events if start.get('kind') == 'action.start'
                          and start.get('action_id') == event.get('action_id')]
                return (end_counts.get(event.get('action_id')) == 1 and len(starts) == 1
                        and positions[starts[0]['event_id']] < positions[event['event_id']]
                        and starts[0].get('input', {}).get('code') == event['input']['code']
                        and _official_unlogged_safety_rejection(event))
            if (len(ends) < len(blocks) or len(closes) != 1
                    or closes[0] is not events[-1]
                    or output.get('evaluation_status') != 'completed'
                    or closes[0].get('output', {}).get('evaluation_status') != 'completed'
                    or not all(bound_rejection(event) for event in unlogged)):
                raise ValueError('appworld_projection_io_count_mismatch')
        patches, seen_actions = [], set()
        def match_original(body, old, *, observation=False):
            # The Markdown writer adds a fence-separating newline even when
            # the submitted code/observation has none. Accept only an exact
            # original projection, with that one explicit boundary removed.
            candidates = [body]
            if body.endswith('\n'): candidates.append(body[:-1])
            if not observation and body.endswith('\n'):
                # World.execute strips code before storing the public input.
                # Recover only the submitted boundary whitespace retained in
                # this exact native event, then require the entire old view to
                # match byte for byte. Never normalize inner code or use AST
                # similarity as a substitute for this equality.
                official_input = body[:-1]
                if official_input and official_input == official_input.strip():
                    leading = old[:len(old) - len(old.lstrip())]
                    trailing = old[len(old.rstrip()):]
                    reconstructed = leading + official_input + trailing
                    if reconstructed not in candidates:
                        candidates.append(reconstructed)
            matches = []
            for raw in candidates:
                projected = (native_view({'observation': raw})['observation']
                             if observation else native_view(raw))
                # Official World.execute returns the untrimmed message but
                # stores message.rstrip() in environment_io. The Markdown
                # writer adds one newline. Reproduce only that documented
                # observation boundary; code bytes stay exact and observations
                # are never changed in the returned patches.
                if projected == old or observation and projected == old.rstrip():
                    matches.append(raw)
            # Serialized JSON observations are canonically decoded by the
            # legacy view, so its one fence newline has the same projected
            # bytes with or without removal. Observations are never patched.
            if not matches or not observation and len(matches) != 1:
                raise ValueError('appworld_projection_old_value_mismatch')
            return matches[0]
        for index, (event, (raw_code, raw_observation)) in enumerate(zip(ends, blocks)):
            old_code = event['input']['code']
            actual_code = match_original(raw_code, old_code)
            match_original(raw_observation, event['output']['observation'], observation=True)
            action_id = event['action_id']
            if action_id in seen_actions:
                raise ValueError('appworld_projection_action_ambiguous')
            seen_actions.add(action_id)
            code = native_view(actual_code, preserve_credential_call_structure=True)
            if native_contains_secret(code):
                raise ValueError('appworld_projection_credential_detected')
            replacements = {action_id: code}
            for dispatch in dispatch_by_action.get(action_id, []):
                # Preserve the proposed import lines. Only the host's existing
                # documented import normalization is used to bind it to the
                # actual World input; the proposal itself is not rewritten as
                # if the model had omitted those lines.
                actual_lines = iter(actual_code.splitlines(keepends=True))
                original_lines = []
                for line in dispatch['input']['code'].splitlines(keepends=True):
                    _, removed = normalize_appworld_code(line)
                    original_lines.append(line if removed else next(actual_lines, None))
                if any(line is None for line in original_lines) or next(actual_lines, None) is not None:
                    raise ValueError('appworld_projection_dispatch_source_mismatch')
                original = ''.join(original_lines)
                if native_view(original) != dispatch['input']['code']:
                    raise ValueError('appworld_projection_dispatch_source_mismatch')
                proposed_view = native_view(original, preserve_credential_call_structure=True)
                if native_contains_secret(proposed_view):
                    raise ValueError('appworld_projection_credential_detected')
                replacements[dispatch['action_id']] = proposed_view
            for related in events:
                destination_code = replacements.get(related.get('action_id'))
                related_old = related.get('input', {}).get('code')
                if (destination_code is not None and destination_code != related_old
                        and related.get('kind') in ('action.start', 'action.end')
                        and isinstance(related_old, str)
                        and (related_old == old_code or any(
                            related.get('action_id') == dispatch['action_id']
                            and related_old == dispatch['input']['code']
                            for dispatch in dispatch_by_action.get(action_id, [])))):
                    patches.append({'run_id': run_id, 'event_id': related['event_id'],
                        'old_code': related_old, 'code': destination_code,
                        'source': {'path': str(path), 'input_index': index}})
        result.update(status='complete', patches=patches)
    except (OSError, UnicodeError, ValueError, TypeError, AttributeError, RuntimeError) as error:
        reason = str(error)
        result['reason'] = reason if re.fullmatch(r'appworld_projection_[a-z_]+', reason) else 'appworld_projection_unknown'
    return result


def project_official_code_request(request, *, evidence_root=None):
    """Local command boundary: accept only verified source paths and IDs.

    The native caller compares a historical atomic row with its verified
    report row, or a completed execution snapshot with its in-memory trial,
    before invoking this command. No task body, trace, code, or credential-
    bearing context is accepted through stdin.
    """
    trial_id = request.get('trial_id') if isinstance(request, dict) else None
    run_id = request.get('execution_run_id') if isinstance(request, dict) else None
    result = {'schema_version': 1,
              'trial_id': trial_id if isinstance(trial_id, str) and re.fullmatch(r'[a-f0-9]{32}', trial_id) else None,
              'execution_run_id': run_id if isinstance(run_id, str) and re.fullmatch(r'[a-f0-9]{40}', run_id) else None,
              'status': 'unknown', 'patches': []}
    try:
        historical_keys = {'schema_version', 'source_report', 'trial_id', 'task_id', 'execution_run_id'}
        completed_keys = {'schema_version', 'source_kind', 'source_trial', 'trial_id', 'task_id', 'execution_run_id'}
        if (not isinstance(request, dict) or set(request) not in (historical_keys, completed_keys)
                or type(request.get('schema_version')) is not int or request.get('schema_version') != 1):
            raise ValueError('appworld_projection_request_invalid')
        if (not isinstance(request.get('trial_id'), str)
                or not re.fullmatch(r'[a-f0-9]{32}', request['trial_id'])):
            raise ValueError('appworld_projection_identity_invalid')
        root = Path(os.path.abspath(evidence_root or ROOT / 'trace_output/appworld'))
        completed = set(request) == completed_keys
        if completed:
            kind = request.get('source_kind')
            if kind not in ('completed_capture', 'historical_snapshot'):
                raise ValueError('appworld_projection_source_kind_invalid')
            trial = Path(request['source_trial'])
            if (not trial.is_absolute() or not trial.is_file()
                    or not Path(os.path.abspath(trial)).is_relative_to(root)
                    or trial.name != ('completed-execution.json' if kind == 'completed_capture'
                                      else 'historical-execution.json')
                    or trial.parent.name != request['trial_id']
                    or trial.parent.parent.name != '.work'
                    or trial.stat().st_size > 64 * 1024 * 1024):
                raise ValueError('appworld_projection_completed_source_invalid')
        else:
            report = Path(request['source_report'])
            trial = report.parent / 'trials' / (request['trial_id'] + '.json')
            if (not report.is_absolute() or not report.is_file()
                    or not Path(os.path.abspath(report)).is_relative_to(root)
                    or not trial.is_file()
                    or trial.resolve().parent != report.resolve().parent / 'trials'
                    or trial.stat().st_size > 64 * 1024 * 1024):
                raise ValueError('appworld_projection_atomic_source_invalid')
        row = json.loads(trial.read_text(encoding='utf-8'))
        evidence = row.get('evidence') or {}
        if (row.get('trial_id') != request['trial_id'] or row.get('task_id') != request['task_id']
                or evidence.get('execution_run_id') != request['execution_run_id']):
            raise ValueError('appworld_projection_atomic_identity_mismatch')
        if completed and (row.get('execution_status') != 'completed'
                          or evidence.get('execution_status') != 'completed'
                          or evidence.get('output', {}).get('evaluation_status') != 'completed'):
            raise ValueError('appworld_projection_execution_not_completed')
        output = evidence.get('output') or {}
        directory = output.get('official_evidence_dir')
        experiment = output.get('official_experiment_name')
        if (not isinstance(directory, str) or not Path(directory).is_absolute()
                or not isinstance(experiment, str)
                or not re.fullmatch(r'agentflow-[a-f0-9]{32}', experiment)
                or Path(directory).resolve().name != experiment
                or not Path(os.path.abspath(directory)).is_relative_to(root)):
            raise ValueError('appworld_projection_sidecar_identity_invalid')
        task_root = Path(directory) / 'tasks'
        folders = [path for path in task_root.iterdir() if path.is_dir()]
        if len(folders) != 1 or not re.fullmatch(r'[a-z0-9]+_[0-9]+', folders[0].name):
            raise ValueError('appworld_projection_task_ambiguous')
        raw_id = request['task_id'].removeprefix('appworld-')
        if re.fullmatch(r'[a-z0-9]+_[0-9]+', raw_id) and raw_id != folders[0].name:
            raise ValueError('appworld_projection_task_mismatch')
        source = ({'source_kind': request['source_kind'], 'source_trial': str(trial)}
                  if completed else {'source_report': str(report)})
        return project_official_code_io({
            'schema_version': 1, **source,
            'trial_id': row['trial_id'], 'task_id': row['task_id'],
            'task_initial_state': {'source_task_id': folders[0].name}, 'evidence': evidence},
            evidence_root=root)
    except (OSError, UnicodeError, ValueError, TypeError, AttributeError, RuntimeError) as error:
        reason = str(error)
        result['reason'] = reason if re.fullmatch(r'appworld_projection_[a-z_]+', reason) else 'appworld_projection_unknown'
    return result


def real_clock():
    return time.clock_gettime(time.CLOCK_MONOTONIC)


def appworld_failure_code(observation):
    """Classify the current explicit exception, never incidental HTTP numbers."""
    text = observation if isinstance(observation, str) else ""
    # stdout may include previous attempts or API documentation. The final
    # traceback is the current failed execution's factual exception boundary.
    traceback = text.rfind("Traceback (most recent call last):")
    if traceback >= 0:
        text = text[traceback:]
    if re.search(r"(?im)^\s*TypeError:.*(?:positional argument|positional arguments)", text):
        return "appworld_type_error_positional"
    status = list(re.finditer(
        r"(?i)(?:status[_ ]?code|HTTP\s+status|HTTP(?:Error|Exception))\s*(?:is\s*|[:=]\s*)?(401|422)\b", text))
    if status:
        return {"401": "appworld_auth_401", "422": "appworld_validation_422"}[status[-1].group(1)]
    return "appworld_code_error"


_APP_PROTECTED_METHODS = {
    "venmo": {"search_users"},
    "spotify": {
        # Private account/library reads require the login token too. Keep the
        # set explicit so public catalog reads are still allowed without one.
        "show_account", "show_album_library", "show_playlist_library",
        "show_downloaded_songs", "show_song_queue", "show_currently_playing",
        "play_music", "like_song", "unlike_song",
    },
}


def _appworld_api_calls(code):
    """Return public ``apis.app.method`` calls without executing model code."""
    try:
        tree = ast.parse(code)
    except (SyntaxError, ValueError, TypeError):
        return []
    calls = []
    for node in ast.walk(tree):
        if not (isinstance(node, ast.Call) and isinstance(node.func, ast.Attribute)
                and isinstance(node.func.value, ast.Attribute)
                and isinstance(node.func.value.value, ast.Name)
                and node.func.value.value.id == "apis"):
            continue
        calls.append((node.func.value.attr, node.func.attr,
                      frozenset(keyword.arg for keyword in node.keywords)))
    return calls


def appworld_completion_submitted(code):
    """Return whether this executed block submitted the public completion API.

    ``world.task_completed()`` is an official postcondition and may remain
    false when the agent has honestly reported completion but a state test
    still fails.  In that case the coordinator must stop asking the skill to
    redispatch the same work; the final evaluator will retain the failure.
    Only a direct top-level AST call to the public supervisor API is accepted
    here.  Calls nested under ``if``/``for``/function definitions are not
    treated as submitted because static inspection cannot prove that branch
    actually executed; the next turn can then observe the real result.
    """
    try:
        tree = ast.parse(code)
    except (SyntaxError, ValueError, TypeError):
        return False

    def completion_call(node):
        """Accept a direct call or a pure display wrapper around one.

        AppWorld examples commonly use ``print(apis.supervisor.complete_task())``;
        only unwrap the harmless builtins used for displaying a result.  Do not
        walk arbitrary nested expressions, because static inspection cannot
        prove that calls under conditionals/functions actually ran.
        """
        if (isinstance(node, ast.Call) and isinstance(node.func, ast.Attribute)
                and isinstance(node.func.value, ast.Attribute)
                and isinstance(node.func.value.value, ast.Name)
                and node.func.value.value.id == "apis"
                and node.func.value.attr == "supervisor"
                and node.func.attr == "complete_task"):
            return True
        if (isinstance(node, ast.Call) and isinstance(node.func, ast.Name)
                and node.func.id in {"print", "str", "repr"}
                and len(node.args) == 1 and not node.keywords):
            return completion_call(node.args[0])
        if (isinstance(node, ast.Call) and isinstance(node.func, ast.Attribute)
                and isinstance(node.func.value, ast.Name)
                and node.func.value.id == "json" and node.func.attr == "dumps"
                and len(node.args) == 1
                and {keyword.arg for keyword in node.keywords} <= {
                    "indent", "sort_keys", "ensure_ascii", "separators"}):
            return completion_call(node.args[0])
        return False

    def displayed_completion(node):
        if completion_call(node):
            return True
        # Real traces commonly print the public completion response.  Unwrap
        # only pure one-argument display/serialization calls; do not walk
        # arbitrary nested expressions, which could be conditional or a
        # helper function that was never executed.
        if not (isinstance(node, ast.Call) and len(node.args) == 1
                and not node.keywords):
            return False
        func = node.func
        allowed = ((isinstance(func, ast.Name) and func.id in {"print", "str", "repr"})
                   or (isinstance(func, ast.Attribute) and isinstance(func.value, ast.Name)
                       and func.value.id == "json" and func.attr == "dumps"))
        return allowed and displayed_completion(node.args[0])

    for statement in tree.body:
        value = None
        if isinstance(statement, ast.Expr):
            value = statement.value
        elif isinstance(statement, (ast.Assign, ast.AnnAssign)):
            value = statement.value
        if displayed_completion(value):
            return True
    return False


def appworld_missing_access_token(code, authenticated_apps):
    """Guard only known protected calls after a successful login.

    AppWorld's public APIs persist Python variables between submissions. A
    model that prints a login response and then calls a protected endpoint
    without assigning its token otherwise causes repeated 401/re-login loops.
    This adapter returns a factual repair prompt before execution; it does not
    inspect hidden state or alter the business simulator.
    """
    for app, method, keywords in _appworld_api_calls(code):
        if app not in authenticated_apps:
            continue
        if method in _APP_PROTECTED_METHODS.get(app, set()):
            if "access_token" not in keywords:
                return True
    return False


def appworld_progress_delta(previous, current):
    """Count observed successful progress, never code edits or failed feedback.

    A successful empty display may still mutate the world, so its progress is
    unknown and allowed. This counter is factual context, not a loop verdict.
    """
    if current.get("status") != "completed":
        return 0
    observation = current.get("observation")
    if not isinstance(observation, str):
        return 0
    if not observation.strip():
        return 1
    return int(not isinstance(previous, dict) or
               previous.get("status") != "completed" or
               observation != previous.get("observation"))


def appworld_semantic_no_progress(previous, current):
    """Identify a repeated factual result without treating code edits as progress.

    AppWorld keeps source variables in the model history, so loop state must not
    use the submitted code as a progress signal.  Count only an identical
    structured failure or an explicit unchanged/no-op result; ordinary reads
    that happen to return the same collection are not marked as waste.
    """
    if not isinstance(previous, dict) or not isinstance(current, dict):
        return False
    if previous.get("status") != current.get("status"):
        return False
    if previous.get("status") == "failed":
        return (previous.get("error_code") == current.get("error_code") and
                previous.get("observation") == current.get("observation"))
    if previous.get("status") != "completed":
        return False
    observation = current.get("observation")
    if not isinstance(observation, str) or not observation.strip() or observation != previous.get("observation"):
        return False
    return bool(re.search(r"\bno\s+changes?\b|\bunchanged\b|\balready\b|\bnothing\s+to\b",
                          observation, flags=re.IGNORECASE))


def appworld_repeated_failure_stop(previous_signature, current_signature,
                                   repeat_count, *, limit=2):
    """Allow one repair, then stop an unchanged factual failure.

    Coordinator/skill turns can otherwise redispatch the same failed code until
    the global step budget is exhausted.  Keep this state in the trial only;
    no fingerprint is persisted and ordinary successful pagination is ignored.
    """
    if current_signature is None:
        return None, 0, False
    count = repeat_count + 1 if current_signature == previous_signature else 1
    return current_signature, count, count >= limit


def normalize_appworld_code(code):
    """Remove only the harmless import of the pre-instantiated public API.

    AppWorld already provides ``apis`` in the execution namespace.  Models
    occasionally emit ``import apis`` anyway; executing that line creates a
    deterministic sandbox error and can trigger coordinator redispatch loops.
    Keep all other source unchanged and report the normalization locally.
    """
    if not isinstance(code, str) or not code:
        return code, False
    lines = code.splitlines(keepends=True)
    kept = []
    removed = False
    pattern = re.compile(
        r"^\s*(?:import\s+apis|from\s+apis\s+import\s+[A-Za-z_][\w]*(?:\s*,\s*[A-Za-z_][\w]*)*)\s*(?:#.*)?(?:\r?\n)?$")
    for line in lines:
        if pattern.match(line):
            removed = True
            continue
        kept.append(line)
    return "".join(kept), removed


os.environ.setdefault("APPWORLD_ROOT", str(HERE / "runtime/world"))
for directory in [HERE, HERE / "runtime", ROOT / "tools/trajectory_evolution/experiments/bfcl-v3",
                  ROOT / "tools/trajectory_evolution/clients/python"]:
    sys.path.insert(0, str(directory))

from appworld_agent_runtime import AgentRunner, COORDINATOR_INSTRUCTION
from appworld_skill import read_reply, submit_recovered_code, native_capture_point
from evolution import Evolution, EvolutionError
from evolution_entry import evaluate




def public_value(value):
    if value is None or isinstance(value, (str, int, float, bool)):
        return value
    if isinstance(value, dict):
        return {str(k): public_value(v) for k, v in value.items()}
    if isinstance(value, (list, tuple)):
        return [public_value(v) for v in value]
    if hasattr(value, "model_dump"):
        return public_value(value.model_dump(mode="json"))
    if hasattr(value, "isoformat"):
        return value.isoformat()
    raise TypeError("appworld_public_value_unsupported")


def public_context(world):
    task = world.task
    supervisor = {name: getattr(task.supervisor, name) for name in
                  ["first_name", "last_name", "email", "phone_number"]}
    docs = task.api_docs
    # Discover all other public schemas through the official api_docs APIs.
    bootstrap_docs = {name: public_value(docs[name]) for name in
                      ["api_docs", "supervisor"] if name in docs}
    return {"instruction": task.instruction, "supervisor": supervisor,
            "datetime": public_value(task.datetime),
            "app_descriptions": public_value(task.app_descriptions),
            "bootstrap_api_docs": bootstrap_docs}


def public_auth_parameter_contracts(world):
    """Read required-token method names from the same public API registry."""
    pairs = []
    for app, methods in world.task.api_docs.items():
        if not isinstance(app, str) or not isinstance(methods, dict):
            continue
        for method, doc in methods.items():
            if (not isinstance(method, str) or not isinstance(doc, dict)
                    or doc.get('app_name') != app or doc.get('api_name') != method):
                continue
            parameters = doc.get('parameters')
            if isinstance(parameters, list) and any(
                    isinstance(parameter, dict) and parameter.get('name') == 'access_token'
                    and parameter.get('required') is True for parameter in parameters):
                pairs.append([app, method])
    return sorted(pairs)


def public_saved_result_contracts(world):
    """Only public read-only APIs with collection return schemas qualify."""
    pairs = []
    for app, methods in world.task.api_docs.items():
        if not isinstance(app, str) or app in ('api_docs', 'supervisor') or not isinstance(methods, dict):
            continue
        for method, doc in methods.items():
            if (not isinstance(method, str) or not isinstance(doc, dict)
                    or doc.get('app_name') != app or doc.get('api_name') != method
                    or doc.get('method') != 'GET'
                    or not isinstance(doc.get('response_schemas'), dict)
                    or re.search(r'login|logout|token|password|credential|secret|auth', method, re.I)
                    or re.match(r'(?:create|delete|update|move|add|remove|send|pay|play|like|follow|unfollow|download|clear|mark)_', method)):
                continue
            success = (doc.get('response_schemas') or {}).get('success')
            if isinstance(success, list):
                pairs.append([app, method])
    return sorted(pairs)


def tracker_metrics(tracker):
    """Read the original tracker; do not infer success from completion alone."""
    if hasattr(tracker, "to_dict"):
        raw = tracker.to_dict()
    elif hasattr(tracker, "model_dump"):
        raw = tracker.model_dump(mode="json")
    else:
        raw = {}
    passes = raw.get("passes", getattr(tracker, "passes", None))
    failures = raw.get("failures", getattr(tracker, "failures", None))
    if isinstance(passes, list) and isinstance(failures, list):
        passed, failed = len(passes), len(failures)
    else:
        passed = raw.get("num_passed", getattr(tracker, "num_passed", None))
        failed = raw.get("num_failed", getattr(tracker, "num_failed", None))
    total = raw.get("num_tests", getattr(tracker, "num_tests", None))
    success = raw.get("success", getattr(tracker, "success", None))
    if (not isinstance(passed, int) or not isinstance(failed, int)
            or not isinstance(total, int) or total <= 0 or not isinstance(success, bool)):
        raise RuntimeError("appworld_tracker_schema_unknown")
    return {"appworld_reward": float(success), "strict_passed": int(success),
            "quality_score": passed / total, "official_tests_total": total,
            "official_tests_passed": passed, "official_tests_failed": failed}


def synthetic_training_spec(task, train_path=None):
    """Admit only canonical synthetic tasks sourced from public official train."""
    initial = task.get('initial_state') or {}
    goal = initial.get('training_goal')
    if goal is None:
        return None
    canonical_path = ROOT / 'tools/trajectory_evolution/experiments/appworld/datasets/train.jsonl'
    pending_path = os.environ.get('APPWORLD_SYNTHETIC_TRAIN_PACK') if train_path is None else None
    path = train_path or pending_path or canonical_path
    rows = [json.loads(line) for line in Path(path).read_text().splitlines() if line.strip()]
    source = initial.get('source_task_id')
    if pending_path:
        official_rows = [json.loads(line) for line in canonical_path.read_text().splitlines() if line.strip()]
        source_rows = [row for row in official_rows
                       if row.get('initial_state', {}).get('source_task_id') == source
                       and not row.get('initial_state', {}).get('training_goal')]
        original_initial = {key: value for key, value in initial.items()
                            if key not in ('training_instruction', 'training_goal')}
        if len(source_rows) != 1 or original_initial != source_rows[0]['initial_state']:
            raise RuntimeError('appworld_synthetic_training_public_source_invalid')
    else:
        official_rows = rows
    official = {row['initial_state']['source_task_id'] for row in rows
                if not row.get('initial_state', {}).get('training_goal')}
    if pending_path:
        official = {row['initial_state']['source_task_id'] for row in official_rows
                    if not row.get('initial_state', {}).get('training_goal')}
    # Native entry deliberately omits split and task_id. Bind the delivered
    # initial state to one canonical public-training exercise instead.
    canonical = [row for row in rows if row.get('initial_state') == initial]
    public_goal = goal in ({'kind': 'cart_all_to_wishlist'},
                          {'kind': 'archive_read_nonstarred_inbox'})
    if isinstance(goal, dict) and goal.get('kind') == 'offline_playlist_downloaded_duration':
        seconds = goal.get('min_duration_seconds')
        public_goal = (set(goal) == {'kind', 'min_duration_seconds', 'require_no_all_downloaded_match'}
                       and isinstance(seconds, int) and not isinstance(seconds, bool)
                       and 0 < seconds <= 86400
                       and goal.get('require_no_all_downloaded_match') is True)
    if isinstance(goal, dict) and goal.get('kind') == 'cart_selected_rating_to_wishlist':
        threshold = goal.get('rating_below')
        public_goal = (set(goal) == {'kind', 'rating_below'}
                       and isinstance(threshold, (int, float)) and not isinstance(threshold, bool)
                       and 0 < threshold <= 5)
    if isinstance(goal, dict) and goal.get('kind') == 'order_selected_wishlist_preserve_cart':
        phrase = goal.get('product_name_contains')
        public_goal = (set(goal) == {'kind', 'product_name_contains'} and isinstance(phrase, str)
                       and 1 <= len(phrase) <= 80 and phrase == phrase.strip().lower()
                       and re.fullmatch(r'[a-z0-9]+(?: [a-z0-9]+)*', phrase) is not None)
    if isinstance(goal, dict) and goal.get('kind') == 'cross_app_playlist_edit':
        public_goal = (set(goal) == {'kind', 'require_all_recommendations'}
                       and goal.get('require_all_recommendations') is True)
    if isinstance(goal, dict) and goal.get('kind') == 'spotify_artist_aggregate':
        order, limit = goal.get('order'), goal.get('limit')
        public_goal = (set(goal) == {'kind', 'order', 'limit', 'deduplicate_songs'}
                       and order in {'most', 'least'} and isinstance(limit, int)
                       and not isinstance(limit, bool) and 1 <= limit <= 20
                       and goal.get('deduplicate_songs') is True)
    if isinstance(goal, dict) and goal.get('kind') == 'spotify_library_unique_artist_histogram':
        public_goal = (set(goal) == {'kind', 'min_playlists'}
                       and type(goal.get('min_playlists')) is int and goal['min_playlists'] == 6)
    if isinstance(goal, dict) and goal.get('kind') == 'cross_app_auth_recovery_summary':
        public_goal = (set(goal) == {'kind', 'require_actual_401'}
                       and goal.get('require_actual_401') is True)
    if isinstance(goal, dict) and goal.get('kind') == 'amazon_collection_quantity_summary':
        collections, threshold = goal.get('collections'), goal.get('rating_at_least')
        keys = {'kind', 'collections', 'rating_at_least'}
        reproduction = goal.get('reproduce_container_error')
        allowed_keys = (set(goal) == keys or
                        set(goal) == keys | {'reproduce_container_error'} and
                        isinstance(reproduction, str) and reproduction in {'cart', 'wish_list'})
        public_goal = (allowed_keys
                       and isinstance(collections, list) and 1 <= len(collections) <= 2
                       and all(isinstance(value, str) and value in {'cart', 'wish_list'} for value in collections)
                       and len(set(collections)) == len(collections)
                       and (reproduction is None or reproduction in collections)
                       and isinstance(threshold, (int, float)) and not isinstance(threshold, bool)
                       and 0 <= threshold <= 5)
    if isinstance(goal, dict) and goal.get('kind') == 'offline_album_downloaded_duration':
        seconds = goal.get('min_duration_seconds')
        public_goal = (set(goal) == {'kind', 'min_duration_seconds'}
                       and isinstance(seconds, int) and not isinstance(seconds, bool)
                       and 0 < seconds <= 86400)
    if isinstance(goal, dict) and goal.get('kind') == 'spotify_genre_artist_set':
        genres = goal.get('genres')
        public_goal = (set(goal) == {'kind', 'genres'} and isinstance(genres, list)
                       and 1 <= len(genres) <= 10
                       and all(isinstance(value, str) and re.fullmatch(r'[a-z]+(?:[ -][a-z]+)*', value)
                               for value in genres) and len(set(genres)) == len(genres))
    if isinstance(goal, dict) and goal.get('kind') == 'venmo_date_direction_total':
        days = goal.get('days')
        public_goal = (set(goal) == {'kind', 'collection', 'direction', 'days'}
                       and goal.get('collection') in {'transactions', 'payment_requests'}
                       and goal.get('direction') in {'sent', 'received'}
                       and isinstance(days, int) and not isinstance(days, bool) and 1 <= days <= 365)
    if (task.get('split') not in (None, 'train') or source not in official or len(canonical) != 1
            or canonical[0].get('split') != 'train'
            or task.get('task_id') not in (None, canonical[0]['task_id'])
            or not isinstance(initial.get('training_instruction'), str)
            or not initial['training_instruction'].strip()
            or not public_goal):
        raise RuntimeError('appworld_synthetic_training_scope_invalid')
    task.setdefault('task_id', canonical[0]['task_id'])
    return goal


def _synthetic_login(world, app):
    """Log in to one public app for synthetic training fixtures."""
    passwords = {row['account_name']: row['password']
                 for row in world.apis.supervisor.show_account_passwords()}
    supervisor = world.task.supervisor
    username = supervisor.phone_number if app == 'phone' else supervisor.email
    return getattr(world.apis, app).login(username=username,
                                          password=passwords[app])['access_token']


def _synthetic_spotify_playlists(world, token):
    playlists = {}
    for page_index in range(1000):
        page = world.apis.spotify.show_playlist_library(
            page_index=page_index, page_limit=20, access_token=token)
        if not page:
            return playlists
        for row in page:
            key = row['playlist_id']
            if key in playlists:
                raise RuntimeError('appworld_synthetic_training_pagination_duplicate')
            playlists[key] = {'title': row['title'],
                              'song_ids': list(row.get('song_ids') or [])}
    raise RuntimeError('appworld_synthetic_training_pagination_incomplete')


def _synthetic_public_pages(api, token, id_field):
    result = {}
    for page_index in range(1000):
        page = api(access_token=token, page_index=page_index, page_limit=20)
        if not page:
            return result
        for row in page:
            key = row[id_field]
            if key in result:
                raise RuntimeError('appworld_synthetic_training_pagination_duplicate')
            result[key] = row
    raise RuntimeError('appworld_synthetic_training_pagination_incomplete')


def _prepare_cross_app_playlist_fixture(world):
    """Seed one explicit public Phone recommendation for a synthetic TRAIN row.

    The canonical source task is official TRAIN data but its original phone
    conversation is unrelated to playlist editing.  A deterministic message
    gives the training exercise a real cross-app contract without touching
    validation or holdout tasks.
    """
    spotify_token = _synthetic_login(world, 'spotify')
    playlists = _synthetic_spotify_playlists(world, spotify_token)
    target_id, target = next(((key, value) for key, value in playlists.items()
                              if value['song_ids']), (None, None))
    if target is None:
        raise RuntimeError('appworld_synthetic_training_playlist_fixture_empty')
    remove_id = target['song_ids'][0]
    catalogue = world.apis.spotify.search_songs(query='', page_index=0, page_limit=20)
    add_id = next((row['song_id'] for row in catalogue if row['song_id'] not in target['song_ids']), None)
    if add_id is None:
        raise RuntimeError('appworld_synthetic_training_playlist_add_fixture_empty')
    phone_token = _synthetic_login(world, 'phone')
    messages = []
    for page_index in range(1000):
        page = world.apis.phone.search_text_messages(
            query='', only_latest_per_contact=False, page_index=page_index,
            page_limit=20, access_token=phone_token)
        if not page:
            break
        messages.extend(page)
    supervisor_phone = world.task.supervisor.phone_number
    contact = next((item.get('sender', {}).get('phone_number') for item in messages
                    if item.get('sender', {}).get('phone_number') not in (None, supervisor_phone)), None)
    if contact is None:
        contact = next((item.get('receiver', {}).get('phone_number') for item in messages
                        if item.get('receiver', {}).get('phone_number') not in (None, supervisor_phone)), None)
    if contact is None:
        raise RuntimeError('appworld_synthetic_training_phone_contact_fixture_empty')
    message = (f'[AppWorld training playlist update] playlist_id={target_id} '
               f'add_song_id={add_id} remove_song_id={remove_id}')
    world.apis.phone.send_text_message(phone_number=contact, message=message,
                                       access_token=phone_token)


def synthetic_training_snapshot(world, goal):
    """Read the exercise state only through its public API contract."""
    kind = goal['kind']
    if kind in {'spotify_library_unique_artist_histogram', 'cross_app_auth_recovery_summary'}:
        token = _synthetic_login(world, 'spotify')
        playlists = _synthetic_spotify_playlists(world, token)
        if kind == 'spotify_library_unique_artist_histogram':
            if len(playlists) < goal['min_playlists']:
                raise RuntimeError('appworld_synthetic_training_pagination_capacity_insufficient')
            song_ids = {sid for playlist in playlists.values() for sid in playlist['song_ids']}
            songs = {sid: world.apis.spotify.show_song(song_id=sid) for sid in sorted(song_ids)}
            return {'playlists': playlists, 'songs': songs}
        phone_token = _synthetic_login(world, 'phone')
        messages = _synthetic_public_pages(lambda **kwargs: world.apis.phone.search_text_messages(
            query='', only_latest_per_contact=False, **kwargs), phone_token, 'text_message_id')
        relationships = world.apis.phone.show_contact_relationships(access_token=phone_token)
        return {'playlists': playlists, 'messages': messages, 'relationships': relationships}
    if kind == 'offline_album_downloaded_duration':
        token = _synthetic_login(world, 'spotify')
        albums = _synthetic_public_pages(world.apis.spotify.show_album_library, token, 'album_id')
        downloaded = _synthetic_public_pages(world.apis.spotify.show_downloaded_songs, token, 'song_id')
        return {'albums': {key: list(row['song_ids']) for key, row in albums.items()},
                'downloaded': {key: row['duration'] for key, row in downloaded.items()},
                'queue': [{key: row[key] for key in ('song_id', 'is_current', 'is_playing')}
                          for row in world.apis.spotify.show_song_queue(access_token=token)]}
    if kind == 'spotify_genre_artist_set':
        token = _synthetic_login(world, 'spotify')
        playlists = _synthetic_spotify_playlists(world, token)
        songs = {song_id: world.apis.spotify.show_song(song_id=song_id)
                 for song_id in sorted({song_id for row in playlists.values() for song_id in row['song_ids']})}
        following = _synthetic_public_pages(world.apis.spotify.show_following_artists, token, 'artist_id')
        return {'playlists': playlists, 'songs': songs, 'following': set(following)}
    if kind == 'venmo_date_direction_total':
        token = _synthetic_login(world, 'venmo')
        api = (world.apis.venmo.show_transactions if goal['collection'] == 'transactions' else
               world.apis.venmo.show_received_payment_requests if goal['direction'] == 'received' else
               world.apis.venmo.show_sent_payment_requests)
        id_field = 'transaction_id' if goal['collection'] == 'transactions' else 'payment_request_id'
        return {'rows': _synthetic_public_pages(api, token, id_field),
                'today': str(world.task.datetime)[:10], 'email': world.task.supervisor.email}
    if kind == 'cross_app_playlist_edit':
        phone_token = _synthetic_login(world, 'phone')
        messages = []
        for page_index in range(1000):
            page = world.apis.phone.search_text_messages(
                query='', only_latest_per_contact=False, page_index=page_index,
                page_limit=20, access_token=phone_token)
            if not page:
                break
            messages.extend(page)
        else:
            raise RuntimeError('appworld_synthetic_training_pagination_incomplete')
        spotify_token = _synthetic_login(world, 'spotify')
        playlists = _synthetic_spotify_playlists(world, spotify_token)
        recommendations = []
        pattern = re.compile(
            r'\[AppWorld training playlist update\]\s+playlist_id=(\d+)\s+'
            r'add_song_id=(\d+)\s+remove_song_id=(\d+)')
        for row in messages:
            match = pattern.search(str(row.get('message', '')))
            if match:
                recommendations.append({'playlist_id': int(match.group(1)),
                                        'add_song_id': int(match.group(2)),
                                        'remove_song_id': int(match.group(3))})
        return {'messages': len(messages), 'playlists': playlists,
                'recommendations': recommendations}
    app = ('amazon' if kind in ('cart_all_to_wishlist', 'cart_selected_rating_to_wishlist', 'order_selected_wishlist_preserve_cart', 'amazon_collection_quantity_summary') else
           'spotify' if kind in ('offline_playlist_downloaded_duration', 'spotify_artist_aggregate') else 'gmail')
    passwords = {row['account_name']: row['password']
                 for row in world.apis.supervisor.show_account_passwords()}
    token = getattr(world.apis, app).login(username=world.task.supervisor.email,
                                         password=passwords[app])['access_token']
    if app == 'amazon':
        cart = world.apis.amazon.show_cart(access_token=token)['cart_items']
        wish = world.apis.amazon.show_wish_list(access_token=token)
        result = {'cart': {row['product_id']: row['quantity'] for row in cart},
                  'wish': {row['product_id']: row['quantity'] for row in wish}}
        if kind == 'amazon_collection_quantity_summary':
            product_ids = {product_id for collection in goal['collections']
                           for product_id in result['wish' if collection == 'wish_list' else 'cart']}
            result['product_ratings'] = {product_id: world.apis.amazon.show_product(
                product_id=product_id)['rating'] for product_id in sorted(product_ids)}
        if kind == 'cart_selected_rating_to_wishlist':
            result['cart_ratings'] = {row['product_id']: world.apis.amazon.show_product(
                product_id=row['product_id'])['rating'] for row in cart}
        if kind == 'order_selected_wishlist_preserve_cart':
            result['wish_names'] = {row['product_id']: row['product_name'].lower() for row in wish}
            orders = {}
            for page_index in range(1000):
                page = world.apis.amazon.show_orders(access_token=token, page_index=page_index, page_limit=20)
                if not page:
                    break
                for order in page:
                    key = order['order_id']
                    if key in orders:
                        raise RuntimeError('appworld_synthetic_training_pagination_duplicate')
                    items = order['order_items']
                    if len({item['product_id'] for item in items}) != len(items):
                        raise RuntimeError('appworld_synthetic_training_order_duplicate_product')
                    orders[key] = {item['product_id']: {'ordered_quantity': item['ordered_quantity'],
                        'returned_quantity': item['returned_quantity']} for item in items}
            else:
                raise RuntimeError('appworld_synthetic_training_pagination_incomplete')
            result['orders'] = orders
        return result
    if app == 'spotify':
        def pages(api, id_field):
            result = {}
            for page_index in range(1000):
                page = api(access_token=token, page_index=page_index, page_limit=20)
                if not page:
                    return result
                for row in page:
                    key = row[id_field]
                    if key in result:
                        raise RuntimeError('appworld_synthetic_training_pagination_duplicate')
                    result[key] = row
            raise RuntimeError('appworld_synthetic_training_pagination_incomplete')
        library = pages(world.apis.spotify.show_playlist_library, 'playlist_id')
        if kind == 'spotify_artist_aggregate':
            songs = {}
            for playlist in library.values():
                for song_id in playlist.get('song_ids', []):
                    if song_id not in songs:
                        songs[song_id] = world.apis.spotify.show_song(song_id=song_id)
            return {'playlists': {key: list(row['song_ids']) for key, row in library.items()},
                    'songs': songs}
        downloaded = pages(world.apis.spotify.show_downloaded_songs, 'song_id')
        return {'playlists': {key: list(row['song_ids']) for key, row in library.items()},
                'downloaded': {key: row['duration'] for key, row in downloaded.items()},
                'queue': [{key: row[key] for key in ('song_id', 'is_current', 'is_playing')}
                          for row in world.apis.spotify.show_song_queue(access_token=token)]}
    threads = {}
    for archived in (False, True):
        for page_index in range(1000):
            page = world.apis.gmail.show_inbox_threads(access_token=token,
                archived=archived, spam=False, page_index=page_index, page_limit=20)
            if not page:
                break
            for row in page:
                key = row['email_thread_id']
                if key in threads:
                    raise RuntimeError('appworld_synthetic_training_pagination_duplicate')
                threads[key] = {name: row[name] for name in
                    ('read', 'starred', 'archived', 'spam', 'email_ids')}
        else:
            raise RuntimeError('appworld_synthetic_training_pagination_incomplete')
    return {'threads': threads}


def synthetic_training_metrics(world, goal, before, execution_outcomes=None):
    """Evaluate this task's own public postconditions, never original hidden tests."""
    after = synthetic_training_snapshot(world, goal)
    checks = []
    if goal['kind'] in {'spotify_library_unique_artist_histogram', 'cross_app_auth_recovery_summary'}:
        raw = world.apis.supervisor.show_active_task().get('answer')
        try:
            answer = json.loads(raw) if isinstance(raw, str) else raw
        except (ValueError, TypeError):
            answer = None
        def same_public_counts(actual, expected):
            if type(actual) is not type(expected):
                return False
            if isinstance(expected, dict):
                return actual.keys() == expected.keys() and all(
                    same_public_counts(actual[key], value) for key, value in expected.items())
            return actual == expected
        if goal['kind'] == 'spotify_library_unique_artist_histogram':
            histogram = {}
            # Each globally unique song contributes once to each credited name.
            for song in before['songs'].values():
                for name in {artist['name'] for artist in song.get('artists', [])}:
                    histogram[name] = histogram.get(name, 0) + 1
            expected = {'playlist_count': len(before['playlists']),
                        'distinct_song_count': len(before['songs']),
                        'artist_distinct_song_counts': histogram}
            checks = [('complete_public_global_song_artist_histogram', same_public_counts(answer, expected)),
                      ('playlist_and_song_state_preserved', after == before)]
        else:
            expected = {'phone_message_count': len(before['messages']),
                        'phone_relationship_count': len(before['relationships']),
                        'spotify_playlist_count': len(before['playlists'])}
            checks = [('complete_public_cross_app_summary', same_public_counts(answer, expected)),
                      ('phone_and_spotify_state_preserved', after == before)]
            outcomes = execution_outcomes or []
            failures = []
            for index, outcome in enumerate(outcomes):
                if (outcome.get('failed') is not True or
                        re.search(r'(?<![0-9])401(?![0-9])', outcome.get('observation', '')) is None):
                    continue
                try:
                    tree = ast.parse(outcome.get('code', ''))
                except (ValueError, TypeError, SyntaxError):
                    continue
                for node in ast.walk(tree):
                    if (isinstance(node, ast.Call) and ast.unparse(node.func) ==
                            'apis.phone.show_contact_relationships' and
                            any(kw.arg == 'access_token' and isinstance(kw.value, ast.Call)
                                and isinstance(kw.value.func, ast.Name) and kw.value.func.id == 'str'
                                and len(kw.value.args) == 1 and not kw.value.keywords
                                and isinstance(kw.value.args[0], ast.Constant)
                                and type(kw.value.args[0].value) is int and kw.value.args[0].value == 0
                                for kw in node.keywords)):
                        failures.append(index)
                        break
            documented = logged_in = repaired = False
            if len(failures) == 1:
                for outcome in outcomes[failures[0] + 1:]:
                    if outcome.get('failed') is not False:
                        continue
                    try:
                        tree = ast.parse(outcome.get('code', ''))
                    except (ValueError, TypeError, SyntaxError):
                        continue
                    calls = [node for node in ast.walk(tree) if isinstance(node, ast.Call)]
                    docs = {tuple(sorted((kw.arg, kw.value.value) for kw in node.keywords
                                 if isinstance(kw.value, ast.Constant))) for node in calls
                            if ast.unparse(node.func) == 'apis.api_docs.show_api_doc'}
                    required_docs = {(('api_name', 'login'), ('app_name', 'phone')),
                                     (('api_name', 'show_contact_relationships'), ('app_name', 'phone'))}
                    if required_docs <= docs and not any(
                            ast.unparse(node.func).startswith('apis.phone.') for node in calls):
                        documented = True
                        continue
                    if documented and any(ast.unparse(node.func) == 'apis.phone.login' and
                            {'username', 'password'} <= {kw.arg for kw in node.keywords} for node in calls):
                        logged_in = True
                    if logged_in and any(ast.unparse(node.func) == 'apis.phone.show_contact_relationships'
                            and 'access_token' in {kw.arg for kw in node.keywords} for node in calls):
                        repaired = True
            checks.append(('one_actual_phone_401_then_documented_login_and_successful_read', repaired))
        failures = [name for name, passed in checks if not passed]
        passed = len(checks) - len(failures)
        return {'appworld_reward': float(not failures), 'strict_passed': int(not failures),
                'quality_score': passed / len(checks), 'official_tests_total': len(checks),
                'official_tests_passed': passed, 'official_tests_failed': len(failures),
                'reward_source': 'public_synthetic_training_goal',
                'training_feedback': {'source': 'public_synthetic_training_goal', 'failures': failures}}
    if goal['kind'] == 'amazon_collection_quantity_summary':
        expected = {}
        for collection in goal['collections']:
            quantities = before['wish' if collection == 'wish_list' else 'cart']
            selected = {key: quantity for key, quantity in quantities.items()
                        if before['product_ratings'][key] >= goal['rating_at_least']}
            expected[collection] = {'product_count': len(selected), 'unit_count': sum(selected.values())}
        raw = world.apis.supervisor.show_active_task().get('answer')
        try:
            answer = json.loads(raw) if isinstance(raw, str) else None
            typed = (isinstance(answer, dict) and set(answer) == set(expected)
                     and all(isinstance(value, dict) and set(value) == {'product_count', 'unit_count'}
                             and all(isinstance(count, int) and not isinstance(count, bool) and count >= 0
                                     for count in value.values()) for value in answer.values()))
            correct = typed and answer == expected
        except (ValueError, TypeError):
            correct = False
        checks = [('public_product_and_unit_summary', correct),
                  ('original_cart_wish_list_and_product_ratings_preserved', after == before)]
        if goal.get('reproduce_container_error'):
            method = ('show_wish_list' if goal['reproduce_container_error'] == 'wish_list' else 'show_cart')
            outcomes = execution_outcomes or []
            reproductions = [index for index, outcome in enumerate(outcomes)
                             if outcome.get('failed') is True
                             and re.search(r'\bTypeError: (?:list|string) indices must be integers',
                                           outcome.get('observation', ''))
                             and any(app == 'amazon' and api == method
                                     for app, api, _ in _appworld_api_calls(outcome.get('code', '')))]
            checks.append(('exactly_one_actual_container_failure_reproduced', len(reproductions) == 1))
            documented, repaired = False, False
            for index, outcome in enumerate(outcomes):
                if len(reproductions) != 1 or index <= reproductions[0] or outcome.get('failed') is not False:
                    continue
                calls = _appworld_api_calls(outcome.get('code', ''))
                reads_collection = any(app == 'amazon' and api == method for app, api, _ in calls)
                try:
                    tree = ast.parse(outcome.get('code', ''))
                except (ValueError, TypeError, SyntaxError):
                    continue
                exact_doc = any(isinstance(node, ast.Call) and isinstance(node.func, ast.Attribute)
                    and node.func.attr == 'show_api_doc' and isinstance(node.func.value, ast.Attribute)
                    and node.func.value.attr == 'api_docs' and isinstance(node.func.value.value, ast.Name)
                    and node.func.value.value.id == 'apis'
                    and {kw.arg: kw.value.value for kw in node.keywords if isinstance(kw.value, ast.Constant)}
                        == {'app_name': 'amazon', 'api_name': method} for node in ast.walk(tree))
                if exact_doc and not reads_collection:
                    documented = True
                elif reads_collection and documented:
                    repaired = True
            checks.append(('exact_public_schema_inspected_after_failure_before_successful_read', repaired))
    elif goal['kind'] == 'offline_album_downloaded_duration':
        eligible = []
        downloaded = before['downloaded']
        for songs in before['albums'].values():
            present = set(songs) & set(downloaded)
            if (len(songs) == len(set(songs)) and 0 < len(present) < len(songs)
                    and sum(downloaded[key] for key in present) >= goal['min_duration_seconds']):
                eligible.append(songs)
        if not eligible:
            raise RuntimeError('appworld_synthetic_training_offline_album_fixture_empty')
        queue = after['queue']
        queued = [row['song_id'] for row in queue]
        checks = [('playing_album_with_sufficient_partial_downloaded_duration',
                   any(row['is_current'] and row['is_playing'] for row in queue)
                   and any(len(queued) == len(songs) and set(queued) == set(songs) for songs in eligible)),
                  ('album_library_and_downloads_preserved',
                   after['albums'] == before['albums'] and after['downloaded'] == downloaded)]
    elif goal['kind'] == 'spotify_genre_artist_set':
        genres = set(goal['genres'])
        targets = {artist['id'] for song in before['songs'].values()
                   if str(song['genre']).strip().casefold() in genres for artist in song['artists']}
        if not targets or not targets - before['following']:
            raise RuntimeError('appworld_synthetic_training_genre_artist_fixture_empty')
        checks = [('all_and_only_matching_song_artists_followed',
                   after['following'] == before['following'] | targets),
                  ('playlist_and_song_state_preserved', after['playlists'] == before['playlists']
                   and after['songs'] == before['songs'])]
    elif goal['kind'] == 'venmo_date_direction_total':
        from datetime import date, timedelta
        from decimal import Decimal, InvalidOperation
        today = date.fromisoformat(before['today'])
        first = today - timedelta(days=goal['days'] - 1)
        email = before['email']
        matches = [row for row in before['rows'].values()
                   if first <= date.fromisoformat(row['created_at'][:10]) <= today
                   and row['receiver' if goal['direction'] == 'received' else 'sender']['email'] == email]
        if not matches:
            raise RuntimeError('appworld_synthetic_training_date_direction_fixture_empty')
        total = sum((Decimal(str(row['amount'])) for row in matches), Decimal(0))
        raw = world.apis.supervisor.show_active_task().get('answer')
        try:
            answer = Decimal(str(raw).strip())
            correct = answer.is_finite() and answer == total
        except (InvalidOperation, ValueError, TypeError):
            correct = False
        checks = [('inclusive_calendar_window_direction_and_collection_total', correct),
                  ('venmo_collection_preserved', after == before)]
    elif goal['kind'] == 'cross_app_playlist_edit':
        recommendations = before['recommendations']
        if len(recommendations) != 1:
            raise RuntimeError('appworld_synthetic_training_playlist_recommendation_fixture_empty')
        recommendation = recommendations[0]
        playlist_id = recommendation['playlist_id']
        original = before['playlists'].get(playlist_id)
        if original is None:
            raise RuntimeError('appworld_synthetic_training_playlist_fixture_missing')
        expected_playlists = {key: dict(value) for key, value in before['playlists'].items()}
        expected_song_ids = list(original['song_ids'])
        remove_id, add_id = recommendation['remove_song_id'], recommendation['add_song_id']
        if remove_id not in expected_song_ids or add_id in expected_song_ids:
            raise RuntimeError('appworld_synthetic_training_playlist_recommendation_invalid')
        expected_song_ids.remove(remove_id)
        expected_song_ids.append(add_id)
        expected_playlists[playlist_id] = {**original, 'song_ids': expected_song_ids}
        checks = [('all_phone_recommendations_applied_exactly_once',
                   after['playlists'] == expected_playlists),
                  ('unrelated_playlists_preserved',
                   all(after['playlists'].get(key) == value
                       for key, value in before['playlists'].items() if key != playlist_id))]
    elif goal['kind'] == 'spotify_artist_aggregate':
        song_to_artists = {key: [artist['name'] for artist in value.get('artists', [])]
                           for key, value in before['songs'].items()}
        counts = {}
        for song_ids in before['playlists'].values():
            for song_id in set(song_ids):
                artists = song_to_artists.get(song_id)
                if artists is None:
                    raise RuntimeError('appworld_synthetic_training_artist_fixture_incomplete')
                for artist in artists:
                    counts[artist] = counts.get(artist, 0) + 1
        key = (lambda item: (-item[1], item[0].casefold())) if goal['order'] == 'most' else \
              (lambda item: (item[1], item[0].casefold()))
        expected = [name for name, _ in sorted(counts.items(), key=key)[:goal['limit']]]
        raw_answer = world.apis.supervisor.show_active_task().get('answer')
        try:
            answer = json.loads(raw_answer) if isinstance(raw_answer, str) else raw_answer
        except (TypeError, ValueError):
            answer = raw_answer
        if isinstance(answer, list):
            actual = [str(item).strip() for item in answer]
        elif isinstance(answer, str):
            actual = [item.strip() for item in answer.split(',') if item.strip()]
        else:
            actual = []
        checks = [('artist_answer_matches_public_deduplicated_aggregate',
                   [item.casefold() for item in actual] == [item.casefold() for item in expected]),
                  ('playlist_library_preserved', after['playlists'] == before['playlists'])]
    elif goal['kind'] == 'cart_all_to_wishlist':
        if not before['cart'] or not any(quantity > 1 for quantity in before['cart'].values()):
            raise RuntimeError('appworld_synthetic_training_quantity_fixture_empty')
        expected = dict(before['wish'])
        for product_id, quantity in before['cart'].items():
            expected[product_id] = expected.get(product_id, 0) + quantity
        checks = [('cart_empty', not after['cart']),
                  ('wishlist_preserves_full_quantities', after['wish'] == expected)]
    elif goal['kind'] == 'cart_selected_rating_to_wishlist':
        targets = {key: quantity for key, quantity in before['cart'].items()
                   if before['cart_ratings'][key] < goal['rating_below']}
        if (not targets or len(targets) == len(before['cart'])
                or not any(quantity > 1 for quantity in targets.values())):
            raise RuntimeError('appworld_synthetic_training_selective_quantity_fixture_empty')
        expected_wish = dict(before['wish'])
        for key, quantity in targets.items():
            expected_wish[key] = expected_wish.get(key, 0) + quantity
        remaining = {key: quantity for key, quantity in before['cart'].items() if key not in targets}
        checks = [('all_target_units_removed_and_other_cart_quantities_preserved', after['cart'] == remaining),
                  ('wishlist_gained_full_target_quantities_and_preserved_original', after['wish'] == expected_wish)]
    elif goal['kind'] == 'order_selected_wishlist_preserve_cart':
        targets = {key: quantity for key, quantity in before['wish'].items()
                   if goal['product_name_contains'] in before['wish_names'][key]}
        if (not before['cart'] or not targets or set(targets) & set(before['cart'])
                or not any(quantity > 1 for quantity in targets.values())):
            raise RuntimeError('appworld_synthetic_training_selective_order_fixture_empty')
        remaining = {key: quantity for key, quantity in before['wish'].items() if key not in targets}
        new_orders = [row for key, row in after['orders'].items() if key not in before['orders']]
        exact = (len(new_orders) == 1 and new_orders[0] == {
            key: {'ordered_quantity': quantity, 'returned_quantity': 0} for key, quantity in targets.items()})
        checks = [('one_order_contains_only_all_target_units', exact),
                  ('original_cart_products_and_quantities_preserved', after['cart'] == before['cart']),
                  ('wishlist_targets_removed_and_other_quantities_preserved', after['wish'] == remaining),
                  ('original_orders_preserved', all(after['orders'].get(key) == row
                                                    for key, row in before['orders'].items()))]
    elif goal['kind'] == 'offline_playlist_downloaded_duration':
        downloaded = before['downloaded']
        minimum = goal['min_duration_seconds']
        eligible = []
        all_downloaded_match = False
        for songs in before['playlists'].values():
            present = [song for song in songs if song in downloaded]
            duration = sum(downloaded[song] for song in present)
            if songs and len(present) == len(songs) and duration >= minimum:
                all_downloaded_match = True
            if (len(songs) == len(set(songs)) and 0 < len(present) < len(songs)
                    and duration >= minimum):
                eligible.append(songs)
        if not eligible or (goal['require_no_all_downloaded_match'] and all_downloaded_match):
            raise RuntimeError('appworld_synthetic_training_offline_fixture_empty')
        queue = after['queue']
        queued = [row['song_id'] for row in queue]
        playing = any(row['is_current'] and row['is_playing'] for row in queue)
        checks = [('playing_eligible_library_playlist', playing and any(
                       len(queued) == len(songs) and set(queued) == set(songs) for songs in eligible)),
                  ('library_and_original_downloads_preserved',
                   after['playlists'] == before['playlists'] and after['downloaded'] == downloaded)]
    else:
        initial = before['threads']
        targets = {key for key, row in initial.items()
                   if row['read'] and not row['starred'] and not row['archived']}
        if not targets:
            raise RuntimeError('appworld_synthetic_training_archive_fixture_empty')
        expected = {key: {**row, 'archived': True} if key in targets else row
                    for key, row in initial.items()}
        checks = [('all_targets_archived', all(after['threads'].get(key, {}).get('archived') is True
                                               for key in targets)),
                  ('all_other_thread_properties_preserved', after['threads'] == expected)]
    checks.append(('task_completed_success',
                   world.apis.supervisor.show_active_task()['status'] == 'success'))
    failures = [{'requirement': name, 'label': 'public_api_postcondition_not_met'}
                for name, passed in checks if not passed]
    passed = sum(ok for _, ok in checks)
    success = not failures
    return {'appworld_reward': float(success), 'strict_passed': int(success),
            'quality_score': passed / len(checks),
            'reward_source': 'public_synthetic_training_goal',
            'official_goal_evaluated': False,
            'synthetic_tests_total': len(checks), 'synthetic_tests_passed': passed,
            'synthetic_tests_failed': len(failures),
            'training_feedback': {'source': 'public_synthetic_training_goal', 'failures': failures}}


def install_synthetic_training_instruction(world, instruction):
    """Change only the public task instruction in this independent world copy."""
    tasks = world.models.supervisor.Task.all()
    if len(tasks) != 1:
        raise RuntimeError('appworld_synthetic_training_public_task_invalid')
    active = tasks[0]
    active.instruction = instruction
    active.save()
    world.task.instruction = instruction
    if world.apis.supervisor.show_active_task()['instruction'] != instruction:
        raise RuntimeError('appworld_synthetic_training_public_instruction_mismatch')


def reply_diagnostic(runner):
    """Preserve only structural failure evidence before private logs are removed."""
    models, stable_errors = [], set()
    budget_notice = False
    expected = getattr(runner.client, "_appworld_expected_dialogue", None)
    for path in (runner.root / "sessionlogs").rglob("*.jsonl"):
        for line in path.read_text(encoding="utf-8").splitlines():
            try:
                event = json.loads(line)
            except ValueError:
                continue
            data = event.get("data") or {}
            if not expected or expected not in [event.get("dialogue_id"),
                    (data.get("meta") or {}).get("dialogue_id")]:
                continue
            for code in re.findall(r"\bappworld_reply_[a-z0-9_]+\b", line):
                stable_errors.add(code)
            budget_notice |= any(signal in line.lower() for signal in
                ["sdk_call_budget_exceeded", "tool_call_budget_exceeded",
                 "tool execution exceeded remaining node budget"])
            if event.get("type") != "llm.response":
                continue
            response = data.get("provider_response") or data.get("response") or {}
            choices = response.get("choices") or []
            message = choices[0].get("message", {}) if choices else {}
            models.append({"timestamp_us": event.get("timestamp_us", 0),
                "skill": data.get("skill_name"),
                "finish_reason": choices[0].get("finish_reason") if choices else None,
                "http_status": (data.get("meta") or {}).get("http_status"),
                "tool_names": [call.get("function", {}).get("name")
                    for call in message.get("tool_calls", [])],
                "legacy_function_call": bool(message.get("function_call")),
                "has_text": bool(message.get("content"))})
    models.sort(key=lambda item: item.pop("timestamp_us", 0))
    state = json.loads(Path(runner.meta["state_path"]).read_text())
    module = Path(runner.meta["module_path"]).read_text()
    nonce = re.search(r"_REQUEST_ID = ['\"]([a-f0-9]+)['\"]", module)
    turn = re.search(r"_TURN = ([0-9]+)", module)
    return {"last_model_responses": models[-4:], "stable_reply_errors": sorted(stable_errors),
        "rpc": getattr(runner.client, "_appworld_rpc_diagnostic", {}),
        "state_nonce_matches_host": state.get("request_id") == runner.meta.get("request_id"),
        "module_nonce_matches_state": bool(nonce) and nonce[1] == state.get("request_id"),
        "module_turn_matches_state": bool(turn) and int(turn[1]) == state.get("turn"),
        "sdk_budget_stop_observed": budget_notice}


def natural_model_stop(diagnostic):
    """Only a proven current-turn normal provider stop reaches final grading."""
    models = diagnostic.get("last_model_responses") or []
    if not models:
        return False
    last = models[-1]
    return (diagnostic.get("rpc", {}).get("matched_terminal") is True
            and last.get("http_status") in (200, "200")
            and last.get("finish_reason") == "stop"
            and not last.get("tool_names") and not last.get("legacy_function_call")
            and not diagnostic.get("stable_reply_errors")
            and diagnostic.get("sdk_budget_stop_observed") is False
            and all(diagnostic.get(key) is True for key in
                ["state_nonce_matches_host", "module_nonce_matches_state", "module_turn_matches_state"]))


def _model_texts(runner):
    """Read current-dialogue provider text and terminal metadata for recovery."""
    expected = getattr(runner.client, "_appworld_expected_dialogue", None)
    found = []
    for path in (runner.root / "sessionlogs").rglob("*.jsonl"):
        for line in path.read_text(encoding="utf-8").splitlines():
            try:
                event = json.loads(line)
            except (TypeError, ValueError):
                continue
            data = event.get("data") or {}
            if event.get("type") != "llm.response" or (expected and expected not in
                    [event.get("dialogue_id"), (data.get("meta") or {}).get("dialogue_id")]):
                continue
            response = data.get("provider_response") or data.get("response") or {}
            choices = response.get("choices") or []
            message = choices[0].get("message", {}) if choices else {}
            content = message.get("content")
            if isinstance(content, str):
                found.append((event.get("timestamp_us", 0),
                              choices[0].get("finish_reason") if choices else None,
                              bool(message.get("tool_calls")),
                              bool(message.get("function_call")), content))
    return [item[1:] for item in sorted(found)]


def recover_single_fenced_code(runner, diagnostic):
    """Recover a lone executable fence from a proven normal provider stop.

    The recovered block is submitted through the same reply schema and then
    follows the ordinary action/policy path. Multiple blocks, malformed Python,
    parser errors or non-terminal responses are never recovered.
    """
    if not natural_model_stop(diagnostic):
        return None
    for finish_reason, has_tools, has_function, text in reversed(_model_texts(runner)):
        if finish_reason != "stop" or has_tools or has_function:
            continue
        blocks = re.findall(r"```(?:python|py)?[ \t]*\r?\n?(.*?)```", text,
                            flags=re.IGNORECASE | re.DOTALL)
        if len(blocks) != 1:
            continue
        code = blocks[0].strip()
        if not code or len(code) > 65536:
            continue
        try:
            ast.parse(code)
        except (SyntaxError, ValueError, TypeError):
            continue
        submit_recovered_code(runner.meta, code)
        return code
    return None


def loop_model_policy(policy, loop):
    """Consume the proposed control in the actual next model request.

    Native L3 verifies the replan receipt against this request, rather than
    counting a configured rule or a proposed decision as an application.
    """
    decision = loop.get("decision", "continue")
    if decision in {"stop", "deny", "skip", "reject"}:
        return "stop", policy
    if isinstance(loop.get("input"), dict) and loop["input"].get("continue") is False:
        return "stop", policy
    context = loop.get("replan_context")
    if decision == "replan" and isinstance(context, str) and context.strip():
        if len(context.encode("utf-8")) > 2048:
            raise RuntimeError("policy_loop_context_invalid")
        return "replan", policy + "\n\n" + context
    return "continue", policy


class AppWorldCaptureBoundaryError(RuntimeError):
    """Offline native capture cannot proceed without an observed lifecycle."""


class Trial:
    def __init__(self, task):
        self.task = task
        trial_url = os.environ.get("TRAJECTORY_EVOLUTION_TRIAL_URL", "")
        self.evo = Evolution(trial_url, os.environ.get("TRAJECTORY_EVOLUTION_TRIAL_TOKEN", ""),
                             timeout_ms=5000, retries=0) if trial_url else None
        self.run_context = None
        self.boundary_diagnostics = []
        self.calls = self.covered_calls = self.known_tokens = 0
        self.tokens_complete = True
        self.applied = {"L1": 0, "L2": 0, "L3": 0}
        self.degraded_points = 0
        self.framework_capture = {"calls": 0, "transport_attempts": 0, "failures": 0}
        # The nested runtime persists these counters for the whole task. Keep
        # the last snapshot so each outer turn compares only its increment;
        # treating the cumulative value as a per-turn count falsely marks all
        # turns after the first as incomplete.
        self._framework_snapshot = {"calls": 0, "transport_attempts": 0, "failures": 0}
        # The trial endpoint may be served by more than one native worker.
        # Keep the model-facing experience suffix idempotent at the AppWorld
        # boundary as a second guard; the first matching advice remains
        # available, while later identical injections cannot inflate context
        # or change the task solely because a request landed on another worker.
        self._seen_model_experience_contexts = set()
        self.level = "L0"
        self.l1_window_mode = None
        self.observed_phase_experiences = None
        self.auth_parameter_experiences = None
        self.auth_fallback_experiences = None
        self.auth_parameter_contracts = None
        self.auth_structured_missing = False
        self.shape_error_auth_experiences = None
        self.saved_result_experiences = None
        self.saved_result_contracts = None
        trial_config = os.environ.get("TRAJECTORY_EVOLUTION_TRIAL_CONFIG", "")
        version_path = Path(trial_config).with_name("version.json") if trial_config else None
        if version_path and version_path.is_file():
            artifact = json.loads(version_path.read_text()).get("artifact", {})
            self.level = str(artifact.get("level", "L0"))
            from appworld_skill import appworld_l1_window_mode, appworld_observed_phase_experiences
            self.l1_window_mode = appworld_l1_window_mode(artifact)
            self.observed_phase_experiences = appworld_observed_phase_experiences(artifact)
            appworld_options = artifact.get('agent', {}).get('appworld_options', {})
            if isinstance(appworld_options, dict) and 'l1_saved_result_experiences' in appworld_options:
                from appworld_skill import appworld_saved_result_experiences
                self.saved_result_experiences = appworld_saved_result_experiences(artifact)
            shape_forwarding = (appworld_options.get('l1_shape_error_state_forwarding', False)
                                if isinstance(appworld_options, dict) and self.level in {'L1', 'L2', 'L3'}
                                else False)
            if not isinstance(shape_forwarding, bool):
                raise ValueError('appworld_shape_error_state_forwarding_invalid')
            if shape_forwarding:
                from appworld_skill import appworld_shape_error_auth_experiences
                self.shape_error_auth_experiences = appworld_shape_error_auth_experiences(artifact)
                if self.shape_error_auth_experiences is None:
                    raise ValueError('appworld_shape_error_state_forwarding_source_invalid')
            if isinstance(appworld_options, dict) and 'l1_auth_structured_missing' in appworld_options:
                from appworld_skill import appworld_auth_structured_missing
                self.auth_structured_missing = appworld_auth_structured_missing(artifact)
            if isinstance(appworld_options, dict) and 'l1_auth_parameter_experiences' in appworld_options:
                from appworld_skill import appworld_auth_parameter_experiences
                self.auth_parameter_experiences = appworld_auth_parameter_experiences(artifact)
                if self.auth_parameter_experiences is not None:
                    excluded = {(ref['memory_id'], ref['content_revision'])
                                for ref in self.auth_parameter_experiences}
                    self.auth_fallback_experiences = [dict(ref) for ref in
                        artifact.get('policy', {}).get('options', {}).get('approved_experiences', [])
                        if (ref.get('memory_id'), ref.get('content_revision')) not in excluded]

    def _point_gap(self, fields):
        self.degraded_points += 1
        self.framework_capture["failures"] += 1
        context = getattr(self, "run_context", None)
        if self.evo is not None and isinstance(context, dict) and context.get("run_id"):
            try:
                self.evo._invoke("capture.gap", {"run_id": context["run_id"],
                    "reason": "missing_boundary", "error_code": "observer_unavailable"},
                    uuid.uuid4().hex, 1.0, 0)
            except Exception:
                pass
        return {"context": fields.get("context", {}), "input": fields.get("input"),
                "decision": "continue" if fields.get("kind") == "loop" else "allow",
                "degraded": True, "capture_complete": False}

    def _write_boundary_diagnostic(self):
        evidence = getattr(self, "evidence_dir", None)
        if evidence is None:
            return
        try:
            path = Path(evidence) / "host-error-diagnostic.json"
            previous = json.loads(path.read_text()) if path.is_file() else {}
            previous["native_run_boundaries"] = self.boundary_diagnostics
            path.write_text(json.dumps(previous, ensure_ascii=False), encoding="utf-8")
        except (OSError, ValueError, TypeError):
            pass

    def _run_boundary(self, fields):
        """Retry only one native point identity, never the Agent or AppWorld."""
        safe = self.evo._point_fields(native_view(fields))
        request_id = uuid.uuid4().hex
        phase = fields.get("phase")
        if not hasattr(self, "boundary_diagnostics"):
            self.boundary_diagnostics = []
        def real_context(value):
            run = value.get("run_id") if isinstance(value, dict) else None
            return (isinstance(run, str) and bool(run) and len(run) <= 256
                    and not run.startswith("unobserved-") and not value.get("degraded"))
        if phase != "before" and not real_context(fields.get("context")):
            raise AppWorldCaptureBoundaryError("appworld_native_run_context_missing")
        for attempt in (1, 2):
            try:
                result = self.evo._invoke("point", safe, request_id, 5.0, 0)
                context = result.get("context") or {}
                if phase == "before":
                    if result.get("degraded") or not real_context(context):
                        raise AppWorldCaptureBoundaryError("appworld_native_run_start_unconfirmed")
                    self.run_context = context
                elif not result.get("degraded") and result.get("pending") is True:
                    # Native point keeps the end request pending until its
                    # registered children really report. Poll actual status,
                    # with a deadline; never synthesize child completions.
                    now = real_clock()
                    limit = now + 60.0
                    task_deadline = getattr(self, "_task_deadline", None)
                    if isinstance(task_deadline, (int, float)) and not isinstance(task_deadline, bool):
                        limit = min(limit, task_deadline)
                    deadline = min(now + 10.0, limit)
                    event_count = None
                    closed = False
                    while real_clock() < deadline:
                        remaining = deadline - real_clock()
                        try:
                            status = self.evo._invoke("run.status", {"run_id": self.run_context["run_id"]},
                                uuid.uuid4().hex, min(1.0, max(0.001, remaining)), 0)
                            if status.get("closed") is True:
                                if status.get("capture_complete") is not True:
                                    raise AppWorldCaptureBoundaryError("appworld_native_run_close_incomplete")
                                result = {**result, "closed": True, "pending": False,
                                          "capture_complete": status["capture_complete"]}
                                closed = True
                                break
                            observed = status.get("event_count")
                            if (isinstance(observed, (int, float)) and not isinstance(observed, bool)
                                    and 0 <= observed < float("inf") and observed == int(observed)):
                                if event_count is not None and observed > event_count:
                                    # A registered child is still reporting. Keep
                                    # its original execution, within the task's
                                    # remaining budget and a 60-second close cap.
                                    deadline = min(limit, real_clock() + 10.0)
                                event_count = max(event_count or 0, observed)
                        except EvolutionError as error:
                            if (error.status not in (0, 408, 429) and error.status < 500
                                    or str(error).endswith("_cancelled")):
                                raise AppWorldCaptureBoundaryError(str(error)) from None
                        remaining = deadline - real_clock()
                        if remaining > 0:
                            time.sleep(min(0.1, remaining))
                    if not closed:
                        raise AppWorldCaptureBoundaryError("appworld_native_run_close_pending_timeout")
                    self.boundary_diagnostics.append({"phase": phase, "request_id": request_id,
                        "status": "pending_children_closed", "run_id": self.run_context["run_id"]})
                    self._write_boundary_diagnostic()
                elif result.get("degraded") or not (result.get("closed") is True or result.get("run_closed") is True):
                    raise AppWorldCaptureBoundaryError("appworld_native_run_close_unconfirmed")
                if attempt > 1:
                    self.boundary_diagnostics.append({"phase": phase, "request_id": request_id,
                        "status": "recovered", "attempt": attempt, "run_id": self.run_context["run_id"]})
                    self._write_boundary_diagnostic()
                return self.evo._point_observed(safe, result)
            except Exception as error:
                code = (str(error) if isinstance(error, (EvolutionError, AppWorldCaptureBoundaryError))
                        and re.fullmatch(r"[a-z][a-z0-9_]{0,95}", str(error)) else "observer_unavailable")
                record = {"phase": phase, "request_id": request_id,
                    "status": "failed", "attempt": attempt, "error_code": code}
                if real_context(getattr(self, "run_context", None)):
                    record["run_id"] = self.run_context["run_id"]
                self.boundary_diagnostics.append(record);self._write_boundary_diagnostic()
                retriable = (not isinstance(error, AppWorldCaptureBoundaryError)
                             and (not isinstance(error, EvolutionError) or error.status == 0 or error.status >= 500))
                if attempt < 2 and retriable:
                    continue
                self.degraded_points += 1
                self.framework_capture["failures"] += 1
                raise AppWorldCaptureBoundaryError("appworld_native_run_boundary_failed") from None

    def point(self, fields):
        # The normal SDK fail-open behavior still handles child observations;
        # an offline run's start/end must instead fail visibly and boundedly.
        if self.evo is not None and fields.get("kind") == "run":
            return self._run_boundary(fields)
        try:
            if self.evo is None:
                raise RuntimeError("appworld_standalone_no_native_boundary")
            if fields.get("phase") in {"after", "abort"}:
                context = fields.get("context") or {}
                kind = fields.get("kind", context.get("kind", "custom"))
                required = "control_id" if kind == "loop" else "activity_id" if kind != "run" else "run_id"
                # A failed before-boundary never creates a native activity.
                # Skip its after telemetry instead of submitting a false ID.
                if not isinstance(context.get(required), str) or not context[required]:
                    return self._point_gap(fields)
            safe = native_view(fields)
            result = native_capture_point(self.evo, safe)
            if result.get("degraded"):
                self._point_gap(fields)
            return result
        except Exception:
            return self._point_gap(fields)

    def account(self, usage):
        calls = int(usage.get("llm_calls", 0) or 0)
        self.calls += calls
        self.covered_calls += int(usage.get("token_covered_calls", 0) or 0)
        self.known_tokens += int(usage.get("total_tokens", 0) or 0)
        if calls:
            self.tokens_complete &= bool(usage.get("tokens_complete"))

    def observe_framework(self, runner):
        """Account real closed child-model lifecycles once per outer turn."""
        try:
            state = json.loads(Path(runner.meta["state_path"]).read_text())
            current = state.get("framework_capture")
            if not isinstance(current, dict):
                raise ValueError("framework_capture_missing")
            # appworld_agent_runtime resets this child snapshot at every
            # coordinator turn.  Treat the values as a per-turn snapshot;
            # subtracting the previous turn would manufacture a failure on
            # every turn after the first one.
            delta = {}
            for key in self.framework_capture:
                value = current.get(key)
                if not isinstance(value, int) or isinstance(value, bool) or value < 0:
                    raise ValueError("framework_capture_invalid")
                delta[key] = value
                self._framework_snapshot[key] = value
                self.framework_capture[key] += delta[key]
            expected = int(runner._partial_usage.get("llm_calls", 0) or 0)
            if delta["calls"] != expected:
                self.framework_capture["failures"] += 1
        except (OSError, ValueError, TypeError, KeyError, AttributeError):
            self.framework_capture["failures"] += 1

    def receipts(self):
        # task.status exposes only a total receipt count. A requested L3
        # policy can apply L1, L2 and L3 at different boundaries, so that
        # total cannot be attributed to its configured level. The completed
        # native trace supplies per-boundary counts to the report aggregator.
        self.applied = {level: 0 if self.level.upper() == "L0" else None
                        for level in ("L1", "L2", "L3")}

    def costs(self, started):
        complete = self.tokens_complete and not self.degraded_points and not self.framework_capture["failures"]
        return {"tokens": self.known_tokens if complete else None,
                "known_agent_tokens": self.known_tokens, "tokens_complete": complete,
                "model_calls": self.calls if complete else None, "token_covered_calls": self.covered_calls,
                "latency_ms": round((real_clock() - started) * 1000, 3),
                "l1_applied": self.applied["L1"], "l2_applied": self.applied["L2"],
                "l3_applied": self.applied["L3"], "level": self.level,
                "degraded_boundary_points": self.degraded_points,
                "framework_capture": {**self.framework_capture,
                    "status": "complete" if self.framework_capture["failures"] == 0
                              and not self.degraded_points and self.framework_capture["calls"] == self.calls else "incomplete"}}

    def receive(self):
        started = real_clock()
        timeout = (self.task.get("budget") or {}).get("timeout_ms")
        if (isinstance(timeout, (int, float)) and not isinstance(timeout, bool)
                and 0 < timeout < float("inf")):
            self._task_deadline = started + timeout / 1000.0
        synthetic_goal = synthetic_training_spec(self.task)
        raw_id = str((self.task.get("initial_state") or {})["source_task_id"])
        if not re.fullmatch(r"[a-z0-9]+_[0-9]+", raw_id):
            raise RuntimeError("appworld_task_id_invalid")
        # Public instruction is required by the native learning summary. It
        # lives in specs.json, outside the separate hidden ground_truth files.
        specs = json.loads((Path(os.environ["APPWORLD_ROOT"]) /
                            "data/tasks" / raw_id / "specs.json").read_text())
        instruction = ((self.task['initial_state']['training_instruction'])
                       if synthetic_goal else specs['instruction'])
        experiment = "agentflow-" + uuid.uuid4().hex
        evidence_root = Path(os.environ.get("APPWORLD_EVIDENCE_ROOT",
            str(ROOT / "trace_output/appworld/training/official-evidence")))
        evidence = evidence_root / experiment
        evidence.mkdir(parents=True, exist_ok=False)
        self.evidence_dir = evidence
        begin = self.point({"phase": "before", "kind": "run", "level": self.level,
            "capabilities": ["model", "actions", "loop"],
            "conversation_id": self.task.get("conversation_id"),
            "input": {"task_id": self.task.get("task_id", "appworld-" + raw_id),
                      "benchmark": "appworld", "instruction": instruction,
                      "native_view_redacted": True}})
        self.run_context = begin.get("context", {})
        runner = world = None
        history = []
        reason = "max_steps"
        tool_errors = repeated_code = decisions = 0
        model_parse_retries = 0
        previous_code = None
        last_result = None
        progress_counter = 0
        semantic_no_progress_count = 0
        previous_failure_signature = None
        repeated_failure_count = 0
        authenticated_apps = set()
        try:
            from appworld import AppWorld
            AgentRunner._load_env_file(os.environ)
            for name in ["HTTP_PROXY", "HTTPS_PROXY", "ALL_PROXY", "http_proxy", "https_proxy", "all_proxy"]:
                os.environ.pop(name, None)
            official_link = Path(os.environ["APPWORLD_ROOT"]) / "experiments/outputs" / experiment
            official_link.parent.mkdir(parents=True, exist_ok=True)
            official_link.symlink_to(evidence, target_is_directory=True)
            world = AppWorld(task_id=raw_id, experiment_name=experiment,
                             raise_on_unsafe_syntax=True, null_patch_unsafe_execution=True,
                             timeout_seconds=int(os.environ.get("APPWORLD_CODE_TIMEOUT", "30")),
                             **({'load_ground_truth': False} if synthetic_goal else {}))
            synthetic_before = None
            synthetic_execution_outcomes = []
            if synthetic_goal:
                install_synthetic_training_instruction(world, instruction)
                if synthetic_goal['kind'] == 'cross_app_playlist_edit':
                    _prepare_cross_app_playlist_fixture(world)
                synthetic_before = synthetic_training_snapshot(world, synthetic_goal)
            context = public_context(world)
            if self.auth_parameter_experiences is not None:
                self.auth_parameter_contracts = public_auth_parameter_contracts(world)
            if self.saved_result_experiences is not None:
                self.saved_result_contracts = public_saved_result_contracts(world)
            policy = ("Complete the user's AppWorld task by submitting Python code. "
                      "The public `apis` object is already instantiated in the execution "
                      "namespace. Do not import apis: it is an existing object, not an "
                      "importable module. Python variables persist across code blocks. "
                      "Call apis.<app>.<api>(...) and print useful results. Discover "
                      "available APIs and their exact parameters through apis.api_docs "
                      "using the public documentation below. Use keyword arguments exactly as "
                      "the docs specify, never positional arguments. Keep full API results in variables; "
                      "print only needed IDs, fields, and counts, never full lists, threads, "
                      "payloads, or formatted JSON. For state-changing APIs, act only on objects "
                      "explicitly required by the request. Before playback or another thresholded "
                      "mutation, compute the exact threshold from current public state; if no "
                      "eligible candidate exists, leave state unchanged. When matching a collection, "
                      "paginate to the documented empty-page boundary before mutating. For authenticated APIs, "
                      "consult the documented login API and obtain the supervisor's "
                      "account credentials using the documented supervisor API; keep "
                      "the returned access token in a variable and supply it where the "
                      "API documentation requires it. An API error is an observation "
                      "to diagnose before repeating that code. No hidden evaluation or "
                      "host filesystem access. Submit apis.supervisor.complete_task() "
                      "when the requested work is done.\n\n" +
                      json.dumps(context, ensure_ascii=False))
            if re.search(r"\bhabit\b|\bstreak\b", instruction, flags=re.IGNORECASE):
                policy += ("\n\nFor a read-only Simple Note habit or streak question, batch the "
                           "document lookup, login, paginated note reads, and streak "
                           "calculation in as few code submissions as the public API "
                           "allows. Do not redispatch the skill merely to repeat a "
                           "successful read; after the answer is verified, submit "
                           "complete_task.")
            runner = AgentRunner(self.level)
            for turn in range(int(os.environ.get("APPWORLD_MAX_STEPS", "30"))):
                # The loop controller needs a stable semantic observation.  The
                # submitted source is already present in the model history; putting
                # it in loop state made harmless variable/comment edits look like
                # progress and defeated the native no-progress counter.  Keep only
                # the public result and factual progress so repeated no-change
                # observations receive the same fingerprint.
                loop = self.point({"phase": "before", "kind": "loop", "name": "appworld-interaction",
                    "context": self.run_context,
                    "state": {"last_result": last_result,
                              "progress": progress_counter,
                              "task_completed": bool(world.task_completed())}})
                loop_decision, turn_policy = loop_model_policy(policy, loop)
                stop = loop_decision == "stop"
                self.point({"phase": "after", "kind": "loop",
                    "context": loop.get("context", self.run_context), "executed": True,
                    "decision": loop_decision, "output": {"stopped": stop}})
                if stop:
                    reason = "policy_stop"
                    break
                model_input = {"system": turn_policy, "messages": history, "turn": turn,
                               "native_view_redacted": True}
                model = self.point({"phase": "before", "kind": "agent-turn", "name": "appworld-agentflow",
                    "context": loop.get("context", self.run_context), "input": model_input,
                    # Keep model and action observations in one per-trial stream.  The
                    # native runtime uses this stable scope to prevent a failure from
                    # leaking across concurrent tasks while still exposing a compact
                    # structured error to the next L1 query.
                    "extensions": {"point_reference": {"parent": self.run_context.get(
                        "run_id", "appworld-" + raw_id)}},
                    "binding": {"pointer": "/system", "mode": "text"}})
                # Strategies may change only the native /system binding. The
                # real public history retains the official simulator values.
                native_prepared = model.get("input", model_input)
                prepared = {**model_input, "native_view_redacted": True}
                if isinstance(native_prepared, dict) and isinstance(native_prepared.get("system"), str):
                    observed_system = native_prepared["system"]
                    original_system_view = native_view(turn_policy)
                    if observed_system != original_system_view:
                        if observed_system.startswith(original_system_view):
                            suffix = observed_system[len(original_system_view):]
                            if "<trajectory-experience>" in suffix:
                                if suffix in self._seen_model_experience_contexts:
                                    prepared["system"] = turn_policy
                                else:
                                    self._seen_model_experience_contexts.add(suffix)
                                    prepared["system"] = turn_policy + suffix
                            else:
                                prepared["system"] = turn_policy + suffix
                        elif REDACTION not in observed_system:
                            prepared["system"] = observed_system
                if not isinstance(prepared, dict) or not isinstance(prepared.get("system"), str):
                    prepared = model_input
                if model.get("decision") in {"stop", "deny", "skip", "reject"}:
                    raise RuntimeError("policy_model_rejected")
                runner._partial_usage = {}
                accounted = False
                try:
                    runner.configure(policy=prepared["system"], tools_schema=[],
                                     messages=prepared.get("messages", history), turn=turn,
                                     native_context=model.get("context", self.run_context),
                                     native_replan_context=(loop.get("replan_context")
                                         if loop_decision == "replan" else None),
                                     **({'l1_window_mode': self.l1_window_mode,
                                         'observed_phase_experiences': self.observed_phase_experiences}
                                        if self.l1_window_mode is not None else {}),
                                     **({'auth_parameter_experiences': self.auth_parameter_experiences,
                                         'auth_fallback_experiences': self.auth_fallback_experiences,
                                         'auth_parameter_contracts': self.auth_parameter_contracts,
                                         **({'auth_structured_missing': True} if self.auth_structured_missing else {})}
                                        if self.auth_parameter_experiences is not None else {}),
                                     **({'shape_error_auth_experiences': self.shape_error_auth_experiences}
                                        if self.shape_error_auth_experiences is not None else {}),
                                     **({'saved_result_experiences': self.saved_result_experiences,
                                         'saved_result_contracts': self.saved_result_contracts}
                                        if self.saved_result_experiences is not None else {}))
                    try:
                        _, result = runner.ask([
                            {"role": "system", "content": COORDINATOR_INSTRUCTION},
                            {"role": "user", "content": "[AppWorld request]\nDispatch appworld-office once. "
                             "Read its complete current public request and use appworld_submit_code once. "
                             "Do not execute AppWorld APIs in this coordinator."}],
                            user_id="appworld-training", dialogue_id="appworld-" + raw_id, turn=turn)
                    finally:
                        self.observe_framework(runner)
                    usage = result.get("usage") or {}
                    self.account(usage)
                    accounted = True
                    code = read_reply(runner.meta)["code"]
                except Exception as reply_error:
                    if not accounted:
                        self.account(runner._partial_usage)
                    diagnostic = {}
                    try:
                        diagnostic = reply_diagnostic(runner)
                        (evidence / "reply-diagnostic.json").write_text(
                            json.dumps(native_view(diagnostic), ensure_ascii=False))
                    except (OSError, ValueError, TypeError):
                        pass
                    recovered_code = None
                    if str(reply_error) == "appworld_reply_missing":
                        try:
                            recovered_code = recover_single_fenced_code(runner, diagnostic)
                        except (OSError, RuntimeError, ValueError, TypeError):
                            recovered_code = None
                    if recovered_code is not None:
                        code = recovered_code
                        self.point({"phase": "after", "kind": "agent-turn",
                            "context": model.get("context", self.run_context),
                            "executed": True, "actual_input": prepared,
                            "output": {"termination_reason": "recovered_fenced_code",
                                       "code_submitted": True}})
                    elif str(reply_error) == "appworld_reply_missing" and natural_model_stop(diagnostic):
                        # A normal provider stop is a terminal agent decision,
                        # even when no more Python block is submitted. The
                        # official world state, including failures, is graded
                        # below without executing invented completion code.
                        reason = "model_stopped_without_code"
                        self.point({"phase": "after", "kind": "agent-turn",
                            "context": model.get("context", self.run_context), "executed": True,
                            "actual_input": prepared,
                            "output": {"termination_reason": reason, "code_submitted": False}})
                        break
                    # Provider-compatible responses can occasionally reach the
                    # private AgentFlow parser with an empty choice list.  A
                    # single malformed turn must not discard the whole
                    # AppWorld task; retry the model on the next loop with a
                    # compact, non-sensitive instruction.  Bound retries so
                    # parser defects cannot create an unbounded loop.
                    if recovered_code is None and isinstance(reply_error, IndexError) and model_parse_retries < 2:
                        model_parse_retries += 1
                        self.point({"phase": "after", "kind": "agent-turn",
                            "context": model.get("context", self.run_context),
                            "executed": False, "status": "failed",
                            "actual_input": prepared,
                            "output": {"error_code": "agent_reply_parse_retry"},
                            "error": {"code": "agent_reply_parse_retry",
                                       "retryability": "retryable"}})
                        history.append({"role": "user", "content":
                            "The previous model response did not submit valid code. "
                            "Submit exactly one executable Python code block through "
                            "the required AppWorld code tool."})
                        reason = "model_retry"
                        continue
                    if recovered_code is None:
                        self.point({"phase": "after", "kind": "agent-turn",
                            "context": model.get("context", self.run_context), "executed": False,
                            "status": "failed",
                            "actual_input": prepared, "output": {"error_code": "agent_reply_invalid"},
                            "error": {"code": "agent_reply_invalid", "retryability": "unknown"}})
                        raise
                decisions += 1
                self.point({"phase": "after", "kind": "agent-turn",
                    "context": model.get("context", self.run_context), "executed": True,
                    "actual_input": prepared, "output": {"code": code}})
                action = self.point({"phase": "before", "kind": "action", "name": "appworld.execute",
                    "context": loop.get("context", self.run_context),
                    "extensions": {"point_reference": {"parent": self.run_context.get(
                        "run_id", "appworld-" + raw_id)}},
                    "input": {"code": code, "native_view_redacted": True}})
                selected = action.get("input")
                original_view = native_view(code)
                if isinstance(selected, dict) and isinstance(selected.get("code"), str) and selected["code"] != original_view:
                    if REDACTION in selected["code"]:
                        raise RuntimeError("policy_action_redaction_invalid")
                    code = selected["code"]
                # Keep the host rewrite internal.  The boundary event schema
                # is shared with business and rejects adapter-only fields;
                # persisting a normalization marker here corrupts capture.
                code, _normalized_import = normalize_appworld_code(code)
                # Count exact repeated submissions after any host policy
                # rewrite, including a submission that is then rejected.  The
                # previous implementation counted only world.execute calls,
                # so repeated policy rejections disappeared from the host
                # metric and were visible only to the unstable model review.
                same_code = code == previous_code
                repeated_code += int(same_code)
                previous_code = code
                if action.get("decision") in {"stop", "deny", "skip", "reject"}:
                    reason = "policy_action_rejected"
                    self.point({"phase": "after", "kind": "action",
                        "context": action.get("context", self.run_context), "executed": False,
                        "status": "failed",
                        "actual_input": {"code": code},
                        "output": {"error_code": reason},
                        "error": {"code": reason, "retryability": "non_retryable"}})
                    # A bounded action guard is feedback, not a task terminal
                    # state.  Preserve the rejected code and a compact reason
                    # in the conversation so the model can repair it on the
                    # next turn; breaking here made L2 controls silently
                    # convert recoverable code repetition into a failed task.
                    next_result = {"status": "rejected", "error_code": reason}
                    progress_counter += appworld_progress_delta(last_result, next_result)
                    last_result = next_result
                    (previous_failure_signature, repeated_failure_count,
                     recovery_exhausted) = appworld_repeated_failure_stop(
                         previous_failure_signature, (reason, ''), repeated_failure_count)
                    history.extend([
                        {"role": "assistant", "content": code},
                        {"role": "user", "content":
                         "Policy rejected the submitted code after repeated failure. "
                         "Change the code or arguments before retrying; do not repeat it."},
                    ])
                    if recovery_exhausted:
                        reason = "bounded_recovery_exhausted"
                        break
                    continue
                if appworld_missing_access_token(code, authenticated_apps):
                    reason = "appworld_missing_access_token"
                    self.point({"phase": "after", "kind": "action",
                        "context": action.get("context", self.run_context),
                        "executed": False, "status": "failed",
                        "actual_input": {"code": code},
                        "output": {"error_code": reason,
                                   "message": "Reuse the access_token returned by the successful login; do not call a protected API without access_token."},
                        "error": {"code": reason, "retryability": "retryable"}})
                    next_result = {"status": "rejected", "error_code": reason}
                    progress_counter += appworld_progress_delta(last_result, next_result)
                    last_result = next_result
                    (previous_failure_signature, repeated_failure_count,
                     recovery_exhausted) = appworld_repeated_failure_stop(
                         previous_failure_signature, (reason, ''), repeated_failure_count)
                    history.extend([
                        {"role": "assistant", "content": code},
                        {"role": "user", "content":
                         (json.dumps({**next_result, 'message':
                             "The host blocked a protected call without access_token. "
                             "Reuse the token variable returned by the successful login; do not login again."})
                          if self.auth_structured_missing else
                          "The host blocked a protected call without access_token. "
                          "Reuse the token variable returned by the successful login; do not login again.")},
                    ])
                    if recovery_exhausted:
                        reason = "bounded_recovery_exhausted"
                        break
                    continue
                observation = world.execute(code)
                if not isinstance(observation, str):
                    raise RuntimeError("appworld_execute_result_invalid")
                failed = observation.lstrip().startswith(("Execution failed", "Traceback"))
                if synthetic_goal and (synthetic_goal.get('reproduce_container_error') or
                                       synthetic_goal['kind'] == 'cross_app_auth_recovery_summary'):
                    synthetic_execution_outcomes.append({'code': code, 'observation': observation, 'failed': failed})
                tool_errors += int(failed)
                failure_code = appworld_failure_code(observation) if failed else ""
                if not failed:
                    authenticated_apps.update(
                        app for app, method, _ in _appworld_api_calls(code)
                        if method == "login")
                next_result = {"status": "failed" if failed else "completed",
                               "observation": observation, "error_code": failure_code}
                progress_counter += appworld_progress_delta(last_result, next_result)
                semantic_no_progress_count += int(appworld_semantic_no_progress(last_result, next_result))
                last_result = next_result
                self.point({"phase": "after", "kind": "action",
                    "context": action.get("context", self.run_context), "executed": True,
                    "status": "failed" if failed else "completed",
                    "actual_input": {"code": code, "native_view_redacted": True},
                    "output": {"observation": observation, "native_view_redacted": True},
                    **({"error": {"code": failure_code, "retryability": "unknown"}} if failed else {})})
                if failed:
                    signature = (failure_code, observation[:1024])
                    (previous_failure_signature, repeated_failure_count,
                     recovery_exhausted) = appworld_repeated_failure_stop(
                         previous_failure_signature, signature, repeated_failure_count)
                    if recovery_exhausted:
                        reason = "bounded_recovery_exhausted"
                        # The loop's normal after-boundary was already
                        # observed above.  Submitting a second after event
                        # for the same control id violates the shared capture
                        # protocol and turns a recoverable task into a broken
                        # trajectory; record the terminal reason only in the
                        # final run receipt.
                        break
                else:
                    previous_failure_signature = None
                    repeated_failure_count = 0
                history.extend([{"role": "assistant", "content": code},
                                {"role": "user", "content": observation}])
                if not failed and (world.task_completed() or appworld_completion_submitted(code)):
                    reason = ("task_completed" if world.task_completed()
                              else "agent_submitted_completion")
                    break
            # Grade only now. Never feed this tracker or its hidden tests back to the model.
            tracker = None
            if synthetic_goal:
                metrics = (synthetic_training_metrics(world, synthetic_goal, synthetic_before,
                                                      synthetic_execution_outcomes)
                           if (synthetic_goal.get('reproduce_container_error') or
                               synthetic_goal['kind'] == 'cross_app_auth_recovery_summary') else
                           synthetic_training_metrics(world, synthetic_goal, synthetic_before))
            else:
                tracker = world.evaluate()
                metrics = tracker_metrics(tracker)
            self.receipts()
            output = {**metrics, **self.costs(started), "termination_reason": reason,
                "task_completed": bool(world.task_completed()), "agent_decisions": decisions,
                "tool_error_count": tool_errors, "repeated_code_count": repeated_code,
                "semantic_no_progress_count": semantic_no_progress_count,
                # These two host counters are diagnostics only.  The formal
                # invalid-loop metric is assigned by the shared model review,
                # using the same evidence contract as business coverage.  Do
                # not collapse repeated source code and an identical public
                # observation into a synthetic score here: the observation
                # does not identify the target object or operation, so doing
                # so can over-count legitimate pagination/target splits.
                "invalid_loop_count": None,
                "invalid_loop_basis": "model_review",
                "official_experiment_name": experiment, "response": "",
                "official_evidence_dir": str(evidence),
                "model_parse_retries": model_parse_retries,
                "evaluation_status": "completed", "error_code": ""}
            # Read the canonical training pack rather than a duplicate selection.
            train_path = ROOT / "tools/trajectory_evolution/experiments/appworld/datasets/train.jsonl"
            train_ids = {json.loads(line)["task_id"].removeprefix("appworld-")
                         for line in train_path.read_text().splitlines() if line.strip()}
            if not synthetic_goal and raw_id in train_ids:
                output["training_feedback"] = {"source": "official_training_tests",
                    "failures": [{"requirement": item.get("requirement"),
                                  "label": item.get("label")} for item in tracker.failures]}
            self.point({"phase": "after", "kind": "run", "context": self.run_context,
                        "output": output, "business_status": "succeeded"
                        if metrics["appworld_reward"] == 1 else "failed"})
            return output
        except AppWorldCaptureBoundaryError:
            # evaluate() submits evaluation.fail for this callback exception.
            # Never return a fabricated close or replay completed Agent work.
            raise
        except Exception as error:
            self.receipts()
            code = str(error)
            safe_code = code if re.fullmatch(r"(?:appworld|agent|policy)_[a-z0-9_]+", code) else "appworld_host_error"
            # Keep enough redacted path information to diagnose host setup
            # failures such as a missing runtime file.  The old output kept
            # only ``FileNotFoundError``, making strict seed failures
            # indistinguishable from missing task data.
            safe_detail = native_view(code)[:1024]
            output = {**self.costs(started), "appworld_reward": None, "quality_score": None,
                "strict_passed": None, "evaluation_status": "errored", "error_code": safe_code,
                "error_type": type(error).__name__, "agent_decisions": decisions,
                "tool_error_count": tool_errors, "repeated_code_count": repeated_code,
                "semantic_no_progress_count": semantic_no_progress_count,
                "invalid_loop_count": None,
                "invalid_loop_basis": "model_review",
                "model_parse_retries": model_parse_retries,
                "response": "", "error_detail": safe_detail}
            if "experiment" in locals() and "evidence" in locals():
                output.update(official_experiment_name=experiment,
                              official_evidence_dir=str(evidence))
                try:
                    (evidence / "host-error-diagnostic.json").write_text(
                        json.dumps({"error_type": type(error).__name__,
                                    "error_detail": safe_detail,
                                    "native_run_boundaries": self.boundary_diagnostics}, ensure_ascii=False),
                        encoding="utf-8")
                except OSError:
                    pass
            self.point({"phase": "after", "kind": "run", "context": self.run_context,
                        "output": output, "business_status": "failed",
                        "status": "failed",
                        "error": {"code": safe_code, "retryability": "unknown"}})
            return output
        finally:
            if world is not None:
                world.close()
            if runner is not None:
                runner.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--run-entry", action="store_true")
    parser.add_argument("--task-id")
    parser.add_argument("--project-official-code", action="store_true")
    args = parser.parse_args()
    if args.project_official_code:
        raw = sys.stdin.read(64 * 1024 * 1024 + 1)
        try:
            request = json.loads(raw) if len(raw.encode('utf-8')) <= 64 * 1024 * 1024 else None
        except (ValueError, TypeError):
            request = None
        print(json.dumps(project_official_code_request(request), ensure_ascii=False, allow_nan=False))
        return 0
    if args.run_entry:
        if not os.environ.get("TRAJECTORY_EVOLUTION_TRIAL_URL"):
            raise RuntimeError("native_evolution_boundary_required")
        if not evaluate(lambda task: Trial(task).receive()):
            raise RuntimeError("appworld_evaluation_entry_not_enabled")
        return 0
    if args.task_id:
        task = {"task_id": "appworld-" + args.task_id,
                "initial_state": {"source_task_id": args.task_id}}
        print(json.dumps(Trial(task).receive(), ensure_ascii=False, indent=2))
        return 0
    parser.error("choose --run-entry or --task-id")


if __name__ == "__main__":
    raise SystemExit(main())
