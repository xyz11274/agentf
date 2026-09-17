"""A transactional appointment service for the independent business test system.

Hospital names are routing aliases in this test catalog. No requests are sent
to those hospitals. Inventory is shared between users; reservations are private.
"""
from __future__ import annotations

from datetime import datetime, timedelta
import hashlib
import json
import re
import uuid
from zoneinfo import ZoneInfo

from .live_information import result
from .time_constraints import dates, clock_constraints, matches_clock

ZONE = ZoneInfo("Asia/Shanghai")
HOSPITALS = [
    {"id": "city", "name": "市人民医院", "aliases": ["人民医院", "市人民医院"]},
    {"id": "city-first", "name": "市第一人民医院", "aliases": ["第一人民医院", "市第一人民医院"]},
    {"id": "ruijin", "name": "上海瑞金医院", "aliases": ["瑞金医院", "上海瑞金医院"]},
    {"id": "301", "name": "北京301医院", "aliases": ["301医院", "北京301医院", "解放军总医院"]},
    {"id": "huaxi", "name": "华西医院", "aliases": ["四川大学华西医院", "华西医院"]},
]
HOSPITALS += [
    {"id": key, "name": name, "aliases": aliases}
    for key, name, aliases in (
        ("xiehe", "北京协和医院", ["协和医院", "北京协和"]),
        ("chaoyang", "北京朝阳医院", ["朝阳医院"]),
        ("anzhen", "北京安贞医院", ["安贞医院"]),
        ("tongren", "北京同仁医院", ["同仁医院"]),
        ("fuwai", "阜外医院", ["北京阜外医院"]),
        ("children", "北京儿童医院", ["儿童医院"]),
        ("tcm", "市中医院", ["中医院", "中医医院"]),
        ("peking-third", "北京大学第三医院", ["北医三院", "北京北医三院"]),
    )
]
DEPARTMENT_ALIASES = {"心血管内科": "心内科", "耳鼻喉": "耳鼻喉科", "体检科": "体检", "体检中心": "体检"}
TEST_DOCTORS = {"测试医师甲": "普通号", "测试专家乙": "专家号", "王医生": "普通号", "李医生": "普通号", "张医生": "专家号"}
DEPARTMENTS = ["消化内科", "皮肤科", "眼科", "骨科", "耳鼻喉科", "内科", "外科", "儿科", "心内科", "呼吸内科", "妇科", "口腔科", "神经内科", "中医内科", "体检", "内分泌科", "泌尿外科", "肿瘤科", "精神科", "康复科", "产科"]


def output(text, data=None, status="succeeded", **extra):
    return result(text, data, status=status, simulated=True, source="business-test-clinic",
                  environment="business_test_system", physical_effect=False,
                  execution="executed" if status == "succeeded" else "not_executed", **extra)


def initialize(store):
    with store.connect() as db:
        db.executescript("""
            CREATE TABLE IF NOT EXISTS clinic_slots (
                id TEXT PRIMARY KEY, capacity INTEGER NOT NULL CHECK(capacity >= 0), payload TEXT NOT NULL);
            CREATE TABLE IF NOT EXISTS clinic_reservations (
                id TEXT PRIMARY KEY, scope TEXT NOT NULL, patient TEXT NOT NULL,
                slot_id TEXT NOT NULL, status TEXT NOT NULL, payload TEXT NOT NULL);
            CREATE INDEX IF NOT EXISTS clinic_stock ON clinic_reservations(slot_id, status);
            CREATE INDEX IF NOT EXISTS clinic_owner ON clinic_reservations(scope, status);
            CREATE UNIQUE INDEX IF NOT EXISTS clinic_duplicate ON clinic_reservations(scope, patient, slot_id)
                WHERE status='booked';
        """)


def date_range(text, today, *, query=False):
    parsed = dates(text, today, default_days=28 if query else None)
    if parsed and not query and any(w in text for w in ("本周", "这周", "周末")) and not re.search(r"(?:周|星期)[一二三四五六日天]", text):
        return max(parsed[0], today), parsed[1]
    return parsed


def catalog():
    return {"hospitals": HOSPITALS, "departments": DEPARTMENTS,
            "doctors": [{"name": name, "appointment_type": kind, "test_profile": True} for name, kind in TEST_DOCTORS.items()],
            "department_aliases": DEPARTMENT_ALIASES, "default_hospital": "市人民医院", "default_patient": "当前测试用户",
            "timezone": "Asia/Shanghai", "booking_horizon_days": 28,
            "notice": "独立业务测试系统的目录与号源，不是同名医院的真实号源。"}


def search_slots(db, args, now):
    hospital = args.get("hospital")
    hospitals = [h for h in HOSPITALS if hospital in [h["id"], h["name"], *h["aliases"]]] if hospital else [HOSPITALS[0]]
    if not hospitals:
        return output("测试系统没有该医院，请从目录选择。", catalog(), status="not_found"), []
    department = DEPARTMENT_ALIASES.get(args.get("department"), args.get("department"))
    if not department:
        return output("请提供科室。", catalog(), status="needs_input", missing_fields=["department"]), []
    if department not in DEPARTMENTS:
        return output("测试系统没有该科室。", catalog(), status="not_found"), []
    date_text = args.get("date", "")
    dates = date_range(date_text, now.date(), query=args["action"] == "appointment_slots")
    if dates is None:
        return output("请提供可确定的就诊日期。", status="needs_input", missing_fields=["date"]), []
    start, end = dates
    if start < now.date() or end >= now.date() + timedelta(days=28):
        return output("只开放今天起28天内的测试号源。", status="failed", error="date_out_of_range"), []
    kind = args.get("appointment_type") or TEST_DOCTORS.get(args.get("doctor"), "普通号")
    if kind in ("专家", "专家门诊", "专家号"):
        kind = "专家号"
    elif kind in ("普通", "普通门诊", "门诊", "普通号", ""):
        kind = "普通号"
    else:
        return output("当前支持普通号和专家号。", status="needs_input", missing_fields=["appointment_type"]), []
    doctor = args.get("doctor") or ("测试专家乙" if kind == "专家号" else "测试医师甲")
    if doctor not in TEST_DOCTORS:
        return output("指定医生不在测试目录，请选择或补充完整医生身份。", {"available_doctors": list(TEST_DOCTORS)}, status="not_found"), []
    if TEST_DOCTORS[doctor] != kind:
        return output("指定医生与号别不匹配。", status="needs_input", missing_fields=["doctor", "appointment_type"]), []
    try:
        time_filter = clock_constraints(args)
    except ValueError as exc:
        return output(str(exc), status="needs_input", missing_fields=["date", "time_period"]), []
    slots = []
    day = start
    while day <= end:
        for hour in ((10, 15) if kind == "专家号" else (9, 10, 11, 14, 15, 16)):
            when = datetime.combine(day, datetime.min.time(), ZONE).replace(hour=hour)
            if when <= now or not matches_clock(when.strftime("%H:%M"), time_filter):
                continue
            h = hospitals[0]
            identity = json.dumps([h["id"], department, doctor, when.isoformat()])
            slot_id = "slot-" + hashlib.sha256(identity.encode()).hexdigest()[:20]
            slot = {"id": slot_id, "hospital": h["name"], "department": department, "doctor": doctor,
                    "appointment_type": kind, "date": day.isoformat(), "time": when.strftime("%H:%M"),
                    "starts_at": when.isoformat(), "timezone": "Asia/Shanghai",
                    "fee": {"amount": 50 if kind == "专家号" else 20, "currency": "CNY", "source": "test_catalog"},
                    "campus": "测试门诊楼", "address": "独立测试环境，无真实就诊地址", "test_profile": True}
            db.execute("INSERT OR IGNORE INTO clinic_slots VALUES (?, ?, ?)", (slot_id, 2, json.dumps(slot, ensure_ascii=False)))
            capacity = db.execute("SELECT capacity FROM clinic_slots WHERE id=?", (slot_id,)).fetchone()[0]
            occupied = db.execute("SELECT count(*) FROM clinic_reservations WHERE slot_id=? AND status='booked'", (slot_id,)).fetchone()[0]
            slots.append({**slot, "capacity": capacity, "remaining": capacity - occupied})
        day += timedelta(days=1)
    return None, slots


def execute(state, args, db, scope, now=None):
    now = now or datetime.now(ZONE)
    now = now.astimezone(ZONE)
    action = args["action"]
    if action == "appointment_catalog":
        return output("已读取业务测试系统的预约目录。", catalog())
    if action == "appointment_query":
        rows = db.execute("SELECT payload FROM clinic_reservations WHERE scope=? ORDER BY id", (scope,)).fetchall()
        records = [json.loads(row[0]) for row in rows]
        if args.get("id"):
            records = [r for r in records if r["id"] == args["id"]]
        else:
            records = [r for r in records if args.get("include_history") or r["status"] == "booked"]
        if args.get("date"):
            dates = date_range(args["date"], now.date(), query=True)
            if dates is None:
                return output("请明确要查询的日期范围。", status="needs_input", missing_fields=["date"])
            records = [r for r in records if dates[0].isoformat() <= r["date"] <= dates[1].isoformat()]
        filters = dict(args)
        if args.get("hospital"):
            match = [h for h in HOSPITALS if args["hospital"] in (h["id"], h["name"], *h["aliases"])]
            filters["hospital"] = match[0]["name"] if match else args["hospital"]
        if args.get("department"):
            filters["department"] = DEPARTMENT_ALIASES.get(args["department"], args["department"])
        for key in ("hospital", "department", "doctor", "appointment_type", "patient", "status"):
            if filters.get(key):
                records = [r for r in records if r.get(key) == filters[key]]
        try:
            time_filter = clock_constraints(args)
        except ValueError as exc:
            return output(str(exc), status="needs_input", missing_fields=["time_period"])
        records = [r for r in records if matches_clock(r["time"], time_filter)]
        return output("已查询测试系统的预约记录。", {"appointments": records}, operation_type="100156")
    old = None
    if action in ("appointment_cancel", "appointment_reschedule"):
        row = db.execute("SELECT payload FROM clinic_reservations WHERE scope=? AND id=?", (scope, args.get("id", ""))).fetchone()
        if not row:
            return output("预约不存在或不属于当前用户。", status="not_found")
        old = json.loads(row[0])
        if action == "appointment_cancel":
            old.update(status="cancelled", cancelled_at=now.isoformat())
            db.execute("UPDATE clinic_reservations SET status='cancelled',payload=? WHERE id=?", (json.dumps(old, ensure_ascii=False), old["id"]))
            state["appointments"][old["id"]] = old
            return output("已取消测试预约并释放号源。", {"appointment": old})
        if old["status"] != "booked":
            return output("只有有效预约可以改约。", status="failed")
        if not args.get("date") and not args.get("slot_id"):
            return output("请提供新的日期或号源。", status="needs_input", missing_fields=["date", "slot_id"])
        args = {**{k: old[k] for k in ("hospital", "department", "appointment_type", "patient", "doctor")}, **args}
    if args.get("slot_id"):
        row = db.execute("SELECT payload FROM clinic_slots WHERE id=?", (args["slot_id"],)).fetchone()
        if not row:
            return output("号源不存在，请先查询。", status="not_found")
        slot = json.loads(row[0])
        constraints = {k: slot[k] for k in ("hospital", "department", "doctor", "appointment_type", "date")}
        error, matching = search_slots(db, {**constraints, **args}, now)
        if error:
            return error
        if not any(candidate["id"] == slot["id"] for candidate in matching):
            return output("所选号源不符合附带的日期/时段/科室等约束。", status="failed", error="slot_constraint_mismatch")
        if action == "appointment_slots":
            return output("已查询指定测试号源库存。", {"slots": [candidate for candidate in matching if candidate["id"] == slot["id"]]})
        slots = [slot]
    else:
        error, slots = search_slots(db, args, now)
        if error:
            return error
        if action == "appointment_slots":
            return output("已查询实时测试号源库存。", {"slots": slots, "catalog": catalog()})
        slots = [s for s in slots if s["remaining"] > 0]
    if not slots:
        return output("没有符合条件的可预约号源。", status="failed", error="no_available_slot")
    slot = slots[0]
    if slot["starts_at"] <= now.isoformat():
        return output("该号源已过期。", status="failed", error="expired_slot")
    patient = args.get("patient") or "当前测试用户"
    duplicate = db.execute("SELECT payload FROM clinic_reservations WHERE scope=? AND patient=? AND slot_id=? AND status='booked'",
                           (scope, patient, slot["id"])).fetchone()
    if duplicate and (not old or json.loads(duplicate[0])["id"] == old["id"]):
        return output("该测试预约已经存在，未重复占用号源。", {"appointment": json.loads(duplicate[0])}, duplicate=True)
    others = db.execute("SELECT id,payload FROM clinic_reservations WHERE scope=? AND patient=? AND status='booked'", (scope, patient)).fetchall()
    if any(json.loads(payload)["starts_at"] == slot["starts_at"] and (not old or rid != old["id"]) for rid, payload in others):
        return output("该患者在此时段已有预约。", status="failed", error="patient_time_conflict")
    capacity = db.execute("SELECT capacity FROM clinic_slots WHERE id=?", (slot["id"],)).fetchone()[0]
    used = db.execute("SELECT count(*) FROM clinic_reservations WHERE slot_id=? AND status='booked'", (slot["id"],)).fetchone()[0]
    if used >= capacity:
        return output("该号源已满，请重新查询。", status="failed", error="slot_full")
    if old:
        old.update(status="rescheduled", replaced_at=now.isoformat())
        db.execute("UPDATE clinic_reservations SET status='rescheduled',payload=? WHERE id=?", (json.dumps(old, ensure_ascii=False), old["id"]))
        state["appointments"][old["id"]] = old
    reservation = {**{k: v for k, v in slot.items() if k not in ("id", "remaining", "capacity")},
                   "id": "booking-" + uuid.uuid4().hex, "slot_id": slot["id"], "patient": patient,
                   "status": "booked", "created_at": now.isoformat(), "environment": "business_test_system",
                   "selection": "explicit_slot" if args.get("slot_id") else "earliest_available_matching_constraints",
                   "hospital_defaulted": not bool(args.get("hospital")),
                   "payment_status": "not_required_in_test_system", "paid_amount": 0,
                   "requested_constraints": {k: args[k] for k in ("date", "time_period", "not_before", "not_after", "department") if k in args}}
    db.execute("INSERT INTO clinic_reservations VALUES (?, ?, ?, ?, 'booked', ?)",
               (reservation["id"], scope, patient, slot["id"], json.dumps(reservation, ensure_ascii=False)))
    state["appointments"][reservation["id"]] = reservation
    return output("预约已在业务测试系统完成，已占用号源。", {"appointment": reservation}, operation_type="100154")
