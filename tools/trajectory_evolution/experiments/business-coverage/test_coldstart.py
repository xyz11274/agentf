"""本地排程回归：不加载凭据、不启动 Agent、不发送模型请求。"""

from contextlib import ExitStack, redirect_stdout, redirect_stderr
from copy import deepcopy
import io
import json
from pathlib import Path
import os
import signal
import tempfile
import unittest
from unittest.mock import patch
import fcntl

import coldstart
import offline_gepa
import offline_publish


class CaptureRecoveryTests(unittest.TestCase):
    def test_cancelled_batch_resumes_even_when_all_trials_completed(self):
        """整批取消与单题成功分开判断，防止卡在完整检查点之外。"""
        trial = {'execution_status': 'completed'}
        self.assertTrue(coldstart.capture_needs_resume(dict(complete=False, trials=[trial])))
        self.assertFalse(coldstart.capture_needs_resume(dict(complete=True, trials=[trial])))

    def test_execution_error_resumes_but_business_failure_does_not(self):
        """保留旧错误重跑边界，不因业务失败而无休止重试。"""
        for trial in ({'execution_status': 'cancelled'},
                      {'execution_status': 'completed', 'error_code': 'execution_error'}):
            self.assertTrue(coldstart.capture_needs_resume(dict(complete=True, trials=[trial])))
        self.assertFalse(coldstart.capture_needs_resume(
            dict(complete=True, trials=[{'execution_status': 'failed'}])))

    def test_partial_json_or_utf8_is_retried_and_reported_once(self):
        """模拟读取撞上 JSON/中文字符写入边界，下一次读取成功后只播报一次。"""
        with tempfile.TemporaryDirectory() as folder, redirect_stdout(io.StringIO()) as output:
            root = Path(folder)
            (root / 'trials').mkdir()
            trial = root / 'trials/one.json'
            seen = set()
            for data in (b'{"task_id":', b'{"task_id":"\xe4\xb8'):
                trial.write_bytes(data)
                coldstart.report_capture_progress(root, seen)
                self.assertFalse(seen)
            trial.write_text(json.dumps(dict(task_id='one', execution_status='completed',
                                             verdict='uncertain', duration_ms=1200)))
            coldstart.report_capture_progress(root, seen, finished=True)
            coldstart.report_capture_progress(root, seen, finished=True)
            self.assertEqual(seen, {trial})
            self.assertEqual(output.getvalue().count('TASK one '), 1)

    def test_permanently_broken_checkpoint_fails_after_exit(self):
        """真正损坏的产物在进程结束后必须报错；诊断只含文件名，不含内容。"""
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            (root / 'trials').mkdir()
            (root / 'trials/broken.json').write_text('{"private-data":')
            with self.assertRaisesRegex(RuntimeError, '^trial_checkpoint_invalid:broken.json$'):
                coldstart.report_capture_progress(root, set(), finished=True)


class GepaFeedbackProgressTests(unittest.TestCase):
    def test_prepare_preserves_local_specialists_without_relaxing_publication(self):
        """保留局部收益供融合；移除绝对质量门槛并补齐循环、时延和留出配置。"""
        with tempfile.TemporaryDirectory() as folder, ExitStack() as stack:
            root = Path(folder)
            config, output, source = root / 'deployment/runtime', root / 'gepa', root / 'training'
            gate = dict(min_quality=0.8, min_pass_rate=0.8, max_quality_drop=0)
            objective = dict(invalid_loop_weight=0.2, token_weight=0.1, latency_weight=0.15)
            defaults = {
                'runtime.json': dict(model={}),
                'service.json': dict(instrumentation={}, control_learning={}, sharing={}),
                'agent-evaluation.json': dict(training={}, gate=gate),
                'evaluation.json': dict(judge={}, variants=[{}]),
                'gepa.json': dict(minibatch_size=32, merge_minibatch_size=16,
                                  candidate_selection_strategy='pareto',
                                  acceptance='strict_improvement', objective=objective),
                'view.json': dict(source_version='dataset-v1'),
                'evolution-memory.json': dict(entries=[dict(candidate=dict(
                    status='EVOLUTION_CANDIDATE_STATUS_PROPOSED'))]),
            }
            saved = {}
            for name, value in dict(ROOT=root, CONFIG=config, PLAN=config.parent / 'plan.json',
                                    STAGE2=output, WORKSPACE=output / 'workspace',
                                    STATUS=output / 'status.json', DATASET=root / 'view.json').items():
                stack.enter_context(patch.object(offline_gepa, name, value))
            stack.enter_context(patch.object(offline_gepa.layout, 'ensure_layout'))
            stack.enter_context(patch.object(offline_gepa.layout, 'SOURCE_DATASET', root / 'data'))
            stack.enter_context(patch.object(offline_gepa, 'checked_stage1', return_value=source))
            stack.enter_context(patch.object(offline_gepa, 'environment', return_value={}))
            stack.enter_context(patch.object(offline_gepa, 'build_view'))
            splits = dict(train=150, val=50, holdout=200)
            stack.enter_context(patch.object(offline_gepa, 'check_dataset', return_value=dict(
                dataset_version='dataset-v1', splits=splits)))
            stack.enter_context(patch.object(offline_gepa, 'memory_hashes', return_value={}))
            stack.enter_context(patch.object(
                offline_gepa, 'runtime_hashes', return_value={'runtime': 'fixture-hash'}))
            stack.enter_context(patch.object(offline_gepa, 'prepare_configuration'))
            stack.enter_context(patch.object(offline_gepa, 'digest', return_value='fixture-hash'))
            stack.enter_context(patch.object(offline_gepa.coverage, 'read',
                side_effect=lambda path: deepcopy(defaults[Path(path).name])))
            stack.enter_context(patch.object(offline_gepa.coverage, 'save',
                side_effect=lambda path, value: saved.__setitem__(Path(path), deepcopy(value))))
            plan = offline_gepa.prepare(
                timeout_hours=50, max_rounds=100, max_model_calls=256,
                max_trials=12000, patience=0)
            generated = saved[config / 'gepa.json']
            self.assertEqual(generated['acceptance'], 'pareto')
            self.assertEqual(generated['candidate_selection_strategy'], 'pareto')
            self.assertEqual(generated['objective'], objective)
            self.assertEqual(generated['timeout_ms'], 50 * 3600000)
            self.assertEqual(generated['max_rounds'], 100)
            self.assertEqual(generated['max_model_calls'], 256)
            self.assertEqual(generated['max_trials'], 12000)
            self.assertEqual(generated['patience'], 0)
            self.assertEqual(generated['holdout_top_k'], 3)
            self.assertEqual(saved[config / 'agent-evaluation.json']['gate'], dict(
                max_quality_drop=0, min_invalid_loop_reduction=0.15,
                max_latency_ratio=1.0, max_token_ratio=1.0))
            self.assertEqual(defaults['agent-evaluation.json']['gate'], gate)
            self.assertEqual(plan['split_counts'], splits)
            self.assertEqual(plan['search_limits'], dict(
                timeout_ms=50 * 3600000, max_rounds=100,
                max_model_calls=256, max_trials=12000, patience=0))
            self.assertEqual(plan['evaluation_parallelism'], 4)
            self.assertEqual(plan['runtime_hashes'], {'runtime': 'fixture-hash'})
            self.assertEqual(defaults['gepa.json']['acceptance'], 'strict_improvement')

            # 已明确配置的循环、时延和 token 目标不能被离线默认值覆盖；绝对门槛仍会移除。
            stricter = dict(gate, min_invalid_loop_reduction=0.25, max_latency_ratio=0.9)
            defaults['agent-evaluation.json']['gate'] = stricter
            second_output = root / 'stricter-gepa'
            with patch.object(offline_gepa, 'STAGE2', second_output), \
                    patch.object(offline_gepa, 'WORKSPACE', second_output / 'workspace'), \
                    patch.object(offline_gepa, 'STATUS', second_output / 'status.json'):
                offline_gepa.prepare()
            self.assertEqual(saved[config / 'agent-evaluation.json']['gate'], dict(
                max_quality_drop=0, min_invalid_loop_reduction=0.25,
                max_latency_ratio=0.9, max_token_ratio=1.0))

    def test_fresh_cleanup_only_removes_active_gepa_and_generated_config(self):
        """fresh 不得删除训练 memory、冻结 dataset 或已发布的稳定版本。"""
        with tempfile.TemporaryDirectory() as folder, ExitStack() as stack:
            root = Path(folder)
            config_root = root / "configs"
            deployment = config_root / "gepa-deployment"
            stage1, stage2 = root / "training", root / "gepa"
            datasets, published = root / "datasets", root / "published"
            for path in (deployment / "runtime", stage1, stage2 / "workspace/agent-evaluation",
                         datasets / "source", datasets / "training-reviewed", published):
                path.mkdir(parents=True)
            (deployment / "runtime/gepa.json").write_text("{}")
            (stage2 / "old-tree.json").write_text("old")
            (stage2 / "runner.lock").write_text("")
            view = datasets / "gepa-view.json"
            view.write_text("{}")
            (stage1 / "memory.pb").write_text("memory")
            (published / "latest.json").write_text("{}")
            patches = {
                "STAGE1": stage1,
                "STAGE2": stage2,
                "CONFIG": deployment / "runtime",
                "DATASET": view,
            }
            for name, value in patches.items():
                stack.enter_context(patch.object(offline_gepa, name, value))
            stack.enter_context(patch.object(offline_gepa.layout, "GEPA", stage2))
            stack.enter_context(patch.object(offline_gepa.layout, "SOURCE_DATASET", datasets / "source"))
            stack.enter_context(patch.object(
                offline_gepa.layout, "TRAINING_DATASET", datasets / "training-reviewed"))
            stack.enter_context(patch.object(offline_gepa.layout, "PUBLISHED", published))
            stack.enter_context(patch.object(offline_gepa.coverage, "CONFIG", config_root))

            offline_gepa.reset_fresh_outputs()

            self.assertFalse(stage2.exists())
            self.assertFalse(deployment.exists())
            self.assertTrue((stage1 / "memory.pb").is_file())
            self.assertTrue(view.is_file())
            self.assertTrue((published / "latest.json").is_file())

    def test_fresh_entry_freezes_formal_budget_but_generic_capacity_accepts_100_rounds(self):
        """本次 fresh 固定 50 轮；通用程序仍接受并验证 100 轮容量。"""
        publication = {"version": "agent-version-v1:" + "a" * 64}
        plan = {"stage": "GEPA L3 search and final holdout"}
        with tempfile.TemporaryDirectory() as folder, patch.object(
                offline_gepa, "LIFECYCLE_LOCK", Path(folder) / "lifecycle.lock"), patch.object(
                offline_gepa, "fresh_preflight") as preflight, patch.object(
                offline_gepa, "reset_fresh_outputs") as reset, patch.object(
                offline_gepa, "prepare", return_value=plan) as prepare, patch.object(
                offline_gepa, "readiness") as ready, patch.object(
                offline_gepa, "run_to_publication", return_value=publication) as run:
            result = offline_gepa.fresh()
        preflight.assert_called_once_with()
        reset.assert_called_once_with()
        prepare.assert_called_once_with(None, None, **offline_gepa.FRESH_SEARCH_LIMITS)
        ready.assert_called_once_with()
        run.assert_called_once()
        self.assertEqual(result, dict(status="published", plan=plan, publication=publication))
        self.assertEqual(offline_gepa.search_limits(
            timeout_hours=50, max_rounds=100, max_model_calls=256,
            max_trials=12000, patience=0), dict(
                timeout_ms=50 * 3600000, max_rounds=100,
                max_model_calls=256, max_trials=12000, patience=0))

    def test_fresh_lifecycle_lock_is_reentrant_and_blocks_other_open_description(self):
        """同一 fresh 内发布可嵌套复用锁，其他进程语义的文件描述符不能进入。"""
        with tempfile.TemporaryDirectory() as folder, patch.object(
                offline_gepa, "LIFECYCLE_LOCK", Path(folder) / "lifecycle.lock"):
            with offline_gepa.lifecycle_lock(), offline_gepa.lifecycle_lock():
                competitor = open(offline_gepa.LIFECYCLE_LOCK, "a+")
                try:
                    with self.assertRaises(BlockingIOError):
                        fcntl.flock(competitor, fcntl.LOCK_EX | fcntl.LOCK_NB)
                finally:
                    competitor.close()
            with open(offline_gepa.LIFECYCLE_LOCK, "a+") as available:
                fcntl.flock(available, fcntl.LOCK_EX | fcntl.LOCK_NB)

    def test_initial_runner_exception_reaps_child_instead_of_leaving_service(self):
        """首次 learn 的监控异常也必须停止子进程，不能只清理 retry 服务。"""
        class Child:
            returncode = None
            stdout = None
            stops = 0

            def poll(self):
                return self.returncode

            def terminate(self):
                self.stops += 1
                self.returncode = 0

            def wait(self, timeout=None):
                return self.returncode

        with tempfile.TemporaryDirectory() as folder, ExitStack() as stack:
            root = Path(folder)
            training, gepa = root / "training", root / "gepa"
            training.mkdir(); gepa.mkdir()
            child = Child()
            for name, value in dict(STAGE1=training, STAGE2=gepa,
                                    STATUS=gepa / "status.json").items():
                stack.enter_context(patch.object(offline_gepa, name, value))
            stack.enter_context(patch.object(offline_gepa, "readiness",
                return_value=(dict(service="local-test.json"), {})))
            stack.enter_context(patch.object(offline_gepa, "progress",
                                             side_effect=RuntimeError("monitor_failed")))
            stack.enter_context(patch.object(offline_gepa.subprocess, "Popen",
                                             return_value=child))
            stack.enter_context(redirect_stdout(io.StringIO()))
            with self.assertRaisesRegex(RuntimeError, "monitor_failed"):
                offline_gepa.run()
            self.assertEqual(child.stops, 1)
            self.assertEqual(json.loads((gepa / "status.json").read_text())["status"], "failed")

    def test_process_group_escalates_to_kill_after_graceful_timeout(self):
        """整个新会话先收 TERM；超时后收 KILL，不能只结束直接子进程。"""
        class Child:
            pid = 43210
            returncode = None
            waits = 0

            def poll(self):
                return self.returncode

            def wait(self, timeout=None):
                self.waits += 1
                if self.waits == 1:
                    raise offline_gepa.subprocess.TimeoutExpired("child", timeout)
                return self.returncode

        child, signals = Child(), []
        def deliver(pid, signum):
            self.assertEqual(pid, child.pid)
            signals.append(signum)
            if signum == signal.SIGKILL:
                child.returncode = -signum
        with patch.object(offline_gepa.os, "killpg", side_effect=deliver):
            offline_gepa._stop_process_group(child, timeout=0.01)
        self.assertEqual(signals, [signal.SIGTERM, signal.SIGKILL])
        self.assertEqual(child.waits, 2)

    def test_publish_recomputes_artifact_version_before_writing(self):
        """声明版本与原生重算结果不一致时，不能写入版本或 latest 登记。"""
        with tempfile.TemporaryDirectory() as folder, ExitStack() as stack:
            root = Path(folder)
            workspace, published = root / "workspace", root / "published"
            source = workspace / "agent-evaluation/job/search/published-version.json"
            source.parent.mkdir(parents=True)
            declared = "agent-version-v1:" + "a" * 64
            source.write_text(json.dumps(dict(
                schema_version=1, version=declared,
                artifact=dict(level="L3", user_id=offline_gepa.USER),
                evaluation=dict(job_id="job", gate=dict(passed=True)))))
            status = root / "status.json"
            status.write_text(json.dumps(dict(status="candidate_ready")))
            stack.enter_context(patch.object(offline_publish.gepa, "STATUS", status))
            stack.enter_context(patch.object(offline_publish.gepa, "WORKSPACE", workspace))
            stack.enter_context(patch.object(offline_publish.gepa, "verify", return_value=dict(
                dataset_version="dataset", source_memory_hashes={})))
            stack.enter_context(patch.object(offline_publish.gepa, "progress", return_value=dict(
                job_id="job", job_status="passed", published_in_offline_workspace=True,
                memory_feedback=dict(status="committed"))))
            stack.enter_context(patch.object(offline_publish.gepa, "LIFECYCLE_LOCK",
                                             root / "lifecycle.lock"))
            stack.enter_context(patch.object(offline_publish.layout, "PUBLISHED", published))
            stack.enter_context(patch.object(offline_publish, "_artifact_version",
                                             return_value="agent-version-v1:" + "b" * 64))
            with self.assertRaisesRegex(RuntimeError, "^gepa_published_version_mismatch$"):
                offline_publish.publish()
            self.assertFalse(published.exists())

    def test_same_artifact_can_keep_independent_publication_proofs(self):
        """相同策略经不同 job 复核时保留两份证明，不误判成策略版本碰撞。"""
        with tempfile.TemporaryDirectory() as folder, ExitStack() as stack:
            root = Path(folder)
            workspace, published = root / "workspace", root / "published"
            status = root / "status.json"
            artifact = dict(level="L3", user_id=offline_gepa.USER,
                            experience_rules=[dict(rule_id="stable-rule")])
            version = "agent-version-v1:" + "a" * 64
            active = {"job_id": "job-one"}

            def write_bundle(job_id):
                source = (workspace / "agent-evaluation" / job_id /
                          "search/published-version.json")
                source.parent.mkdir(parents=True)
                source.write_text(json.dumps(dict(
                    schema_version=1, version=version, artifact=artifact,
                    evaluation=dict(job_id=job_id, gate=dict(passed=True)))))

            def progress():
                return dict(
                    job_id=active["job_id"], job_status="passed",
                    published_in_offline_workspace=True,
                    memory_feedback=dict(status="committed"))

            write_bundle("job-one")
            write_bundle("job-two")
            status.write_text(json.dumps(dict(status="candidate_ready")))
            stack.enter_context(patch.object(offline_publish.gepa, "STATUS", status))
            stack.enter_context(patch.object(offline_publish.gepa, "WORKSPACE", workspace))
            stack.enter_context(patch.object(offline_publish.gepa, "verify", return_value=dict(
                dataset_version="dataset", source_memory_hashes={})))
            stack.enter_context(patch.object(offline_publish.gepa, "progress",
                                             side_effect=progress))
            stack.enter_context(patch.object(offline_publish.gepa, "LIFECYCLE_LOCK",
                                             root / "lifecycle.lock"))
            stack.enter_context(patch.object(offline_publish.layout, "PUBLISHED", published))
            stack.enter_context(patch.object(offline_publish, "_artifact_version",
                                             return_value=version))

            first = offline_publish.publish()
            first_path = published / first["bundle"]
            first_raw = first_path.read_bytes()

            active["job_id"] = "job-two"
            status.write_text(json.dumps(dict(status="candidate_ready")))
            second = offline_publish.publish()
            second_path = published / second["bundle"]

            self.assertEqual(first["version"], second["version"])
            self.assertNotEqual(first_path, second_path)
            self.assertEqual(first_path.read_bytes(), first_raw)
            self.assertTrue(second_path.is_file())
            self.assertEqual(json.loads((published / "latest.json").read_text()), second)
            self.assertEqual(len(list((published / "versions").glob("*.json"))), 2)

    def test_fresh_runner_recovers_same_job_without_changing_budget_then_publishes(self):
        """监督器只能接续同一 job/tree，且每次恢复都显式禁止扩预算。"""
        with tempfile.TemporaryDirectory() as folder, ExitStack() as stack:
            root = Path(folder)
            config, stage2 = root / "config", root / "gepa"
            config.mkdir()
            stage2.mkdir()
            gepa_file = config / "gepa.json"
            gepa_file.write_text(json.dumps(dict(
                timeout_ms=50 * 3600000, max_rounds=50,
                max_model_calls=128, max_trials=6500, patience=0)))
            status_file = stage2 / "status.json"
            calls = []

            def execute(job_id="", *args, **kwargs):
                calls.append((job_id, kwargs.get("allow_budget_extension")))
                offline_gepa.coverage.save(status_file, dict(
                    status="failed" if len(calls) == 1 else "candidate_ready"))
                return 1 if len(calls) == 1 else 0

            def progress(_job_id=""):
                if len(calls) == 1:
                    return dict(job_id="job", job_status="error", gepa=True,
                                memory_feedback=dict(status="none"))
                return dict(job_id="job", job_status="passed", gepa=True,
                            published_in_offline_workspace=True,
                            memory_feedback=dict(status="committed"))

            stack.enter_context(patch.object(offline_gepa, "CONFIG", config))
            stack.enter_context(patch.object(offline_gepa, "STAGE2", stage2))
            stack.enter_context(patch.object(offline_gepa, "STATUS", status_file))
            stack.enter_context(patch.object(offline_gepa, "run", side_effect=execute))
            stack.enter_context(patch.object(offline_gepa, "progress", side_effect=progress))
            publication = {"source_job": "job", "version": "agent-version-v1:" + "b" * 64}
            with redirect_stdout(io.StringIO()):
                result = offline_gepa.run_to_publication(publisher=lambda: publication)
            self.assertEqual(result, publication)
            self.assertEqual(calls, [("", False), ("job", False)])
            self.assertEqual(json.loads(gepa_file.read_text())["max_rounds"], 50)
            terminal = json.loads(status_file.read_text())
            self.assertEqual(terminal["status"], "published")
            self.assertEqual(terminal["publication"], publication)

    def test_fresh_runner_retries_only_proven_empty_initial_launch(self):
        """建树前的瞬时退出可重启；成功观察 job 后仍只运行这一棵树。"""
        with tempfile.TemporaryDirectory() as folder, ExitStack() as stack:
            root = Path(folder)
            config, stage2, workspace = root / "config", root / "gepa", root / "workspace"
            config.mkdir(); stage2.mkdir(); workspace.mkdir()
            (config / "gepa.json").write_text(json.dumps(dict(max_rounds=50)))
            status_file = stage2 / "status.json"
            calls = []

            def execute(job_id="", *args, **kwargs):
                calls.append(job_id)
                offline_gepa.coverage.save(status_file, dict(
                    status="failed" if len(calls) == 1 else "candidate_ready"))
                return 1 if len(calls) == 1 else 0

            def progress(_job_id=""):
                if len(calls) == 1:
                    return dict(phase="waiting_for_gepa_job")
                return dict(job_id="only-job", job_status="passed", gepa=True,
                            published_in_offline_workspace=True,
                            memory_feedback=dict(status="empty"))

            for name, value in dict(CONFIG=config, STAGE2=stage2, WORKSPACE=workspace,
                                    STATUS=status_file).items():
                stack.enter_context(patch.object(offline_gepa, name, value))
            stack.enter_context(patch.object(offline_gepa, "run", side_effect=execute))
            stack.enter_context(patch.object(offline_gepa, "progress", side_effect=progress))
            publication = {"source_job": "only-job"}
            with redirect_stdout(io.StringIO()):
                self.assertEqual(offline_gepa.run_to_publication(
                    publisher=lambda: publication), publication)
            self.assertEqual(calls, ["", ""])

    def test_fresh_runner_fails_fast_on_initial_config_error(self):
        """建树前的确定性配置错误应立即暴露，旧日志不应误判本次退出。"""
        with tempfile.TemporaryDirectory() as folder, ExitStack() as stack:
            root = Path(folder)
            config, stage2, workspace = root / "config", root / "gepa", root / "workspace"
            config.mkdir(); stage2.mkdir(); workspace.mkdir()
            (config / "gepa.json").write_text(json.dumps(dict(max_rounds=50)))
            status_file = stage2 / "status.json"
            log_file = stage2 / "learning.log"
            log_file.write_text("ERROR: gepa_quality_weight_required\n")

            def execute(job_id="", *args, **kwargs):
                self.assertEqual(job_id, "")
                with log_file.open("a") as log:
                    log.write("ERROR: gepa_quality_weight_required\n")
                offline_gepa.coverage.save(status_file, dict(status="failed"))
                return 1

            for name, value in dict(CONFIG=config, STAGE2=stage2, WORKSPACE=workspace,
                                    STATUS=status_file).items():
                stack.enter_context(patch.object(offline_gepa, name, value))
            run = stack.enter_context(patch.object(offline_gepa, "run", side_effect=execute))
            stack.enter_context(patch.object(offline_gepa, "progress", return_value=dict(
                phase="waiting_for_gepa_job")))
            with self.assertRaisesRegex(RuntimeError,
                                        "^gepa_initial_config_error:gepa_quality_weight_required$"):
                offline_gepa.run_to_publication(publisher=lambda: {})
            run.assert_called_once_with("", allow_budget_extension=False)

    def test_fresh_runner_does_not_retry_uncertain_or_existing_initial_state(self):
        """已有树、损坏状态或孤立 search 目录都不能盲目再建一棵树。"""
        cases = ("existing_job", "corrupt_state", "search_directory")
        for case in cases:
            with self.subTest(case=case), tempfile.TemporaryDirectory() as folder, ExitStack() as stack:
                root = Path(folder)
                config, stage2, workspace = root / "config", root / "gepa", root / "workspace"
                config.mkdir(); stage2.mkdir(); (workspace / "agent-evaluation").mkdir(parents=True)
                (config / "gepa.json").write_text(json.dumps(dict(max_rounds=50)))
                status_file = stage2 / "status.json"
                status_file.write_text(json.dumps(dict(status="failed")))
                state_file = workspace / "agent-evaluation/state.json"
                if case == "existing_job":
                    state_file.write_text(json.dumps(dict(jobs=dict(existing=dict(
                        user=offline_gepa.USER, level="L3", status="pending")))))
                elif case == "corrupt_state":
                    state_file.write_text("{")
                else:
                    (workspace / "agent-evaluation/orphan/search").mkdir(parents=True)
                for name, value in dict(CONFIG=config, STAGE2=stage2, WORKSPACE=workspace,
                                        STATUS=status_file).items():
                    stack.enter_context(patch.object(offline_gepa, name, value))
                run = stack.enter_context(patch.object(offline_gepa, "run", return_value=1))
                stack.enter_context(patch.object(offline_gepa, "progress", return_value=dict(
                    phase="loading_gepa_state")))
                with self.assertRaisesRegex(RuntimeError, "^gepa_fresh_job_not_recoverable$"):
                    offline_gepa.run_to_publication(publisher=lambda: {})
                run.assert_called_once_with("", allow_budget_extension=False)

    def test_fresh_runner_never_switches_job_after_first_observation(self):
        """一旦看到 job_id，后续状态即使声称可恢复也不能切换到另一棵树。"""
        with tempfile.TemporaryDirectory() as folder, ExitStack() as stack:
            root = Path(folder)
            config, stage2 = root / "config", root / "gepa"
            config.mkdir(); stage2.mkdir()
            (config / "gepa.json").write_text(json.dumps(dict(max_rounds=50)))
            status_file = stage2 / "status.json"
            status_file.write_text(json.dumps(dict(status="failed")))
            observations = [
                dict(job_id="first-job", job_status="error", gepa=True),
                dict(job_id="other-job", job_status="error", gepa=True),
            ]
            for name, value in dict(CONFIG=config, STAGE2=stage2,
                                    STATUS=status_file).items():
                stack.enter_context(patch.object(offline_gepa, name, value))
            stack.enter_context(patch.object(offline_gepa, "run", return_value=1))
            stack.enter_context(patch.object(offline_gepa, "progress",
                                             side_effect=observations))
            with self.assertRaisesRegex(RuntimeError, "^gepa_fresh_job_changed$"):
                offline_gepa.run_to_publication(publisher=lambda: {})

    def test_rejected_budget_search_extends_without_changing_inputs(self):
        """只扩展尚未进入留出集的预算终止作业，并同步冻结计划指纹。"""
        with tempfile.TemporaryDirectory() as folder, ExitStack() as stack:
            root = Path(folder)
            config = root / "config"
            workspace = root / "workspace"
            job_id = "budget-job"
            tree_dir = workspace / "agent-evaluation" / job_id / "search"
            config.mkdir()
            tree_dir.mkdir(parents=True)
            config_file = config / "gepa.json"
            config_file.write_text(json.dumps(dict(max_rounds=8, max_model_calls=8)))
            plan_file = root / "plan.json"
            plan_file.write_text(json.dumps(dict(config_files={"gepa.json": "old"})))
            (tree_dir / "tree.json").write_text(json.dumps(dict(
                status="validation_rejected", stop_reason="model_budget",
                holdout_started=False, active=None, batch=None, rounds=8, model_calls=8)))
            stack.enter_context(patch.object(offline_gepa, "CONFIG", config))
            stack.enter_context(patch.object(offline_gepa, "WORKSPACE", workspace))
            stack.enter_context(patch.object(offline_gepa, "PLAN", plan_file))
            stack.enter_context(patch.object(offline_gepa, "progress", return_value=dict(
                job_id=job_id, job_status="rejected")))
            self.assertTrue(offline_gepa.extend_retry_budget(job_id))
            extended = json.loads(config_file.read_text())
            self.assertEqual(extended["max_rounds"], 12)
            self.assertEqual(extended["max_model_calls"], 16)
            plan = json.loads(plan_file.read_text())
            self.assertEqual(plan["config_files"]["gepa.json"],
                             offline_gepa.digest(config_file))
            self.assertEqual(plan["budget_extensions"][-1]["job_id"], job_id)

            # 管理服务未接受第一次 retry 时，重复命令复用已有扩容，不再增长。
            self.assertTrue(offline_gepa.extend_retry_budget(job_id))
            self.assertEqual(json.loads(config_file.read_text()), extended)
            self.assertEqual(len(json.loads(plan_file.read_text())["budget_extensions"]), 1)

    def test_explicit_retry_limits_expand_time_and_capacity_atomically(self):
        """50 小时续跑同时扩容轮数、模型调用与耐心，且不能缩减或越过留出边界。"""
        with tempfile.TemporaryDirectory() as folder, ExitStack() as stack:
            root = Path(folder)
            config = root / "config"
            workspace = root / "workspace"
            job_id = "long-search"
            tree_dir = workspace / "agent-evaluation" / job_id / "search"
            config.mkdir()
            tree_dir.mkdir(parents=True)
            config_file = config / "gepa.json"
            config_file.write_text(json.dumps(dict(
                timeout_ms=5 * 3600000, max_rounds=12,
                max_model_calls=16, patience=8)))
            plan_file = root / "plan.json"
            plan_file.write_text(json.dumps(dict(
                config_files={"gepa.json": offline_gepa.digest(config_file)})))
            tree_file = tree_dir / "tree.json"
            tree_file.write_text(json.dumps(dict(
                status="validation_rejected", stop_reason="max_rounds",
                holdout_started=False, final_holdout=None,
                selected_version="seed-version", rounds=12, model_calls=9)))
            stack.enter_context(patch.object(offline_gepa, "CONFIG", config))
            stack.enter_context(patch.object(offline_gepa, "WORKSPACE", workspace))
            stack.enter_context(patch.object(offline_gepa, "PLAN", plan_file))

            self.assertTrue(offline_gepa.set_retry_limits(
                job_id, timeout_hours=50, max_rounds=64,
                max_model_calls=96, patience=24))
            expanded = json.loads(config_file.read_text())
            self.assertEqual(expanded, dict(
                timeout_ms=50 * 3600000, max_rounds=64,
                max_model_calls=96, patience=24))
            plan = json.loads(plan_file.read_text())
            self.assertEqual(plan["config_files"]["gepa.json"],
                             offline_gepa.digest(config_file))
            extension = plan["budget_extensions"][-1]
            self.assertEqual(extension["reason"], "explicit_search_limits")
            self.assertEqual(extension["current"], expanded)

            # 相同命令可安全重放；任何缩容都必须在写文件前失败。
            self.assertFalse(offline_gepa.set_retry_limits(
                job_id, timeout_hours=50, max_rounds=64,
                max_model_calls=96, patience=24))
            with self.assertRaisesRegex(
                    RuntimeError, "^gepa_search_limit_cannot_decrease:max_rounds$"):
                offline_gepa.set_retry_limits(job_id, max_rounds=63)
            self.assertEqual(json.loads(config_file.read_text()), expanded)

            # 显式启动标记与非空 final_holdout 计划任一存在，都证明留出已开始。
            for evidence in (
                    dict(status="passed", holdout_started=True,
                         final_holdout=None, selected_version="v1"),
                    dict(status="running", holdout_started=False,
                         final_holdout=dict(status="running", candidates=[{"node": 3}]),
                         selected_version="v1")):
                tree_file.write_text(json.dumps(evidence))
                with self.assertRaisesRegex(
                        RuntimeError, "^gepa_search_extension_after_holdout$"):
                    offline_gepa.set_retry_limits(job_id, max_rounds=65)



    def test_baseline_report_cannot_replace_frozen_input(self):
        """基线与种子使用相同冻结边界，不能运行途中替换 L0 成绩。"""
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            plan = root / "plan.json"
            plan.write_text("{}")
            (root / "gepa.json").write_text(json.dumps(dict(baseline_report_file="old/report.json")))
            with patch.object(offline_gepa, "PLAN", plan), patch.object(offline_gepa, "CONFIG", root), \
                    patch.object(offline_gepa.layout, "ensure_layout"):
                with self.assertRaisesRegex(RuntimeError, "^gepa_baseline_report_changed$"):
                    offline_gepa.prepare(baseline_report=root / "new/report.json")

    def test_baseline_report_option_only_prepares_new_experiment(self):
        """继续旧作业时不允许通过参数悄悄改基线。"""
        with patch.object(offline_gepa.sys, "argv", ["offline_gepa.py", "--run",
                                                   "--baseline-report", "report.json"]), redirect_stderr(io.StringIO()):
            with self.assertRaises(SystemExit) as stopped:
                offline_gepa.main()
            self.assertEqual(stopped.exception.code, 2)

    def test_incomplete_import_stops_before_creating_experiment(self):
        """预检失败不创建运行目录；正式身份检查仍由内核执行。"""
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            report = root / "report.json"
            report.write_text(json.dumps(dict(complete=False, trials=[{}])))
            with patch.object(offline_gepa, "PLAN", root / "absent.json"), \
                    patch.object(offline_gepa.layout, "ensure_layout"), \
                    patch.object(offline_gepa, "checked_stage1") as stage:
                for kind in ("baseline", "seed"):
                    with self.assertRaisesRegex(RuntimeError, "^gepa_" + kind + "_report_incomplete$"):
                        offline_gepa.prepare(**{kind + "_report": report})
                stage.assert_not_called()

    def test_seed_report_cannot_replace_frozen_input(self):
        """已有配置不可被新的种子报告静默替换，身份检查发生在启动前。"""
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            plan = root / "plan.json"
            plan.write_text("{}")
            (root / "gepa.json").write_text(json.dumps(dict(seed_report_file="old/report.json")))
            with patch.object(offline_gepa, "PLAN", plan), patch.object(offline_gepa, "CONFIG", root), \
                    patch.object(offline_gepa.layout, "ensure_layout"):
                with self.assertRaisesRegex(RuntimeError, "^gepa_seed_report_changed$"):
                    offline_gepa.prepare(root / "new/report.json")

    def test_seed_report_option_only_prepares_new_experiment(self):
        """运行、重试或查询时不能通过参数替换已冻结评分。"""
        with patch.object(offline_gepa.sys, "argv", ["offline_gepa.py", "--run",
                                                   "--seed-report", "report.json"]), redirect_stderr(io.StringIO()):
            with self.assertRaises(SystemExit) as stopped:
                offline_gepa.main()
            self.assertEqual(stopped.exception.code, 2)

    def test_collection_pending_and_commit_are_distinct(self):
        """检查点不等于已入库，且状态查询不显示反馈正文。"""
        with tempfile.TemporaryDirectory() as folder, patch.object(
                offline_gepa, 'WORKSPACE', Path(folder)):
            root = Path(folder) / 'agent-evaluation'
            search = root / 'job/search'
            search.mkdir(parents=True)
            job = dict(user=offline_gepa.USER, level='L3', status='running',
                       gepa=True, queued_ms=1)
            state = root / 'state.json'
            state.write_text(json.dumps(dict(jobs=dict(job=job))))
            tree = search / 'tree.json'
            tree.write_text(json.dumps(dict(status='searching', nodes=[])))
            self.assertEqual(offline_gepa.progress()['memory_feedback']['status'], 'none')
            # 显式续跑旧作业时不能被更新入队、但无关的作业劫持。
            state.write_text(json.dumps(dict(jobs=dict(
                job=job,
                newer=dict(user=offline_gepa.USER, level='L3', status='cancelled',
                           gepa=True, queued_ms=2)))))
            self.assertEqual(offline_gepa.progress('job')['job_id'], 'job')
            state.write_text(json.dumps(dict(jobs=dict(job=job))))
            tree.write_text(json.dumps(dict(status='searching', nodes=[],
                memory_feedback=dict(records=dict(record=dict(
                    status='conflicting', after='不应出现在状态输出中的经验正文'))))))
            feedback = offline_gepa.progress()['memory_feedback']
            self.assertEqual(feedback, dict(status='collected', records=1,
                                           record_states=dict(conflicting=1)))
            job['memory_feedback'] = dict(status='pending', records=1)
            state.write_text(json.dumps(dict(jobs=dict(job=job))))
            self.assertEqual(offline_gepa.progress()['memory_feedback']['status'], 'pending')
            job['memory_feedback'].update(status='committed', result=dict(memory_revision=42))
            state.write_text(json.dumps(dict(jobs=dict(job=job))))
            feedback = offline_gepa.progress()['memory_feedback']
            self.assertEqual(feedback['status'], 'committed')
            self.assertEqual(feedback['memory_revision'], 42)
            # 搜索树暂时不可读不应掩盖已持久化的管理层提交回执。
            tree.write_text('{')
            self.assertEqual(offline_gepa.progress()['memory_feedback']['status'], 'committed')

    def test_runner_forwards_stop_and_saves_paused_state(self):
        """包装运行器只转发本次子进程的停止信号，不遗留 running 状态或重复终止。"""
        class Child:
            returncode = None
            signals = []

            def poll(self):
                return self.returncode

            def send_signal(self, signum):
                self.signals.append(signum)
                self.returncode = 128 + signum

            def wait(self, timeout=None):
                return self.returncode

        with tempfile.TemporaryDirectory() as folder, ExitStack() as stack:
            root = Path(folder)
            training, gepa = root / 'training', root / 'gepa'
            training.mkdir()
            gepa.mkdir()
            child = Child()
            originals = {s: signal.getsignal(s) for s in (signal.SIGINT, signal.SIGTERM)}
            for name, value in dict(STAGE1=training, STAGE2=gepa,
                                    STATUS=gepa / 'status.json').items():
                stack.enter_context(patch.object(offline_gepa, name, value))
            stack.enter_context(patch.object(offline_gepa, 'readiness',
                return_value=(dict(service='local-test.json'), {})))
            stack.enter_context(patch.object(offline_gepa, 'verify', return_value={}))
            stack.enter_context(patch.object(offline_gepa, 'progress',
                return_value=dict(job_id='job', gepa=True, job_status='running')))
            stack.enter_context(patch.object(offline_gepa.subprocess, 'Popen', return_value=child))
            stack.enter_context(patch.object(offline_gepa.time, 'sleep',
                side_effect=lambda _: os.kill(os.getpid(), signal.SIGTERM)))
            stack.enter_context(redirect_stdout(io.StringIO()))
            self.assertEqual(offline_gepa.run(), 143)
            self.assertEqual(child.signals, [signal.SIGTERM])
            self.assertEqual(json.loads((gepa / 'status.json').read_text())['status'], 'paused')
            self.assertEqual({s: signal.getsignal(s) for s in originals}, originals)

    def test_runner_checks_job_and_feedback_instead_of_only_exit_code(self):
        """CLI 正常退出不等于搜索闭环完成；拒绝是有效结论，执行异常不是。"""
        class Child:
            returncode = 0

            def poll(self):
                return self.returncode

            def wait(self, timeout=None):
                return self.returncode

        cases = [(status, 'none', 'failed') for status in
                 ('error', 'inconclusive', 'cancelled', 'interrupted', 'running', 'pending')]
        cases += [('rejected', 'none', 'failed'), ('rejected', 'committed', 'finished'),
                  ('rejected', 'empty', 'finished'), ('rejected', 'rejected', 'failed'),
                  ('rejected', 'obsolete', 'failed'),
                  ('rejected', 'collected', 'failed'), ('rejected', 'pending', 'failed'),
                  ('passed', 'committed', 'candidate_ready')]
        for status, feedback, expected in cases:
            with self.subTest(status=status, feedback=feedback), \
                    tempfile.TemporaryDirectory() as folder, ExitStack() as stack:
                root = Path(folder)
                training, gepa = root / 'training', root / 'gepa'
                training.mkdir()
                gepa.mkdir()
                for name, value in dict(STAGE1=training, STAGE2=gepa,
                                        STATUS=gepa / 'status.json').items():
                    stack.enter_context(patch.object(offline_gepa, name, value))
                stack.enter_context(patch.object(offline_gepa, 'readiness',
                    return_value=(dict(service='local-test.json'), {})))
                stack.enter_context(patch.object(offline_gepa, 'verify', return_value={}))
                stack.enter_context(patch.object(offline_gepa, 'progress', return_value=dict(
                    job_id='job', gepa=True, job_status=status,
                    published_in_offline_workspace=status == 'passed',
                    memory_feedback=dict(status=feedback))))
                stack.enter_context(patch.object(offline_gepa.subprocess, 'Popen', return_value=Child()))
                stack.enter_context(redirect_stdout(io.StringIO()))
                self.assertEqual(offline_gepa.run(), 1 if expected == 'failed' else 0)
                self.assertEqual(json.loads((gepa / 'status.json').read_text())['status'], expected)

    def test_retry_keeps_service_until_feedback_and_closes_on_request_failure(self):
        """同一个服务负责重试到反馈提交；管理请求失败也不能遗留后台进程。"""
        class Child:
            def __init__(self):
                self.returncode = None
                self.stdout = io.StringIO()
                self.stops = 0

            def poll(self):
                return self.returncode

            def terminate(self):
                self.stops += 1
                self.returncode = 0

            def wait(self, timeout=None):
                return self.returncode

        cases = [("passed", "committed", False), ("passed", "committed", True),
                 ("rejected", "committed", False), ("rejected", "empty", False),
                 ("passed", "rejected", False), ("rejected", "obsolete", False),
                 ("error", "committed", False)]
        for final_status, final_feedback, fail in cases:
            with self.subTest(status=final_status, feedback=final_feedback, request_failed=fail), \
                    tempfile.TemporaryDirectory() as folder, \
                    ExitStack() as stack:
                root = Path(folder)
                training, gepa = root / "training", root / "gepa"
                training.mkdir()
                gepa.mkdir()
                child = Child()
                states = [
                    dict(job_id="job", gepa=True, job_status="error"),
                    dict(job_id="job", gepa=True, job_status="running"),
                    dict(job_id="job", gepa=True, job_status=final_status,
                         memory_feedback=dict(status="pending")),
                    dict(job_id="job", gepa=True, job_status=final_status,
                         published_in_offline_workspace=final_status == "passed",
                         memory_feedback=dict(status=final_feedback)),
                ]
                def progress(*_args):
                    return states.pop(0) if len(states) > 1 else states[0]
                for name, value in dict(STAGE1=training, STAGE2=gepa,
                                       STATUS=gepa / "status.json").items():
                    stack.enter_context(patch.object(offline_gepa, name, value))
                stack.enter_context(patch.object(offline_gepa, "readiness",
                    return_value=(dict(service="local-test.json"), {})))
                stack.enter_context(patch.object(offline_gepa, "verify", return_value={}))
                stack.enter_context(patch.object(offline_gepa, "progress", side_effect=progress))
                popen = stack.enter_context(patch.object(
                    offline_gepa.subprocess, "Popen", return_value=child))
                retry = stack.enter_context(patch.object(
                    offline_gepa, "request_retry",
                    side_effect=RuntimeError("retry_failed") if fail else None))
                sleep = stack.enter_context(patch.object(offline_gepa.time, "sleep"))
                # 终态漏判必须在本机有限步内失败，不能让回归测试无限空转。
                def bounded_wait(_):
                    self.assertLessEqual(sleep.call_count, 3, "反馈已终态，运行器仍在等待")
                sleep.side_effect = bounded_wait
                stack.enter_context(redirect_stdout(io.StringIO()))
                complete = final_status in ("passed", "rejected") and final_feedback in (
                    "none", "empty", "committed")
                if fail:
                    with self.assertRaisesRegex(RuntimeError, "retry_failed"):
                        offline_gepa.run("job")
                else:
                    self.assertEqual(offline_gepa.run("job"), 0 if complete else 1)
                    self.assertEqual(sleep.call_count, 2)
                self.assertEqual(popen.call_args.args[0][1], "serve")
                retry.assert_called_once_with(child, {}, "job")
                self.assertEqual(child.stops, 1)
                self.assertTrue(child.stdout.closed)
                status = json.loads((gepa / "status.json").read_text())["status"]
                expected = ("failed" if fail or not complete else
                            "candidate_ready" if final_status == "passed" else "finished")
                self.assertEqual(status, expected)


    def test_retry_attaches_to_automatic_resume_without_duplicate_request(self):
        """服务已恢复同一作业时只持续观察；其他作业或错误状态仍走正式重试接口。"""
        for job, status in (("job", "pending"), ("job", "running"),
                            ("job", "error"), ("other", "running")):
            with self.subTest(job=job, status=status):
                child = type("Child", (), {})()
                child.stdout = io.StringIO(
                    "trajectory-integration.v1 listening=127.0.0.1:12345\n")
                with patch.object(offline_gepa.select, "select",
                                  return_value=([child.stdout], [], [])), patch.object(
                        offline_gepa, "progress",
                        return_value=dict(job_id=job, job_status=status)), patch.object(
                        offline_gepa.urllib.request, "build_opener") as opener:
                    opener.return_value.open.return_value.__enter__.return_value = (
                        io.StringIO('{"ok":true}'))
                    offline_gepa.request_retry(
                        child, {"EVOLUTION_BUSINESS_TOKEN": "local-test-token"}, "job")
                    resumed = job == "job" and status in ("pending", "running")
                    self.assertEqual(opener.call_count, 0 if resumed else 1)
                    if not resumed:
                        request = opener.return_value.open.call_args.args[0]
                        self.assertEqual(json.loads(request.data)["job_id"], "job")

    def test_interrupted_derived_holdout_is_resumable_but_terminal_result_is_not(self):
        """只恢复已经冻结且未完成的派生 Holdout，禁止重复执行已完成结果。"""
        with tempfile.TemporaryDirectory() as folder, patch.object(
                offline_gepa, "WORKSPACE", Path(folder)):
            root = Path(folder) / "agent-evaluation"
            search = root / "job/search"
            search.mkdir(parents=True)
            state_file = root / "state.json"
            tree_file = search / "tree.json"
            replay = dict(protocol="derived-top-k-v2", state="queued")
            final = dict(protocol="derived-top-k-v2", status="running",
                         candidates=[dict(node=27, status="running")])
            derived = dict(protocol="derived-top-k-v2")
            state_file.write_text(json.dumps(dict(jobs=dict(job=dict(
                status="error", final_holdout_replay=replay)))))
            tree_file.write_text(json.dumps(dict(
                status="cancelled", holdout_started=True,
                final_holdout=final, derived_selection_protocol=derived)))
            current = dict(job_id="job", job_status="error", tree_status="cancelled")
            self.assertTrue(offline_gepa.has_derived_holdout_request("job", current))

            # 作业已经产生终态后，即使旧树里仍残留计划也不能再次重放留出集。
            state_file.write_text(json.dumps(dict(jobs=dict(job=dict(
                status="passed", final_holdout_replay=dict(
                    protocol="derived-top-k-v2", state="completed"))))))
            final["status"] = "completed"
            tree_file.write_text(json.dumps(dict(
                status="completed", holdout_started=True,
                final_holdout=final, derived_selection_protocol=derived)))
            self.assertFalse(offline_gepa.has_derived_holdout_request(
                "job", dict(job_id="job", job_status="passed", tree_status="completed")))


if __name__ == '__main__':
    unittest.main()
