"""冻结部署必须对应正在执行的宿主；这些检查不启动 Agent、不访问模型。"""

import hashlib
from pathlib import Path
import sys
from unittest.mock import patch

import pytest

PACKAGE = Path(__file__).resolve().parents[1]
REPO = PACKAGE.parents[2]
sys.path[:0] = [str(PACKAGE / "evals"),
                str(REPO / "tools/trajectory_evolution/experiments/business-coverage")]
import prepare_dataset as preparation
import offline_gepa as offline


def test_host_revision_preserves_the_original_identity_algorithm(tmp_path):
    """抽取前后的源码和模型身份必须产生相同指纹，不能仅因重构废弃旧缓存。"""
    package = tmp_path / "examples/agent/business-simulator"
    source = package / "harness/tool.py"
    source.parent.mkdir(parents=True)
    source.write_text("# 测试宿主\n", encoding="utf-8")
    runtime = tmp_path / "packages/agentflow-runtime/src/runtime.py"
    runtime.parent.mkdir(parents=True)
    runtime.write_text("# 测试运行时\n", encoding="utf-8")
    for path in (package / ".env", package / "tests/test_tool.py", package / "outputs/log.json"):
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text("不属于宿主身份的测试数据", encoding="utf-8")
    env = {"AF_LLM_MODEL": "test-model", "AF_LLM_BASE_URL": "https://example.invalid/v1",
           "AF_LLM_API_KEY": "opaque-test-only-credential"}
    expected = preparation.fingerprint({
        "files": {str(p.relative_to(tmp_path)): hashlib.sha256(p.read_bytes()).hexdigest()
                  for p in (source, runtime)},
        "model": env["AF_LLM_MODEL"], "endpoint": env["AF_LLM_BASE_URL"]})
    with patch.object(preparation, "PACKAGE", package), patch.object(preparation, "REPO", tmp_path):
        actual = preparation.host_revision(env)
        assert actual == expected
        assert preparation.host_revision({**env, "AF_LLM_API_KEY": "another-test-key"}) == actual
        assert preparation.host_revision({**env, "AF_LLM_MODEL": "different-model"}) != actual
        source.write_text("# 修正查询逻辑\n", encoding="utf-8")
        assert preparation.host_revision(env) != actual


@pytest.mark.parametrize("frozen_revision,dataset_version,error", [
    ("current-host", "current-dataset", None),
    ("previous-host", "current-dataset", "gepa_host_changed"),
    (None, "current-dataset", "gepa_host_changed"),
    ("current-host", "changed-goal-contract", "gepa_input_changed"),
])
def test_gepa_verifies_current_host_before_accepting_a_frozen_deployment(
        tmp_path, frozen_revision, dataset_version, error):
    """不模拟评分：只核验匹配部署可继续，源码改变或缺少身份必须拒绝。"""
    source, workspace = tmp_path / "training/evolution", tmp_path / "gepa/workspace"
    config, dataset, plan_file = tmp_path / "config", tmp_path / "dataset.json", tmp_path / "plan.json"
    plan = {"source_memory": "training/evolution", "dataset_view_hash": "digest",
            "config_files": {"evaluation.json": "digest"}, "dataset_version": "current-dataset"}
    files = {plan_file: plan,
             config / "agent-evaluation.json": {"agent": {"revision": frozen_revision}},
             config / "service.json": {"memory_source_dir": "training/evolution",
                                        "data_dir": "gepa/workspace"}}
    with patch.multiple(offline, ROOT=tmp_path, WORKSPACE=workspace, CONFIG=config,
                        DATASET=dataset, PLAN=plan_file), \
            patch.object(offline, "checked_stage1", return_value=source), \
            patch.object(offline, "digest", return_value="digest"), \
            patch.object(offline, "memory_hashes", return_value={}), \
            patch.object(offline, "check_dataset", return_value={"dataset_version": dataset_version}) as check_dataset, \
            patch.object(offline, "environment", return_value={"AF_LLM_MODEL": "test-model"}), \
            patch.object(offline, "host_revision", return_value="current-host") as revision, \
            patch.object(offline.coverage, "read", side_effect=lambda path: files[path]):
        if error is None:
            assert offline.verify() == plan
            check_dataset.assert_called_once()
        else:
            with pytest.raises(RuntimeError, match=f"^{error}$"):
                offline.verify()
            if error == "gepa_host_changed":
                check_dataset.assert_not_called()
            else:
                check_dataset.assert_called_once()
        revision.assert_called_once_with({"AF_LLM_MODEL": "test-model"})


def test_gepa_view_adds_goal_contract_without_repartitioning(tmp_path, monkeypatch):
    """旧视图只补评分引用；题目分组不变，已有契约不能被静默换掉或移除。"""
    import json
    source = tmp_path / "source"
    dataset = tmp_path / "gepa-view.json"
    rows = {
        split: [{"task_id": f"{split}-{i}", "group_id": f"group-{split}-{i}"}
                for i in range(200)]
        for split in ("val", "holdout")}
    real_read = offline.coverage.read
    monkeypatch.setattr(offline, "DATASET", dataset)
    monkeypatch.setattr(offline.layout, "SOURCE_DATASET", source)
    monkeypatch.setattr(offline, "records", lambda directory, split: rows[split])
    monkeypatch.setattr(offline.coverage, "read", lambda path:
        {"metadata": {"dataset_version": "source-v1"}} if path == source / "manifest.json"
        else real_read(path))
    old = offline.build_view()
    new = offline.build_view(tmp_path / "gepa-goals.json")
    assert {key: value for key, value in new.items() if key != "goal_contracts_file"} == old
    assert new["goal_contracts_file"] == "gepa-goals.json"
    assert len(new["search_task_ids"]) == 150
    assert len(new["selection_task_ids"]) == 50
    assert json.loads(dataset.read_text()) == new
    for replacement in (None, tmp_path / "different-goals.json"):
        with pytest.raises(RuntimeError, match="^gepa_view_changed$"):
            offline.build_view(replacement)
    assert json.loads(dataset.read_text()) == new
