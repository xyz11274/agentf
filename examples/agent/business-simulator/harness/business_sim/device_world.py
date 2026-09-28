"""An independently running device world, without benchmark inputs or answer fixtures.

Commands change actuators; the clock changes telemetry. This is an executable
test home, not a connection to the user's physical home.
"""
from __future__ import annotations

from datetime import datetime
import copy
import json
import math
import statistics
import threading
import time
from zoneinfo import ZoneInfo

from .live_information import result


def output(text, data=None, status="succeeded", **extra):
    return result(text, data, status=status, source="business-test-home", simulated=True,
                  execution="executed" if status == "succeeded" else "not_executed",
                  environment="business_test_system", physical_effect=False, **extra)


def initial_world(now=None):
    now = time.time() if now is None else now
    devices = []
    specifications = [
        ("living-light", "客厅", "light", "客厅灯", [], 25),
        ("bedroom-light", "卧室", "light", "卧室顶灯", ["顶灯"], 20),
        ("bedside-light", "卧室", "light", "床头灯", ["床头灯", "床头台灯"], 8),
        ("study-lamp", "书房", "light", "台灯", ["台灯", "书桌灯"], 10),
        ("living-ac", "客厅", "ac", "客厅空调", [], 1200),
        ("bedroom-ac", "卧室", "ac", "卧室空调", [], 1000),
        ("living-curtain", "客厅", "curtain", "客厅窗帘", [], 40),
        ("bedroom-curtain", "卧室", "curtain", "卧室窗帘", [], 40),
        ("living-fan", "客厅", "fan", "风扇", [], 45),
        ("living-tv", "客厅", "tv", "电视", [], 100),
        ("living-humidifier", "客厅", "humidifier", "加湿器", [], 25),
        ("living-purifier", "客厅", "purifier", "空气净化器", [], 35),
        ("living-robot", "客厅", "robot", "扫地机器人", [], 45),
        ("living-speaker", "客厅", "speaker", "音箱", [], 15),
        ("kitchen-plug", "厨房", "plug", "插座", [], 5),
        ("bathroom-heater", "卫生间", "heater", "热水器", [], 1500),
    ]
    for device_id, room, kind, label, aliases, watts in specifications:
        props = {
            "light": {"brightness": 100, "color": "自然光"},
            "ac": {"temperature": 26, "mode": "自动", "fan_speed": "自动"},
            "curtain": {"position": 100}, "fan": {"fan_speed": "中档"},
            "tv": {"volume": 30}, "speaker": {"volume": 30},
            "heater": {"temperature": 45},
        }.get(kind, {})
        # These are declared startup conditions of this test home, not user history.
        power = device_id in ("study-lamp", "living-fan", "bedroom-light", "bedside-light", "living-tv")
        devices.append({"id": device_id, "room": room, "kind": kind, "name": label,
                        "aliases": aliases, "online": True, "power": power,
                        "properties": props, "observed": copy.deepcopy(props),
                        "watts": watts, "energy_kwh": 0.0, "moving": False})
    return {"devices": devices, "world": {"last_tick": now, "current_room": "客厅",
            "timezone": "Asia/Shanghai", "model": "test-home-v1",
            "outdoor": {"temperature": 30.0, "humidity": 72.0},
            "rooms": {room: {"temperature": 29.0, "humidity": 68.0}
                      for room in ("客厅", "卧室", "书房", "厨房", "卫生间")}}}


def ensure(state, now=None):
    if "world" not in state:
        state.update(initial_world(now))
    state.setdefault("home_history", [])


def advance(state, now=None):
    if state.get('evaluation_world'):
        from .evaluation_world import clock
        # 业务日期保持冻结，设备动力学按真实经过的秒数推进。
        # monotonic 在同一主机的 Python 进程间同源；锚点随 world 持久化，
        # 不能放在进程全局，否则每次工具子进程启动都会丢失经过时间。
        ensure(state, clock(state).timestamp())
        world = state['world']
        observed = time.monotonic()
        elapsed = max(0.0, observed - world.get('last_monotonic', observed))
        world['last_monotonic'] = observed
        if now is None:
            # 首次观测或单调时钟回退只重建锚点，不凭空推进设备。
            now = world['last_tick'] + elapsed
    now = time.time() if now is None else now
    ensure(state, now)
    world = state["world"]
    dt = max(0.0, now - world["last_tick"])
    if not dt:
        return
    previous_tick = world["last_tick"]
    from .home_statistics import energy, concurrent
    concurrent(state,previous_tick,now)
    world["last_tick"] = max(now, world["last_tick"])
    for device in state["devices"]:
        if not device["online"]:
            continue
        powered_seconds = dt if device["power"] else 0
        if device["kind"] == "curtain":
            desired = device["properties"]["position"]
            old = device["observed"]["position"]
            distance = desired - old
            movement = min(abs(distance), dt * 5)  # 20 seconds for full travel
            device["observed"]["position"] = round(old + math.copysign(movement, distance), 3)
            device["moving"] = movement < abs(distance)
            powered_seconds = movement / 5
        else:
            device["observed"] = copy.deepcopy(device["properties"])
        device["energy_kwh"] += device["watts"] * powered_seconds / 3600000
        energy(state,device,previous_tick,powered_seconds)
    for room, sensors in world["rooms"].items():
        target_t, target_h = world["outdoor"]["temperature"], world["outdoor"]["humidity"]
        tau_t, tau_h = 7200.0, 7200.0
        for device in state["devices"]:
            if device["room"] != room or not device["online"] or not device["power"]:
                continue
            props = device["properties"]
            if device["kind"] == "ac":
                mode = props["mode"]
                if mode in ("制冷", "睡眠", "自动"):
                    target_t, tau_t = min(target_t, props["temperature"]), 900.0
                elif mode == "制热":
                    target_t, tau_t = max(target_t, props["temperature"]), 900.0
                if mode in ("制冷", "除湿", "睡眠"):
                    target_h, tau_h = 50.0, 1200.0
            elif device["kind"] == "humidifier":
                target_h, tau_h = 65.0, 1200.0
        sensors["temperature"] = target_t + (sensors["temperature"] - target_t) * math.exp(-dt / tau_t)
        sensors["humidity"] = target_h + (sensors["humidity"] - target_h) * math.exp(-dt / tau_h)


KINDS = {"灯": "light", "灯光": "light", "所有灯": "light", "空调": "ac", "窗帘": "curtain",
         "风扇": "fan", "电视": "tv", "电视机": "tv", "加湿器": "humidifier",
         "空气净化器": "purifier", "扫地机器人": "robot", "插座": "plug", "热水器": "heater", "音箱": "speaker"}


def select(state, args):
    candidates = state["devices"]
    if args.get("room"):
        room = {"主卧": "卧室", "客厅里": "客厅"}.get(args["room"], args["room"])
        candidates = [d for d in candidates if d["room"] == room]
    name = args.get("device_id") or args.get("device", "")
    if name:
        exact = [d for d in candidates if name == d["id"] or name == d["name"] or name in d["aliases"]]
        candidates = exact or [d for d in candidates if d["kind"] == KINDS.get(name)]
    if len(candidates) > 1 and not args.get("room") and not args.get("all"):
        local = [d for d in candidates if d["room"] == state["world"]["current_room"]]
        if local:
            candidates = local
    return candidates


def change(device, command):
    action = command["action"]
    if action in ("on", "off"):
        if device["kind"] == "curtain":
            device["properties"]["position"] = 100 if action == "on" else 0
            device["moving"] = device["observed"]["position"] != device["properties"]["position"]
        else:
            device["power"] = action == "on"
        return
    prop, value = command.get("property"), command.get("value")
    if prop not in device["properties"]:
        raise ValueError(f"{device['name']}不支持属性 {prop}")
    if prop in ("temperature", "brightness", "position", "volume"):
        low, high = (16, 30) if device["kind"] == "ac" else (35, 75) if device["kind"] == "heater" else (0, 100)
        if isinstance(value, bool) or not isinstance(value, (int, float)) or not math.isfinite(value) or not low <= value <= high:
            raise ValueError(f"{device['name']}的{prop}需要 {low}–{high} 范围的数字")
    elif prop == "mode":
        value = {"cool": "制冷", "heat": "制热", "dry": "除湿", "sleep": "睡眠", "auto": "自动"}.get(value, value)
        if value not in ("制冷", "制热", "除湿", "睡眠", "自动", "送风"):
            raise ValueError("不支持的空调模式")
    elif prop == "fan_speed":
        value = {"low": "低档", "medium": "中档", "high": "高档", "auto": "自动", "低": "低档", "中": "中档", "高": "高档", "中风": "中档", "中速": "中档"}.get(value, value)
        if value not in ("低档", "中档", "高档", "自动"):
            raise ValueError("风速应为低档、中档、高档或自动")
    elif prop == "color":
        value = {"warm": "暖光", "cool": "冷光", "white": "白光", "暖色": "暖光"}.get(value, value)
        if value not in ("暖光", "冷光", "白光", "自然光"):
            raise ValueError("灯光应为暖光、冷光、白光或自然光")
    device["properties"][prop] = value
    if device["kind"] == "curtain":
        device["moving"] = device["observed"]["position"] != value
    else:
        device["power"] = True
        device["observed"] = copy.deepcopy(device["properties"])


def habits(state, args):
    from .home_statistics import analyze
    selected = {d["id"] for d in select(state, {**args,"all":True})}
    if args.get('compare_device'):
        matches=select(state,{'device':args['compare_device'],'all':True})
        if len(matches)!=1:
            return output('请用唯一设备名称或id指定对比设备。',status='needs_input',missing_fields=['compare_device'])
        args={**args,'compare_device':matches[0]['id']}
    data=analyze(state,args,selected)
    return output('已按积累的实际操作和运行记录统计；历史覆盖范围见结果。',data,operation_type='100053')


def execute(state, args, now=None):
    # 显式 now 用于可重复重放；默认时钟只在 advance 中解析一次。
    advance(state, now)
    now = state['world']['last_tick'] if now is None else now
    action = args["action"]
    if action == "query":
        return output("已读取测试家庭的实时状态。", {"devices": select(state, {**args, "all": True}),
                      "world": state["world"], "connected": True, "environment": "business_test_system", "physical_effect": False})
    if action == "habits":
        return habits(state, args)
    commands = args.get("commands", [])
    if not commands:
        return output("请提供设备控制动作。", status="needs_input", missing_fields=["commands"])
    candidate = copy.deepcopy(state)
    changed = []
    try:
        for command in commands:
            devices = select(candidate, command)
            if not devices:
                return output("没有找到指定设备。", {"command": command}, status="not_found")
            if len(devices) > 1 and not command.get("all"):
                return output("匹配到多台设备，请指定设备或明确控制全部。", {"candidates": devices}, status="needs_input")
            for device in devices:
                if not device["online"]:
                    return output(f"{device['name']}离线，本批操作未执行。", status="failed", error="device_offline", retryable=True)
                before = copy.deepcopy(device)
                change(device, command)
                changed.append({"device_id": device["id"], "command": command, "before": before,
                                "after": copy.deepcopy(device), "timestamp": now, "text": args.get("command_text") or
                                f"{device['name']} {command['action']} {command.get('property','')} {command.get('value','')}"})
    except ValueError as exc:
        return output(str(exc), status="failed", error="invalid_device_command")
    candidate["home_history"].extend(changed)
    state.update(candidate)
    ids = {item["device_id"] for item in changed}
    return output("控制已在业务测试家庭执行。", {"commandList": [{"commandText": args.get("command_text") or
                  "；".join(str(c) for c in commands), "commands": commands}],
                  "devices": [d for d in state["devices"] if d["id"] in ids], "transitions": changed,
                  "settled": not any(d["moving"] for d in state["devices"] if d["id"] in ids)}, operation_type="100051")


def set_online(store, scope, device_id, online):
    """Operator fault injection, intentionally absent from LLM tool schemas."""
    if not isinstance(online, bool):
        raise ValueError("online must be boolean")
    def mutate(state):
        advance(state)
        devices = [d for d in state["devices"] if d["id"] == device_id]
        if not devices:
            return output("设备不存在。", status="not_found")
        devices[0]["online"] = online
        return output("已更新测试设备连接状态。", {"device": devices[0]})
    return store.run(scope, "device-operator", {"action": "set_online"}, mutate)


def tick(store, now=None):
    # 保留 None，由各世界选择物理时钟，不能把评测世界强制推进到主机日期。
    updated = 0
    with store.connect() as db:
        db.execute("BEGIN IMMEDIATE")
        for scope, payload in db.execute("SELECT scope,payload FROM state").fetchall():
            state = json.loads(payload)
            if "world" not in state:
                continue
            advance(state, now)
            db.execute("UPDATE state SET payload=? WHERE scope=?", (json.dumps(state, ensure_ascii=False), scope))
            updated += 1
    return updated


class WorldWorker:
    def __init__(self, store, interval=1.0):
        self.store, self.interval = store, interval
        self.stop = threading.Event()
        self.last_error = None

    def start(self):
        self.thread = threading.Thread(target=self.run, daemon=True)
        self.thread.start()
        return self

    def run(self):
        while not self.stop.wait(self.interval):
            try:
                tick(self.store)
                self.last_error = None
            except Exception as exc:
                self.last_error = type(exc).__name__ + ": " + str(exc)

    def close(self):
        self.stop.set()
        self.thread.join(timeout=5)
