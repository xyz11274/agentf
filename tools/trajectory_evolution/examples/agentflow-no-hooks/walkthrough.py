# walkthrough 接入示例：演示宿主如何在已有执行边界调用公开接口。
# 示例用于说明接入方式，实际执行和业务工具仍由宿主自身负责。
# 需要保留真实请求、结果及因果关联，不能用示例常量代替生产运行证据。

"""协议演示：真实组件调用，固定模型和工具响应。
父程序启动并等待子进程，此示例没有 DataBus 或 AgentFlow 完成回调。
不运行真实 AgentFlow、远程模型或沙箱，不能将此示例当作运行时异步适配器。"""

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


# 取得示例中复用的组件客户端，避免每个打点重新创建资源。
def client():
    return Evolution(
        os.environ.get("EVOLUTION_URL", "http://127.0.0.1:18766"),
        os.environ["EVOLUTION_USER_A_TOKEN"],
    )


# 为当前步骤建立规范身份并取得前置策略决定。
def begin_step(evo, run_id, execution, actor, state, causes=()):
    step = evo.invoke(
        "step.begin",
        {
            "run_id": run_id,
            "execution_id": execution,
            "actor_id": actor,
            "state": state,
            "caused_by": list(causes),
        },
    )
    return {"run_id": run_id, "step_id": step["step_id"]}, step


# 记录当前步骤的真实输出和结束状态，保留与开始事件的对应关系。
def end_step(evo, fields, step, output):
    return evo.invoke(
        "step.end",
        {
            **fields,
            "decision": step["decision"],
            "status": "completed",
            "output": output,
        },
    )


# 返回示例中的固定模型结果，只用于验证接入流程，不代表真实模型能力。
def fixed_model(evo, fields, instruction, reply):
    """以固定响应替代宿主原始模型调用，仅验证协议接线。"""
    prepared = evo.invoke(
        "model.prepare",
        {
            **fields,
            "request": {
                "model": "tutorial-fixed-response",
                "messages": [{"role": "user", "content": instruction}],
            },
        },
    )
    actual_request = prepared["request"]
    # 真实宿主应把 actual_request 发给模型提供方，而非继续发送旧请求体。
    # 该固定示例不理解业务语义，不能证明策略确有收益。
    if not isinstance(actual_request.get("messages"), list):
        raise RuntimeError("tutorial_request_invalid")
    response = {"choices": [{"message": {"role": "assistant", "content": reply}}]}
    evo.invoke(
        "model.observe",
        {
            **fields,
            "call_id": prepared["call_id"],
            "request": actual_request,
            "output": response,
            "status": "completed",
            "transport_allowed": True,
        },
    )
    return response


# 示范协调端提交异步任务并传递上下文，提交确认不关闭整个运行。
def coordinator_dispatch(payload):
    """第一个协调进程提交任务；子任务未完成时保持协调步骤打开。"""
    evo = client()
    fields, step = begin_step(
        evo,
        payload["run_id"],
        "coordinator",
        "coordinator",
        {"completed": [], "image_available": False},
    )
    context = {"fields": fields, "step": step, "manifests": []}
    if step["decision"] == "stop":
        end_step(evo, fields, step, {"reason": step["reason"]})
        return context
    fixed_model(
        evo,
        fields,
        "并行查询穿搭资料和尝试生成图片。",
        "派发两个独立 Skill，允许图片失败后保留穿搭结果。",
    )
    planned = evo.invoke(
        "actions.prepare",
        {
            **fields,
            "actions": [
                {
                    "id": "dispatch-style",
                    "operation": "outfit-stylist",
                    "arguments": {
                        "prompt": "查询适合阴雨天气的穿搭。",
                        "load_skills": [],
                    },
                },
                {
                    "id": "dispatch-image",
                    "operation": "forge-image-gen",
                    "arguments": {"prompt": "尝试生成搭配图片。", "load_skills": []},
                },
            ],
        },
    )["actions"]
    for item in planned:
        context["manifests"].append(
            {
                "exec_skill_key": item["id"],
                "skill_name": "outfit-stylist"
                if item["id"] == "dispatch-style"
                else "forge-image-gen",
                "evolution": {
                    "run_id": fields["run_id"],
                    "parent_step_id": fields["step_id"],
                    "parent_call_id": item["call_id"],
                    "cause_event_id": item["event_id"],
                    "decision": item["decision"],
                    "reason": item["reason"],
                },
            }
        )
    # 序列化上下文模拟 DataBus 或队列交付，不代表 task.end。
    return context


# 示范工作端恢复上下文、执行任务并记录真实终态，结果归属不依赖返回顺序。
def skill_executor(manifest):
    """每次调用独立进程执行；业务失败作为结果记录，而非模拟进程崩溃。"""
    evo = client()
    parent = manifest["evolution"]
    result = {
        "id": manifest["exec_skill_key"],
        "executed": False,
        "status": "cancelled",
        "output": {"reason": parent["reason"]},
    }
    if parent["decision"] == "reject":
        return result
    result["executed"] = True
    fields, step = begin_step(
        evo,
        parent["run_id"],
        manifest["exec_skill_key"],
        "executor",
        {"skill": manifest["skill_name"], "completed": False},
        [parent["cause_event_id"]],
    )
    if step["decision"] == "stop":
        result["output"] = {"reason": step["reason"]}
        result["end_event_id"] = end_step(evo, fields, step, result["output"])[
            "event_id"
        ]
        return result
    fixed_model(
        evo, fields, "执行这个 Skill 的任务。", "调用演示工具，然后报告真实结果。"
    )
    is_style = manifest["skill_name"] == "outfit-stylist"
    action = evo.invoke(
        "actions.prepare",
        {
            **fields,
            "actions": [
                {
                    "id": "tool-1",
                    "operation": "fixture_weather" if is_style else "fixture_image",
                    "arguments": {"city": "杭州"}
                    if is_style
                    else {"subject": "雨天穿搭"},
                }
            ],
        },
    )["actions"][0]
    if action["decision"] == "reject":
        status, executed, output = "cancelled", False, {"reason": action["reason"]}
    elif is_style:
        time.sleep(0.2)  # 故意让成功的独立分支更慢，用于验证失败隔离。
        status, executed, output = (
            "completed",
            True,
            {"weather": "小雨", "advice": "防水外套和防滑鞋"},
        )
    else:
        status, executed, output = (
            "failed",
            True,
            {"error_class": "fixture_image_unavailable"},
        )
    evo.invoke(
        "action.observe",
        {
            **fields,
            "call_id": action["call_id"],
            "status": status,
            "executed": executed,
            "output": output,
        },
    )
    evo.invoke(
        "state.record",
        {
            **fields,
            "before": {"result": None},
            "after": {"status": status, "result": output},
        },
    )
    result.update(status=status, output=output)
    result["end_event_id"] = end_step(evo, fields, step, output)["event_id"]
    return result


# 示范协调端收到结果后恢复原运行，汇聚真实完成分支再决定后续工作。
def coordinator_resume(payload):
    """新协调进程按调用身份汇聚结果，然后生成回答。"""
    evo = client()
    context, results = payload["context"], payload["results"]
    fields = context["fields"]
    if context["step"]["decision"] == "stop":
        return evo.invoke(
            "task.end",
            {
                "run_id": fields["run_id"],
                "status": "completed",
                "business_status": "partial",
                "output": "Coordinator 已按循环决定停止。",
            },
        )
    by_id = {item["exec_skill_key"]: item for item in context["manifests"]}
    causes = []
    for result in results:  # 完成顺序可能与派发顺序不同。
        parent = by_id[result["id"]]["evolution"]
        receipt = evo.invoke(
            "action.observe",
            {
                **fields,
                "call_id": parent["parent_call_id"],
                "status": result["status"],
                "executed": result["executed"],
                "output": result["output"],
            },
        )
        causes.append(receipt["event_id"])
        if result.get("end_event_id"):
            causes.append(result["end_event_id"])
    end_step(evo, fields, context["step"], {"results": results})
    state = {
        "completed": [r["id"] for r in results if r["status"] == "completed"],
        "uncompleted": [r["id"] for r in results if r["status"] != "completed"],
    }
    fields, step = begin_step(
        evo, fields["run_id"], "coordinator", "coordinator", state, causes
    )
    answer = "穿搭建议已完成；图片生成失败，没有取消穿搭分支。"
    if step["decision"] != "stop":
        fixed_model(evo, fields, "汇总两个 Skill 的真实结果。", answer)
    else:
        answer = "已收齐分支结果，Coordinator 按循环决定停止进一步推理。"
    end_step(evo, fields, step, {"answer": answer})
    return evo.invoke(
        "task.end",
        {
            "run_id": fields["run_id"],
            "output": answer,
            "status": "completed",
            "business_status": "partial",
        },
    )


# 在独立执行入口处理子任务，传递序列化身份而非共享 Python 对象。
def run_child(operation, payload):
    # 令牌从受保护的进程环境继承，不能放入 JSON 或命令行参数。
    completed = subprocess.run(
        [sys.executable, str(Path(__file__).resolve()), operation],
        input=json.dumps(payload, ensure_ascii=False),
        capture_output=True,
        text=True,
        encoding="utf-8",
        timeout=45,
    )
    if completed.returncode:
        raise RuntimeError("tutorial_child_failed")
    return json.loads(completed.stdout)


# 程序入口：读取参数后路由到对应实现，统一处理退出状态和资源收尾。
def main():
    operations = {
        "_dispatch": coordinator_dispatch,
        "_skill": skill_executor,
        "_resume": coordinator_resume,
    }
    if len(sys.argv) == 2 and sys.argv[1] in operations:
        print(
            json.dumps(
                operations[sys.argv[1]](json.load(sys.stdin)), ensure_ascii=False
            )
        )
        return
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--level", choices=["L0", "L1", "L2", "L3"], default="L0")
    parser.add_argument("--conversation", default="tutorial-conv-a")
    parser.add_argument("--wait-seconds", type=float, default=30)
    args = parser.parse_args()
    if not 0 < args.wait_seconds <= 1800:
        parser.error("--wait-seconds must be greater than 0 and at most 1800")
    evo = client()
    started = evo.invoke(
        "task.begin",
        {
            "conversation_id": args.conversation,
            "input": "查询杭州雨天穿搭，并尝试生成搭配图片。",
            "level": args.level,
            "capabilities": ["model", "actions", "loop"],
        },
        request_id=uuid.uuid4().hex,
    )
    run_id = started["run_id"]
    print("run_id=" + run_id, flush=True)
    try:
        context = run_child("_dispatch", {"run_id": run_id})
        results = []
        with ThreadPoolExecutor(max_workers=2) as scheduler:
            futures = [
                scheduler.submit(run_child, "_skill", item)
                for item in context["manifests"]
            ]
            for future in as_completed(futures):
                results.append(future.result())
        run_child("_resume", {"context": context, "results": results})
    except Exception:
        try:
            evo.invoke(
                "task.abort",
                {
                    "run_id": run_id,
                    "status": "unknown",
                    "capture_complete": False,
                    "output": {"reason": "tutorial_observation_or_worker_failed"},
                },
            )
        except Exception:
            pass  # 服务恢复必须关闭已输出的 run_id，不能未经证据报告成功。
        raise RuntimeError("tutorial_incomplete_check_task_status") from None
    deadline = time.monotonic() + args.wait_seconds
    while True:
        status = evo.invoke("task.status", {"run_id": run_id})
        if (
            status["evaluation_status"] not in ("pending", "running")
            or time.monotonic() >= deadline
        ):
            break
        time.sleep(0.2)
    print(
        json.dumps(
            {"fixture_only": True, "branch_results": results, **status},
            ensure_ascii=False,
            indent=2,
        )
    )
    if not status["capture_complete"] or status["evaluation_status"] != "completed":
        raise RuntimeError("tutorial_evaluation_not_completed")


if __name__ == "__main__":
    main()
