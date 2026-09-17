#!/usr/bin/env python3
"""Fixed representative eval, parallel across isolated daemons, serial within each."""
from concurrent.futures import ThreadPoolExecutor
import argparse
import json
from pathlib import Path
import subprocess
import sys

PACKAGE = Path(__file__).resolve().parents[1]
REPO = PACKAGE.parents[2]
sys.path.insert(0, str(REPO / "benchmark"))
from run_eval import compute_metrics


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--label", required=True)
    parser.add_argument("--workers", type=int, choices=(1, 2, 3), default=3)
    parser.add_argument("--judge", action="store_true")
    args = parser.parse_args()
    root = PACKAGE / "outputs/representative" / args.label
    if root.exists():
        raise SystemExit("Choose a fresh label; existing evidence is never overwritten")
    root.mkdir(parents=True)
    ids = json.loads((PACKAGE / "evals/representative-30.json").read_text())["case_ids"]

    def run_worker(index):
        label = args.label + f"/worker-{index}"
        command = [sys.executable, str(PACKAGE / "evals/run_representative.py"), "--label", label,
                   "--cases", ",".join(ids[index::args.workers]), "--concurrency", "1"]
        if args.judge:
            command.append("--judge")
        with (root / f"worker-{index}.log").open("w") as log:
            completed = subprocess.run(command, cwd=REPO, stdout=log, stderr=subprocess.STDOUT)
        if completed.returncode:
            raise RuntimeError(f"worker {index} failed; see its log")
        print("COMPLETED", index, flush=True)
        return json.loads((root / f"worker-{index}/report.json").read_text())

    print("RUN_DIRECTORY", root, flush=True)
    with ThreadPoolExecutor(max_workers=args.workers) as pool:
        reports = list(pool.map(run_worker, range(args.workers)))
    cases = [case for report in reports for case in report["cases"]]
    if len(cases) != len(ids) or {c["case_id"] for c in cases} != set(ids):
        raise RuntimeError("Evaluation did not return the exact selected cases")
    cases.sort(key=lambda c: ids.index(c["case_id"]))
    report = {"meta": {"isolation": "independent daemons and databases, serial cases per daemon",
                       "judge": args.judge, "source": "benchmark/datasets/product.json",
                       "workers": [r["meta"] for r in reports]}, **compute_metrics(cases), "cases": cases}
    (root / "report.json").write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n")
    print(json.dumps(report["summary"], ensure_ascii=False), flush=True)


if __name__ == "__main__":
    main()
