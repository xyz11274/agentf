"""Stateful stand-ins for unavailable business agents. Never reads evaluation labels."""
from __future__ import annotations

import copy
import os
import re

from .contracts import SERVICES, contract, validate
from .state import Store, fixtures, next_id, scope_key


def response(text: str, data: dict | None = None, code: str = "", status: str = "succeeded", **extra) -> dict:
    result = {"status": status, "simulated": True, "source": "business-simulator/v1",
              "UserOutputText": text, "UserOutputObject": data or {}, **extra}
    if code:
        result["operation_type"] = code
    return result


def need(*fields: str) -> dict:
    return response("请补充：" + "、".join(fields), status="needs_input", missing_fields=list(fields))


def require(args: dict, *fields: str) -> dict | None:
    missing = [name for name in fields if not args.get(name)]
    return need(*missing) if missing else None


def health(state: dict, a: dict) -> dict:
    action = a["action"]
    if action == "triage":
        if missing := require(a, "query"):
            return missing
        emergency = any(x in a["query"] for x in ("昏迷", "呼吸困难", "剧烈胸痛", "意识不清", "大量出血"))
        return response("出现严重或迅速加重的症状应立即联系急救。" if emergency else "请描述症状位置、持续时间、严重程度和既往病史；持续或加重请就医。",
                        {"symptoms": a["query"], "urgency": "emergency" if emergency else "needs_assessment",
                         "follow_up": ["症状位置", "持续时间", "严重程度", "既往病史/用药"],
                         "records": list(state["records"].values()), "diagnosis": None}, "100155")
    if action == "record_query":
        return response("正在查询模拟健康档案。", {"records": list(state["records"].values())}, "100153")
    if action == "record_save":
        if missing := require(a, "content"):
            return missing
        record_id = a.get("id") or next_id(state, "record")
        if a.get("id") and record_id not in state["records"]:
            return response("未找到待更新档案。", status="not_found")
        record = {"id": record_id, "content": a["content"], "patient": a.get("patient", "当前模拟用户"), "origin": "user_provided"}
        state["records"][record_id] = record
        return response("已保存模拟健康档案。", {"healthRecord": record}, "100152")
    if action == "reminder_query":
        return response("正在为你查询用药提醒，请稍等（模拟服务）。", {"reminders": list(state["reminders"].values())}, "100157")
    if action == "reminder_create":
        if missing := require(a, "medicine", "times"):
            return missing
        if any(not re.fullmatch(r"(?:[01]\d|2[0-3]):[0-5]\d", t) for t in a["times"]):
            raise ValueError("times must contain valid HH:MM values")
        record = {"id": next_id(state, "reminder"), "medicine": a["medicine"], "times": sorted(set(a["times"])),
                  "dosage": a.get("dosage", "未提供；不推导剂量"), "period": a.get("period", "未指定"), "active": True}
        state["reminders"][record["id"]] = record
        return response("已保存模拟用药提醒；此示例不发送到时通知。", {"medicationReminder": record}, "100151")
    if action == "appointment_query":
        items = [item for item in state["appointments"].values()
                 if all(not a.get(k) or a[k] in item.get(k, "") for k in ("hospital", "department", "date"))]
        return response("正在为你查询挂号信息，请稍等（模拟服务）。", {"appointments": items}, "100156")
    if action == "appointment_create":
        if missing := require(a, "hospital", "department", "date"):
            return missing
        item = {k: a.get(k, "") for k in ("hospital", "department", "date", "patient", "doctor", "appointment_type")}
        item.update(id=next_id(state, "appointment"), status="simulated_reserved")
        state["appointments"][item["id"]] = item
        return response("已生成模拟挂号记录；没有向医院预约。", {"appointment": item}, "100154")
    collection = "reminders" if action == "reminder_cancel" else "appointments"
    if missing := require(a, "id"):
        return missing
    item = state[collection].get(a["id"])
    if not item:
        return response("没有这条记录。", status="not_found")
    item["active"] = False
    item["status"] = "cancelled"
    return response("已取消模拟记录。", {collection: [item]})


def home(state: dict, a: dict) -> dict:
    action = a["action"]
    if action == "query":
        items = [d for d in state["devices"] if all(not a.get(k) or a[k] == d[k] for k in ("room", "device"))]
        return response("模拟设备当前状态。", {"devices": items}, status="succeeded" if items else "not_found")
    if action == "habits":
        history = state["home_history"]
        modes = []
        for name, verb in (("ARRIVE_HOME_MODE", "on"), ("LEAVE_HOME_MODE", "off")):
            commands = [h["commandText"] for h in history if any(c["action"] == verb for c in h["commands"])]
            modes.append({"mode": name, "habitTime": None, "commandList": [{"commandText": s} for s in commands[-5:]]})
        return response("已汇总本会话模拟操作；无足够时序数据时不推断习惯时间。", {"modeList": modes, "observations": len(history)}, "100053")
    if missing := require(a, "commands"):
        return missing
    descriptions = []
    for cmd in a["commands"]:
        room = cmd.get("room")
        matches = [d for d in state["devices"] if d["device"] == cmd["device"] and (not room or d["room"] == room)]
        if not matches:
            return response("模拟环境没有该设备。", {"requested": cmd}, status="not_found")
        if len(matches) > 1:
            return response("有多个同类设备，请指定房间。", {"candidates": matches}, status="needs_input", missing_fields=["room"])
        device = matches[0]
        if not device.get("online", True):
            return response("设备离线。", {"device": device}, status="failed")
        if cmd["action"] in ("on", "off"):
            device["power"] = cmd["action"] == "on"
            descriptions.append(f"{'打开' if device['power'] else '关闭'}{device['room']}{device['device']}")
        else:
            if "value" not in cmd or not cmd.get("property"):
                return need("commands[].property", "commands[].value")
            prop, value = cmd["property"], cmd["value"]
            if prop not in device["capabilities"]:
                return response("该设备不支持此属性。", {"capabilities": device["capabilities"]}, status="unsupported")
            if prop == "mode":
                if not isinstance(value, str) or value not in device["capabilities"][prop]:
                    raise ValueError("mode is not supported by this device")
            else:
                lo, hi = device["capabilities"][prop]
                if not isinstance(value, (int, float)) or isinstance(value, bool) or not lo <= value <= hi:
                    raise ValueError(f"{prop} must be between {lo} and {hi}")
            device[prop] = value
            descriptions.append(f"将{device['room']}{device['device']}的{prop}设为{value}")
    # The original contract uses ONE commandText for a multi-device instruction.
    combined = "；".join(descriptions)
    state["home_history"].append({"commandText": combined, "commands": copy.deepcopy(a["commands"])})
    return response("已更新模拟家居状态：" + combined, {"commandList": [{"commandText": combined}], "devices": state["devices"]}, "100051")


def movie(state: dict, a: dict) -> dict:
    catalog = fixtures()["movies"]
    action = a["action"]
    if action == "query":
        return response("模拟播放器状态。", {"player": state["player"], "last_list": state["last_movies"]})
    if action in ("pause", "resume"):
        if not state["player"]:
            return response("还没有选片。", status="not_found")
        state["player"]["status"] = "paused" if action == "pause" else "playing"
        return response("已更新模拟播放器。", {"player": state["player"]}, "100101")
    if action == "recommend":
        genre = a.get("genre") or state["preferences"].get("电影偏好", "")
        words = [x for x in [genre, a.get("actor"), a.get("query")] if x]
        excluded = set(a.get("exclude", []))
        gentle = a.get("family_friendly", False) or any(w in a.get("mood", "") for w in ("治愈", "低落", "心情不好", "温暖"))
        found = [m for m in catalog if m["title"] not in excluded and (not gentle or m["family_friendly"])
                 and all(w in json_text(m) for w in words)]
        found = sorted(found, key=lambda m: (-m["rating"], m["id"]))[:a.get("count", 3)]
        if not found:
            return response("模拟片库没有符合这些条件的影片，可以放宽筛选。", {"aiMovieData": {"list": []}}, "200002", "not_found")
        state["last_movies"] = found
        text = "模拟片库推荐：\n" + "\n".join(f"《{m['title']}》——核心看点：{m['highlight']}" for m in found)
        return response(text, {"aiMovieData": {"list": found}}, "200002")
    if "index" in a:
        index = a["index"] - 1
        matches = state["last_movies"][index:index + 1]
    else:
        title = a.get("title") or a.get("query")
        if not title:
            return need("title 或 index")
        matches = [m for m in catalog if title in (m["title"], m["id"], *m.get("aliases", []))]
    if not matches:
        return response("未找到影片或上一轮列表中的序号。", status="not_found")
    item = matches[0]
    if action == "detail":
        return response(f"模拟片库：《{item['title']}》；核心看点：{item['highlight']}", {"movie": item})
    state["player"] = {"movie_id": item["id"], "title": item["title"], "status": "playing"}
    return response(f"已在模拟播放器选择《{item['title']}》。", {"player": state["player"]}, "100101")


def json_text(value) -> str:
    import json
    return json.dumps(value, ensure_ascii=False)


def gui(state: dict, a: dict) -> dict:
    if a["action"] == "query":
        return response("模拟界面状态。", state["gui"])
    if missing := require(a, "app", "operation"):
        return missing
    app, op = a["app"], a["operation"]
    fields = {"search": ("query",), "play": ("title",), "navigate": ("destination",),
              "setting": ("key", "value"), "order": ("query",), "recharge": ("amount",), "send": ("content",)}
    if missing := require(a, *fields.get(op, ())):
        return missing
    steps = [f"打开{app}"]
    if op in ("search", "order"):
        steps.append(f"搜索{a['query']}")
    elif op == "play":
        steps.extend([f"搜索《{a['title']}》", f"选择第{a['episode']}集" if a.get("episode") else "选择匹配的完整内容", "播放"])
    elif op == "navigate":
        steps.extend([f"设置起点：{a.get('origin', '当前位置（模拟位置）')}", f"搜索目的地：{a['destination']}", "展示并选择路线"])
    elif op == "setting":
        steps.extend([f"进入设置：{a['key']}", f"将其设为{a['value']}"])
    elif op == "recharge":
        steps.extend(["进入充值页", f"输入金额{a['amount']}元"])
    elif op == "send":
        steps.extend(["打开目标会话或收件人页面", "填写用户指定内容"])
    sensitive = op in ("order", "recharge", "send")
    if sensitive:
        steps.append("展示详情，等待用户确认；本模拟不提交到外部服务")
    if a["action"] == "execute" and sensitive and not a.get("confirmed"):
        return response("已生成操作计划，请确认模拟提交。", {"steps": steps}, "300001", "needs_confirmation")
    plan = {"id": next_id(state, "gui"), "app": app, "operation": op, "steps": steps,
            "execution": "simulated" if a["action"] == "execute" else "planned"}
    state["gui"]["plans"].append(plan)
    if a["action"] == "execute":
        state["gui"].update(app=app, last_action=op, parameters={k: v for k, v in a.items() if k not in ("action", "request_id")})
    return response("模拟操作规划：" + " → ".join(steps), {"taskPlan": plan}, "300001")


def memory(state: dict, a: dict) -> dict:
    if a["action"] == "read":
        key = a.get("key")
        values = state["preferences"] if not key else {key: state["preferences"][key]} if key in state["preferences"] else {}
        return response("已读取当前会话显式保存的偏好。", {"preferences": values})
    if missing := require(a, "key", *(["value"] if a["action"] == "write" else [])):
        return missing
    if a["action"] == "write":
        state["preferences"][a["key"]] = a["value"]
    else:
        state["preferences"].pop(a["key"], None)
    return response("偏好已更新。", {"preferences": state["preferences"]})


def lookup(query: str, items: list) -> list:
    # Token and Chinese-bigram matching; no case ids, expected outputs or eval files.
    chars = re.sub(r"[^\w\u4e00-\u9fff]", "", query.lower())
    grams = {chars[i:i + 2] for i in range(max(0, len(chars) - 1))}
    scored = []
    for item in items:
        haystack = json_text(item).lower()
        score = sum(g in haystack for g in grams) + 5 * sum(k in query for k in item.get("keywords", []))
        if score:
            scored.append((score, item))
    return [item for _, item in sorted(scored, key=lambda x: -x[0])[:5]]


def information(state: dict, a: dict, skill: str) -> dict:
    if missing := require(a, "query"):
        return missing
    mode = os.environ.get("BUSINESS_SIM_INFORMATION_MODE", "fixture")
    if mode not in ("fixture", "live"):
        raise ValueError("BUSINESS_SIM_INFORMATION_MODE must be fixture or live")
    if skill != "knowledge-base" and mode == "live":
        if not os.environ.get("WEBSEARCH_BASE_URL") or not os.environ.get("WEBSEARCH_API_KEY"):
            return response("live 模式需要配置 WEBSEARCH_BASE_URL 与 WEBSEARCH_API_KEY。", status="failed")
        from ._live_search import execute
        live = execute({"query": a["query"]})
        if live.get("status") != "succeeded":
            return response("联网查询失败；不会降级伪装成实时结果。", {"error": live.get("error", "unknown")}, status="failed")
        return {**response(live["results"], {"results": live["results"], "sources": live.get("sources", []), "live_data_available": True}),
                "simulated": False, "source": "configured-web-search-provider"}
    data = fixtures()
    if skill == "knowledge-base":
        items = lookup(a["query"], data["knowledge"])
    else:
        topic = a.get("topic", "")
        items = [x for x in data["information"] if not topic or topic == x["topic"]]
        items = lookup(a["query"], items)
        location = a.get("location")
        if location:
            items = [i for i in items if not i.get("location") or i["location"] == location]
    if not items:
        return response("离线资料未命中；不能据此编造实时信息。一般知识可说明依据后回答。",
                        {"results": [], "live_data_available": False}, status="not_found")
    return response("以下为模拟资料快照，并非实时查询。", {"results": items, "live_data_available": False,
                     "requested_date": a.get("date", ""), "snapshot_date": data["snapshot_date"]})


def documents(state: dict, a: dict, skill: str) -> dict:
    bucket = state["documents"].setdefault(skill, {})
    key = a.get("key") or a.get("id")
    if a["action"] in ("get_document", "query"):
        items = [bucket[key]] if key in bucket else list(bucket.values()) if not key else []
        return response("已读取保存的草稿/计划。", {"documents": items}, status="succeeded" if items else "not_found")
    if a["action"] == "delete":
        if not key:
            return need("key")
        if key not in bucket:
            return response("没有这份计划。", status="not_found")
        del bucket[key]
        return response("已删除保存的计划。")
    if missing := require(a, "content"):
        return missing
    key = key or next_id(state, "document")
    previous = bucket.get(key, {})
    versions = previous.get("versions", []) + [a["content"]]
    item = {"key": key, "title": a.get("title") or previous.get("title", key),
            "content": a["content"], "version": len(versions), "versions": versions,
            "execution": "draft_only"}
    bucket[key] = item
    return response("已保存草稿/计划；未对外发送或执行。", {"document": item})


def auxiliary(state: dict, a: dict, skill: str) -> dict:
    if skill == "service-manager" or (skill == "third-party" and a["action"] == "status"):
        rows = [{"skill": s, "legacy_agent": meta[0], "available": state["services"].get(s, True), "transport": "local-simulator"}
                for s, meta in SERVICES.items()]
        return response("本包可用的模拟服务。", {"services": rows})
    if skill == "multi-device":
        devices = fixtures()["terminals"]
        if a["action"] == "select":
            if missing := require(a, "device_id"):
                return missing
            if not any(d["id"] == a["device_id"] for d in devices):
                return response("模拟终端不存在。", status="not_found")
            state["preferences"]["target_device"] = a["device_id"]
        return response("模拟终端列表。", {"devices": devices, "selected": state["preferences"].get("target_device")})
    if skill == "multimodal":
        if a["action"] == "query":
            return documents(state, a, skill)
        if not a.get("content"):
            return response("请提供可读取的内容或媒体描述；不能假装看到未提供的图片。", status="needs_input")
        result = documents(state, a, skill)
        result["UserOutputObject"]["media_generated"] = False
        return result
    if skill in ("third-party", "travel-planner", "foodie", "shopping") and a["action"] == "query":
        return information(state, a, skill)
    return documents(state, a, skill)


def dispatch(skill: str, args: dict, *, store: Store | None = None, scope: str | None = None) -> dict:
    """Framework entry point; identity is exclusively from trusted worker env."""
    if store is None and os.environ.get("BUSINESS_BACKEND_URL"):
        from .backend_client import call
        return call(skill, args)
    try:
        if skill not in SERVICES:
            raise ValueError("unknown business skill")
        validate(args, contract(skill)[2])
        resolved_scope = scope or scope_key()
        local_store = store or Store()
        execution_mode = os.environ.get("BUSINESS_EXECUTION_MODE", "real")
        if execution_mode == "real":
            from .real_services import execute
            try:
                return execute(skill, args, local_store, resolved_scope)
            except Exception as exc:
                # External failures do not become fixture answers or leak request credentials.
                from .live_information import result
                failure = result("执行失败；未使用模拟结果替代。", status="failed", error=type(exc).__name__,
                                 http_status=getattr(exc,"code",None), execution_mode="real")
                return local_store.run(resolved_scope, skill, args, lambda _state: failure)
        if execution_mode != "fixture":
            raise ValueError("BUSINESS_EXECUTION_MODE must be real or fixture")
        prepared_information = None
        # A slow network provider must never hold the SQLite write transaction.
        if (skill in ("web-search", "third-party", "travel-planner", "foodie", "shopping")
                and args["action"] == "query"
                and os.environ.get("BUSINESS_SIM_INFORMATION_MODE") == "live"
                and local_store.read(resolved_scope)["services"].get(skill, True)):
            prepared_information = information({}, args, skill)

        def run(state):
            if state["services"].get(skill, True) is False:
                return response("模拟业务服务暂不可用。", status="failed", retryable=True)
            handlers = {"health-advice": health, "smart-home": home, "movie-query": movie,
                        "device-gui": gui, "user-memory": memory}
            if prepared_information is not None:
                result = prepared_information
            elif skill in handlers:
                result = handlers[skill](state, args)
            elif skill == "knowledge-base" or (skill == "web-search" and args["action"] == "query"):
                result = information(state, args, skill)
            elif skill == "web-search":
                result = documents(state, args, skill)
            else:
                result = auxiliary(state, args, skill)
            return {**result, "legacy_agent": SERVICES[skill][0], "skill": skill}

        return local_store.run(resolved_scope, skill, args, run)
    except (ValueError, TypeError) as exc:
        if os.environ.get("BUSINESS_EXECUTION_MODE","real") == "real":
            from .live_information import result
            return result(str(exc), status="failed", error="invalid_arguments_or_identity")
        return response(str(exc), status="failed", error="invalid_arguments_or_identity")
