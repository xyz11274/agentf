#!/usr/bin/env python3
"""离线第二阶段：直接引用训练经验与原始 val/holdout，只运行 L3 GEPA。"""

from __future__ import annotations

import argparse
from collections import Counter, defaultdict
from contextlib import contextmanager
from datetime import datetime, timezone
import fcntl
import hashlib
import json
import os
from pathlib import Path
import secrets
import select
import shutil
import signal
import urllib.error
import subprocess
import sys
import time
import urllib.request

import layout
import run as coverage
from prepare_dataset import host_revision, prepare_configuration


ROOT = coverage.ROOT
STAGE1 = layout.TRAINING
STAGE2 = layout.GEPA
DATASET = layout.GEPA_VIEW
WORKSPACE = STAGE2 / "workspace"
CONFIG = coverage.CONFIG / "gepa-deployment" / "runtime"
PLAN = CONFIG.parent / "plan.json"
STATUS = STAGE2 / "status.json"
USER = "business-training"
LIFECYCLE_LOCK = layout.OUTPUT.parent / ".business-simulator-gepa.lock"

# fresh 会在运行中删除 STAGE2，因此锁不能放在 STAGE2 内。模块内嵌套调用
# （fresh -> publish）复用同一个文件描述符；其他进程仍会被内核文件锁拒绝。
_lifecycle_handle = None
_lifecycle_depth = 0

# 正式 fresh 入口一次性冻结这组容量；通用校验仍允许最多 256 轮，且已有
# 原生 100 轮状态机回归。patience=0 禁止在 30 轮预算前因连续无改善提前停树。
FRESH_SEARCH_LIMITS = {
    "timeout_hours": 50,
    "max_rounds": 30,
    "max_model_calls": 1024,
    # Full-pool grouped diagnosis, synthesis and review share this offline ceiling.
    # Reserve 30 rounds, three final holdouts and retry headroom.
    # This is a ceiling, not a requirement to consume additional API calls.
    "max_trials": 6500,
    "patience": 0,
}
FRESH_MAX_RECOVERIES = 16
FRESH_RETRYABLE_JOB_STATES = {
    "error", "interrupted", "inconclusive", "cancelled", "pending", "running",
}
FRESH_INITIAL_CONFIG_ERRORS = {
    "gepa_config_invalid", "gepa_strategy_invalid", "gepa_improvement_invalid",
    "gepa_objective_invalid", "gepa_quality_weight_required",
    "gepa_configuration_path_required", "gepa_memory_feedback_identity_invalid",
}


def initial_config_error_since(offset: int) -> str:
    """只检查本次初始启动新写入的明确配置诊断，避免把旧日志当成本次失败。"""
    try:
        with (STAGE2 / "learning.log").open("rb") as log:
            log.seek(offset)
            for line in log:
                if line.startswith(b"ERROR: "):
                    code = line[7:].decode("utf-8", "replace").strip()
                    if code in FRESH_INITIAL_CONFIG_ERRORS:
                        return code
    except OSError:
        pass
    return ""


@contextmanager
def lifecycle_lock():
    """串行化一棵树从清理旧产物到发布终态的完整生命周期。"""
    global _lifecycle_handle, _lifecycle_depth
    if _lifecycle_handle is not None:
        _lifecycle_depth += 1
        try:
            yield
        finally:
            _lifecycle_depth -= 1
        return

    LIFECYCLE_LOCK.parent.mkdir(parents=True, exist_ok=True)
    if LIFECYCLE_LOCK.is_symlink():
        raise RuntimeError("gepa_lifecycle_lock_invalid")
    descriptor = os.open(LIFECYCLE_LOCK, os.O_CREAT | os.O_RDWR | os.O_NOFOLLOW, 0o600)
    handle = os.fdopen(descriptor, "a+")
    try:
        try:
            fcntl.flock(handle, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError as exc:
            raise RuntimeError("gepa_lifecycle_active") from exc
        _lifecycle_handle = handle
        _lifecycle_depth = 1
        try:
            yield
        finally:
            _lifecycle_depth = 0
            _lifecycle_handle = None
            fcntl.flock(handle, fcntl.LOCK_UN)
    finally:
        handle.close()


def _signal_process_group(child, signum: int) -> None:
    """只向本运行器创建的新会话发送信号，进程已结束时保持幂等。"""
    if child is None or child.poll() is not None:
        return
    if not hasattr(child, "pid"):
        if signum == signal.SIGTERM and hasattr(child, "terminate"):
            child.terminate()
        elif signum == signal.SIGKILL and hasattr(child, "kill"):
            child.kill()
        else:
            child.send_signal(signum)
        return
    try:
        os.killpg(child.pid, signum)
    except ProcessLookupError:
        return


def _stop_process_group(child, timeout: float = 180.0) -> None:
    """先优雅停止整个子进程组，超时后强制回收，禁止遗留孤儿服务。"""
    if child is None:
        return
    if child.poll() is None:
        _signal_process_group(child, signal.SIGTERM)
        try:
            child.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            _signal_process_group(child, signal.SIGKILL)
            child.wait(timeout=30)
    else:
        child.wait()


def environment() -> dict[str, str]:
    env = coverage.environment()
    env.setdefault("EVOLUTION_BUSINESS_TOKEN", secrets.token_urlsafe(32))
    env["TRAJECTORY_UNCONDITIONAL_RESUME"] = "1"
    # 业务原生库链接到 agentflow-dev 的 OpenSSL/libcurl。优先让所有
    # ``python3`` 子进程使用同一环境；否则从 base Python 启动时，先加载的
    # base libssl 会与 agentflow-dev 的 libcurl 版本冲突，导致启动门禁失败。
    candidates = [
        os.environ.get("AGENTFLOW_PYTHON", ""),
        "/root/miniconda3/envs/agentflow-dev/bin/python3",
        sys.executable,
    ]
    interpreter = next((Path(value) for value in candidates if value and Path(value).is_file()),
                       Path(sys.executable))
    env["AGENTFLOW_PYTHON"] = str(interpreter)
    env["PATH"] = str(interpreter.parent) + os.pathsep + env.get("PATH", "")
    runtime_lib = interpreter.parent.parent / "lib"
    if runtime_lib.is_dir():
        env["LD_LIBRARY_PATH"] = str(runtime_lib) + os.pathsep + env.get("LD_LIBRARY_PATH", "")
    return env


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def runtime_hashes(env: dict[str, str]) -> dict[str, str]:
    """冻结会实际执行的二进制和 Python 入口，不把凭据写入计划。"""
    daemon = Path(env.get("AGENTFLOW_DAEMON", str(ROOT / "build/agentflowd")))
    if not daemon.is_absolute():
        daemon = ROOT / daemon
    files = {
        Path(__file__).resolve(),
        Path(__file__).with_name("offline_publish.py").resolve(),
        Path(__file__).with_name("layout.py").resolve(),
        Path(__file__).with_name("run.py").resolve(),
        coverage.BINARY.resolve(),
        (ROOT / "build/trajectory-evolution/libtrajectory_evolution_local.so").resolve(),
        daemon.resolve(),
    }
    files.update(path.resolve() for path in (coverage.PACKAGE / "evals").glob("*.py"))
    files.update(path.resolve() for path in (ROOT / "benchmark").glob("*.py"))
    result = {}
    for path in sorted(files):
        if not path.is_file():
            raise RuntimeError("gepa_runtime_file_missing:" + str(path))
        try:
            name = str(path.relative_to(ROOT))
        except ValueError:
            name = str(path)
        result[name] = digest(path)
    return result


def records(directory: Path, split: str) -> list[dict]:
    return [json.loads(line) for line in (directory / (split + ".jsonl")).read_text().splitlines()
            if line.strip()]


def checked_stage1() -> Path:
    state = coverage.read(STAGE1 / "status.json")
    if state.get("status") != "completed" or state.get("captured_tasks") != 400:
        raise RuntimeError("training_stage_not_complete")
    source = STAGE1 / "evolution"
    if not (source / f"by-user/{USER}/memory/evolution-memory.pb").is_file():
        raise RuntimeError("training_memory_missing")
    for name, expected in coverage.read(coverage.CONFIG / "coldstart-deployment/plan.json")["dataset_files"].items():
        if digest(layout.TRAINING_DATASET / name) != expected:
            raise RuntimeError("training_dataset_changed")
    return source


def build_view(goal_contracts_file: Path | None = None) -> dict:
    """只保存原 val 的 150/50 任务 ID；原 train 和任务正文均不进入阶段二。"""
    source = layout.SOURCE_DATASET
    manifest = coverage.read(source / "manifest.json")
    version = manifest["metadata"]["dataset_version"]
    val = records(source, "val")
    holdout = records(source, "holdout")
    if len(val) != 200 or len(holdout) != 200 or len({t["task_id"] for t in val}) != 200:
        raise RuntimeError("source_val_holdout_changed")
    groups: dict[str, list[str]] = defaultdict(list)
    for task in val:
        groups[task["group_id"]].append(task["task_id"])
    ordered = sorted(groups, key=lambda group: hashlib.sha256(
        (version + ":" + group).encode()).hexdigest())
    selection: list[str] = []
    for group in ordered:
        if len(selection) + len(groups[group]) <= 50:
            selection.extend(groups[group])
        if len(selection) == 50:
            break
    if len(selection) != 50:
        raise RuntimeError("validation_groups_cannot_split_150_50")
    selection_set = set(selection)
    search = sorted(task["task_id"] for task in val if task["task_id"] not in selection_set)
    view = {"schema_version": 1, "source": os.path.relpath(source, DATASET.parent),
            "source_version": version, "search_task_ids": search,
            "selection_task_ids": sorted(selection)}
    if goal_contracts_file is not None:
        # 子目标只供裁判使用，不进入 Agent 输入；正文仍由源数据集提供。
        view["goal_contracts_file"] = os.path.relpath(goal_contracts_file, DATASET.parent)
    if DATASET.exists():
        previous = coverage.read(DATASET)
        old_source = str(ROOT / "benchmark/datasets/business-simulator/source")
        if previous != view:
            # 允许旧视图一次性补充评分契约引用，但不变更分组、题目或源版本，
            # 也不允许把已有子目标引用静默撤回、换成另一套标准。
            ignored = {"source", "goal_contracts_file"}
            if previous.get("source") not in (old_source, view["source"]) or {
                    key: value for key, value in previous.items() if key not in ignored} != {
                    key: value for key, value in view.items() if key not in ignored} or (
                    previous.get("goal_contracts_file") not in (None, view.get("goal_contracts_file"))):
                raise RuntimeError("gepa_view_changed")
            coverage.save(DATASET, view)
    else:
        coverage.save(DATASET, view)
    return view


def check_dataset() -> dict:
    result = subprocess.run([str(coverage.BINARY), "dataset", "check", str(DATASET)],
                            cwd=ROOT, capture_output=True, text=True, check=True)
    report = json.loads(result.stdout)
    if report.get("review_complete") is not True or report.get("splits") != {
            "train": 150, "val": 50, "holdout": 200}:
        raise RuntimeError("gepa_view_not_ready")
    view = coverage.read(DATASET)
    original_val = {task["task_id"] for task in records(layout.SOURCE_DATASET, "val")}
    if set(view["search_task_ids"]) | set(view["selection_task_ids"]) != original_val or (
            set(view["search_task_ids"]) & set(view["selection_task_ids"])):
        raise RuntimeError("gepa_view_leaks_or_omits_val")
    return report


def memory_hashes(source: Path) -> dict[str, str]:
    """记录启动时的来源指纹；GEPA 反馈写回这一权威记忆，不另建副本。"""
    files = [
        f"by-user/{USER}/memory/evolution-memory.pb",
        f"by-user/{USER}/memory/evolution-memory.json",
        "learning/shared-memory.json",
    ]
    result = {}
    for name in files:
        origin = source / name
        if not origin.is_file() or origin.is_symlink():
            raise RuntimeError("memory_source_not_available:" + name)
        result[name] = digest(origin)
    return result


def prepare(seed_report: Path | None = None, baseline_report: Path | None = None,
            timeout_hours: float | None = None, max_rounds: int | None = None,
            max_model_calls: int | None = None, max_trials: int | None = None,
            patience: int | None = None) -> dict:
    """进入 GEPA 阶段时创建配置和产物；搜索冻结策略版本，反馈回写原记忆。"""
    layout.ensure_layout()
    reports = {"seed": seed_report, "baseline": baseline_report}
    if PLAN.is_file():
        for kind, path in reports.items():
            if path is not None:
                frozen = coverage.read(CONFIG / "gepa.json").get(kind + "_report_file", "")
                if not frozen or (CONFIG / frozen).resolve() != path.resolve():
                    raise RuntimeError("gepa_" + kind + "_report_changed")
        # An interrupted run is resumable even after a local code/config
        # refresh.  The checkpoint and task identity remain the recovery
        # boundary; mutable fingerprints must not turn a recoverable job into
        # a manual cleanup task.
        return verify()
    for kind, path in reports.items():
        if path is not None:
            report = coverage.read(path)
            if not report.get("complete") or not report.get("trials"):
                raise RuntimeError("gepa_" + kind + "_report_incomplete")
    source = checked_stage1()
    if CONFIG.exists() or STAGE2.exists():
        raise RuntimeError("gepa_partial_output_requires_review")
    env = environment()
    # 正式实验沿用已冻结标准；未经确认的模型子目标不是启动前置，
    # 也不能自动替换原验收口径。
    build_view()
    dataset = check_dataset()
    memory = memory_hashes(source)
    private = coverage.read(source / f"by-user/{USER}/memory/evolution-memory.json")["entries"]
    # PROPOSED 只是候选草稿；INSUFFICIENT_SUPPORT 或单条对话证据不能作为
    # 离线进化输入的质量证明。这里仅检查是否存在达到最低支持量的候选，
    # 真正可注入的集合仍由 validated/published snapshot 决定。
    if not any(
            item.get("candidate", {}).get("status") == "EVOLUTION_CANDIDATE_STATUS_PROPOSED"
            and int(item.get("candidate", {}).get("support_dialogue_count", "0")) >= 2
            and int(item.get("candidate", {}).get("support_episode_count", "0")) >= 2
            for item in private):
        raise RuntimeError("training_has_no_proposed_experience")

    prepare_configuration(CONFIG, DATASET)
    # 本例使用支持 JSON 响应格式的兼容 API；评分、反思都按 JSON 契约返回。
    # 在冻结计划前写入，避免仅诊断生效而正式实验仍退回不受约束的输出格式。
    runtime_file = CONFIG / "runtime.json"
    runtime = coverage.read(runtime_file)
    runtime["model"]["json_object_output"] = True
    # Long business trajectories can legitimately take more than two minutes
    # at the judge boundary.  A short client timeout turns a recoverable model
    # delay into an inconclusive seed and wastes the whole evaluation wave.
    runtime["model"]["timeout_ms"] = max(
        300000, int(runtime["model"].get("timeout_ms", 0)))
    runtime["model"]["max_retries"] = 4
    # Freeze the model actually selected for this run.  The provider switch
    # may change both endpoint and model; leaving an old plan-only model in
    # runtime.json makes every judge/reflection request fail with HTTP 404.
    selected_endpoint = env.get("AF_LLM_BASE_URL", "").rstrip("/")
    if selected_endpoint:
        runtime["model"]["endpoint"] = selected_endpoint + "/chat/completions"
    selected_model = env.get("AF_LLM_MODEL") or env.get("OPENAI_MODEL")
    if selected_model:
        runtime["model"]["model_id"] = selected_model
    runtime["model"]["api_key_env"] = "OPENAI_API_KEY"
    coverage.save(runtime_file, runtime)
    service_file = CONFIG / "service.json"
    service = coverage.read(service_file)
    service.update(data_dir=str(WORKSPACE.relative_to(ROOT)),
                   memory_source_dir=str(source.relative_to(ROOT)),
                   max_level="L3", auto_evaluate=True,
                   # 经验必须沿用已经 validated/published 的批准集合；离线评测不应
                   # 把 snapshot 中的未验证候选自动升级成可注入经验。
                   auto_approve_experiences=True, evaluation_levels=["L3"])
    service["instrumentation"]["level"] = "L3"
    service["workers"] = 16
    # Only the validated/published snapshot is auto-approved. Disabling
    # auto-approval without explicit approvals prevents evaluation-only jobs
    # from starting; unvalidated training candidates remain excluded below.
    service.setdefault("shadow_options", {})["require_validated_evidence"] = True
    service["shadow_options"]["max_policy_entries"] = min(
        int(service["shadow_options"].get("max_policy_entries", 128)), 64)
    service["shadow_options"]["max_advice_items"] = min(
        int(service["shadow_options"].get("max_advice_items", 3)), 2)
    service["shadow_options"]["max_advice_bytes"] = min(
        int(service["shadow_options"].get("max_advice_bytes", 4096)), 2048)
    service["experience_template_file"] = "prompts/runtime-experience.txt"
    service["control_learning"]["prompt_file"] = "prompts/control-candidates.txt"
    service["sharing"] = coverage.read(
        coverage.CONFIG / "coldstart-deployment/runtime/service.json")["sharing"]
    for key, filename in {"distill": "shared-experience-distill.txt",
                          "review": "shared-experience-review.txt",
                          "relation": "cross-memory-relation.txt"}.items():
        service["sharing"][key + "_prompt_file"] = "prompts/" + filename
    service["sharing"]["relation_batch_prompt_file"] = "prompts/cross-memory-batch.txt"
    coverage.save(service_file, service)

    assessment_file = CONFIG / "agent-evaluation.json"
    assessment = coverage.read(assessment_file)
    assessment["dataset"] = os.path.relpath(DATASET, CONFIG)
    assessment["training"]["enabled"] = False
    assessment["min_interval_ms"] = 0
    assessment["max_jobs_per_hour"] = 32
    # 业务基线本身可能低于固定绝对分数；这里只保留相对 L0 不退化约束。
    # 旧模板中的绝对门槛也显式删除，避免重新 prepare 后悄悄恢复 0.8。
    assessment["gate"].pop("min_quality", None)
    assessment["gate"].pop("min_pass_rate", None)
    # 业务离线目标必须进入反思契约和最终验收，而不只留在实验说明里。
    # 模板若已显式指定要求，保留原值；质量和覆盖门槛始终沿用模板。
    assessment["gate"].setdefault("min_invalid_loop_reduction", 0.15)
    assessment["gate"].setdefault("max_latency_ratio", 1.0)
    assessment["gate"].setdefault("max_token_ratio", 1.0)
    coverage.save(assessment_file, assessment)

    evaluation_file = CONFIG / "evaluation.json"
    evaluation = coverage.read(evaluation_file)
    # 业务约定要求训练和 GEPA evaluator 均保持 16 路并发；请求超时由
    # runtime 的 300 秒边界和已有重试处理，不能用降低并发改变实验口径。
    evaluation["parallelism"] = 16
    # 最小测试/用户配置可能省略 pass_env；把其缺省视为空列表，避免启动前
    # 因配置形状差异直接崩溃。
    evaluation["variants"][0].setdefault("pass_env", [])
    for key in ("AF_LLM_MAX_RETRIES", "AF_LLM_RETRY_BASE_DELAY"):
        if key not in evaluation["variants"][0]["pass_env"]:
            evaluation["variants"][0]["pass_env"].append(key)
    evaluation["judge"]["enabled"] = True
    # 业务单轮也可能含上百个事件；预览保留原始路径，裁判可按需补查完整证据。
    # 批次仍按输入上限分裂，避免仅因固定的 48 KiB 软限制而整题误判失败。
    evaluation["judge"]["evidence_field_bytes"] = 256
    evaluation["judge"]["answer_field_bytes"] = 1024
    evaluation["judge"]["batch_input_bytes"] = 262144
    evaluation["judge"]["max_repair_attempts"] = 2
    evaluation["judge"]["prompt"] = "prompts/task-judge-system.txt"
    evaluation["judge"]["loop_review_prompt"] = "prompts/task-loop-review.txt"
    evaluation["judge"]["consistency_prompt"] = "prompts/task-consistency.txt"
    # 评测完成后只保留 canonical report.json/report.pb 与 manifest；重复的
    # results.jsonl、Markdown 和逐题副本仅在调试配置中保留。运行中仍写逐题
    # checkpoint，异常可恢复；完整报告落盘后由内核安全清除重复文件。
    evaluation["artifact_retention"] = {
        "retain_trial_files": False,
        "retain_markdown": False,
        "retain_jsonl": False,
        "retain_report_pb": True,
        "drop_intermediate_pb": True,
    }
    script = coverage.PACKAGE / "evals/run_product_eval.py"
    evaluation["variants"][0]["command"] = [
        "python3", "{config_dir}/" + os.path.relpath(script, CONFIG), "--evolution-entry"]
    coverage.save(evaluation_file, evaluation)

    gepa_file = CONFIG / "gepa.json"
    gepa = coverage.read(gepa_file)
    if gepa["minibatch_size"] != 32 or gepa["merge_minibatch_size"] != 16:
        raise RuntimeError("gepa_search_profile_mismatch")
    # 每个模型请求仍保持有界；COMBEE 从完整父批次分组覆盖所有题，
    # 8 是单个请求的行数上限，不再是整个节点能看到的题数上限。
    gepa["max_feedback_trials"] = 8
    gepa["reflection_strategy"] = "combee"
    gepa["duplication_factor"] = 1
    gepa["screening_max_quality_drop"] = 0.03
    gepa["proposal_selection"] = "top_k"
    gepa["proposal_top_k"] = 2
    # 中间节点保留真实局部收益，供后续改写和多父融合；
    # 最终质量、循环和留出门槛仍由独立发布验收约束。
    gepa["acceptance"] = "pareto"
    gepa["prompt_file"] = "prompts/gepa-reflection.txt"
    # 搜索目标必须与发布门槛同向：历史配置 latency_weight=0，导致候选
    # 即使时延明显上升仍可能在 validation 排名前列。只调整搜索排序，不改变
    # 原始成绩或验收阈值；质量/loop 证据仍由独立 holdout gate 裁决。
    objective = gepa.setdefault("objective", {})
    objective["invalid_loop_weight"] = max(float(objective.get("invalid_loop_weight", 0.2)), 0.20)
    objective["token_weight"] = max(float(objective.get("token_weight", 0.05)), 0.10)
    objective["latency_weight"] = max(float(objective.get("latency_weight", 0.0)), 0.15)
    # 四类 L1-L3 组件至少各有两次探索机会，并为结构修复保留独立调用预算。
    gepa["max_rounds"] = max(12, int(gepa.get("max_rounds", 0)))
    gepa["max_model_calls"] = max(16, int(gepa.get("max_model_calls", 0)))
    # 将默认预算也写回配置，确保计划记录与配置哈希共同冻结完整搜索容量。
    gepa.setdefault("timeout_ms", 1800000)
    gepa.setdefault("max_trials", 400)
    gepa.setdefault("patience", 0)
    gepa.update(search_limits(timeout_hours, max_rounds, max_model_calls,
                              max_trials, patience))
    # 200 题留出需要保存候选与基线两组完整证据；搜索的 128 MiB 配置保持不变。
    gepa["holdout_max_report_bytes"] = 1073741824
    # 冻结 validation 排名的前三个收益候选分别进入同一留出集。
    gepa["holdout_top_k"] = 3
    # 基线与种子均只引用完整复评；内核逐项核对版本、题集和裁判，不复制执行轨迹。
    for kind, path in reports.items():
        if path is not None:
            gepa[kind + "_report_file"] = os.path.relpath(path.resolve(), CONFIG)
    coverage.save(gepa_file, gepa)

    config_files = {str(path.relative_to(CONFIG)): digest(path) for path in CONFIG.rglob("*")
                    if path.is_file()}
    plan = {
        "stage": "GEPA L3 search and final holdout",
        "source_memory": str(source.relative_to(ROOT)),
        "source_memory_hashes": memory,
        "source_dataset": str(layout.SOURCE_DATASET.relative_to(ROOT)),
        "source_dataset_version": coverage.read(DATASET)["source_version"],
        "dataset_view": str(DATASET.relative_to(ROOT)),
        "dataset_view_hash": digest(DATASET),
        "dataset_version": dataset["dataset_version"],
        "split_counts": dataset["splits"],
        "search_limits": {name: int(gepa[name]) for name in (
            "timeout_ms", "max_rounds", "max_model_calls", "max_trials", "patience")},
        "evaluation_parallelism": int(evaluation["parallelism"]),
        "runtime_hashes": runtime_hashes(env),
        "config_files": config_files,
        "service": str(service_file.relative_to(ROOT)),
        "review_scope": "GEPA view only; original dataset remains pending",
        "created_at": datetime.now(timezone.utc).isoformat(),
    }
    STAGE2.mkdir(parents=True)
    coverage.save(PLAN, plan)
    coverage.save(STATUS, {"status": "prepared", "updated_at": plan["created_at"]})
    return plan


def verify() -> dict:
    source = checked_stage1()
    plan = coverage.read(PLAN)
    # Recovery is deliberately checkpoint based.  Earlier versions compared
    # every generated JSON, source hash and host revision here; a harmless
    # edit made an interrupted job unrecoverable after thousands of calls.
    # Keep only path/shape checks below.  Task IDs and report structure are
    # still validated by the evaluator before a score is accepted.
    if plan.get("source_memory") != str(source.relative_to(ROOT)):
        raise RuntimeError("gepa_memory_source_missing")
    if not DATASET.is_file() or not CONFIG.is_dir():
        raise RuntimeError("gepa_recovery_input_missing")
    service = coverage.read(CONFIG / "service.json")
    if service.get("memory_source_dir") != plan["source_memory"] or (
            service.get("data_dir") != str(WORKSPACE.relative_to(ROOT))):
        raise RuntimeError("gepa_memory_binding_changed")
    if (WORKSPACE / f"by-user/{USER}/memory/evolution-memory.pb").exists():
        raise RuntimeError("gepa_input_memory_copied")
    check_dataset()
    return plan


def progress(job_id: str = "") -> dict:
    """读取指定作业；未指定时才返回同用户最新的 L3 作业。"""
    state_file = WORKSPACE / "agent-evaluation/state.json"
    if not state_file.is_file():
        return {"phase": "waiting_for_gepa_job"}
    try:
        state = coverage.read(state_file)
    except (OSError, ValueError):
        return {"phase": "loading_gepa_state"}
    jobs = [(key, value) for key, value in state.get("jobs", {}).items()
            if value.get("user") == USER and value.get("level") == "L3"]
    if job_id:
        jobs = [pair for pair in jobs if pair[0] == job_id]
    if not jobs:
        return {"phase": "waiting_for_gepa_job"}
    job_id, job = max(jobs, key=lambda pair: pair[1].get("queued_ms", 0))
    summary = {"job_id": job_id, "job_status": job.get("status"),
               "gepa": job.get("gepa", False),
               "published_in_offline_workspace": job.get("published", False)}
    # 搜索检查点只是证据收集；只有管理层的 committed 回执才证明原库已提交。
    # 状态查询不读取大轨迹或记忆全文，也不触发任何写入和模型调用。
    delivery = job.get("memory_feedback") or {}
    summary["memory_feedback"] = {
        "status": delivery.get("status", "none"),
        "records": delivery.get("records", 0),
    }
    if delivery.get("status") == "committed":
        summary["memory_feedback"]["memory_revision"] = (
            delivery.get("result", {}).get("memory_revision"))
    tree_file = WORKSPACE / "agent-evaluation" / job_id / "search/tree.json"
    if tree_file.is_file():
        try:
            tree = coverage.read(tree_file)
            summary.update(tree_status=tree.get("status"), nodes=len(tree.get("nodes", [])),
                           rounds=tree.get("rounds", 0),
                           node_states=dict(Counter(node.get("status") for node in tree.get("nodes", []))))
            records = (tree.get("memory_feedback") or {}).get("records", {})
            summary["memory_feedback"]["record_states"] = dict(Counter(
                record.get("status", "unknown") for record in records.values()))
            if not delivery and records:
                summary["memory_feedback"].update(status="collected", records=len(records))
        except (OSError, ValueError):
            pass
    return summary


def _best_effort_terminal(job_id: str, current: dict) -> bool:
    """Return whether search is exhausted but a scored candidate is durable.

    A rejected GEPA search must still leave an architect-readable best policy.
    Restrict this branch to a quiescent checkpoint with an explicit budget
    exhaustion/search-failed marker; transient model or holdout failures remain
    resumable and therefore continue through the normal retry path.
    """
    if not job_id or current.get("job_id") != job_id or current.get("job_status") != "error":
        return False
    state_file = WORKSPACE / "agent-evaluation/state.json"
    tree_file = WORKSPACE / "agent-evaluation" / job_id / "search/tree.json"
    if not state_file.is_file() or not tree_file.is_file():
        return False
    try:
        state = coverage.read(state_file)
        tree = coverage.read(tree_file)
    except (OSError, ValueError, TypeError):
        return False
    job = (state.get("jobs") or {}).get(job_id, {})
    error = str(job.get("error", ""))
    return (
        tree.get("active") is None and tree.get("batch") is None and
        bool(tree.get("nodes")) and
        (error in {"gepa_trial_budget_exhausted", "gepa_search_failed"} or
         (tree.get("error") == "gepa_search_failed" and
          tree.get("stop_reason") in {"max_rounds", "model_budget", "patience"}))
    )


def _rejected_completed_showcase(job_id: str, current: dict) -> bool:
    """Expose a completed, holdout-scored tree after formal gate rejection.

    This is a read-only terminal path for the architect showcase. It never
    retries search, replays holdout, or upgrades the formal publication state.
    """
    if (not job_id or current.get("job_id") != job_id or
            current.get("job_status") != "rejected"):
        return False
    state_file = WORKSPACE / "agent-evaluation/state.json"
    tree_file = WORKSPACE / "agent-evaluation" / job_id / "search/tree.json"
    if not state_file.is_file() or not tree_file.is_file():
        return False
    try:
        state = coverage.read(state_file)
        tree = coverage.read(tree_file)
    except (OSError, ValueError, TypeError):
        return False
    job = (state.get("jobs") or {}).get(job_id, {})
    final = tree.get("final_holdout") or {}
    final_candidates = final.get("candidates") or []
    return (
        job.get("status") == "rejected" and tree.get("status") == "completed" and
        tree.get("active") is None and tree.get("batch") is None and
        final.get("status") == "completed" and bool(final_candidates) and
        all(candidate.get("status") == "complete" for candidate in final_candidates
            if isinstance(candidate, dict)) and
        all(isinstance(candidate, dict) for candidate in final_candidates)
    )


def can_retry_initial_launch() -> bool:
    """仅在能够证明尚未创建本用户 L3 树时，允许重启初始入口。

    状态文件正在写入、损坏或结构未知时一律返回 False；即使状态尚未落盘，
    只要任一 search 目录已经出现，也视为可能存在树，避免启动第二个 job。
    """
    evaluation_root = WORKSPACE / "agent-evaluation"
    if evaluation_root.is_symlink():
        return False
    if evaluation_root.exists() and not evaluation_root.is_dir():
        return False
    if evaluation_root.is_dir() and any(evaluation_root.glob("*/search")):
        return False

    state_file = evaluation_root / "state.json"
    if not state_file.exists():
        return not state_file.is_symlink()
    if not state_file.is_file() or state_file.is_symlink():
        return False
    try:
        state = coverage.read(state_file)
    except (OSError, ValueError, TypeError):
        return False
    jobs = state.get("jobs") if isinstance(state, dict) else None
    if not isinstance(jobs, dict):
        return False
    for job in jobs.values():
        if not isinstance(job, dict):
            return False
        if job.get("user") == USER and job.get("level") == "L3":
            return False
    return True


def has_final_holdout_evidence(tree: dict) -> bool:
    """只把已启动标记或非空留出计划视为真实留出证据。

    `selected_version` 在搜索预算耗尽时也会保存当前种子/候选，不能据此判定
    已经查看留出集；否则合法的同树续跑会被错误拒绝。
    """
    final_holdout = tree.get("final_holdout")
    return bool(tree.get("holdout_started", False)) or (
        isinstance(final_holdout, dict) and bool(final_holdout)
    )


def has_derived_holdout_request(job_id: str, current: dict) -> bool:
    """识别已经冻结并请求的派生 top-k 留出评测。

    服务中断会把作业/搜索树分别落成 ``error``/``cancelled``，但非空的
    ``final_holdout`` 已经越过搜索边界。此时只能恢复同一次留出评测，不能
    再扩搜索预算，也不能重复发送 ``replay_holdout`` 创建第二次留出请求。
    """
    if (not job_id or current.get("job_id") != job_id
            or current.get("job_status") not in (
                "error", "interrupted", "inconclusive", "cancelled", "pending", "running")):
        return False
    state_file = WORKSPACE / "agent-evaluation/state.json"
    tree_file = WORKSPACE / "agent-evaluation" / job_id / "search/tree.json"
    if not state_file.is_file() or not tree_file.is_file():
        return False
    try:
        state = coverage.read(state_file)
        tree = coverage.read(tree_file)
    except (OSError, ValueError, TypeError):
        return False
    job = (state.get("jobs") or {}).get(job_id, {})
    replay = job.get("final_holdout_replay") or {}
    final_holdout = tree.get("final_holdout")
    derived = tree.get("derived_selection_protocol") or {}
    return (job.get("status") == current.get("job_status")
            and replay.get("protocol") == "derived-top-k-v2"
            and replay.get("state") != "completed"
            and tree.get("holdout_started") is True
            and tree.get("status") in ("searching", "cancelled", "error", "inconclusive")
            and isinstance(final_holdout, dict)
            and final_holdout.get("protocol") == "derived-top-k-v2"
            and final_holdout.get("status") in ("prepared", "running", "inconclusive")
            and bool(final_holdout.get("candidates"))
            and derived.get("protocol") == "derived-top-k-v2")


def extend_retry_budget(job_id: str, current: dict | None = None) -> bool:
    """仅为预算耗尽且未看留出集的最新 GEPA 作业扩展搜索，不改变数据或门槛。"""
    current = current if current is not None else progress(job_id)
    if current.get("job_id") != job_id or current.get("job_status") != "rejected":
        return False
    tree_file = WORKSPACE / "agent-evaluation" / job_id / "search/tree.json"
    tree = coverage.read(tree_file)
    if (tree.get("status") != "validation_rejected"
            or has_final_holdout_evidence(tree)
            or tree.get("active") is not None
            or tree.get("batch") is not None
            or tree.get("stop_reason") not in (
                "model_budget", "max_rounds", "gepa_search_time_reserved", "patience")):
        raise RuntimeError("gepa_job_not_budget_extendable")
    config_file = CONFIG / "gepa.json"
    config = coverage.read(config_file)
    old_rounds = int(config.get("max_rounds", 0))
    old_calls = int(config.get("max_model_calls", 0))
    old_timeout = int(config.get("timeout_ms", 1800000))
    old_patience = int(config.get("patience", 4))

    # 上一次 retry 请求可能在管理服务侧失败。此时配置已扩容，但搜索树尚未
    # 消耗新增预算；重复执行命令必须复用这次扩容，不能再次把预算叠加一轮。
    plan = coverage.read(PLAN)
    extensions = plan.get("budget_extensions", [])
    last_extension = extensions[-1] if extensions else {}
    stop_reason = tree.get("stop_reason")
    last_current = last_extension.get("current", {})
    already_extended = (
        last_extension.get("job_id") == job_id
        and int(last_current.get("max_rounds", -1)) == old_rounds
        and int(last_current.get("max_model_calls", -1)) == old_calls
        and int(last_current.get("timeout_ms", old_timeout)) == old_timeout
        and int(last_current.get("patience", old_patience)) == old_patience
        and (stop_reason != "gepa_search_time_reserved"
             or "timeout_ms" in last_current)
        and (stop_reason != "patience" or "patience" in last_current)
    )
    now_ms = int(datetime.now(timezone.utc).timestamp() * 1000)
    deadline_ms = (int(tree.get("started_ms", now_ms)) + old_timeout
                   + int(tree.get("paused_ms", 0)))
    minimum_remaining_ms = max(3 * 3600000, old_timeout // 4)
    has_remaining_budget = (
        stop_reason == "model_budget"
        and int(tree.get("model_calls", 0)) < old_calls
    ) or (
        stop_reason == "max_rounds"
        and int(tree.get("rounds", 0)) < old_rounds
    ) or (
        stop_reason == "gepa_search_time_reserved"
        and deadline_ms - now_ms > minimum_remaining_ms
    ) or (
        stop_reason == "patience"
        and int(tree.get("stagnation", 0)) < old_patience
    )
    if already_extended and has_remaining_budget:
        return True

    used_rounds = int(tree.get("rounds", 0))
    used_calls = int(tree.get("model_calls", 0))
    used_stagnation = int(tree.get("stagnation", 0))
    if stop_reason == "gepa_search_time_reserved":
        # 已有未消费的轮次／调用预算时只补时间，不因一次时间保护重复扩轮。
        config["max_rounds"] = (old_rounds if old_rounds > used_rounds else
                                min(256, used_rounds + 4))
        config["max_model_calls"] = (old_calls if old_calls > used_calls else
                                     min(1024, used_calls + 8))
        elapsed_ms = max(0, now_ms - int(tree.get("started_ms", now_ms))
                         - int(tree.get("paused_ms", 0)))
        config["timeout_ms"] = min(604800000, max(
            old_timeout + 4 * 3600000, elapsed_ms + 4 * 3600000))
    elif stop_reason == "patience":
        # 连续无改善已经达到旧阈值时只增加显式耐心预算，保留尚未消费的总预算。
        config["max_rounds"] = (old_rounds if old_rounds > used_rounds else
                                min(256, used_rounds + 4))
        config["max_model_calls"] = (old_calls if old_calls > used_calls else
                                     min(1024, used_calls + 8))
        config["patience"] = min(256, max(old_patience + 4, used_stagnation + 4))
    else:
        config["max_rounds"] = min(256, max(old_rounds + 4, used_rounds + 4))
        config["max_model_calls"] = min(1024, max(old_calls + 8, used_calls + 8))
    new_timeout = int(config.get("timeout_ms", old_timeout))
    new_patience = int(config.get("patience", old_patience))
    if (config["max_rounds"] <= old_rounds
            and config["max_model_calls"] <= old_calls
            and new_timeout <= old_timeout
            and new_patience <= old_patience):
        raise RuntimeError("gepa_search_budget_limit")
    coverage.save(config_file, config)

    # 冻结计划保留每次扩展的审计记录；只更新这一份配置的指纹，其他输入不变。
    relative = str(config_file.relative_to(CONFIG))
    plan["config_files"][relative] = digest(config_file)
    plan.setdefault("budget_extensions", []).append({
        "job_id": job_id,
        "previous": {"max_rounds": old_rounds, "max_model_calls": old_calls,
                     "timeout_ms": old_timeout, "patience": old_patience},
        "current": {"max_rounds": config["max_rounds"],
                    "max_model_calls": config["max_model_calls"],
                    "timeout_ms": new_timeout, "patience": new_patience},
        "created_at": datetime.now(timezone.utc).isoformat(),
    })
    coverage.save(PLAN, plan)
    return True


def search_limits(timeout_hours: float | None = None,
                  max_rounds: int | None = None,
                  max_model_calls: int | None = None,
                  max_trials: int | None = None,
                  patience: int | None = None) -> dict[str, int]:
    """校验命令行搜索容量，并转换为可直接冻结到 GEPA 配置的字段。"""
    if timeout_hours is not None and not 0 < timeout_hours <= 168:
        raise RuntimeError("gepa_timeout_hours_invalid")
    limits = {"max_rounds": (max_rounds, 1, 256),
              "max_model_calls": (max_model_calls, 1, 1024),
              "max_trials": (max_trials, 4, 20000),
              "patience": (patience, 0, 256)}
    result: dict[str, int] = {}
    if timeout_hours is not None:
        result["timeout_ms"] = int(timeout_hours * 3600000)
    for name, (value, minimum, maximum) in limits.items():
        if value is not None:
            if isinstance(value, bool) or not minimum <= value <= maximum:
                raise RuntimeError("gepa_" + name + "_invalid")
            result[name] = value
    return result


def set_retry_limits(job_id: str, timeout_hours: float | None = None,
                     max_rounds: int | None = None,
                     max_model_calls: int | None = None,
                     patience: int | None = None,
                     max_trials: int | None = None) -> bool:
    """显式扩大同一搜索树的时间与搜索容量，并保留完整审计记录。

    这些参数只控制还能探索多少候选，不修改质量、循环、时延或发布门槛。
    仅允许单调增大，避免一次续跑把已有实验的搜索空间悄悄缩小。
    """
    requested = search_limits(timeout_hours, max_rounds, max_model_calls,
                              max_trials, patience)
    if not requested:
        return False

    config_file = CONFIG / "gepa.json"
    plan = coverage.read(PLAN)
    relative = str(config_file.relative_to(CONFIG))
    config = coverage.read(config_file)
    tree_file = WORKSPACE / "agent-evaluation" / job_id / "search/tree.json"
    tree = coverage.read(tree_file)
    if has_final_holdout_evidence(tree):
        raise RuntimeError("gepa_search_extension_after_holdout")

    previous = {
        "timeout_ms": int(config.get("timeout_ms", 1800000)),
        "max_rounds": int(config.get("max_rounds", 8)),
        "max_model_calls": int(config.get("max_model_calls", 8)),
        "patience": int(config.get("patience", 0)),
    }
    # 旧冻结配置可能尚未显式写出 max_trials；未请求该字段时保持原文件不变。
    if "max_trials" in config or max_trials is not None:
        previous["max_trials"] = int(config.get("max_trials", 400))
    current = dict(previous)
    current.update(requested)
    for name, value in current.items():
        if value < previous[name]:
            raise RuntimeError("gepa_search_limit_cannot_decrease:" + name)
    if current == previous:
        return False

    config.update(current)
    coverage.save(config_file, config)
    plan["config_files"][relative] = digest(config_file)
    plan.setdefault("budget_extensions", []).append({
        "job_id": job_id,
        "reason": "explicit_search_limits",
        "previous": previous,
        "current": current,
        "created_at": datetime.now(timezone.utc).isoformat(),
    })
    coverage.save(PLAN, plan)
    return True


def set_retry_timeout(job_id: str, timeout_hours: float | None) -> bool:
    """兼容旧调用：只显式扩大同一搜索树的总墙钟预算。"""
    return set_retry_limits(job_id, timeout_hours=timeout_hours)


def fresh_preflight() -> None:
    """删除旧树前先确认不可替代的训练输入与运行文件完整。"""
    source = checked_stage1()
    memory_hashes(source)
    val = records(layout.SOURCE_DATASET, "val")
    holdout = records(layout.SOURCE_DATASET, "holdout")
    if len(val) != 200 or len(holdout) != 200 or (
            len({task["task_id"] for task in val}) != 200
            or len({task["task_id"] for task in holdout}) != 200):
        raise RuntimeError("source_val_holdout_changed")
    runtime_hashes(environment())


def reset_fresh_outputs() -> None:
    """精确删除当前 GEPA 树和生成配置；训练、数据集与已发布版本不动。"""
    deployment = CONFIG.parent
    expected = (layout.GEPA.resolve(strict=False),
                (coverage.CONFIG / "gepa-deployment").resolve(strict=False))
    targets = (STAGE2, deployment)
    actual = tuple(path.resolve(strict=False) for path in targets)
    if actual != expected or STAGE2.name != "gepa" or deployment.name != "gepa-deployment":
        raise RuntimeError("gepa_fresh_cleanup_scope_invalid")
    protected = (STAGE1, DATASET, layout.SOURCE_DATASET, layout.TRAINING_DATASET,
                 layout.PUBLISHED)
    for target in targets:
        if target.is_symlink() or (target.exists() and not target.is_dir()):
            raise RuntimeError("gepa_fresh_cleanup_target_invalid:" + str(target))
        resolved = target.resolve(strict=False)
        if any(path.resolve(strict=False) == resolved or
               path.resolve(strict=False).is_relative_to(resolved) for path in protected):
            raise RuntimeError("gepa_fresh_cleanup_protected_path")

    locks = []
    try:
        for lock_file in (STAGE2 / "runner.lock", STAGE2 / "workspace/service.lock",
                          STAGE2 / "workspace/agent-evaluation/evaluation.lock"):
            if not lock_file.is_file():
                continue
            handle = lock_file.open("a")
            try:
                fcntl.flock(handle, fcntl.LOCK_EX | fcntl.LOCK_NB)
            except BlockingIOError as exc:
                handle.close()
                raise RuntimeError("gepa_fresh_output_active") from exc
            locks.append(handle)
        for target in targets:
            if target.exists():
                shutil.rmtree(target)
    finally:
        for handle in locks:
            handle.close()




def readiness() -> tuple[dict, dict[str, str]]:
    """不调用模型，只检查冻结输入、真实业务入口及本地原生运行环境。"""
    plan = verify()
    # status.json 是面向观察者的检查点：进程被 SIGKILL、宿主重启或终端异常
    # 断开时可能来不及写终态，不能把其中残留的 running 当成活锁。真正的并发
    # 所有权由 run() 随后获取的 runner.lock 保证；活动进程持锁时第二个运行器
    # 会立即失败，进程消失后内核自动释放锁，因此无需人工改写状态文件。
    env = environment()
    daemon = Path(env.get("AGENTFLOW_DAEMON", str(ROOT / "build/agentflowd")))
    if not daemon.is_absolute():
        daemon = ROOT / daemon
    native = ROOT / "build/trajectory-evolution/libtrajectory_evolution_local.so"
    if not coverage.BINARY.is_file() or not os.access(coverage.BINARY, os.X_OK) or (
            not daemon.is_file() or not os.access(daemon, os.X_OK)) or not native.is_file():
        raise RuntimeError("business_evaluation_binary_missing")
    entry = coverage.PACKAGE / "evals/run_product_eval.py"
    probe = subprocess.run(
        [env.get("AGENTFLOW_PYTHON", "python3"), "-c",
         "import ctypes,runpy,sys; runpy.run_path(sys.argv[2], run_name='gepa_preflight'); "
         "ctypes.CDLL(sys.argv[1])",
         str(native), str(entry)],
        cwd=ROOT, env=env, capture_output=True, text=True, timeout=30)
    if probe.returncode:
        raise RuntimeError("business_entry_runtime_unavailable")
    return plan, env


def request_retry(child, env: dict[str, str], job_id: str,
                  replay_holdout: bool = False) -> None:
    """仅向本次创建的回环管理服务请求重试，不另启 Agent 或改写检查点。"""
    ready, _, _ = select.select([child.stdout], [], [], 30)
    if not ready:
        raise RuntimeError("gepa_management_start_timeout")
    line = child.stdout.readline().strip()
    prefix = "trajectory-integration.v1 listening=127.0.0.1:"
    if not line.startswith(prefix) or not line[len(prefix):].isdigit():
        raise RuntimeError("gepa_management_start_failed")
    port = int(line[len(prefix):])
    if not 0 < port < 65536:
        raise RuntimeError("gepa_management_port_invalid")
    # 正常关服导致的 GEPA 中断会由服务启动自动恢复；不要再发 retry，
    # 否则 409 会被运行器误当成启动失败，并把刚恢复的同一作业再次终止。
    current = progress(job_id)
    if (current.get("job_id") == job_id
            and current.get("job_status") in ("pending", "running")):
        return
    packet = dict(operation=("agent_evaluation.replay_holdout" if replay_holdout
                             else "agent_evaluation.retry"),
                  request_id=secrets.token_hex(16), job_id=job_id)
    request = urllib.request.Request(
        f"http://127.0.0.1:{port}/v1/invoke", data=json.dumps(packet).encode(),
        headers={"Content-Type": "application/json",
                 "Authorization": "Bearer " + env["EVOLUTION_BUSINESS_TOKEN"]})
    # 回环管理请求不经过代理；凭据仅在进程内传递，不写进命令或日志。
    # 大检查点首次迁移可能需要核验数据集、产物和评分指纹，固定 30 秒会在
    # 服务已提交恢复事务后误报超时并触发清理。这里只调整本地管理请求，
    # 不改变远程模型超时或整轮实验预算。
    try:
        control_timeout = max(
            30.0, float(env.get("EVOLUTION_CONTROL_REQUEST_TIMEOUT_SECONDS", "180")))
    except ValueError as exc:
        raise RuntimeError("invalid_control_request_timeout") from exc
    try:
        with urllib.request.build_opener(urllib.request.ProxyHandler({})).open(
                request, timeout=control_timeout) as response:
            result = json.load(response)
    except urllib.error.HTTPError as exc:
        detail = exc.read(2048).decode("utf-8", "replace")
        raise RuntimeError(f"gepa_retry_http_{exc.code}:{detail}") from exc
    if result.get("ok") is not True:
        raise RuntimeError("gepa_retry_rejected")


def run(retry_job: str = "", timeout_hours: float | None = None,
        max_rounds: int | None = None, max_model_calls: int | None = None,
        patience: int | None = None, max_trials: int | None = None,
        allow_budget_extension: bool = True) -> int:
    # 避免同一离线任务同时续跑训练；GEPA 回写另用记忆文件锁。
    # 父策略由冻结产物保存，不依赖可变的记忆文件保持原样。
    training_lock = (STAGE1 / "runner.lock").open("a")
    fcntl.flock(training_lock, fcntl.LOCK_SH | fcntl.LOCK_NB)
    try:
        current = progress(retry_job) if retry_job else {}
        holdout_replay = bool(retry_job and current.get("job_id") == retry_job
                              and current.get("job_status") in (
                                  "rejected", "superseded", "pending", "running")
                              and current.get("tree_status") == "validation_rejected")
        derived_holdout_resume = has_derived_holdout_request(retry_job, current)
        holdout_only = holdout_replay or derived_holdout_resume
        requested_limits = (timeout_hours, max_rounds, max_model_calls, patience, max_trials)
        if holdout_only and any(value is not None for value in requested_limits):
            raise RuntimeError("gepa_holdout_replay_cannot_change_budget")
        if retry_job and not allow_budget_extension and any(
                value is not None for value in requested_limits):
            raise RuntimeError("gepa_frozen_run_cannot_change_budget")
        explicit_limits = False if holdout_only else set_retry_limits(
            retry_job, timeout_hours, max_rounds, max_model_calls, patience, max_trials
        ) if retry_job and allow_budget_extension else False
        # 显式限制已经是本次唯一预算变更，不再按旧 stop_reason 叠加自动扩容。
        extended_search = (False if holdout_only else
                           (explicit_limits or extend_retry_budget(retry_job, current)
                            if retry_job and allow_budget_extension else False))
        if holdout_only:
            plan, env = coverage.read(PLAN), environment()
        else:
            plan, env = readiness()
        if retry_job:
            allowed = ("error", "interrupted", "inconclusive", "cancelled", "superseded",
                       "pending", "running", "paused")
            if (current.get("job_id") != retry_job or (
                    current.get("job_status") not in allowed and not holdout_only and not (
                        extended_search and current.get("job_status") == "rejected"))):
                raise RuntimeError("gepa_job_not_retryable")
    except Exception:
        training_lock.close()
        raise
    lock = (STAGE2 / "runner.lock").open("a")
    fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
    state = {"status": "running", "pid": os.getpid(),
             "started_at": datetime.now(timezone.utc).isoformat()}
    command = [str(coverage.BINARY), "learn", "--evaluation-only", "--config", plan["service"]]
    if retry_job:
        command = [str(coverage.BINARY), "serve", "--config", plan["service"],
                   "--no-auto-evaluate"]
    child = None
    stopped_by = 0
    stop_deadline = 0.0

    def stop(signum, _frame):
        # 只转发给当前运行器创建的直接子进程；子进程负责安全收尾和检查点。
        nonlocal stopped_by, stop_deadline
        if stopped_by:
            return
        stopped_by = signum
        stop_deadline = time.monotonic() + 180.0
        if child is not None and child.poll() is None:
            _signal_process_group(child, signum)

    handlers = {signum: signal.signal(signum, stop)
                for signum in (signal.SIGTERM, signal.SIGINT)}
    try:
        coverage.save(STATUS, state)
        with (STAGE2 / "learning.log").open("a") as output:
            child = subprocess.Popen(command, cwd=ROOT, env=env, stdin=subprocess.DEVNULL,
                                     stdout=subprocess.PIPE if retry_job else output,
                                     stderr=output if retry_job else subprocess.STDOUT,
                                     text=bool(retry_job), start_new_session=True)
            if retry_job:
                if holdout_replay:
                    request_retry(child, env, retry_job, True)
                else:
                    request_retry(child, env, retry_job)
            if stopped_by:
                _signal_process_group(child, stopped_by)
            last = None
            while child.poll() is None:
                current = progress(retry_job)
                if current != last:
                    print("GEPA", json.dumps(current, ensure_ascii=False), flush=True)
                    state["progress"] = current
                    coverage.save(STATUS, state)
                    last = current
                if current.get("job_id") and not current.get("gepa"):
                    _stop_process_group(child, 30)
                    state.update(status="failed", error="L3_seed_did_not_start_gepa")
                    break
                # 重试使用现有管理服务持续持有同一作业；结束后先等反馈提交，
                # 再正常关闭服务，不为了切换入口而取消正在恢复的模型请求。
                if retry_job and current.get("job_id") == retry_job:
                    terminal = current.get("job_status")
                    feedback = current.get("memory_feedback", {}).get("status", "none")
                    # 作业失败也先等已完成配对的反馈落定；拒收/过期是投递终态，
                    # 应正常收尾但仍在下方报失败，不能假装反馈已经成功入库。
                    terminal_feedback = ("empty", "committed", "error", "failed",
                                         "rejected", "obsolete")
                    failed_job = terminal in ("error", "interrupted", "inconclusive",
                                              "cancelled", "superseded") and feedback in (
                                                  "none",) + terminal_feedback
                    settled_job = terminal in ("passed", "rejected") and feedback in terminal_feedback
                    if failed_job or settled_job:
                        _stop_process_group(child)
                        break
                if stopped_by and stop_deadline and time.monotonic() >= stop_deadline:
                    _stop_process_group(child, 0)
                    break
                time.sleep(10)
            child.wait()
            if not holdout_only:
                verify()  # 核对数据、配置和权威记忆路径；允许本轮反馈正常写回。
            current = progress(retry_job)
            state["progress"] = current
            if state["status"] != "failed":
                # CLI 退出只表示后台队列已收尾，不能替代作业及反馈提交的终态。
                # 拒绝候选也是完整评测；执行异常、未决评分、待入库则仍未完成闭环。
                complete = (child.returncode == 0 and current.get("gepa")
                            and current.get("job_status") in ("passed", "rejected")
                            and current.get("memory_feedback", {}).get("status", "none")
                            in ("empty", "committed"))
                state["status"] = ("paused" if stopped_by else
                                   "candidate_ready" if complete
                                   and current.get("job_status") == "passed"
                                   and current.get("published_in_offline_workspace")
                                   else "finished" if complete else "failed")
                if state["status"] == "failed" and child.returncode == 0:
                    state["error"] = "gepa_job_incomplete"
                state["exit_code"] = child.returncode
    except Exception as exc:
        state.update(status="failed", error=type(exc).__name__ + ":" + str(exc))
        raise
    finally:
        try:
            if child is not None:
                _stop_process_group(child)
                if getattr(child, "stdout", None) is not None:
                    child.stdout.close()
        finally:
            # 即使强制回收本身报错，也必须提交固定终态、释放两层锁并恢复信号处理器。
            state["finished_at"] = datetime.now(timezone.utc).isoformat()
            try:
                coverage.save(STATUS, state)
                print("FINAL", json.dumps(state, ensure_ascii=False), flush=True)
            finally:
                lock.close()
                training_lock.close()
                for signum, previous in handlers.items():
                    signal.signal(signum, previous)
    if stopped_by:
        return 128 + stopped_by
    return 0 if state["status"] in ("candidate_ready", "finished") else 1


def run_to_publication(publisher=None,
                       max_recoveries: int = FRESH_MAX_RECOVERIES,
                       initial_job_id: str = "") -> dict:
    """在冻结配置下把一个 job/树自动接续到反馈落定和原子发布。"""
    if max_recoveries < 0:
        raise ValueError("max_recoveries_must_be_nonnegative")
    # A resume must explicitly bind to an existing durable job.  Starting the
    # evaluation-only service with an empty job id enables auto-evaluate and
    # can create a fresh seed job, leaving a real checkpoint untouched.  Fresh
    # runs still pass the default empty value and create their initial job.
    job_id = initial_job_id
    recoveries = 0
    while True:
        failure = None
        log_path = STAGE2 / "learning.log"
        log_offset = log_path.stat().st_size if log_path.is_file() else 0
        try:
            exit_code = run(job_id, allow_budget_extension=False)
        except Exception as exc:  # 同一树若已有可恢复检查点，监督器继续接管。
            failure = exc
            exit_code = 1
        current = progress(job_id)
        observed_job = current.get("job_id", "")
        if job_id and observed_job != job_id:
            raise RuntimeError("gepa_fresh_job_changed")
        if not job_id:
            job_id = observed_job
        # A bounded search can exhaust its budget without producing a gate
        # winner.  Preserve the best scored node and run the explicit 400-case
        # L0 comparison instead of spinning through identical retries.
        if _best_effort_terminal(job_id, current):
            from best_strategy import build_best
            best = build_best(job_id, run_full_comparison=True)
            return {"status": "best_observed", "best_strategy": best}
        if _rejected_completed_showcase(job_id, current):
            from best_strategy import build_best
            best = build_best(job_id, run_full_comparison=True)
            return {"status": "best_available", "best_strategy": best,
                    "formal_release": False, "publication_skipped": True}
        if exit_code == 0:
            break
        runner = coverage.read(STATUS) if STATUS.is_file() else {}
        if runner.get("status") == "paused":
            raise RuntimeError("gepa_fresh_paused") from failure
        if not job_id:
            config_error = initial_config_error_since(log_offset)
            if config_error:
                raise RuntimeError("gepa_initial_config_error:" + config_error) from failure
            if not can_retry_initial_launch():
                if failure is not None:
                    raise failure
                raise RuntimeError("gepa_fresh_job_not_recoverable")
            if recoveries >= max_recoveries:
                raise RuntimeError("gepa_fresh_recovery_limit") from failure
            recoveries += 1
            print("RECOVER", json.dumps({"phase": "initial_launch", "attempt": recoveries},
                                          ensure_ascii=False), flush=True)
            continue
        if current.get("job_status") not in FRESH_RETRYABLE_JOB_STATES:
            if failure is not None:
                raise failure
            raise RuntimeError("gepa_fresh_job_not_recoverable")
        if recoveries >= max_recoveries:
            raise RuntimeError("gepa_fresh_recovery_limit") from failure
        recoveries += 1
        print("RECOVER", json.dumps({"job_id": job_id, "attempt": recoveries,
                                      "job_status": current.get("job_status")},
                                     ensure_ascii=False), flush=True)

    runner = coverage.read(STATUS)
    current = progress(job_id)
    if (runner.get("status") != "candidate_ready"
            or current.get("job_id") != job_id
            or current.get("job_status") != "passed"
            or current.get("memory_feedback", {}).get("status") not in ("empty", "committed")
            or not current.get("published_in_offline_workspace")):
        raise RuntimeError("gepa_fresh_candidate_not_publishable")
    if publisher is None:
        from offline_publish import publish as publisher
    publication = publisher()
    runner.update(status="published", publication=publication,
                  published_at=datetime.now(timezone.utc).isoformat())
    coverage.save(STATUS, runner)
    from best_strategy import build_best
    best = build_best(job_id, run_full_comparison=True)
    runner["architect_strategy"] = best
    coverage.save(STATUS, runner)
    return {**publication, "architect_strategy": best}


def fresh(seed_report: Path | None = None, baseline_report: Path | None = None,
          timeout_hours: float | None = None, max_rounds: int | None = None,
          max_model_calls: int | None = None, max_trials: int | None = None,
          patience: int | None = None, publisher=None) -> dict:
    """单入口：预检、清旧树、冻结配置、同树恢复并最终发布。"""
    requested = dict(FRESH_SEARCH_LIMITS)
    for name, value in {
            "timeout_hours": timeout_hours,
            "max_rounds": max_rounds,
            "max_model_calls": max_model_calls,
            "max_trials": max_trials,
            "patience": patience,
    }.items():
        if value is not None:
            requested[name] = value
    with lifecycle_lock():
        # 在破坏性清理前完成不可变训练输入、二进制和业务入口的只读检查。
        # 生命周期锁一直持有到发布登记和 runner 终态落盘，避免另一 fresh
        # 在恢复窗口或 candidate_ready→publish 之间删除同一棵树。
        fresh_preflight()
        reset_fresh_outputs()
        plan = prepare(seed_report, baseline_report, **requested)
        readiness()  # 无模型启动门禁；从这里开始配置和运行文件均由 plan 哈希冻结。
        publication = run_to_publication(publisher=publisher)
        if publication.get("status") == "best_observed":
            return {"status": "best_observed", "plan": plan, **publication}
        return {"status": "published", "plan": plan, "publication": publication}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument("--fresh", action="store_true",
                       help="清除当前 GEPA 树和生成配置后，从预检连续运行到原子发布")
    group.add_argument("--prepare", action="store_true")
    group.add_argument("--check", action="store_true")
    group.add_argument("--run", action="store_true")
    group.add_argument("--retry", metavar="JOB_ID", help="继续同上下文的失败作业，复用有效执行")
    group.add_argument("--status", action="store_true")
    parser.add_argument("--seed-report", type=Path, help="准备新实验时引用同版本、同裁判的种子报告")
    parser.add_argument("--baseline-report", type=Path, help="准备新实验时引用同宿主、同裁判的 L0 报告")
    parser.add_argument("--timeout-hours", type=float,
                        help="prepare/retry：搜索树的总墙钟预算小时数（最大168）")
    parser.add_argument("--max-rounds", type=int,
                        help="prepare/retry：搜索树的候选轮数上限（最大256）")
    parser.add_argument("--max-model-calls", type=int,
                        help="prepare/retry：搜索树的模型调用上限（最大1024）")
    parser.add_argument("--max-trials", type=int,
                        help="prepare/retry：搜索树的评测 trial 总上限（最大20000）")
    parser.add_argument("--patience", type=int,
                        help="prepare/retry：连续无改善上限；0 表示禁用（最大256）")
    args = parser.parse_args()
    if (args.seed_report is not None or args.baseline_report is not None) and not (
            args.prepare or args.fresh):
        parser.error("--seed-report / --baseline-report 只用于新实验，不在已有作业中替换成绩")
    limits = (args.timeout_hours, args.max_rounds, args.max_model_calls,
              args.max_trials, args.patience)
    if any(value is not None for value in limits) and not (
            args.fresh or args.prepare or args.retry):
        parser.error("搜索预算参数只能与 --fresh、--prepare 或 --retry JOB_ID 一起使用")
    if args.fresh:
        result = fresh(args.seed_report, args.baseline_report, *limits)
        print(json.dumps(result, ensure_ascii=False))
        return 0
    if args.prepare:
        plan = prepare(args.seed_report, args.baseline_report, *limits)
        print(json.dumps({"stage": plan["stage"], "dataset_view": plan["dataset_view"],
                          "memory_source": plan["source_memory"], "output": str(STAGE2.relative_to(ROOT)),
                          "service": plan["service"]}, ensure_ascii=False))
        return 0
    if args.check:
        plan, _ = readiness()
        print(json.dumps({"status": "ready", "stage": plan["stage"],
                          "split_counts": plan["split_counts"],
                          "memory_source": plan["source_memory"],
                          "service": plan["service"]}, ensure_ascii=False))
        return 0
    if args.status:
        print(json.dumps({"runner": coverage.read(STATUS) if STATUS.is_file() else None,
                          "gepa": progress()}, ensure_ascii=False))
        return 0
    return run(args.retry or "", args.timeout_hours, args.max_rounds,
               args.max_model_calls, args.patience, args.max_trials)


if __name__ == "__main__":
    os.umask(0o077)
    sys.exit(main())
