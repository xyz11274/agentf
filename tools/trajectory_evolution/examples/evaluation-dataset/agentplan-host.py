"""Real tool-using Agent example for the language-neutral evaluation host contract.

Python is only the example host, not a requirement of the C++ evaluator. Model
and image requests use the subscriber's Agent Plan endpoint; never fall back to
the ordinary pay-as-you-go route. Requires Pillow only for image decoding.
"""

import base64
from concurrent.futures import ThreadPoolExecutor, as_completed
from io import BytesIO
import json
import os
from pathlib import Path
import sys
import threading
import time
import urllib.error
import urllib.parse
import urllib.request


class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, *args, **kwargs):
        return None


OPENER = urllib.request.build_opener(urllib.request.ProxyHandler({}), NoRedirect())


def main():
    request = json.load(sys.stdin)
    task = request["task"]
    base = os.environ.get("OPENAI_BASE_URL", "").rstrip("/")
    key = os.environ.get("OPENAI_API_KEY", "")
    model = os.environ.get("OPENAI_MODEL", "")
    image_model = os.environ.get("ARK_IMAGE_MODEL", "doubao-seedream-5.0-lite")
    parsed = urllib.parse.urlsplit(base)
    if (parsed.scheme, parsed.netloc, parsed.path) != ("https", "ark.cn-beijing.volces.com", "/api/plan/v3") or not key or not model:
        print(json.dumps(dict(schema_version=1, trial_id=request["trial_id"], execution_status="environment_error",
                              output="请配置 Agent Plan 接口、模型和环境变量凭据。", state={})))
        return
    timeout = float(os.environ.get("AF_EVAL_API_TIMEOUT_SECONDS", "1800"))
    if not 0 < timeout <= 86400:
        raise ValueError("invalid timeout")
    root = Path.cwd().resolve()
    state, generated = {}, {}
    events, lock = [], threading.Lock()
    counters = {"steps": 0, "tool_calls": 0, "tokens": 0, "loop_count": 0}
    run_id = request["trial_id"]

    def event(kind, **fields):
        with lock:
            identifier = f"e{len(events) + 1}"
            events.append(dict(event_id=identifier, kind=kind, producer_id="example-host", actor_id="agent",
                               sequence=len(events) + 1, timestamp_us=str(time.time_ns() // 1000), **fields))
        return identifier

    def path_in_workspace(name):
        relative = Path(name)
        if relative.is_absolute() or ".." in relative.parts or not relative.parts:
            raise ValueError("path_rejected")
        path = (root / relative).resolve()
        if root not in path.parents:
            raise ValueError("path_rejected")
        return path

    def post(suffix, payload, limit=32 * 1024 * 1024):
        http = urllib.request.Request(base + suffix, data=json.dumps(payload).encode(),
                                      headers={"Authorization": "Bearer " + key, "Content-Type": "application/json"})
        with OPENER.open(http, timeout=timeout) as response:
            body = response.read(limit + 1)
        if len(body) > limit:
            raise ValueError("response_limit")
        return json.loads(body)

    def tool(name, arguments):
        if name == "read_source":
            source = arguments["name"]
            return task["initial_state"].get("data_sources", {}).get(source, {"error": "source_not_found", "retryable": False})
        if name == "write_file":
            target = path_in_workspace(arguments["path"])
            content = arguments["content"]
            if key in content or len(content.encode()) > 1024 * 1024:
                raise ValueError("content_rejected")
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text(content, encoding="utf-8")
            media = "application/json" if target.suffix == ".json" else "text/plain"
            if media == "application/json":
                json.loads(content)
            with lock:
                generated[arguments["path"]] = media
            return {"path": arguments["path"], "written": True}
        if name == "generate_image":
            from PIL import Image
            filename = arguments.get("path", "poster.png")
            target = path_in_workspace(filename)
            with lock:
                if state.get("image_requested"):
                    return {"error": "image_already_requested", "retryable": False}
                state["image_requested"] = True
            result = post("/images/generations", dict(model=image_model, prompt=arguments["prompt"],
                          size=task["initial_state"].get("image_size", "2K"), response_format="b64_json",
                          sequential_image_generation="disabled", watermark=True))
            item = result["data"][0]
            if item.get("b64_json"):
                image_bytes = base64.b64decode(item["b64_json"], validate=True)
            else:
                url = item["url"]
                location = urllib.parse.urlsplit(url)
                if location.scheme != "https" or not any((location.hostname or "").endswith(suffix)
                        for suffix in (".volces.com", ".byteimg.com", ".volccdn.com")):
                    raise ValueError("image_download_origin_rejected")
                # Never send the API credential to a signed artifact URL.
                with OPENER.open(url, timeout=timeout) as download:
                    image_bytes = download.read(16 * 1024 * 1024 + 1)
            if len(image_bytes) > 16 * 1024 * 1024:
                raise ValueError("image_size_limit")
            with Image.open(BytesIO(image_bytes)) as image:
                image.load()
                if image.width * image.height > 40000000:
                    raise ValueError("image_pixel_limit")
                target.parent.mkdir(parents=True, exist_ok=True)
                image.save(target, format="PNG")
                info = {"decoded": True, "width": image.width, "height": image.height, "path": filename}
            with lock:
                state["image"] = info
                generated[filename] = "image/png"
            return info
        raise ValueError("unknown_tool")

    definitions = [
        ("read_source", "读取一个数据源；不同源可并行。", {"name": {"type": "string"}}, ["name"]),
        ("write_file", "写入测试工作区文件。", {"path": {"type": "string"}, "content": {"type": "string"}}, ["path", "content"]),
        ("generate_image", "生成、下载并解码一张图片；每个任务最多请求一次。", {"prompt": {"type": "string"}, "path": {"type": "string"}}, ["prompt", "path"]),
    ]
    tools = [dict(type="function", function=dict(name=name, description=description,
                  parameters=dict(type="object", properties=properties, required=required, additionalProperties=False)))
             for name, description, properties, required in definitions]
    messages = [{"role": "system", "content": (Path(__file__).parent / "agent-system.txt").read_text(encoding="utf-8")}]
    start = event("run.start", input=task["task_input"])
    previous = start
    output, status = "", "completed"
    turns = [task["task_input"], *task.get("followup_inputs", [])]
    for turn_index, turn in enumerate(turns):
        messages.append({"role": "user", "content": turn})
        while True:
            if counters["steps"] >= int(task["budget"]["max_steps"]):
                output, status = "达到任务步骤预算，尚未完成。", "failed"
                break
            counters["steps"] += 1
            counters["loop_count"] += 1
            iteration = counters["steps"]
            step = f"step-{iteration}"
            context = dict(execution_id="main", step_id=step, loop_id=f"user-turn-{turn_index + 1}", iteration=iteration)
            step_start = event("step.start", caused_by=[previous], **context)
            llm_id = f"llm-{iteration}"
            llm_start = event("action.start", action_id=llm_id, operation="llm.chat", input={"turn": turn_index + 1}, caused_by=[step_start], **context)
            try:
                result = post("/chat/completions", dict(model=model, messages=messages, tools=tools,
                              thinking={"type": "disabled"}, max_tokens=4096))
                choice = result["choices"][0]
                if choice.get("finish_reason") == "length":
                    raise ValueError("model_output_truncated")
                message = choice["message"]
                counters["tokens"] += int(result.get("usage", {}).get("total_tokens", 0))
                llm_end = event("action.end", action_id=llm_id, status="completed", output={"content": message.get("content") or ""}, caused_by=[llm_start], **context)
            except Exception as exc:
                code = f"http_{exc.code}" if isinstance(exc, urllib.error.HTTPError) else "model_request_failed"
                llm_end = event("action.end", action_id=llm_id, status="failed", output={"error": code}, error={"code": code, "retryability": "unknown"}, caused_by=[llm_start], **context)
                previous = event("step.end", status="failed", caused_by=[llm_end], **context)
                output, status = "模型调用未完成，不能声称任务成功。", "failed"
                break
            calls = message.get("tool_calls") or []
            assistant = {"role": "assistant", "content": message.get("content") or ""}
            if calls:
                assistant["tool_calls"] = calls
            messages.append(assistant)
            if not calls:
                output = assistant["content"]
                previous = event("step.end", status="completed", output=output, caused_by=[llm_end], **context)
                event("loop.decision", decision="stop", reason="当前用户轮已得到答复。", caused_by=[previous], **context)
                break
            if counters["tool_calls"] + len(calls) > int(task["budget"]["max_tool_calls"]):
                previous = event("step.end", status="failed", caused_by=[llm_end], **context)
                output, status = "达到工具调用预算。", "failed"
                break
            counters["tool_calls"] += len(calls)
            futures, completed = {}, {}
            with ThreadPoolExecutor(max_workers=4) as pool:
                for call in calls:
                    name = call["function"]["name"]
                    try:
                        args = json.loads(call["function"]["arguments"])
                    except (ValueError, TypeError):
                        args = {}
                    call_start = event("action.start", action_id=call["id"], operation=name, input=args, caused_by=[llm_end], **context)
                    futures[pool.submit(tool, name, args)] = (call, call_start)
                ends = []
                for future in as_completed(futures):
                    call, call_start = futures[future]
                    try:
                        answer = future.result()
                    except Exception as exc:
                        answer = {"error": f"http_{exc.code}" if isinstance(exc, urllib.error.HTTPError) else "tool_failed", "retryable": False}
                    fields = dict(action_id=call["id"], status="failed" if answer.get("error") else "completed", output=answer, caused_by=[call_start], **context)
                    if answer.get("error"):
                        fields["error"] = {"code": answer["error"], "retryability": "retryable" if answer.get("retryable") else "non_retryable"}
                    ends.append(event("action.end", **fields))
                    completed[call["id"]] = answer
                for call in calls:
                    messages.append({"role": "tool", "tool_call_id": call["id"], "content": json.dumps(completed[call["id"]], ensure_ascii=False)})
            previous = event("step.end", status="completed", caused_by=ends, **context)
            event("loop.decision", decision="continue", reason="消费工具结果后继续处理。", caused_by=[previous], **context)
        if status != "completed":
            break
    event("run.end", caused_by=[e["event_id"] for e in events], output=output,
          outcome={"execution_status": status, "business_status": "unknown"})
    trace = dict(schema_version=1, trace_kind="trajectory.v1", identity={"run_id": run_id, "agent_id": "agentplan-example",
                 "user_id": "evaluation-user", "conversation_id": run_id}, events=events)
    print(json.dumps(dict(schema_version=1, trial_id=run_id, execution_status=status, output=output, state=state,
                          metrics=counters, trace=trace, artifacts=[{"path": name, "media_type": media} for name, media in generated.items()]), ensure_ascii=False))


if __name__ == "__main__":
    try:
        main()
    except Exception:
        # Provider errors can include signed URLs and credentials. Do not dump
        # tracebacks, response bodies, or request headers to the evaluator.
        print("example_host_failed", file=sys.stderr)
        sys.exit(2)
