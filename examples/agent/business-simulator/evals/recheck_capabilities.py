#!/usr/bin/env python3
"""Offline gap probes, not an Agent benchmark or an expected-answer provider.

Every probe uses a temporary database. No model, network, phone or production
backend is called. Observations describe adapters; an LLM may work around some
of them. A reproduced gap is a finding, never a passing business task.
"""
from __future__ import annotations

import argparse
from datetime import datetime, timedelta, timezone
import hashlib
import json
import os
from pathlib import Path
import sys
import tempfile
from unittest.mock import patch

PACKAGE = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(PACKAGE / "harness"))
from business_sim import device_world
from business_sim.services import dispatch
from business_sim.state import Store, fresh_state, scope_key
from business_sim.appointments import ZONE
from audit_capabilities import audit


def adapter_probes():
    findings = []

    def record(name, expected, observed, gap, cases=(), relevance="dataset"):
        findings.append({"id": name, "expected": expected, "observed": observed,
                         "status": "gap_reproduced" if gap else "not_reproduced",
                         "related_cases": list(cases), "relevance": relevance})

    os.environ["BUSINESS_EXECUTION_MODE"] = "real"
    os.environ.pop("BUSINESS_BACKEND_URL", None)
    with tempfile.TemporaryDirectory(prefix="business-capability-recheck-") as directory:
        store = Store(Path(directory) / "audit.sqlite3")

        def call(name, skill, **args):
            return dispatch(skill, args, store=store, scope=scope_key("audit-"+name, name))

        def successful(name, skill, **args):
            result = call(name, skill, **args)
            if result["status"] != "succeeded":
                raise RuntimeError(f"Probe setup failed: {name}: {result['status']}")
            return result["UserOutputObject"]

        unsupported = {}
        for op in ("favorite", "install", "uninstall", "checkin", "comment", "taxi"):
            answer = call("gui-actions", "device-gui", action="plan", app="审计测试应用", operation=op, query="审计内容")
            unsupported[op] = answer["status"]
        record("gui_action_vocabulary", "Plans can express favorite/install/checkin/comment/taxi steps",
               unsupported, all(v == "failed" for v in unsupported.values()),
               ["TGZS_WYYL_015", "TGJK_WYD_002", "TGZS_YDYL_001"], "schema_and_dataset")

        text = "审计消息：明天下午三点见"
        plan = successful("gui-send", "device-gui", action="plan", app="审计聊天应用", operation="send", content=text)["taskPlan"]
        record("gui_send_content_lost", "A saved send plan retains the supplied message and target",
               {"steps": plan["steps"], "constraints": plan["constraints"], "message_retained": text in json.dumps(plan, ensure_ascii=False)},
               text not in json.dumps(plan, ensure_ascii=False), relevance="boundary_probe")

        clinics = {}
        for hospital in ("协和医院", "朝阳医院", "安贞医院", "同仁医院", "阜外医院", "儿童医院", "中医院", "北医三院"):
            result = call("clinic-catalog-"+hospital, "health-advice", action="appointment_create", hospital=hospital, department="内科", date="明天")
            clinics[hospital] = result["status"]
        record("hospital_catalog", "Requested hospitals are represented in the declared test world",
               clinics, all(s == "not_found" for s in clinics.values()), ["SYS_DYT_01_081", "SYS_DYT_02_015", "SYS_DYT_03_032"])

        departments = {dept: call("departments-"+dept, "health-advice", action="appointment_create", hospital="市人民医院", department=dept, date="明天")["status"]
                       for dept in ("神经内科", "中医内科", "心血管内科", "体检")}
        record("department_catalog", "Clinical department names and aliases from the dataset resolve",
               departments, all(s == "not_found" for s in departments.values()), ["SYS_DYT_01_085", "SYS_DYT_01_089", "SYS_DYT_03_032"])

        answer = call("doctor", "health-advice", action="appointment_create", hospital="华西医院", department="耳鼻喉科", doctor="王医生", date="明天")
        record("doctor_catalog", "Named doctors can be resolved or disambiguated against a useful catalog",
               {"status": answer["status"], "data": answer["UserOutputObject"]}, answer["status"] == "not_found", ["SYS_DYT_01_086"])

        for probe, date_text, requested_hour in (("appointment_evening", "明天晚上", 18), ("appointment_chinese_hour", "明天下午3点", 15)):
            answer = call(probe,"health-advice",action="appointment_create",hospital="市人民医院",department="内科",date=date_text)
            item = answer.get("UserOutputObject",{}).get("appointment",{})
            hour = int(item["time"].split(":")[0]) if item else None
            record(probe,"Honor the supplied time or reject/clarify; never silently book a different time",
                   {"input":date_text,"status":answer["status"],"booked_time":item.get("time")},
                   answer["status"]=="succeeded" and (hour is None or hour < requested_hour),relevance="boundary_probe")

        name = "appointment-filter"
        for period in ("上午", "下午"):
            successful(name, "health-advice", action="appointment_create", hospital="市人民医院", department="内科", date="明天"+period)
        rows = successful(name, "health-advice", action="appointment_query", date="明天", time_period="下午")["appointments"]
        record("appointment_query_period", "An afternoon filter excludes morning reservations",
               {"requested": "下午", "returned_times": [r["time"] for r in rows]}, any(r["time"] < "12:00" for r in rows), relevance="boundary_probe")

        original = successful("update-reminder","health-advice",action="reminder_create",medicine="审计药",times=["08:00"])["medicationReminder"]
        answer = call("update-reminder", "health-advice", action="reminder_update", id=original["id"], times=["08:30"])
        record("reminder_update", "Existing reminders can be adjusted, including meal-relative changes",
               {"status": answer["status"], "reason": answer.get("UserOutputText")}, answer["status"] != "succeeded" or answer.get("UserOutputObject",{}).get("medicationReminder",{}).get("times") != ["08:30"], ["QA_JK_WYYL_011"])

        start = datetime.now(ZONE).date() + timedelta(days=1)
        end = start + timedelta(days=14)
        r = successful("weekday-reminder", "health-advice", action="reminder_create", medicine="审计样本", times=["08:00"], period="周一到周五",
                       start_date=start.isoformat(), end_date=end.isoformat())["medicationReminder"]
        rules = [line for line in Path(r["calendar_file"]["path"]).read_text().splitlines() if line.startswith("RRULE:")]
        record("reminder_weekday_becomes_daily", "Unsupported recurrence is rejected or weekdays are preserved",
               {"requested_period": r["period"], "calendar_rules": rules}, any("FREQ=DAILY" in line and "BYDAY" not in line for line in rules), relevance="boundary_probe")

        name = "reminder-filter"
        first = successful(name, "health-advice", action="reminder_create", medicine="审计甲", times=["08:00"])["medicationReminder"]
        second = successful(name, "health-advice", action="reminder_create", medicine="审计乙", times=["20:00"])["medicationReminder"]
        successful(name, "health-advice", action="reminder_cancel", id=second["id"])
        rows = successful(name, "health-advice", action="reminder_query", id=first["id"], medicine="审计甲")["reminders"]
        record("reminder_query_filter", "An explicit reminder ID/medicine filter is honored",
               {"returned": [{"medicine": r["medicine"], "active": r["active"]} for r in rows]}, len(rows) != 1, relevance="boundary_probe")

        name = "record-filter"
        first = successful(name, "health-advice", action="record_save", patient="审计甲", record_fields={"symptoms": "审计症状甲"})["healthRecord"]
        successful(name, "health-advice", action="record_save", patient="审计乙", record_fields={"symptoms": "审计症状乙"})
        rows = successful(name, "health-advice", action="record_query", id=first["id"], patient="审计甲")["records"]
        record("record_query_filter", "Patient and record ID filters select only the requested local record",
               {"patients_returned": [r["patient"] for r in rows]}, len(rows) != 1, relevance="boundary_probe")

        item = successful("record-fact", "health-advice", action="record_save", record_fields={"symptoms": "咳嗽三天", "medication": "尚未服药"})["healthRecord"]
        record("record_negated_fact_dropped", "A true negative fact is retained instead of treated as a placeholder",
               item["record_fields"], "medication" not in item["record_fields"], relevance="boundary_probe")

        state = fresh_state()
        begin = datetime.now(ZONE).replace(hour=18, minute=30, second=0, microsecond=0) - timedelta(days=4)
        state["world"]["last_tick"]=begin.timestamp()
        for offset in range(3):
            device_world.execute(state, {"action": "control", "commands": [{"device": "客厅灯", "action": "on"}]}, now=(begin+timedelta(days=offset)).timestamp())
        habits = device_world.execute(state, {"action": "habits", "analysis":"energy", "start_date":begin.date().isoformat(), "end_date":(begin+timedelta(days=2)).date().isoformat()}, now=(begin+timedelta(days=2)).timestamp())
        obj = habits["UserOutputObject"]
        record("home_habit_analysis", "Time-window energy ranking and co-occurrence have dedicated observations/analysis",
               {"fields": sorted(obj), "inference": obj.get("inference"), "modes": obj.get("modeList")},
               not obj.get("energy") or not obj.get("hourly_usage") or "cooccurrence" not in obj, ["ZJ_JJ_XGCX_003", "ZJ_JJ_XGCX_004", "ZJ_JJ_XGCX_005"])
        record("home_habit_operation_code", "The original habits response carries operation_type 100053",
               {"operation_type": habits.get("operation_type"), "has_mode_list": bool(obj.get("modeList"))},
               habits.get("operation_type") != "100053", ["ZJ_JJ_XGCX_001"])

        recommendations = [{"title": f"审计影片{i}", "highlight": f"审计简介{i}", "source_url": f"https://example.invalid/{i}"} for i in (1, 2)]
        successful("movie-detail", "movie-query", action="save_recommendations", recommendations=recommendations)
        data = successful("movie-detail", "movie-query", action="detail", index=2)
        movies = data.get("aiMovieData", {}).get("list", [])
        record("movie_detail_selection", "A detail request resolves the requested index rather than returning the full list",
               {"requested_index": 2, "returned_titles": [m["title"] for m in movies]}, len(movies) == 2, ["TGYJ_WYD_012"])

        successful("client-a", "user-memory", action="write", key="审计偏好", value="审计偏好值")
        data = dispatch("user-memory",{"action":"read","key":"审计偏好"},store=store,scope=scope_key("audit-client-a","client-b"))["UserOutputObject"]
        record("memory_same_user_new_client", "If long-term cross-device memory is required, a new client retains the user's preference",
               {"same_user": True, "different_client": True, "preferences": data["preferences"]}, not data["preferences"], relevance="product_boundary")

        archive={"hourly":{"time":["2023-09-05T12:00"],"relative_humidity_2m":[60],"temperature_2m":[22],"wind_speed_10m":[6],"precipitation":[0]}}
        with patch("business_sim.live_information.fetch_json",return_value=archive) as fetched:
            answer = call("historical-weather", "web-search", action="weather", location="北京", start_date="2023-09-05", end_date="2023-09-05")
        record("historical_weather", "Historical scenarios need archival data beyond the forecast adapter",
               {"status": answer["status"], "network":"stubbed_boundary_test", "url":fetched.call_args.args[0]}, answer["status"] != "succeeded" or "archive-api.open-meteo.com" not in fetched.call_args.args[0], ["DH_WD_LWWD_03_007"], "boundary_probe_related_dataset")

        name = "retry-without-id"
        for _ in range(2):
            successful(name, "health-advice", action="reminder_create", medicine="审计重试药", times=["08:00"])
        rows = successful(name, "health-advice", action="reminder_query")["reminders"]
        record("write_retry_requires_request_id", "Retry safety requires a stable identity for each logical write",
               {"repeated_calls_without_request_id": 2, "reminder_count": len(rows)}, len(rows) == 2, relevance="operational_risk")
    return findings


def dataset_candidates(report):
    hospitals = ("协和医院", "朝阳医院", "安贞医院", "同仁医院", "阜外医院", "儿童医院", "中医院", "北医三院")
    actions = ("收藏", "安装", "卸载", "签到", "评论", "打车")
    groups = {"expanded_hospital_mentions": [], "expanded_gui_action_mentions": []}
    for case in report["case_details"]:
        for turn in case["turns"]:
            row = {"id": case["id"], "turn": turn["turn"], "source": case["source"], "excel_row": case["excel_row"]}
            found = [word for word in hospitals if word in turn["user"]]
            if found and any(f["capability"] == "hospital" for f in turn["findings"]):
                groups["expanded_hospital_mentions"].append({**row, "matches": found, "user": turn["user"]})
            found = [word for word in actions if word in turn["user"] and word in turn["expected_behavior"]]
            if found and turn["plan_only_expectation"]:
                groups["expanded_gui_action_mentions"].append({**row, "matches": found, "user": turn["user"]})
    return {key: {"cases": len({r["id"] for r in rows}), "turns": len(rows), "items": rows} for key, rows in groups.items()}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    report = audit()
    probes = adapter_probes()
    hashes = {str(p.relative_to(PACKAGE)): hashlib.sha256(p.read_bytes()).hexdigest()
              for root in (PACKAGE / "harness/business_sim", PACKAGE / "evals") for p in sorted(root.glob("*.py"))}
    result = {"checked_at": datetime.now(timezone.utc).isoformat(),
              "notice": "Adapter probes plus static candidates, not LLM benchmark pass/fail counts. Probe data is isolated synthetic diagnostic input, never inserted into benchmark cases or runtime state.",
              "cases": report["cases"], "turns": report["turns"], "conversion": report["conversion"],
              "label_gaps": report["label_gaps"], "sources": report["sources"], "normalized_sha256": report["normalized_sha256"],
              "source_sha256": hashes, "dataset_candidates": dataset_candidates(report), "probes": probes}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, ensure_ascii=False, indent=2)+"\n")
    print(json.dumps({"output": str(args.output), "probe_count": len(probes),
                      "gaps_reproduced": sum(p["status"] == "gap_reproduced" for p in probes),
                      "not_reproduced": [p["id"] for p in probes if p["status"] != "gap_reproduced"],
                      "candidates": {k: {f: v[f] for f in ("cases", "turns")} for k, v in result["dataset_candidates"].items()}}, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
