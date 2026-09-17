#!/usr/bin/env python3
"""Independent outcome checks for the fixed development set; never imported by agents.

The original benchmark report/labels stay untouched. These checks verify state,
not general answer quality. Missing input/history and manual review stay separate.
"""
from collections import Counter
from datetime import datetime, timedelta
import argparse
import hashlib
import json
import re
from pathlib import Path

HOME = {
    "ZJ_JJ_SBKG_001": {"living-light": {"power": True}},
    "ZJ_JJ_SBKG_002": {"study-lamp": {"power": False}, "living-fan": {"power": False}},
    "ZJ_JJ_SBKG_003": {"bedroom-light": {"power": False}, "bedside-light": {"power": False}},
    "ZJ_JJ_SBTJ_001": {"living-ac": {"temperature": 25, "power": True}},
    "ZJ_JJ_SBTJ_002": {"living-ac": {"mode": "制冷", "fan_speed": "中档"}, "living-light": {"brightness": 50}},
    "ZJ_JJ_SBTJ_003": {"living-ac": {"temperature": 26, "mode": "制热"}, "living-curtain": {"position": 50}, "bedside-light": {"color": "暖光"}},
    "ZJ_JJ_KSLD_001": {"study-lamp": {"power": True}, "living-curtain": {"position": 0}, "living-fan": {"power": True}},
    "ZJ_JJ_KSLD_002": {"living-light": {"power": False}, "living-tv": {"power": False}, "bedside-light": {"power": True},
                       "living-ac": {"temperature": 27, "mode": "睡眠"}, "living-curtain": {"position": 50}},
    "SYS_DYT_01_071": {"living-light": {"power": True}},
    "SYS_DYT_01_077": {"living-light": {"brightness": 50}},
}
BOOKING = {
    "ZJ_JK_GH_001": {"department": "消化内科", "appointment_type": "专家号", "weekday": 0},
    "ZJ_JK_GH_002": {"hospital": "上海瑞金医院", "department": "皮肤科", "tomorrow": True, "morning": True},
    "ZJ_JK_GH_003": {"department": "眼科", "weekend": True, "not_before": "15:00"},
    "SYS_DYT_01_082": {"hospital": "市第一人民医院", "department": "消化内科", "tomorrow": True, "morning": True},
    "SYS_DYT_01_084": {"hospital": "北京301医院", "department": "骨科", "weekday": 0},
}


def check_case(case):
    cid = case["case_id"]
    evidence = case.get("business_execution", {})
    state, events = evidence.get("state", {}), evidence.get("events", [])
    checks = []
    notes = []
    group = "query"
    outcome = "passed"

    def check(name, ok):
        checks.append({"name": name, "passed": bool(ok)})

    def successful(skill, action):
        return [e["result"] for e in events if e["skill"] == skill and e["action"] == action and e["result"]["status"] == "succeeded"]

    def home(expected):
        actual = {d["id"]: d for d in state.get("devices", [])}
        for did, fields in expected.items():
            device = actual.get(did, {})
            for key, value in fields.items():
                check(f"{did}.{key}={value}", device.get(key, device.get("properties", {}).get(key)) == value)
        controls = successful("smart-home", "control")
        check("有成功控制事件", controls)
        check("整批一次提交", len(controls) == 1 and len(controls[0]["UserOutputObject"].get("commandList", [])) == 1)

    check("所有轮次均收到回复且无超时", all(t.get("response") and not t.get("error") and not t.get("timed_out") for t in case["turns"]))
    if cid in HOME:
        group = "home_control"
        expected = HOME[cid]
        if cid == "ZJ_JJ_KSLD_002":
            # The raw utterance specifies bedroom for the bedside light but leaves
            # AC/curtain room implicit. Either coherent room selection is allowed.
            actual = {d["id"]: d for d in state.get("devices", [])}
            selected_room = "bedroom" if actual.get("bedroom-ac", {}).get("properties", {}).get("mode") == "睡眠" else "living"
            expected = {(key.replace("living-", selected_room + "-") if key in ("living-ac", "living-curtain") else key): value for key, value in expected.items()}
        home(expected)
    elif cid in BOOKING:
        group = "booking"
        records = [r for r in state.get("appointments", {}).values() if r.get("status") == "booked"]
        check("确实有且只有一个有效预约", len(records) == 1)
        check("有成功预约事件", successful("health-advice", "appointment_create"))
        record = records[0] if records else {}
        when = datetime.fromisoformat(record["starts_at"]) if record.get("starts_at") else None
        created = datetime.fromisoformat(record["created_at"]) if record.get("created_at") else None
        for key, value in BOOKING[cid].items():
            if key == "weekday":
                # All weekday cases here explicitly request NEXT Monday.
                wanted = created.date() - timedelta(days=created.weekday()) + timedelta(days=7) if created else None
                ok = when and when.weekday() == value and when.date() == wanted
            elif key == "tomorrow":
                ok = when and created and when.date() == created.date() + timedelta(days=1)
            elif key == "morning":
                ok = when and when.hour < 12
            elif key == "weekend":
                ok = when and when.weekday() in (5, 6)
            elif key == "not_before":
                ok = when and when.strftime("%H:%M") >= value
            else:
                ok = record.get(key) == value
            check(f"预约满足 {key}={value}", ok)
        check("返回号源编号和医生", record.get("slot_id") and record.get("doctor"))
    elif cid.startswith("ZJ_JJ_XGCX"):
        group, outcome = "home_habits", "insufficient_history"
        observations = successful("smart-home", "habits")
        check("实际调用历史统计", observations)
        check("没有虚构历史或为了查询而控制设备", not state.get("home_history"))
        check("缺历史时不填造模式与时刻", observations and not observations[-1]["UserOutputObject"].get("modeList") and observations[-1]["UserOutputObject"].get("enough_history") is False)
        notes.append("原始前置条件没有历史，缺少习惯数据不能记为完整习惯任务成功。")
    elif cid.startswith("ZJ_JK_GHCX"):
        check("真实查询预约，空结果允许", successful("health-advice", "appointment_query"))
        check("没有为查询创建预约", not state.get("appointments"))
    elif cid in ("ZJ_JK_CYTX_001", "ZJ_JK_CYTX_004"):
        group = "reminder"
        records = list(state.get("reminders", {}).values())
        expected_times = {"08:00", "20:00"} if cid.endswith("001") else {"08:00"}
        check("创建且仅创建一条提醒", len(records) == 1)
        record = records[0] if records else {}
        check("药名与时刻符合输入/前置医嘱", record.get("medicine") == "维生素D" and set(record.get("times", [])) == expected_times)
        check("未臆造剂量", any(word in record.get("dosage", "") for word in ("未提供", "未指定", "未明确", "遵医嘱")) and not re.search(r"[0-9一二三四五六七八九十]+\s*(?:片|粒|滴|毫克|mg|IU)", record.get("dosage", ""), re.I))
        artifact = record.get("calendar_file", {})
        path = Path(artifact.get("path", "/nonexistent"))
        check("实际日历文件及哈希可复核", path.is_file() and hashlib.sha256(path.read_bytes()).hexdigest() == artifact.get("sha256"))
        if cid.endswith("004"):
            actions = [e["action"] for e in events]
            check("先读既有档案，再生成提醒", "record_query" in actions and "reminder_create" in actions and actions.index("record_query") < actions.index("reminder_create"))
    elif cid == "ZJ_JK_CYTX_002":
        check("真实查询提醒", successful("health-advice", "reminder_query"))
        check("查询不创建提醒", not state.get("reminders"))
    elif cid == "ZJ_JK_JKDA_002":
        check("真实查询档案", successful("health-advice", "record_query"))
        check("未伪造健康档案", not state.get("records"))
    elif cid == "ZJ_JK_JKDA_001":
        outcome = "needs_input"
        check("缺少就诊资料时未写入虚构诊断", not state.get("records"))
        text = case["turns"][0].get("response", "")
        check("明确请求补充资料", any(word in text for word in ("提供", "补充", "告诉")))
        notes.append("用例没有给出就诊资料，只能请求补充，不能记为档案更新完成。")
    elif cid.startswith("CK_GUI"):
        group = "gui_plan"
        amount = 100 if cid.endswith("001") else 88
        plans = state.get("gui", {}).get("plans", [])
        matches = [p for p in plans if p.get("operation") == "recharge" and p.get("constraints", {}).get("amount") == amount]
        if cid.endswith("003"):
            matches = [p for p in matches if p.get("constraints", {}).get("phone_number") == "18610002895"]
        check("有保存的充值计划且号码/金额正确", matches)
        check("明确只生成计划，未声称实际支付", matches and all(p.get("execution") == "planned" and p.get("device_connected") is False for p in matches))
        notes.append("此处验收原数据要求的步骤；没有验证真实手机点击或支付。")
    elif cid in ("QA_JJ_009", "TGJK_YD2_010"):
        group = "conditional"
        weather = successful("web-search", "weather")
        if not weather:
            weather = [e["result"] for e in events if e["skill"] == "web-search" and e["result"].get("UserOutputObject", {}).get("humidity_assessment")]
        check("真实天气事实作为条件依据", weather)
        humid = weather[-1]["UserOutputObject"].get("humidity_assessment", {}).get("is_humid") if weather else None
        if cid == "QA_JJ_009":
            if successful("smart-home", "control"):
                home({"living-ac": {"temperature": 25, "mode": "除湿"}})
            else:
                check("未凭未知条件执行设备动作", not state.get("home_history"))
            outcome = "manual_review"
            notes.append("很闷需要结合温度、地点和上游判断；不能仅用湿度布尔值自动判定条件。本例必须同时人工检查天气文字与条件执行。")
        else:
            plans = state.get("gui", {}).get("plans", [])
            check("独立导航计划保留终点/避堵/最快约束", any(p.get("constraints", {}).get("destination") == "市人民医院" and p["constraints"].get("avoid_congestion") is True and p["constraints"].get("route_preference") == "fastest" for p in plans))
            reminders = list(state.get("reminders", {}).values())
            check("符合实际湿度分支，无重复提醒", (humid is True and not reminders) or (humid is False and len(reminders) == 1 and set(reminders[0].get("times", [])) == {"08:00", "20:00"}))
            check("第二轮延续前文地区", len(case["turns"]) == 2 and "川西" in case["turns"][1].get("response", ""))
            notes.append("天气为湿时不创建提醒是正确分支；原expected_skills仍要求健康skill，原分数保留。徒步文字未自动判定质量。")
    else:
        group, outcome = "text", "manual_review"
        notes.append("写作/长文翻译需内容评分，不能用非空回复或数据库存在替代质量判断。")
    if any(not item["passed"] for item in checks):
        outcome = "failed"
    return {"case_id": cid, "group": group, "outcome": outcome, "checks": checks, "notes": notes,
            "original_rule_success": case.get("case_success"), "original_skill_checks": [t["checks"].get("skills_exact") for t in case["turns"]]}


def assess(report):
    cases = [check_case(case) for case in report["cases"]]
    return {"notice": "开发集的独立业务状态检查，不改变原标签和评分；不等于全量成功率或文字质量评分。",
            "cases": cases, "summary": dict(Counter(c["outcome"] for c in cases)),
            "groups": {group: dict(Counter(c["outcome"] for c in cases if c["group"] == group)) for group in sorted({c["group"] for c in cases})}}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("report", type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    report = assess(json.loads(args.report.read_text()))
    destination = args.output or args.report.with_name("business-checks.json")
    destination.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n")
    print(json.dumps({key: value for key, value in report.items() if key != "cases"}, ensure_ascii=False, indent=2))
    for case in report["cases"]:
        if case["outcome"] == "failed":
            print(case["case_id"], "; ".join(check["name"] for check in case["checks"] if not check["passed"]))
