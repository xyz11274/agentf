#!/usr/bin/env python3
"""业务实验目录迁移与初始化：数据集外置，三个阶段只保存各自的产物。"""

from __future__ import annotations

import fcntl
import json
import os
from pathlib import Path
import shutil

ROOT = Path(__file__).resolve().parents[4]
OUTPUT = ROOT / "trace_output/business-simulator"
DATASETS = Path(__file__).resolve().parent / "datasets"
PACK = DATASETS / "pack"
TRAINING = OUTPUT / "training"
GEPA = OUTPUT / "gepa"
PUBLISHED = OUTPUT / "published"
SOURCE_DATASET = PACK
# Native dataset build artifacts are ephemeral; the checked-in pack stays JSONL-only.
TRAINING_DATASET = Path(os.environ.get("BUSINESS_DATASET_RUNTIME", "/tmp/business-simulator-dataset"))
GEPA_VIEW = PACK / "gepa-view.json"


def _move_without_alias(source: Path, target: Path, legacy_target: Path | None = None) -> None:
    """迁移已存在的位置；只删除经目标校验的旧兼容链接。"""
    if source.is_symlink():
        resolved = source.resolve(strict=False)
        if not target.exists() or resolved not in {target.resolve(), legacy_target}:
            raise RuntimeError(f"layout_alias_invalid:{source}")
        source.unlink()
        return
    if not source.exists():
        if not target.exists():
            raise RuntimeError(f"layout_source_missing:{source}")
        return
    if target.exists():
        raise RuntimeError(f"layout_target_exists:{target}")
    target.parent.mkdir(parents=True, exist_ok=True)
    source.rename(target)


def _discard_unstarted_gepa(path: Path) -> None:
    """仅清理曾错误准备、尚未提交任何 GEPA 作业的临时副本。"""
    if not path.exists():
        return
    status = json.loads((path / "status.json").read_text())
    state_file = path / "workspace/agent-evaluation/state.json"
    state = json.loads(state_file.read_text()) if state_file.exists() else {}
    if status.get("status") != "prepared" or state.get("jobs"):
        raise RuntimeError("old_gepa_output_requires_manual_review")
    shutil.rmtree(path)


def _normalize_training_config() -> None:
    """只修正旧生成配置中的位置字段，不改模型、评分、提示词或历史轨迹。"""
    directory = Path(__file__).resolve().parent / "coldstart-deployment"
    runtime = directory / "runtime"
    if not runtime.is_dir():
        return

    def save_if_changed(path: Path, before: dict, after: dict) -> None:
        if before == after:
            return
        temporary = path.with_suffix(path.suffix + ".new")
        if temporary.exists():
            raise RuntimeError(f"config_temp_exists:{temporary}")
        temporary.write_text(json.dumps(after, ensure_ascii=False, indent=2) + "\n")
        temporary.replace(path)

    for path in sorted(runtime.glob("*.json")):
        before = json.loads(path.read_text())
        after = json.loads(path.read_text())
        if path.name == "service.json":
            after["data_dir"] = str((TRAINING / "evolution").relative_to(ROOT))
            after["experience_template_file"] = "prompts/runtime-experience.txt"
            sharing = after.get("sharing", {})
            for key, filename in {"distill": "shared-experience-distill.txt",
                                  "review": "shared-experience-review.txt",
                                  "relation": "cross-memory-relation.txt"}.items():
                sharing[key + "_prompt_file"] = "prompts/" + filename
            sharing["relation_batch_prompt_file"] = "prompts/cross-memory-batch.txt"
            if "control_learning" in after:
                after["control_learning"]["prompt_file"] = "prompts/control-candidates.txt"
        elif path.name == "agent-evaluation.json":
            after["dataset"] = os.path.relpath(TRAINING_DATASET, runtime)
        elif path.name == "gepa.json":
            after["prompt_file"] = "prompts/gepa-reflection.txt"
        elif path.name == "evaluation.json" or path.name.startswith("training-"):
            after["judge"]["prompt"] = "prompts/task-judge-system.txt"
            after["judge"]["loop_review_prompt"] = "prompts/task-loop-review.txt"
            script = ROOT / "examples/agent/business-simulator/evals/run_product_eval.py"
            after["variants"][0]["command"] = [
                "python3", "{config_dir}/" + os.path.relpath(script, runtime),
                "--evolution-entry"]
        save_if_changed(path, before, after)

    builder = directory / "dataset.json"
    if builder.is_file():
        before = json.loads(builder.read_text())
        after = json.loads(builder.read_text())
        for source in after.get("sources", []):
            if source.get("kind") == "golden":
                source["path"] = "training.jsonl"
        save_if_changed(builder, before, after)


def ensure_layout() -> dict[str, str]:
    """幂等迁移已完成的训练结果，并创建在线和离线通用的阶段目录。"""
    OUTPUT.mkdir(parents=True, exist_ok=True)
    old_training = OUTPUT / "coldstart"
    active = old_training if old_training.exists() else TRAINING
    status_file = active / "status.json"
    if not status_file.is_file() or json.loads(status_file.read_text()).get("status") != "completed":
        raise RuntimeError("training_stage_not_complete")
    lock_file = active / "runner.lock"
    with lock_file.open("a") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        _move_without_alias(old_training, TRAINING)
        _move_without_alias(OUTPUT / "dataset", SOURCE_DATASET,
                            ROOT / "benchmark/datasets/business-simulator/source")
        _move_without_alias(TRAINING / "dataset", TRAINING_DATASET,
                            ROOT / "benchmark/datasets/business-simulator/training-reviewed")
        _discard_unstarted_gepa(OUTPUT / "offline-gepa")
        generated = TRAINING / "gepa-dataset"
        if generated.is_dir():
            if not (generated / "manifest.json").is_file():
                raise RuntimeError("old_gepa_dataset_requires_manual_review")
            shutil.rmtree(generated)
        old_config = ROOT / "configs/experiments/business-coverage/coldstart-gepa-deployment"
        if old_config.is_dir():
            shutil.rmtree(old_config)
        old_offline_config = ROOT / "configs/experiments/business-coverage/offline-gepa"
        if old_offline_config.is_dir():
            service = json.loads((old_offline_config / "runtime/service.json").read_text())
            if service.get("data_dir") != str(OUTPUT / "offline-gepa/workspace") or (
                    OUTPUT / "offline-gepa").exists():
                raise RuntimeError("old_gepa_config_requires_manual_review")
            shutil.rmtree(old_offline_config)
        legacy = ROOT / "trace_output/business-simulator-legacy"
        for name in ("adaptation-test", "adaptation-evaluation"):
            source = OUTPUT / name
            if source.is_dir():
                legacy.mkdir(parents=True, exist_ok=True)
                target = legacy / name
                if target.exists():
                    raise RuntimeError(f"legacy_target_exists:{target}")
                source.rename(target)
        old_runtime = OUTPUT / "config"
        if old_runtime.is_dir():
            destination = Path(__file__).resolve().parent / "legacy-runtime"
            if destination.exists():
                raise RuntimeError(f"legacy_config_target_exists:{destination}")
            old_runtime.rename(destination)
        # 执行计划是配置而非轨迹产物；历史轨迹正文不做替换。
        old_plan = TRAINING / "plan.json"
        new_plan = Path(__file__).resolve().parent / "coldstart-deployment/plan.json"
        if old_plan.is_file():
            if new_plan.exists():
                raise RuntimeError("training_plan_target_exists")
            plan = json.loads(old_plan.read_text())
            def relative(value: str) -> str:
                path = Path(value)
                if not path.is_absolute() or not path.is_relative_to(ROOT):
                    return value
                old = ROOT / "configs/experiments/business-coverage"
                if path.is_relative_to(old):
                    path = Path(__file__).resolve().parent / path.relative_to(old)
                return str(path.relative_to(ROOT))
            plan["service"] = relative(plan["service"])
            for batch in plan["batches"]:
                batch["config"] = relative(batch["config"])
            temporary = new_plan.with_suffix(".json.new")
            temporary.write_text(json.dumps(plan, ensure_ascii=False, indent=2) + "\n")
            temporary.replace(new_plan)
            old_plan.unlink()
        _normalize_training_config()
        # 阶段目录按实际执行创建；没有发布版本时不保留空 published。
        if PUBLISHED.is_dir() and not any(PUBLISHED.iterdir()):
            PUBLISHED.rmdir()
    return {"training": str(TRAINING.relative_to(ROOT)),
            "gepa": str(GEPA.relative_to(ROOT)),
            "published": str(PUBLISHED.relative_to(ROOT)),
            "dataset": str(SOURCE_DATASET.relative_to(ROOT))}


if __name__ == "__main__":
    print(json.dumps(ensure_layout(), ensure_ascii=False, indent=2))
