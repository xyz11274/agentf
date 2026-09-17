"""Business effects, time evolution, inventory contention and owner isolation."""
from concurrent.futures import ThreadPoolExecutor
from datetime import datetime, timedelta
import copy
import json
from pathlib import Path
import sys
import time
from urllib.request import Request, build_opener, ProxyHandler

import pytest

PACKAGE = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(PACKAGE / "harness"))
from business_sim import appointments, device_world
from business_sim.state import Store, fresh_state, scope_key
from business_sim.services import dispatch
from business_sim.backend import BusinessServer


@pytest.fixture
def environment(tmp_path, monkeypatch):
    monkeypatch.setenv("BUSINESS_EXECUTION_MODE", "real")
    monkeypatch.delenv("BUSINESS_BACKEND_URL", raising=False)
    return Store(tmp_path / "business.sqlite3"), scope_key("user-a", "client-a")


def call(environment, skill, **args):
    return dispatch(skill, args, store=environment[0], scope=environment[1])


def devices(environment):
    return {d["id"]: d for d in environment[0].read(environment[1])["devices"]}


def test_controls_are_persisted_with_one_batch_and_actual_transitions(environment):
    commands = [
        {"device": "空调", "action": "set", "property": "temperature", "value": 25},
        {"device": "空调", "action": "set", "property": "mode", "value": "制冷"},
        {"device": "空调", "action": "set", "property": "fan_speed", "value": "中档"},
        {"device": "灯", "room": "卧室", "all": True, "action": "off"},
        {"device": "床头灯", "action": "set", "property": "color", "value": "暖光"},
    ]
    outcome = call(environment, "smart-home", action="control", commands=commands, command_text="设置晚间家庭设备", request_id="one-batch")
    assert outcome["status"] == "succeeded", outcome
    assert outcome["environment"] == "business_test_system" and outcome["physical_effect"] is False
    assert len(outcome["UserOutputObject"]["commandList"]) == 1
    actual = devices(environment)
    assert actual["living-ac"]["properties"] == {"temperature": 25, "mode": "制冷", "fan_speed": "中档"}
    assert actual["bedroom-light"]["power"] is False
    assert actual["bedside-light"]["properties"]["color"] == "暖光"
    assert len(environment[0].read(environment[1])["home_history"]) == 6
    again = call(environment, "smart-home", action="control", commands=commands, command_text="设置晚间家庭设备", request_id="one-batch")
    assert again["replayed"] is True
    assert len(environment[0].read(environment[1])["home_history"]) == 6
    assert Store(environment[0].path).read(environment[1])["devices"] == environment[0].read(environment[1])["devices"]


def test_batch_failure_cannot_leave_partial_device_effects(environment):
    call(environment, "smart-home", action="query")
    device_world.set_online(*environment, "living-ac", False)
    before = copy.deepcopy(devices(environment))
    result = call(environment, "smart-home", action="control", commands=[
        {"device": "客厅灯", "action": "on"}, {"device": "空调", "action": "set", "property": "temperature", "value": 25}])
    assert result["status"] == "failed" and result["error"] == "device_offline"
    assert devices(environment) == before
    device_world.set_online(*environment, "living-ac", True)
    result = call(environment, "smart-home", action="control", commands=[
        {"device": "客厅灯", "action": "on"}, {"device": "空调", "action": "set", "property": "temperature", "value": 90}])
    assert result["status"] == "failed" and devices(environment)["living-light"]["power"] is False


def test_device_world_evolves_after_command_without_llm():
    state = fresh_state()
    state.update(device_world.initial_world(1000))
    device_world.execute(state, {"action": "control", "commands": [
        {"device": "窗帘", "action": "set", "property": "position", "value": 50},
        {"device": "空调", "action": "set", "property": "mode", "value": "除湿"}]}, now=1000)
    curtain = next(d for d in state["devices"] if d["id"] == "living-curtain")
    assert curtain["observed"]["position"] == 100 and curtain["moving"] is True
    device_world.advance(state, 1004)
    assert curtain["observed"]["position"] == 80 and curtain["moving"] is True
    device_world.advance(state, 2200)
    assert curtain["observed"]["position"] == 50 and curtain["moving"] is False
    assert state["world"]["rooms"]["客厅"]["humidity"] < 58
    assert next(d for d in state["devices"] if d["id"] == "living-ac")["energy_kwh"] > .3


def test_no_fabricated_habits_and_observations_can_produce_statistics():
    state = fresh_state()
    initial = device_world.execute(state, {"action": "habits"}, now=1000)
    assert initial["UserOutputObject"]["modeList"] == []
    start = datetime(2026, 9, 17, 18, 30, tzinfo=appointments.ZONE)
    for i in range(3):
        device_world.execute(state, {"action": "control", "command_text": "开客厅灯", "commands": [
            {"device": "客厅灯", "action": "on"}]}, (start + timedelta(days=i)).timestamp())
    outcome = device_world.execute(state, {"action": "habits", "device": "客厅灯"}, (start + timedelta(days=2)).timestamp())
    assert outcome["UserOutputObject"]["modeList"][0]["habitTime"] == "18:30"
    assert outcome["UserOutputObject"]["modeList"][0]["observation_days"] == 3


def test_device_ambiguity_is_not_arbitrarily_resolved(environment):
    result = call(environment, "smart-home", action="control", commands=[{"device": "灯", "room": "卧室", "action": "off"}])
    assert result["status"] == "needs_input"
    assert len(result["UserOutputObject"]["candidates"]) == 2
    assert not environment[0].read(environment[1])["home_history"]


def test_appointment_booking_query_retry_and_cancel_release(environment):
    slots = call(environment, "health-advice", action="appointment_slots", hospital="上海瑞金医院", department="皮肤科", date="明天上午")
    assert slots["status"] == "succeeded", slots
    slot = slots["UserOutputObject"]["slots"][0]
    args = {"action": "appointment_create", "slot_id": slot["id"], "request_id": "book-once"}
    created = call(environment, "health-advice", **args)
    assert created["status"] == "succeeded", created
    record = created["UserOutputObject"]["appointment"]
    assert record["hospital"] == "上海瑞金医院" and record["time"] == "09:00"
    assert call(environment, "health-advice", **args)["replayed"] is True
    query = call(environment, "health-advice", action="appointment_query", date="未来一周")
    assert len(query["UserOutputObject"]["appointments"]) == 1
    assert call((environment[0], scope_key("other", "client")), "health-advice", action="appointment_cancel", id=record["id"])["status"] == "not_found"
    assert call(environment, "health-advice", action="appointment_cancel", id=record["id"])["status"] == "succeeded"
    assert call(environment, "health-advice", action="appointment_query")["UserOutputObject"]["appointments"] == []
    inventory = call(environment, "health-advice", action="appointment_slots", hospital="上海瑞金医院", department="皮肤科", date="明天上午")
    assert inventory["UserOutputObject"]["slots"][0]["remaining"] == 2


def test_last_slot_concurrency_never_overbooks_and_cancel_releases(environment):
    slot = call(environment, "health-advice", action="appointment_slots", department="消化内科", date="明天上午")["UserOutputObject"]["slots"][0]
    with environment[0].connect() as db:
        db.execute("UPDATE clinic_slots SET capacity=1 WHERE id=?", (slot["id"],))
    def reserve(i):
        scope = scope_key(f"patient-{i}", "client")
        return scope, call((environment[0], scope), "health-advice", action="appointment_create", slot_id=slot["id"], request_id="one")
    with ThreadPoolExecutor(max_workers=6) as pool:
        results = list(pool.map(reserve, range(6)))
    winners = [(scope, result) for scope, result in results if result["status"] == "succeeded"]
    assert len(winners) == 1
    assert all(r["error"] == "slot_full" for _, r in results if r["status"] != "succeeded")
    scope, booked = winners[0]
    call((environment[0], scope), "health-advice", action="appointment_cancel", id=booked["UserOutputObject"]["appointment"]["id"])
    assert reserve(10)[1]["status"] == "succeeded"


def test_bad_reschedule_preserves_existing_reservation(environment):
    record = call(environment, "health-advice", action="appointment_create", department="骨科", hospital="北京301医院", date="下周一")["UserOutputObject"]["appointment"]
    failed = call(environment, "health-advice", action="appointment_reschedule", id=record["id"], hospital="不存在的医院", date="明天")
    assert failed["status"] == "not_found"
    original = call(environment, "health-advice", action="appointment_query", id=record["id"])["UserOutputObject"]["appointments"][0]
    assert original["status"] == "booked" and original["starts_at"] == record["starts_at"]
    changed = call(environment, "health-advice", action="appointment_reschedule", id=record["id"], date="后天下午")
    assert changed["status"] == "succeeded", changed
    assert changed["UserOutputObject"]["appointment"]["time"] == "14:00"
    assert len(call(environment, "health-advice", action="appointment_query")["UserOutputObject"]["appointments"]) == 1


def test_unknown_doctor_past_dates_and_conflicting_patient_fail(environment):
    for extra in ({"doctor": "不存在的医生", "date": "明天"}, {"date": "2020-01-01"}, {"date": "随便未来哪一天"}):
        assert call(environment, "health-advice", action="appointment_create", department="眼科", **extra)["status"] != "succeeded"
    first = call(environment, "health-advice", action="appointment_create", department="眼科", date="明天上午")
    assert first["status"] == "succeeded"
    conflict = call(environment, "health-advice", action="appointment_create", department="骨科", date="明天上午")
    assert conflict["status"] == "failed" and conflict["error"] == "patient_time_conflict"


def test_weekend_not_too_early_and_specialist_constraints(environment):
    record = call(environment, "health-advice", action="appointment_create", department="眼科", date="周末下午，别太早")["UserOutputObject"]["appointment"]
    assert datetime.fromisoformat(record["starts_at"]).weekday() in (5, 6)
    assert record["time"] >= "15:00"
    record = call(environment, "health-advice", action="appointment_create", department="消化内科", date="下周一", appointment_type="专家号")["UserOutputObject"]["appointment"]
    assert datetime.fromisoformat(record["starts_at"]).weekday() == 0 and record["appointment_type"] == "专家号"


def test_transaction_failure_rolls_back_related_inventory(environment):
    store, scope = environment
    appointments.initialize(store)
    def fail(state, db):
        db.execute("INSERT INTO clinic_slots VALUES ('rollback', 1, '{}')")
        state["preferences"]["should_not_exist"] = True
        return {"status": "failed"}
    store.run(scope, "test", {"action": "rollback"}, fail, transactional=True)
    with store.connect() as db:
        assert db.execute("SELECT count(*) FROM clinic_slots WHERE id='rollback'").fetchone()[0] == 0
    assert "should_not_exist" not in store.read(scope)["preferences"]
    assert store.events(scope)[0]["result"]["status"] == "failed"


def test_independent_http_world_updates_between_requests_and_isolates_users(environment):
    server = BusinessServer(("127.0.0.1", 0), environment[0].path).start()
    opener = build_opener(ProxyHandler({}))
    def request(path, body=None, user="user-a"):
        req = Request(server.url + path, data=json.dumps(body).encode() if body is not None else None,
                      headers={"X-Business-User": user, "X-Business-Client": "client-a", "Content-Type": "application/json"})
        with opener.open(req, timeout=5) as reply:
            return json.load(reply)
    try:
        request("/v1/services/smart-home", {"action": "control", "commands": [{"device": "窗帘", "action": "off"}]})
        initial = next(d for d in request("/v1/devices")["devices"] if d["id"] == "living-curtain")
        time.sleep(1.2)
        current = next(d for d in request("/v1/state")["devices"] if d["id"] == "living-curtain")
        assert current["observed"]["position"] < initial["observed"]["position"]
        assert next(d for d in request("/v1/devices", user="other")["devices"] if d["id"] == "living-curtain")["properties"]["position"] == 100
        offline = request("/v1/devices/living-ac/connection", {"online": False})
        assert offline["UserOutputObject"]["device"]["online"] is False
        assert request("/health")["world_worker_running"] is True
    finally:
        server.close()


def test_selected_slot_cannot_discard_time_constraints_or_query_into_booking(environment):
    slot = call(environment, "health-advice", action="appointment_slots", department="眼科", date="明天上午")["UserOutputObject"]["slots"][0]
    wrong = call(environment, "health-advice", action="appointment_create", slot_id=slot["id"], date="明天下午")
    assert wrong["status"] == "failed" and wrong["error"] == "slot_constraint_mismatch"
    queried = call(environment, "health-advice", action="appointment_slots", slot_id=slot["id"])
    assert queried["status"] == "succeeded"
    assert not environment[0].read(environment[1])["appointments"]


def test_reset_releases_only_current_users_inventory(environment):
    slot = call(environment, "health-advice", action="appointment_slots", department="眼科", date="明天上午")["UserOutputObject"]["slots"][0]
    other = (environment[0], scope_key("other", "client"))
    assert call(environment, "health-advice", action="appointment_create", slot_id=slot["id"])["status"] == "succeeded"
    assert call(other, "health-advice", action="appointment_create", slot_id=slot["id"])["status"] == "succeeded"
    environment[0].reset(environment[1])
    available = call(environment, "health-advice", action="appointment_slots", slot_id=slot["id"])
    assert available["UserOutputObject"]["slots"][0]["remaining"] == 1
    assert len(call(other, "health-advice", action="appointment_query")["UserOutputObject"]["appointments"]) == 1


def test_pending_update_is_not_a_health_record_and_real_fields_survive_updates(environment):
    for args in ({"content": "用户看完医生，需要更新健康档案"},
                 {"record_fields": {"diagnosis": "未提供，等待用户补充"}}):
        result = call(environment, "health-advice", action="record_save", **args)
        assert result["status"] == "needs_input"
        assert not environment[0].read(environment[1])["records"]
    saved = call(environment, "health-advice", action="record_save", record_fields={"allergies": "用户明确提供的过敏史", "diagnosis": "未提供"})
    item = saved["UserOutputObject"]["healthRecord"]
    assert "diagnosis" not in item["record_fields"]
    updated = call(environment, "health-advice", action="record_save", id=item["id"], record_fields={"doctor_advice": "用户转述：一周后复查"})
    assert updated["UserOutputObject"]["healthRecord"]["record_fields"]["allergies"] == "用户明确提供的过敏史"
    assert len(environment[0].read(environment[1])["records"]) == 1


def test_weather_city_summaries_preserve_identity_and_weight_hourly_samples():
    from unittest.mock import patch
    from business_sim.live_information import weather
    def payload(times, values):
        return {"hourly": {"time": times, "relative_humidity_2m": values,
                           "temperature_2m": [20 + i for i in range(len(values))]}}
    replies = [payload(["2026-09-17T00:00", "2026-09-17T01:00", "2026-09-18T00:00"], [20, 40, 100]),
               payload(["2026-09-17T00:00", "2026-09-17T01:00"], [80, 60]),
               payload(["2026-09-17T00:00"], [55])]
    with patch("business_sim.live_information.fetch_json", return_value=replies):
        data = weather({"location": "江南", "date": "本周"})["UserOutputObject"]
    summaries = {row["name"]: row for row in data["location_summaries"]}
    assert summaries["杭州"]["humidity_mean_pct"] == 53.3
    assert summaries["苏州"]["humidity_mean_pct"] == 70
    assert summaries["南京"]["humidity_mean_pct"] == 55
    assert data["humidity_mean_pct"] == 59.2
    assert summaries["杭州"]["daily_high_min_c"] == 21
    assert summaries["杭州"]["temperature_max_c"] == 22
