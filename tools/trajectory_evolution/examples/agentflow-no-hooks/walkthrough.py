"""Protocol exerciser: real Evolution RPCs, fixed model/tool responses.

A parent program launches and joins child processes. There is no DataBus or
AgentFlow completion callback here. This does not run AgentFlow, call a model
provider, or test a sandbox. Never use it as the runtime's async adapter.
"""
import argparse
from concurrent.futures import ThreadPoolExecutor, as_completed
import json
import os
from pathlib import Path
import subprocess
import sys
import time
import uuid

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "clients" / "python"))
from evolution import Evolution


def client():
    return Evolution(
        os.environ.get("EVOLUTION_URL", "http://127.0.0.1:18766"),
        os.environ["EVOLUTION_USER_A_TOKEN"],
    )


def begin_step(evo, run_id, execution, actor, state, causes=()):
    step = evo.invoke("step.begin", {
        "run_id": run_id, "execution_id": execution, "actor_id": actor,
        "state": state, "caused_by": list(causes),
    })
    return {"run_id": run_id, "step_id": step["step_id"]}, step


def end_step(evo, fields, step, output):
    return evo.invoke("step.end", {
        **fields, "decision": step["decision"], "status": "completed", "output": output,
    })


def fixed_model(evo, fields, instruction, reply):
    """The fixed provider stands in for the host's original model call."""
    prepared = evo.invoke("model.prepare", {
        **fields, "request": {
            "model": "tutorial-fixed-response",
            "messages": [{"role": "user", "content": instruction}],
        },
    })
    actual_request = prepared["request"]
    # A real host passes actual_request to its own provider, not the old body.
    # The fixture deliberately ignores semantics; it cannot prove policy benefit.
    if not isinstance(actual_request.get("messages"), list):
        raise RuntimeError("tutorial_request_invalid")
    response = {"choices": [{"message": {"role": "assistant", "content": reply}}]}
    evo.invoke("model.observe", {
        **fields, "call_id": prepared["call_id"], "request": actual_request,
        "output": response, "status": "completed", "transport_allowed": True,
    })
    return response


def coordinator_dispatch(payload):
    """First Coordinator process: leave its step open while Skills are pending."""
    evo = client()
    fields, step = begin_step(evo, payload["run_id"], "coordinator", "coordinator",
                              {"completed": [], "image_available": False})
    context = {"fields": fields, "step": step, "manifests": []}
    if step["decision"] == "stop":
        end_step(evo, fields, step, {"reason": step["reason"]})
        return context
    fixed_model(evo, fields, "并行查询穿搭资料和尝试生成图片。",
                "派发两个独立 Skill，允许图片失败后保留穿搭结果。")
    planned = evo.invoke("actions.prepare", {**fields, "actions": [
        {"id": "dispatch-style", "operation": "outfit-stylist",
         "arguments": {"prompt": "查询适合阴雨天气的穿搭。", "load_skills": []}},
        {"id": "dispatch-image", "operation": "forge-image-gen",
         "arguments": {"prompt": "尝试生成搭配图片。", "load_skills": []}},
    ]})["actions"]
    for item in planned:
        context["manifests"].append({
            "exec_skill_key": item["id"],
            "skill_name": "outfit-stylist" if item["id"] == "dispatch-style" else "forge-image-gen",
            "evolution": {"run_id": fields["run_id"], "parent_step_id": fields["step_id"],
                          "parent_call_id": item["call_id"], "cause_event_id": item["event_id"],
                          "decision": item["decision"], "reason": item["reason"]},
        })
    # Serializing this context emulates DataBus/queue handoff. It is not task.end.
    return context


def skill_executor(manifest):
    """One process per invocation; business failure is a result, not a crash."""
    evo = client()
    parent = manifest["evolution"]
    result = {"id": manifest["exec_skill_key"], "executed": False,
              "status": "cancelled", "output": {"reason": parent["reason"]}}
    if parent["decision"] == "reject":
        return result
    result["executed"] = True
    fields, step = begin_step(evo, parent["run_id"], manifest["exec_skill_key"], "executor",
                              {"skill": manifest["skill_name"], "completed": False},
                              [parent["cause_event_id"]])
    if step["decision"] == "stop":
        result["output"] = {"reason": step["reason"]}
        result["end_event_id"] = end_step(evo, fields, step, result["output"])["event_id"]
        return result
    fixed_model(evo, fields, "执行这个 Skill 的任务。", "调用演示工具，然后报告真实结果。")
    is_style = manifest["skill_name"] == "outfit-stylist"
    action = evo.invoke("actions.prepare", {**fields, "actions": [{
        "id": "tool-1", "operation": "fixture_weather" if is_style else "fixture_image",
        "arguments": {"city": "杭州"} if is_style else {"subject": "雨天穿搭"},
    }]})["actions"][0]
    if action["decision"] == "reject":
        status, executed, output = "cancelled", False, {"reason": action["reason"]}
    elif is_style:
        time.sleep(0.2)  # Make the successful independent branch deliberately slower.
        status, executed, output = "completed", True, {"weather": "小雨", "advice": "防水外套和防滑鞋"}
    else:
        status, executed, output = "failed", True, {"error_class": "fixture_image_unavailable"}
    evo.invoke("action.observe", {
        **fields, "call_id": action["call_id"], "status": status,
        "executed": executed, "output": output,
    })
    evo.invoke("state.record", {
        **fields, "before": {"result": None}, "after": {"status": status, "result": output},
    })
    result.update(status=status, output=output)
    result["end_event_id"] = end_step(evo, fields, step, output)["event_id"]
    return result


def coordinator_resume(payload):
    """New Coordinator process: join by invocation ID, then produce the answer."""
    evo = client()
    context, results = payload["context"], payload["results"]
    fields = context["fields"]
    if context["step"]["decision"] == "stop":
        return evo.invoke("task.end", {"run_id": fields["run_id"], "status": "completed",
            "business_status": "partial", "output": "Coordinator 已按循环决定停止。"})
    by_id = {item["exec_skill_key"]: item for item in context["manifests"]}
    causes = []
    for result in results:  # Completion order may differ from dispatch order.
        parent = by_id[result["id"]]["evolution"]
        receipt = evo.invoke("action.observe", {
            **fields, "call_id": parent["parent_call_id"], "status": result["status"],
            "executed": result["executed"], "output": result["output"],
        })
        causes.append(receipt["event_id"])
        if result.get("end_event_id"):
            causes.append(result["end_event_id"])
    end_step(evo, fields, context["step"], {"results": results})
    state = {"completed": [r["id"] for r in results if r["status"] == "completed"],
             "uncompleted": [r["id"] for r in results if r["status"] != "completed"]}
    fields, step = begin_step(evo, fields["run_id"], "coordinator", "coordinator", state, causes)
    answer = "穿搭建议已完成；图片生成失败，没有取消穿搭分支。"
    if step["decision"] != "stop":
        fixed_model(evo, fields, "汇总两个 Skill 的真实结果。", answer)
    else:
        answer = "已收齐分支结果，Coordinator 按循环决定停止进一步推理。"
    end_step(evo, fields, step, {"answer": answer})
    return evo.invoke("task.end", {"run_id": fields["run_id"], "output": answer,
        "status": "completed", "business_status": "partial"})


def run_child(operation, payload):
    # Token is inherited from the protected process environment, never JSON/argv.
    completed = subprocess.run(
        [sys.executable, str(Path(__file__).resolve()), operation],
        input=json.dumps(payload, ensure_ascii=False), capture_output=True,
        text=True, encoding="utf-8", timeout=45,
    )
    if completed.returncode:
        raise RuntimeError("tutorial_child_failed")
    return json.loads(completed.stdout)


def main():
    operations = {"_dispatch": coordinator_dispatch, "_skill": skill_executor, "_resume": coordinator_resume}
    if len(sys.argv) == 2 and sys.argv[1] in operations:
        print(json.dumps(operations[sys.argv[1]](json.load(sys.stdin)), ensure_ascii=False))
        return
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--level", choices=["L0", "L1", "L2", "L3"], default="L0")
    parser.add_argument("--conversation", default="tutorial-conv-a")
    parser.add_argument("--wait-seconds", type=float, default=30)
    args = parser.parse_args()
    if not 0 < args.wait_seconds <= 1800:
        parser.error("--wait-seconds must be greater than 0 and at most 1800")
    evo = client()
    started = evo.invoke("task.begin", {
        "conversation_id": args.conversation, "input": "查询杭州雨天穿搭，并尝试生成搭配图片。",
        "level": args.level, "capabilities": ["model", "actions", "loop"],
    }, request_id=uuid.uuid4().hex)
    run_id = started["run_id"]
    print("run_id=" + run_id, flush=True)
    try:
        context = run_child("_dispatch", {"run_id": run_id})
        results = []
        with ThreadPoolExecutor(max_workers=2) as scheduler:
            futures = [scheduler.submit(run_child, "_skill", item) for item in context["manifests"]]
            for future in as_completed(futures):
                results.append(future.result())
        run_child("_resume", {"context": context, "results": results})
    except Exception:
        try:
            evo.invoke("task.abort", {"run_id": run_id, "status": "unknown", "capture_complete": False,
                                     "output": {"reason": "tutorial_observation_or_worker_failed"}})
        except Exception:
            pass  # Service recovery must close the printed run_id; never claim success.
        raise RuntimeError("tutorial_incomplete_check_task_status") from None
    deadline = time.monotonic() + args.wait_seconds
    while True:
        status = evo.invoke("task.status", {"run_id": run_id})
        if status["evaluation_status"] not in ("pending", "running") or time.monotonic() >= deadline:
            break
        time.sleep(0.2)
    print(json.dumps({"fixture_only": True, "branch_results": results, **status}, ensure_ascii=False, indent=2))
    if not status["capture_complete"] or status["evaluation_status"] != "completed":
        raise RuntimeError("tutorial_evaluation_not_completed")


if __name__ == "__main__":
    main()
