"""离线冻结验收子目标：只接标准任务与模型配置，不读取 Agent 结果或学习记忆。"""

from __future__ import annotations

from concurrent.futures import ThreadPoolExecutor, as_completed
from copy import deepcopy
import fcntl
import hashlib
import http.client
import json
import math
from pathlib import Path
import re
import time
from urllib.parse import urlsplit


HERE = Path(__file__).resolve().parent
DEFAULT_OUTPUT = HERE / "datasets/gepa-goals.json"
PROMPTS = {stage: HERE / ("prepare-goals-" + stage + ".txt") for stage in ("extract", "review")}
GOAL_FIELDS = {"id", "expected_behavior", "due_turn", "critical", "weight", "conditional"}
ISSUES = {"missing_requirement", "added_requirement", "conditional_changed", "deadline_changed",
          "duplicate_goal", "constraint_changed", "existing_goal_changed", "invalid_schema"}
SAFE_FAILURES = {"model_endpoint_invalid", "model_credentials_missing", "model_input_budget_exceeded",
                 "model_http_failed", "model_timeout", "model_response_budget_exceeded",
                 "model_response_incomplete", "model_transport_or_json_failed", "model_secret_reflection",
                 "goal_review_invalid", "goal_review_exhausted"}


class PreparationError(RuntimeError):
    """只携带固定错误码，不把提供方正文、请求头或底层异常带入日志。"""


def _canonical(value):
    """规范 JSON 保证摘要不受字典插入次序影响，并拒绝非有限数值。"""
    return json.dumps(value, ensure_ascii=False, sort_keys=True, separators=(",", ":"),
                      allow_nan=False)


def _digest(value):
    """对公开契约计算摘要；调用方不传凭据或整个环境。"""
    return hashlib.sha256(_canonical(value).encode()).hexdigest()


def _identifier(value):
    """标识只含安全字符，不能将任意正文作为错误诊断标识。"""
    return isinstance(value, str) and bool(re.fullmatch(r"[A-Za-z0-9_.:-]{1,256}", value))


def _source_task(task):
    """仅选择原输入、验收和禁止项；技能标签、初态、轨迹、策略均不进入提取输入。"""
    if not isinstance(task, dict) or not isinstance(task.get("followup_inputs", []), list):
        raise PreparationError("task_inputs_invalid")
    inputs = [task.get("task_input")] + task.get("followup_inputs", [])
    attributes = task.get("attributes", {})
    if not isinstance(attributes, dict):
        raise PreparationError("task_attributes_invalid")
    turns = attributes.get("turn_expectations")
    if not _identifier(task.get("task_id")) or not _identifier(task.get("task_version")):
        raise PreparationError("task_identity_invalid")
    if any(not isinstance(text, str) or not text for text in inputs):
        raise PreparationError("task_inputs_invalid")
    if turns is None:
        if len(inputs) != 1:
            raise PreparationError("multi_turn_expectations_missing")
        turns = [{"turn": 1, "expected_behavior": task.get("expected_behavior", "")}]
    if not isinstance(turns, list) or len(turns) != len(inputs):
        raise PreparationError("turn_expectations_invalid")
    result = []
    for index, (turn, text) in enumerate(zip(turns, inputs), 1):
        if not isinstance(turn, dict) or isinstance(turn.get("turn"), bool) or turn.get("turn") != index:
            raise PreparationError("turn_expectations_invalid")
        expected = turn.get("expected_behavior", "")
        if not isinstance(expected, str):
            raise PreparationError("turn_expectations_invalid")
        row = {"turn": index, "user_input": text, "expected_behavior": expected}
        if "criteria" in turn:
            row["criteria"] = deepcopy(turn["criteria"])
        result.append(row)
    constraints = task.get("constraints", [])
    if not isinstance(constraints, list) or any(not isinstance(x, str) for x in constraints):
        raise PreparationError("constraints_invalid")
    source = {"task_id": task["task_id"], "task_version": task["task_version"],
              "turns": result, "constraints": constraints}
    if len(_canonical(source).encode()) > 65536:
        raise PreparationError("task_input_budget_exceeded")
    return source


def _validate(proposal, source):
    """校验既有目标契约；原 criteria 逐字段保留，目标身份在全会话唯一。"""
    if not isinstance(proposal, dict) or set(proposal) != {"turn_expectations"}:
        raise PreparationError("criteria_schema_invalid")
    rows = proposal["turn_expectations"]
    if not isinstance(rows, list) or len(rows) != len(source["turns"]):
        raise PreparationError("criteria_turn_count_invalid")
    ids, count = {f"constraint-{i + 1}" for i in range(len(source["constraints"]))}, 0
    for original, row in zip(source["turns"], rows):
        if not isinstance(row, dict) or set(row) != {"turn", "criteria"} or isinstance(row["turn"], bool) or row["turn"] != original["turn"]:
            raise PreparationError("criteria_turn_identity_invalid")
        criteria = row["criteria"]
        if not isinstance(criteria, list) or not 1 <= len(criteria) <= 32:
            raise PreparationError("criteria_count_invalid")
        existing = "criteria" in original
        if existing and criteria != original["criteria"]:
            raise PreparationError("existing_criteria_changed")
        for ordinal, criterion in enumerate(criteria, 1):
            if not isinstance(criterion, dict) or set(criterion) - GOAL_FIELDS:
                raise PreparationError("criterion_fields_invalid")
            identifier = criterion.get("id", f"turn-{row['turn']}-{ordinal}")
            text = criterion.get("expected_behavior")
            due = criterion.get("due_turn", row["turn"])
            weight = criterion.get("weight", 1)
            if not _identifier(identifier) or identifier in ids:
                raise PreparationError("criterion_identity_invalid")
            if not isinstance(text, str) or not text.strip() or len(text.encode()) > 32768:
                raise PreparationError("criterion_text_invalid")
            if isinstance(due, bool) or not isinstance(due, (int, float)) or not math.isfinite(due) or int(due) != due or not row["turn"] <= due <= len(rows):
                raise PreparationError("criterion_deadline_invalid")
            if isinstance(weight, bool) or not isinstance(weight, (int, float)) or not math.isfinite(weight) or not 0 < weight <= 100:
                raise PreparationError("criterion_weight_invalid")
            if any(key in criterion and not isinstance(criterion[key], bool) for key in ("critical", "conditional")):
                raise PreparationError("criterion_flags_invalid")
            # 新子目标只能拆分要求，不得利用权重或关键标记改变发布门槛。
            if not existing and (weight != 1 or criterion.get("critical", False)):
                raise PreparationError("generated_goal_priority_changed")
            ids.add(identifier)
            count += 1
    if count + len(source["constraints"]) > 256:
        raise PreparationError("criteria_total_budget_exceeded")
    return deepcopy(rows)


def _model_config(model, environment, timeout, max_repairs, prompts):
    """兼容公开 runtime.model 字段；缓存身份不包含密钥值或环境全集。"""
    model = model or {}
    endpoint = model.get("endpoint") or environment.get("OPENAI_BASE_URL", "")
    identity = model.get("model_id") or environment.get("OPENAI_MODEL", "")
    api_key_env = model.get("api_key_env", "OPENAI_API_KEY")
    if not isinstance(endpoint, str) or not isinstance(identity, str) or not isinstance(api_key_env, str):
        raise PreparationError("model_configuration_invalid")
    endpoint = endpoint.rstrip("/")
    if endpoint and not endpoint.endswith("/chat/completions"):
        endpoint += "/chat/completions"
    limit = model.get("max_output_tokens", 8192)
    if isinstance(limit, bool) or not isinstance(limit, (int, float)) or not math.isfinite(limit) or int(limit) != limit or not 512 <= limit <= 16384:
        raise PreparationError("model_output_limit_invalid")
    public = {"endpoint": endpoint, "model_id": identity, "max_output_tokens": int(limit),
              "thinking": "disabled", "temperature": 0, "timeout": timeout,
              "max_repairs": max_repairs, "prompt_hashes": {k: _digest(v) for k, v in prompts.items()},
              "extractor_version": 1}
    return public, api_key_env


def request_model(stage, payload, *, environment, model, timeout, prompt, api_key_env):
    """单次标准 HTTP 请求，无隐式传输重试；不记录响应正文、请求头或底层异常。

    请求只包含选定任务原输入和验收，不发送轨迹、记忆或策略。
    """
    endpoint = urlsplit(model["endpoint"])
    if endpoint.scheme != "https" or not endpoint.hostname or endpoint.username or endpoint.password or endpoint.query or endpoint.fragment:
        raise PreparationError("model_endpoint_invalid")
    key = environment.get(api_key_env, "")
    if not key or not model["model_id"]:
        raise PreparationError("model_credentials_missing")
    body = _canonical({"model": model["model_id"], "temperature": 0,
                       "max_tokens": model["max_output_tokens"], "thinking": {"type": "disabled"},
                       "response_format": {"type": "json_object"},
                       "messages": [{"role": "system", "content": prompt},
                                    {"role": "user", "content": _canonical(payload)}]}).encode()
    if len(body) > 196608:
        raise PreparationError("model_input_budget_exceeded")
    connection = http.client.HTTPSConnection(endpoint.hostname, endpoint.port, timeout=timeout)
    deadline = time.monotonic() + timeout
    try:
        connection.request("POST", endpoint.path, body, {"Content-Type": "application/json",
                           "Authorization": "Bearer " + key})
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise PreparationError("model_timeout")
        if connection.sock is not None:
            connection.sock.settimeout(remaining)
        response = connection.getresponse()
        if response.status != 200:
            raise PreparationError("model_http_failed")
        data = bytearray()
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise PreparationError("model_timeout")
            sock = connection.sock or getattr(getattr(response.fp, "raw", None), "_sock", None)
            if sock is not None:
                sock.settimeout(remaining)
            chunk = response.read1(min(16384, 262145 - len(data)))
            if not chunk:
                break
            data.extend(chunk)
            if len(data) > 262144:
                raise PreparationError("model_response_budget_exceeded")
        raw = json.loads(data)
        choice = raw["choices"][0]
        if choice.get("finish_reason") != "stop":
            raise PreparationError("model_response_incomplete")
        return json.loads(choice["message"]["content"])
    except PreparationError:
        raise
    except Exception:
        raise PreparationError("model_transport_or_json_failed") from None
    finally:
        connection.close()


def _extract(source, environment, model, api_key_env, prompts, timeout, max_repairs, request):
    """独立提取、独立复核；修复预算只用于修改候选，不做无限传输重试。"""
    correction = None
    for attempt in range(max_repairs + 1):
        payload = {"source": source}
        if correction is not None:
            payload["correction"] = correction
        proposal = request("extract", payload, environment=environment, model=model, timeout=timeout,
                           prompt=prompts["extract"], api_key_env=api_key_env)
        serialized = _canonical(proposal)
        if len(serialized.encode()) > 131072:
            raise PreparationError("model_response_budget_exceeded")
        secret = environment.get(api_key_env, "")
        if secret and secret in serialized:
            raise PreparationError("model_secret_reflection")
        try:
            rows = _validate(proposal, source)
        except PreparationError:
            correction = {"issues": ["invalid_schema"]}
            continue
        review = request("review", {"source": source, "proposal": proposal}, environment=environment,
                         model=model, timeout=timeout, prompt=prompts["review"], api_key_env=api_key_env)
        if not isinstance(review, dict) or set(review) != {"approved", "issues"} or not isinstance(review["approved"], bool) or not isinstance(review["issues"], list):
            raise PreparationError("goal_review_invalid")
        issues = review["issues"]
        if len(issues) > 16 or any(not isinstance(x, str) or x not in ISSUES for x in issues):
            raise PreparationError("goal_review_invalid")
        if review["approved"] and not issues:
            return rows
        if review["approved"] or not issues:
            raise PreparationError("goal_review_invalid")
        correction = {"issues": issues, "previous_proposal": proposal}
    raise PreparationError("goal_review_exhausted")


def _write(path, value):
    """同目录原子替换；只有主线程写盘，不同任务线程不能互相覆盖结果。"""
    temporary = path.with_name(path.name + ".new")
    temporary.write_text(_canonical(value) + "\n", encoding="utf-8")
    temporary.replace(path)


def prepare(tasks, *, source_version, output=DEFAULT_OUTPUT, environment=None, model=None,
            enabled=True, concurrency=16, timeout=120, max_repairs=1, request=None):
    """冻结输入清单的验收目标；任何失败都拒绝整体成功，保留已复核项便于恢复。

    调用方传入 run.environment() 和公开 runtime.model；测试可注入无网络请求桩。
    val 和 holdout 的验收契约只来自原始任务，不读取任何策略或执行反馈。
    """
    if not isinstance(source_version, str) or not source_version:
        raise PreparationError("source_version_missing")
    if isinstance(concurrency, bool) or not isinstance(concurrency, int) or not 1 <= concurrency <= 16:
        raise PreparationError("concurrency_invalid")
    if isinstance(timeout, bool) or not isinstance(timeout, (int, float)) or not math.isfinite(timeout) or not 0 < timeout <= 120:
        raise PreparationError("timeout_invalid")
    if isinstance(max_repairs, bool) or not isinstance(max_repairs, int) or not 0 <= max_repairs <= 2:
        raise PreparationError("repair_budget_invalid")
    sources = [_source_task(task) for task in tasks]
    if not sources or len({x["task_id"] for x in sources}) != len(sources):
        raise PreparationError("task_selection_invalid")
    environment = dict(environment or {})
    prompts = {stage: path.read_text(encoding="utf-8") for stage, path in PROMPTS.items()}
    public, api_key_env = _model_config(model, environment, timeout, max_repairs, prompts)
    output = Path(output)
    output.parent.mkdir(parents=True, exist_ok=True)
    state_path = output.with_name(output.stem + ".state.json")
    with output.with_name(output.name + ".lock").open("a") as lock:
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            raise PreparationError("goal_preparation_busy") from None
        saved, state = {}, {}
        if output.exists() and state_path.exists():
            try:
                old = json.loads(output.read_text(encoding="utf-8"))
                checkpoint = json.loads(state_path.read_text(encoding="utf-8"))
                if old.get("source_version") == source_version and checkpoint.get("source_version") == source_version:
                    saved = {row["task_id"]: row for row in old["tasks"]}
                    state = checkpoint["tasks"]
            except Exception:
                raise PreparationError("goal_checkpoint_invalid") from None
        results, verified, pending, errors = {}, {}, [], {}
        for source in sources:
            task_id = source["task_id"]
            fingerprint = _digest({"source_version": source_version, "task": source, "model": public})
            cached, record = saved.get(task_id), state.get(task_id, {})
            if cached and record.get("fingerprint") == fingerprint and record.get("digest") == _digest(cached):
                if cached.get("task_version") != source["task_version"]:
                    raise PreparationError("goal_checkpoint_invalid")
                _validate({"turn_expectations": cached["turn_expectations"]}, source)
                results[task_id], verified[task_id] = cached, record
            elif all("criteria" in turn for turn in source["turns"]):
                rows = [{"turn": turn["turn"], "criteria": turn["criteria"]} for turn in source["turns"]]
                _validate({"turn_expectations": rows}, source)
                result = {"task_id": task_id, "task_version": source["task_version"], "turn_expectations": rows}
                results[task_id] = result
                verified[task_id] = {"fingerprint": fingerprint, "digest": _digest(result)}
            else:
                pending.append((source, fingerprint))

        def persist():
            """正式契约只存子目标；状态文件只存身份摘要，两者均不含模型凭据。"""
            document = {"schema_version": 1, "source_version": source_version,
                        "tasks": [results[x["task_id"]] for x in sources if x["task_id"] in results]}
            _write(output, document)
            _write(state_path, {"schema_version": 1, "source_version": source_version,
                                "tasks": verified, "errors": errors})
            return document

        if pending and not enabled:
            persist()
            raise PreparationError("criteria_missing_model_disabled")
        failures = 0
        with ThreadPoolExecutor(max_workers=concurrency) as pool:
            jobs = {pool.submit(_extract, source, environment, public, api_key_env, prompts, timeout,
                                max_repairs, request or request_model): (source, fingerprint) for source, fingerprint in pending}
            for future in as_completed(jobs):
                source, fingerprint = jobs[future]
                try:
                    rows = future.result()
                except Exception as exc:
                    failures += 1
                    # 固定白名单错误码可供离线排障；绝不保存异常正文或提供方返回体。
                    code = str(exc) if isinstance(exc, PreparationError) else "request_failed"
                    errors[source["task_id"]] = code if code in SAFE_FAILURES else "request_failed"
                    persist()
                    continue
                result = {"task_id": source["task_id"], "task_version": source["task_version"],
                          "turn_expectations": rows}
                results[source["task_id"]] = result
                verified[source["task_id"]] = {"fingerprint": fingerprint, "digest": _digest(result)}
                persist()
        document = persist()
        if failures:
            raise PreparationError("goal_preparation_incomplete")
        return document
