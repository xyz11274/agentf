#!/usr/bin/env python3
"""Replay the frozen real-v1 L0/L3 comparison, without training or publication."""

import argparse
import copy
import fcntl
import hashlib
import json
import os
from pathlib import Path
import shlex
import socket
import subprocess
import sys
import tempfile
import uuid
from datetime import datetime, timezone

ROOT = Path(__file__).resolve().parents[1]
MANIFEST = ROOT / "configs/reproduction/v1-l3/manifest.json"


def read_json(path):
    return json.loads(path.read_text(encoding="utf-8"))


def digest(path):
    value = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            value.update(chunk)
    return value.hexdigest()


def verified_inputs(manifest):
    for relative, expected in manifest["sha256"].items():
        path = ROOT / relative
        if not path.is_file() or digest(path) != expected:
            raise ValueError("Frozen input missing or changed: " + relative)
    bundle = read_json(ROOT / manifest["published_version"])
    if bundle["version"] != manifest["candidate_version"] or not bundle["evaluation"]["gate"]["passed"]:
        raise ValueError("Frozen publication does not match the accepted candidate")
    config = read_json(ROOT / manifest["evaluation_config"])
    host = config["variants"][0]
    config["variants"] = []
    for name in ("baseline", "candidate"):
        variant = copy.deepcopy(host)
        variant.update(id=name, version=manifest[name + "_version"],
                       artifact=read_json(ROOT / manifest[name + "_artifact"]))
        # Store trial diagnostics in its own workdir, not the old experiment.
        variant["command"][-1] = variant["command"][-1].replace(
            manifest["original_daemon_log"], "daemon.log")
        config["variants"].append(variant)
    if config["splits"] != ["holdout"] or len(config["task_ids"]) != 15:
        raise ValueError("Expected the frozen 15-task holdout")
    return config


def environment(manifest, env_file):
    # Parse assignments only; never execute a credential file as shell code.
    values = {}
    if env_file.is_file():
        for line in env_file.read_text(encoding="utf-8").splitlines():
            words = shlex.split(line, comments=True)
            if words and words[0] == "export":
                words = words[1:]
            if not words:
                continue
            if len(words) != 1 or "=" not in words[0]:
                raise ValueError("Credential file must contain simple NAME=value assignments")
            key, value = words[0].split("=", 1)
            values[key] = value
    result = os.environ.copy()
    for key in manifest["credential_env"]:
        if not result.get(key) and values.get(key):
            result[key] = values[key]
    for key in ("OPENAI_API_KEY", "OPENAI_IMAGE_API_KEY", "ARK_API_KEY"):
        if not result.get(key):
            raise ValueError("Missing credential environment variable: " + key)
    result["AF_LLM_API_KEY"] = result["OPENAI_API_KEY"]
    result.update({key: value.replace("{repo}", str(ROOT))
                   for key, value in manifest["environment"].items()})
    result["PATH"] = str(Path(manifest["python"]).parent) + os.pathsep + result.get("PATH", "")
    return result


def check_ports(manifest):
    for port in manifest["ports"]:
        with socket.socket() as probe:
            try:
                probe.bind(("127.0.0.1", port))
            except OSError:
                raise ValueError(f"Test port {port} is in use; no existing service was stopped") from None


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true", help="validate frozen files and environment; no API calls")
    parser.add_argument("--env-file", type=Path, default=ROOT / "examples/agent/outfit-test-agent/.env")
    parser.add_argument("--output-root", type=Path, help="new result directory (existing paths are refused)")
    args = parser.parse_args()
    os.chdir(ROOT)
    manifest = read_json(MANIFEST)
    config = verified_inputs(manifest)
    env = environment(manifest, args.env_file)
    cli = ROOT / manifest["cli"]
    if not cli.is_file() or not Path(manifest["python"]).is_file():
        raise ValueError("Build the evolution CLI and activate the original v1 Python environment first")
    # Separate reproductions must not compete for the original trial ports.
    with (ROOT / "build/reproduce-v1-l3.lock").open("a") as lock:
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            raise ValueError("Another fixed-v1 reproduction is running") from None
        check_ports(manifest)
        subprocess.run([str(cli), "dataset", "check", str(ROOT / manifest["dataset"]),
                        "--require-reviewed"], env=env, check=True, timeout=30, stdout=subprocess.DEVNULL)
        subprocess.run([manifest["python"], "-B", "-c",
                        "import fastapi,uvicorn,httpx,requests,PIL,agentflow_runtime,evolution_entry,evolution_configured"],
                       env=env, check=True, timeout=30, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        print("Frozen L0/L3 versions verified; 15 holdout tasks, 30 real Agent executions.", flush=True)
        print("deepseek-v4-flash; temperature=0; thinking=disabled; no training or publication.", flush=True)
        if args.check:
            print("Preflight passed. No Agent or model API was called.")
            return 0
        stamp = datetime.now(timezone.utc).strftime("%Y%m%d-%H%M%S") + "-" + uuid.uuid4().hex[:6]
        output = (args.output_root or ROOT / "trace_output/v1-l3-reproduction" / stamp).absolute()
        if output.exists() or output.is_symlink():
            raise ValueError("Result directory already exists; select a new --output-root")
        # The generated input stays under build; trace_output holds results only.
        prep = Path(tempfile.mkdtemp(prefix="v1-l3-reproduce-", dir=ROOT / "build"))
        config_path = prep / "evaluation.json"
        config_path.write_text(json.dumps(config, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
        config_path.chmod(0o600)
        print("Results: " + str(output), flush=True)
        process = subprocess.Popen([str(cli), "dataset", "run", str(ROOT / manifest["dataset"]),
                                    "--config", str(config_path), "--output-root", str(output)], env=env)
        try:
            result = process.wait()
        except KeyboardInterrupt:
            process.terminate()
            try:
                process.wait(timeout=30)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
            return 130
        if (output / "report.json").is_file():
            summary = read_json(output / "report.json").get("summary", {}).get("by_variant", {})
            print("\nvariant  passed/trials  invalid-loop mean  mean agent ms")
            for name in ("baseline", "candidate"):
                value = summary.get(name)
                if not value:
                    print(name, "not completed")
                    continue
                print(name, f'{value.get("passed", 0)}/{value.get("trials", 0)}',
                      value.get("invalid_loop_assessment", {}).get("mean", "unscored"),
                      value.get("mean_agent_duration_ms", "unscored"))
            print("Report: " + str(output / "report.md"))
        print("Exit 1 means a scored task failed; exit 3 means incomplete evaluation. Neither is hidden.")
        return result


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (ValueError, OSError, subprocess.SubprocessError) as error:
        # Never print environment values, model replies or raw subprocess output.
        print("ERROR: " + (str(error) if isinstance(error, ValueError) else type(error).__name__), file=sys.stderr)
        sys.exit(2)
