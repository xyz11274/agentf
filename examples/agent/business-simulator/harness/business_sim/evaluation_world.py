"""可重复的业务测试世界。仅评测初始化启用，不读取题目或标准答案。"""
from copy import deepcopy
from datetime import datetime, timedelta
import hashlib
import json
from functools import lru_cache
from pathlib import Path
from zoneinfo import ZoneInfo

from .live_information import result


def clock(state, zone="Asia/Shanghai"):
    value = state.get("evaluation_world", {}).get("clock")
    return datetime.fromisoformat(value).astimezone(ZoneInfo(zone)) if value else datetime.now(ZoneInfo(zone))


def load_profile(path):
    profile = json.loads(Path(path).read_text(encoding="utf-8"))
    if profile.get("schema_version") != 1:
        raise ValueError("Unsupported evaluation profile version")
    timestamp = datetime.fromisoformat(profile["clock"])
    if timestamp.tzinfo is None:
        raise ValueError("Evaluation clock requires a timezone")
    days = profile.get("history_days", 0)
    if isinstance(days, bool) or not isinstance(days, int) or not 0 <= days <= 90:
        raise ValueError("history_days must be an integer in [0, 90]")
    from .state import fixtures
    # 固定完整数据内容及哈希，不在执行中重新读取可能已被替换的资料。
    profile.setdefault("catalog", fixtures())
    encoded = json.dumps(profile, ensure_ascii=False, sort_keys=True).encode()
    profile["sha256"] = hashlib.sha256(encoded).hexdigest()
    return profile


@lru_cache(maxsize=8)
def _home_snapshot(clock_text, history_days):
    """相同环境只生成一次历史；每个用户获得深拷贝，不共享可变状态。"""
    from .device_world import initial_world, execute, advance
    now = datetime.fromisoformat(clock_text)
    start = now - timedelta(days=history_days)
    state = initial_world(start.replace(hour=0, minute=0, second=0).timestamp())
    for day in range(history_days):
        date = start + timedelta(days=day)
        for hour, verb in ((8, "off"), (18, "on")):
            # 用实际状态机生成可追溯的历史及能耗，不直接填充 modeList 答案。
            timestamp = date.replace(hour=hour, minute=0, second=0).timestamp()
            execute(state, {"action": "control", "commands": [
                {"device_id": device, "action": verb}
                for device in ("living-light", "living-ac", "study-lamp")
            ]}, now=timestamp)
    advance(state, now.timestamp())
    # 历史与当前设备状态是不同维度：恢复声明的启动状态，保留累计记录。
    state["devices"] = initial_world(now.timestamp())["devices"]
    return state


def seed(state, profile):
    """准备相同初始世界，不根据问题、预期动作或题目 ID 选择答案。"""
    from .knowledge import execute as knowledge
    profile = deepcopy(profile)
    now = datetime.fromisoformat(profile['clock'])
    state.update(deepcopy(_home_snapshot(profile['clock'], profile.get('history_days', 0))))
    for item in profile["catalog"].get("knowledge", []):
        knowledge(state, {"action": "import_text", "key": item["id"],
                          "title": item["title"], "content": item["text"]})
        document = state['knowledge_documents'][item['id']]
        document.update(source_kind='simulation_product_manual', source_url=item.get('source', ''),
                        imported_at=now.isoformat())
    state["evaluation_world"] = profile


def output(state, text, data=None, status="succeeded", **extra):
    profile = state["evaluation_world"]
    return result(text, data, status=status, source="evaluation-snapshot", simulated=True,
                  environment="business_test_system", physical_effect=False,
                  snapshot_sha256=profile["sha256"], **extra)


def seed_trial_state(state):
    """消费通用评测器交付的初态文件；原生流程不依赖 Excel 或 Python 评分器。"""
    import os
    from .state import PACKAGE, next_id
    source = Path(os.environ['TRAJECTORY_EVOLUTION_TRIAL_INITIAL_STATE'])
    if source.is_symlink() or source.stat().st_size > 1048576:
        raise ValueError('Invalid trial initial state file')
    initial = json.loads(source.read_text(encoding='utf-8'))
    allowed = {'schema', 'profile_sha256', 'precondition', 'device_type', 'voiceprint_id'}
    if not isinstance(initial, dict) or set(initial) - allowed or initial.get('schema') != 'business-simulator.initial-state.v1':
        raise ValueError('Unsupported business initial state')
    profile = load_profile(PACKAGE/'data/evaluation-profile.json')
    if initial.get('profile_sha256') != profile['sha256']:
        raise ValueError('Business evaluation profile changed; rebuild the dataset')
    text = initial.get('precondition', '')
    if text not in ('', '无', '已注入声纹 B，用户喜好科幻', '已注入声纹 B，用户喜好动画', '已有健康档案及医嘱用药记录'):
        raise ValueError('Unsupported business precondition')
    seed(state, profile)
    if '用户喜好科幻' in text:
        state['preferences']['电影偏好'] = '科幻'
    elif '用户喜好动画' in text:
        state['preferences']['电影偏好'] = '动画'
    if text == '已有健康档案及医嘱用药记录':
        rid = next_id(state, 'record')
        state['records'][rid] = {'id': rid, 'origin': 'synthetic_precondition',
            'content': '模拟医嘱记录：维生素D，每天08:00；剂量未提供，不能自行补全。'}
    target = {'1': 'phone-1', '2': 'speaker-1', '3': 'tv-1'}.get(initial.get('device_type'))
    if target:
        state['preferences']['target_device'] = 'virtual-' + target


def weather(state, args):
    from .time_constraints import dates
    location = args.get("location", "").strip()
    if not location:
        return output(state, "请提供查询地点。", status="needs_input", missing_fields=["location"])
    today = clock(state).date()
    if args.get("start_date"):
        start = datetime.fromisoformat(args["start_date"]).date()
        end = datetime.fromisoformat(args.get("end_date", args["start_date"])).date()
    else:
        span = dates(args.get("date") or "今天", today)
        if not span:
            return output(state, "请提供明确日期范围。", status="needs_input")
        start, end = span
    if not 0 <= (end - start).days <= 31:
        return output(state, "仿真天气一次最多查询32天。", status="unsupported")
    settings = state["evaluation_world"].get("weather", {})
    row = settings.get(location, settings.get("default"))
    if not row:
        return output(state, "当前快照未提供该地区天气。", status="not_found")
    daily = [{"date": (start + timedelta(days=i)).isoformat(), **row}
             for i in range((end - start).days + 1)]
    return output(state, "业务仿真天气，非实时预报。", {
        "location": location, "start_date": start.isoformat(), "end_date": end.isoformat(),
        "daily": daily, "humidity_assessment": {"is_humid": row["humidity"] >= 80},
        "rain_expected": row["rain"], "source": "declared-test-weather"})


def movie(state, args):
    catalog = state["evaluation_world"]["catalog"]["movies"]
    action = args["action"]
    if action == "recommend":
        genre = args.get("genre") or state["preferences"].get("电影偏好", "")
        rows = [m for m in catalog if (not genre or genre in m["type"])
                and (not args.get("actor") or args["actor"] in m["actors"])
                and m["title"] not in args.get("exclude", [])
                and (not args.get("family_friendly") or m["family_friendly"])]
        if args.get("query"):
            from .services import lookup
            rows = lookup(args["query"], rows)
        rows = sorted(rows, key=lambda m: (-m["rating"], m["id"]))[:args.get("count", 3)]
        state["last_movies"] = deepcopy(rows)
        return output(state, "测试片库推荐；评分为样例数据。", {"aiMovieData": {"list": rows}},
                      status="succeeded" if rows else "not_found", operation_type="200002")
    if action == "query":
        return output(state, "测试播放器当前状态。", {"player": state["player"], "aiMovieData": {"list": state["last_movies"]}})
    if action in ("pause", "resume"):
        if not state["player"]:
            return output(state, "尚未选片。", status="not_found")
        state["player"]["status"] = "paused" if action == "pause" else "playing"
    elif action in ("play", "detail"):
        if args.get("index"):
            i = args["index"] - 1
            items = state["last_movies"]
            selected = items[i] if 0 <= i < len(items) else None
        else:
            title = args.get("title") or args.get("query")
            selected = next((m for m in catalog if title in [m["id"], m["title"], *m.get("aliases", [])]), None)
        if selected is None:
            return output(state, "测试片库未找到该影片或序号。", status="not_found")
        if action == "detail":
            return output(state, "测试影片资料。", {"movie": selected}, operation_type="200003")
        state["player"] = {"movie_id": selected["id"], "title": selected["title"], "status": "playing"}
    else:
        from .movies import execute
        return execute(state, args)
    return output(state, "已更新测试播放器状态。", {"player": state["player"]}, operation_type="100101")


def execute(state, skill, args):
    """返回 None 表示继续运行已有本地状态机；禁止隐式回落到实时网络。"""
    profile = state.get("evaluation_world")
    if not profile:
        return None
    action = args["action"]
    if skill == 'multi-device' and not action.startswith('browser_'):
        from .device_connectors import connected_devices
        return connected_devices(state, {**args, 'execution_target': 'virtual'}, physical=[])
    if (skill == 'service-manager' and action in ('list', 'status')) or (skill == 'third-party' and action == 'status'):
        return output(state, '当前固定业务测试环境。', {'profile': profile['name'],
                      'clock': profile['clock'], 'network': 'snapshot_only',
                      'external_response_count': len(profile.get('external_responses', []))})
    if skill == "movie-query" and not action.startswith("browser_"):
        return movie(state, args)
    if skill == "knowledge-base" and action in ("search", "search_local"):
        from .knowledge import execute as knowledge
        answer = knowledge(state, {**args, "action": "search_local"})
        return output(state, answer["UserOutputText"], answer["UserOutputObject"], status=answer["status"])
    if skill == "web-search" and (action == "weather" or (action == "query" and args.get("topic") == "weather")):
        return weather(state, args)
    if skill == "web-search" and action == "current_time":
        now = clock(state, args.get("timezone", "Asia/Shanghai"))
        return output(state, "仿真时钟。", {"datetime": now.isoformat(), "date": now.date().isoformat(), "hour": now.hour})
    external = (action.startswith(("browser_", "device_")) or args.get("execution_target") == "home_assistant"
                or action in ("email_send", "webhook_post", "generate_image", "generate_video", "import_url", "geocode", "route")
                or (action in ("query", "search") and skill in ("web-search", "third-party", "travel-planner", "foodie", "shopping")))
    if not external:
        return None
    arguments = {k: v for k, v in args.items() if k != "request_id"}
    for item in profile.get("external_responses", []):
        if item["skill"] == skill and item["arguments"] == arguments:
            answer = deepcopy(item["result"])
            return {**answer, "simulated": True, "source": "evaluation-snapshot", "snapshot_sha256": profile["sha256"]}
    if action == "query" and skill in ("web-search", "third-party", "travel-planner", "foodie", "shopping"):
        from .services import lookup
        # 固定快照与普通查询采用相同筛选条件。不能因为进入评测模式，
        # 就忽略调用者明确传入的主题或地点，并把跨域资料当作成功结果。
        topic, location = args.get("topic", ""), args.get("location", "")
        rows = [row for row in profile["catalog"].get("information", [])
                if (not topic or row.get("topic") == topic)
                and (not location or not row.get("location") or row["location"] == location)]
        rows = lookup(args.get("query", ""), rows)
        return output(state, "固定版本测试资料；不代表实时信息。", {"results": rows}, status="succeeded" if rows else "not_found")
    return output(state, "当前快照没有此操作的外部回执，不调用真实外部服务。", status="unsupported", error="snapshot_miss")
