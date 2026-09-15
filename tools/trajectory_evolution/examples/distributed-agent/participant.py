"""Independent send/receive/collect processes over a small durable demo mailbox.

This exercises real Evolution HTTP calls, not a real model provider or framework.
SQLite is only the example's message transport; Evolution does not depend on it.
"""
import argparse
import json
import os
from pathlib import Path
import sqlite3
import sys
import time
import uuid

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "clients/python"))
from evolution import Evolution


def model(rpc, run, step, key, text):
    prepared = rpc("model.prepare", {"run_id": run, "step_id": step["step_id"],
        "request": {"messages": [{"role": "system", "content": "协议示例，不调用外部模型。"},
                                  {"role": "user", "content": text}]}}, key + "-prepare")
    # Replace this fixture with the original model client's real send/terminal callback.
    actual_request = prepared["request"]
    output = {"content": text, "provider": "local-fixture"}
    rpc("model.observe", {"run_id": run, "step_id": step["step_id"], "call_id": prepared["call_id"],
        "request": actual_request, "output": output, "transport_allowed": True,
        "status": "completed"}, key + "-observe")
    return output


def send(args, rpc):
    if args.mailbox.exists():
        raise RuntimeError("mailbox_already_exists_use_a_new_path")
    args.mailbox.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
    with sqlite3.connect(args.mailbox) as db:
        os.chmod(args.mailbox, 0o600)
        db.execute("CREATE TABLE metadata (key TEXT PRIMARY KEY, value TEXT)")
        db.execute("CREATE TABLE jobs (id TEXT PRIMARY KEY, payload TEXT, state TEXT, result TEXT)")
        begin_id = uuid.uuid4().hex
        db.execute("INSERT INTO metadata VALUES ('begin_id', ?)", (begin_id,))
        db.commit()
        run = rpc("task.begin", {"conversation_id": "distributed-demo", "input": "并发检索并汇总结果",
            "level": args.level, "capabilities": ["model", "actions", "loop"]}, begin_id)["run_id"]
        db.execute("INSERT INTO metadata VALUES ('run_id', ?)", (run,))
        db.commit()
        step = rpc("step.begin", {"run_id": run, "state": "准备两项独立任务"}, "sender-start")
        if step["decision"] == "stop":
            raise RuntimeError("demo_sender_stopped_by_policy")
        model(rpc, run, step, "sender-model", "把两项工作交给独立执行进程")
        jobs = [{"id": name, "operation": "submit_job", "arguments": {"topic": name},
                 "delegate": {"execution_id": name, "actor_id": "worker", "completion": args.completion}}
                for name in ["job-a", "job-b"]]
        plans = rpc("actions.prepare", {"run_id": run, "step_id": step["step_id"], "actions": jobs}, "sender-plan")["actions"]
        for plan in plans:
            if plan["decision"] != "allow":
                rpc("action.observe", {"run_id": run, "step_id": step["step_id"], "call_id": plan["call_id"],
                    "executed": False, "status": "cancelled", "output": plan["reason"]}, plan["id"] + "-skip")
                continue
            # Save context before delivery. Do not put service tokens into the message.
            payload = {"context": plan["context"], "topic": plan["id"]}
            db.execute("INSERT INTO jobs VALUES (?, ?, 'queued', NULL)", (plan["id"], json.dumps(payload)))
            db.commit()
            if args.completion == "delivery":
                rpc("action.observe", {"run_id": run, "step_id": step["step_id"], "call_id": plan["call_id"],
                    "executed": True, "status": "completed", "output": {"queued": plan["id"]}}, plan["id"] + "-delivered")
        if args.completion == "delivery":
            # Receiver processes need not have started. The sender remains independent.
            next_step = rpc("step.begin", {"run_id": run, "state": "后台任务已投递，继续自己的准备工作"}, "sender-next")
            if next_step["decision"] != "stop":
                model(rpc, run, next_step, "sender-background", "完成不依赖子结果的准备")
            rpc("step.end", {"run_id": run, "step_id": next_step["step_id"],
                "decision": next_step["decision"], "status": "completed"}, "sender-end")
    return {"run_id": run, "mailbox": str(args.mailbox), "sender_exited_without_waiting": True}


def receive(args, rpc):
    with sqlite3.connect(args.mailbox, timeout=10) as db:
        # Demo-only ownership claim; a production queue supplies its own lease/recovery policy.
        db.execute("BEGIN IMMEDIATE")
        row = db.execute("SELECT payload, state FROM jobs WHERE id=?", (args.job,)).fetchone()
        if row is None or row[1] != "queued":
            raise RuntimeError("job_not_queued_do_not_repeat_business_work")
        db.execute("UPDATE jobs SET state='running' WHERE id=?", (args.job,))
        db.commit()
        ctx = json.loads(row[0])["context"]
        step = rpc("step.begin", dict(ctx, state={"topic": args.job, "received": True}), args.job + "-start")
        if args.barrier:
            deadline = time.monotonic() + 15
            while db.execute("SELECT count(*) FROM jobs WHERE state='queued'").fetchone()[0]:
                if time.monotonic() > deadline:
                    raise RuntimeError("demo_peer_not_started")
                time.sleep(0.02)
        if step["decision"] == "stop":
            rpc("step.end", {"run_id": ctx["run_id"], "step_id": step["step_id"],
                "decision": "stop", "status": "completed"}, args.job + "-stop")
            status, output = "cancelled", {"reason": "loop_stopped"}
        else:
            model(rpc, ctx["run_id"], step, args.job + "-model", "处理 " + args.job)
            plan = rpc("actions.prepare", {"run_id": ctx["run_id"], "step_id": step["step_id"],
                "actions": [{"id": "lookup", "operation": "lookup", "arguments": {"topic": args.job}}]}, args.job + "-tools")["actions"][0]
            executed = plan["decision"] == "allow"
            status = ("failed" if args.fail else "completed") if executed else "cancelled"
            output = {"job": args.job, "result": "known_lookup_failure" if args.fail else "fixture_lookup_result"}
            rpc("action.observe", {"run_id": ctx["run_id"], "step_id": step["step_id"], "call_id": plan["call_id"],
                "status": status, "executed": executed, "output": output}, args.job + "-tool-result")
        if args.wait_for:
            deadline = time.monotonic() + 15
            while db.execute("SELECT state FROM jobs WHERE id=?", (args.wait_for,)).fetchone()[0] != "done":
                if time.monotonic() > deadline:
                    raise RuntimeError("demo_peer_not_finished")
                time.sleep(0.02)
        done = rpc("execution.end", {"run_id": ctx["run_id"], "execution_id": ctx["execution_id"],
            "status": status, "output": output}, args.job + "-end")
        result = {"job": args.job, "status": status, "output": output, "event_id": done["event_id"]}
        db.execute("UPDATE jobs SET state='done', result=? WHERE id=?", (json.dumps(result), args.job))
        db.commit()
        return result


def collect(args, rpc):
    with sqlite3.connect(args.mailbox) as db:
        if db.execute("SELECT count(*) FROM jobs WHERE state!='done'").fetchone()[0]:
            raise RuntimeError("results_not_ready_do_not_end_task")
        run = db.execute("SELECT value FROM metadata WHERE key='run_id'").fetchone()[0]
        results = [json.loads(row[0]) for row in db.execute("SELECT result FROM jobs ORDER BY id")]
    step = rpc("step.begin", {"run_id": run, "state": results,
        "caused_by": [item["event_id"] for item in results]}, "collector-start")
    answer = {"results": results}
    if step["decision"] != "stop":
        model(rpc, run, step, "collector-model", "分别汇总已成功与未完成的任务")
    rpc("step.end", {"run_id": run, "step_id": step["step_id"], "decision": step["decision"],
        "status": "completed", "output": answer}, "collector-step-end")
    rpc("task.end", {"run_id": run, "output": answer, "status": "completed",
        "business_status": "partial" if any(r["status"] != "completed" for r in results) else "succeeded"}, "collector-end")
    return rpc("task.status", {"run_id": run}, "collector-status")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("role", choices=["send", "receive", "collect"])
    parser.add_argument("mailbox", type=Path)
    parser.add_argument("--job", choices=["job-a", "job-b"])
    parser.add_argument("--level", choices=["L0", "L1", "L2", "L3"], default="L0")
    parser.add_argument("--completion", choices=["execution", "delivery"], default="delivery")
    parser.add_argument("--fail", action="store_true")
    parser.add_argument("--barrier", action="store_true", help="Test-only: wait for both receivers to start.")
    parser.add_argument("--wait-for", choices=["job-a", "job-b"], help="Test-only: force reverse terminal order.")
    args = parser.parse_args()
    if args.role != "send" and not args.mailbox.is_file():
        parser.error("mailbox_missing_run_send_first")
    if args.role == "receive" and not args.job:
        parser.error("receive_requires_job")
    client = Evolution(os.environ["EVOLUTION_URL"], os.environ["EVOLUTION_TOKEN"])
    result = {"send": send, "receive": receive, "collect": collect}[args.role](args, client.invoke)
    print(json.dumps(result, ensure_ascii=False))


if __name__ == "__main__":
    main()
