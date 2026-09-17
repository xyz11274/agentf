"""Stateful multi-device test world used when physical devices are unavailable."""
from __future__ import annotations

import copy
from datetime import datetime, timezone

from .live_information import result
from .state import next_id


DEVICE_TEMPLATES = [
    {
        "id": "virtual-phone-1", "name": "测试手机", "device_type": "phone",
        "model": "Android Test Phone", "os": "Android 15 (test)", "battery": 86,
        "capabilities": ["gui", "location", "notification", "message", "call", "camera", "media", "file"],
    },
    {
        "id": "virtual-tablet-1", "name": "测试平板", "device_type": "tablet",
        "model": "Android Test Tablet", "os": "Android 15 (test)", "battery": 71,
        "capabilities": ["gui", "notification", "message", "camera", "media", "document", "file"],
    },
    {
        "id": "virtual-desktop-1", "name": "测试电脑", "device_type": "desktop",
        "model": "Desktop Test Station", "os": "Linux (test)", "battery": None,
        "capabilities": ["gui", "notification", "message", "media", "document", "file"],
    },
    {
        "id": "virtual-tv-1", "name": "测试电视", "device_type": "tv",
        "model": "Living Room Test TV", "os": "TV OS (test)", "battery": None,
        "capabilities": ["notification", "media", "cast"],
    },
    {
        "id": "virtual-speaker-1", "name": "测试音箱", "device_type": "speaker",
        "model": "Test Smart Speaker", "os": "Speaker OS (test)", "battery": None,
        "capabilities": ["notification", "audio", "voice", "media", "smart_home"],
    },
    {
        "id": "virtual-watch-1", "name": "测试手表", "device_type": "watch",
        "model": "Test Watch", "os": "Wear OS (test)", "battery": 64,
        "capabilities": ["notification", "location", "health", "message"],
    },
]


def _now() -> str:
    return datetime.now(timezone.utc).isoformat()


def ensure(state: dict) -> dict:
    world = state.setdefault("virtual_device_world", {})
    if "devices" not in world:
        devices = []
        for template in DEVICE_TEMPLATES:
            item = copy.deepcopy(template)
            item.update({
                "status": "online", "transport": "business-test-bus",
                "device_kind": "virtual", "simulated": True, "physical_effect": False,
                "current_activity": None, "inbox": [], "synced_data": {},
                "last_seen": _now(),
            })
            devices.append(item)
        world.update({"devices": devices, "selected": None, "tasks": [], "events": [], "shared_data": {}})
    return world


def public_device(device: dict) -> dict:
    return {key: copy.deepcopy(value) for key, value in device.items() if key not in ("inbox", "synced_data")}


def _answer(text: str, data: dict | None = None, status: str = "succeeded", **extra) -> dict:
    return {
        **result(text, data, status=status, source="business-test-device-bus"),
        "simulated": True,
        "environment": "business_test_system",
        "physical_effect": False,
        **extra,
    }


def _find(world: dict, device_id: str | None) -> dict | None:
    return next((device for device in world["devices"] if device["id"] == device_id), None)


def _online(device: dict | None) -> dict | None:
    if not device:
        return _answer("测试设备不存在。", status="not_found", execution="not_executed")
    if device["status"] != "online":
        return _answer("测试设备当前离线，任务没有送达。", {"device": public_device(device)},
                       status="failed", execution="not_executed", retryable=True)
    return None


def execute(state: dict, args: dict) -> dict:
    world = ensure(state)
    action = args["action"]
    if action not in ("list", "status", "history") and not args.get("request_id"):
        return _answer("虚拟设备写操作需要 request_id，以便安全重试。", status="needs_input",
                       missing_fields=["request_id"], execution="not_executed")

    if action in ("list", "status"):
        devices = [public_device(device) for device in world["devices"]]
        return _answer("已读取有状态虚拟设备群。",
                       {"devices": devices, "selected": world.get("selected"),
                        "online_count": sum(d["status"] == "online" for d in devices),
                        "task_count": len(world["tasks"])}, execution="observed")

    if action == "history":
        items = world["events"]
        if args.get("device_id"):
            items = [item for item in items if args["device_id"] in item.get("device_ids", [])]
        return _answer("已读取虚拟设备操作历史。", {"events": copy.deepcopy(items),
                       "tasks": copy.deepcopy(world["tasks"])}, execution="observed")

    if action == "select":
        device = _find(world, args.get("device_id"))
        if error := _online(device):
            return error
        world["selected"] = device["id"]
        event = {"id": next_id(state, "device-event"), "type": "select",
                 "device_ids": [device["id"]], "created_at": _now()}
        world["events"].append(event)
        return _answer("已选择虚拟测试设备。", {"device_id": device["id"], "device": public_device(device)},
                       execution="selected")

    if action == "set_online":
        device = _find(world, args.get("device_id"))
        if not device:
            return _answer("测试设备不存在。", status="not_found", execution="not_executed")
        online = args.get("online")
        if online is None:
            return _answer("请指定测试设备在线状态。", status="needs_input", missing_fields=["online"])
        device["status"] = "online" if online else "offline"
        device["last_seen"] = _now()
        event = {"id": next_id(state, "device-event"), "type": "set_online",
                 "device_ids": [device["id"]], "online": online, "created_at": _now()}
        world["events"].append(event)
        return _answer("已更新虚拟设备在线状态。", {"device": public_device(device)},
                       execution="simulated_and_state_observed")

    if action == "broadcast":
        content = args.get("content")
        if not content:
            return _answer("广播需要内容。", status="needs_input", missing_fields=["content"])
        wanted = args.get("target_device_ids") or [d["id"] for d in world["devices"]]
        missing = [device_id for device_id in wanted if not _find(world, device_id)]
        if missing:
            return _answer("部分测试设备不存在，未执行广播。", {"missing_device_ids": missing},
                           status="not_found", execution="not_executed")
        receipts = []
        message_id = next_id(state, "device-message")
        for device_id in wanted:
            device = _find(world, device_id)
            delivered = device["status"] == "online" and "notification" in device["capabilities"]
            if delivered:
                device["inbox"].append({"id": message_id, "content": content, "received_at": _now()})
            receipts.append({"device_id": device_id, "delivered": delivered,
                             "reason": None if delivered else "offline_or_notification_unsupported"})
        event = {"id": message_id, "type": "broadcast", "device_ids": wanted,
                 "content": content, "receipts": receipts, "created_at": _now()}
        world["events"].append(event)
        return _answer("广播已在虚拟设备总线执行并逐设备回读。",
                       {"message_id": message_id, "receipts": receipts},
                       execution="simulated_and_state_observed")

    if action == "sync":
        data_type = args.get("data_type")
        payload = args.get("payload")
        missing = [key for key, value in (("data_type", data_type), ("payload", payload)) if value is None]
        if missing:
            return _answer("同步需要 data_type 和 payload。", status="needs_input", missing_fields=missing)
        targets = args.get("target_device_ids") or [d["id"] for d in world["devices"]]
        devices = [_find(world, device_id) for device_id in targets]
        if any(device is None for device in devices):
            return _answer("同步目标包含不存在的测试设备。", status="not_found", execution="not_executed")
        for device in devices:
            if error := _online(device):
                return error
        version = len(world["shared_data"].get(data_type, [])) + 1
        record = {"version": version, "payload": copy.deepcopy(payload), "updated_at": _now()}
        world["shared_data"].setdefault(data_type, []).append(record)
        for device in devices:
            device["synced_data"][data_type] = copy.deepcopy(record)
        event = {"id": next_id(state, "device-event"), "type": "sync", "device_ids": targets,
                 "data_type": data_type, "version": version, "created_at": _now()}
        world["events"].append(event)
        return _answer("数据已同步到虚拟设备并回读版本。",
                       {"data_type": data_type, "version": version,
                        "receipts": [{"device_id": d["id"], "version": version} for d in devices]},
                       execution="simulated_and_state_observed")

    if action == "handoff":
        source_id = args.get("source_device_id") or world.get("selected")
        target_id = args.get("target_device_id") or args.get("device_id")
        source, target = _find(world, source_id), _find(world, target_id)
        missing = [key for key, value in (("source_device_id", source_id), ("target_device_id", target_id)) if not value]
        if missing:
            return _answer("接力需要来源和目标测试设备。", status="needs_input", missing_fields=missing)
        if error := _online(source):
            return error
        if error := _online(target):
            return error
        payload = copy.deepcopy(args.get("payload"))
        if payload is None:
            payload = copy.deepcopy(source.get("current_activity"))
        if payload is None:
            return _answer("来源设备没有可接力任务，请提供 payload。", status="needs_input", missing_fields=["payload"])
        task = {"id": next_id(state, "device-task"), "kind": args.get("task_kind", "generic"),
                "source_device_id": source_id, "target_device_id": target_id, "payload": payload,
                "status": "continued", "created_at": _now()}
        source["current_activity"] = {**copy.deepcopy(payload), "handoff_status": "sent", "task_id": task["id"]}
        target["current_activity"] = {**copy.deepcopy(payload), "handoff_status": "received", "task_id": task["id"]}
        world["tasks"].append(task)
        world["events"].append({"id": task["id"], "type": "handoff", "device_ids": [source_id, target_id],
                                "created_at": task["created_at"]})
        return _answer("任务已在虚拟设备之间接力，并回读目标设备状态。",
                       {"task": copy.deepcopy(task), "target_device": public_device(target)},
                       execution="simulated_and_state_observed")

    return _answer("该多设备测试动作暂不支持。", status="unsupported", execution="not_executed")
