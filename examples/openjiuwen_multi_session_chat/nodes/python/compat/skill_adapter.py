#!/usr/bin/env python3
import calendar
import datetime as dt
import json
import re
from typing import Dict, List


SKILL_CATALOG = [
    {
        "skill": "skill_search",
        "workflow_id": "skill-search-workflow",
        "title": "搜索检索",
        "description": "检索背景资料、新闻线索和外部信息。",
        "handles": ["搜索", "检索", "新闻", "热点", "资料背景"],
        "does_not_handle": ["实时天气", "未接入外部数据源的即时事实问答"],
        "abstain_when": "需要实时在线数据但当前示例未接入对应工具",
    },
    {
        "skill": "skill_summary",
        "workflow_id": "skill-summary-workflow",
        "title": "总结归纳",
        "description": "整理已有信息并输出结构化总结。",
        "handles": ["总结上条消息", "归纳已有结果", "复盘对话内容"],
        "does_not_handle": ["直接回答外部事实问题"],
        "abstain_when": "没有可总结的历史或中间结果",
    },
    {
        "skill": "skill_date",
        "workflow_id": "skill-date-workflow",
        "title": "日期时间",
        "description": "处理日期、星期、时间安排等问题。",
        "handles": ["几号", "星期几", "日期推算", "当前时间"],
        "does_not_handle": ["天气", "新闻", "泛化常识问答"],
        "abstain_when": "输入不包含明确日期时间语义",
    },
    {
        "skill": "skill_delegate",
        "workflow_id": "skill-delegate-workflow",
        "title": "委托规划",
        "description": "生成步骤方案、计划和执行建议。",
        "handles": ["计划", "方案", "任务拆解", "执行编排"],
        "does_not_handle": ["纯事实问答", "日期计算", "搜索结果本身"],
        "abstain_when": "请求并不需要计划或协调",
    },
]
SKILL_META = {item["skill"]: item for item in SKILL_CATALOG}
ACTION_ORDER = {"done": 3, "start": 2, "waiting": 1, "skip": 0}
SUMMARY_INTENT = re.compile(r"总结|概括|摘要|提炼|复盘|整理|上条消息|上一条|总结一下|归纳")
SEARCH_INTENT = re.compile(r"搜索|检索|查询|查找|资料|背景|新闻|热点|信息|百科|来源|外部")
SEARCH_UNSUPPORTED = re.compile(r"天气|气温|降雨|降水|风力|台风|空气质量")
DATE_CORE = re.compile(r"几号|日期|时间|几点|几分|星期|周[一二三四五六日天]|本周|下周|上周|月|年|UTC")
DATE_RELATIVE = re.compile(r"今天|明天|后天|昨天|今晚|明早|现在")
DELEGATE_INTENT = re.compile(r"计划|方案|安排|执行|协调|分工|步骤|路线|委托|代理|拆解|排期")
GREETING_PATTERN = re.compile(
    r"^\s*(?:hi|hello|hey|你好|您好|嗨|哈喽|早上好|中午好|下午好|晚上好|在吗)[!！,.。?？\s]*$",
    re.IGNORECASE,
)
WEEKDAY = {
    "周一": 0,
    "星期一": 0,
    "周二": 1,
    "星期二": 1,
    "周三": 2,
    "星期三": 2,
    "周四": 3,
    "星期四": 3,
    "周五": 4,
    "星期五": 4,
    "周六": 5,
    "星期六": 5,
    "周日": 6,
    "星期日": 6,
    "周天": 6,
    "星期天": 6,
}
CN_NUM = {"一": 1, "二": 2, "三": 3, "四": 4, "五": 5, "六": 6}


def parse_json(value: str, default):
    value = (value or "").strip()
    if not value:
        return default
    try:
        return json.loads(value)
    except Exception:
        return default


def parse_jsonl(value: str) -> List[Dict]:
    value = (value or "").strip()
    if not value:
        return []
    parsed = parse_json(value, None)
    if isinstance(parsed, list):
        return [item for item in parsed if isinstance(item, dict)]
    if isinstance(parsed, dict):
        if isinstance(parsed.get("orchestration_plan"), list):
            return [item for item in parsed["orchestration_plan"] if isinstance(item, dict)]
        return [parsed]
    items: List[Dict] = []
    for raw in value.splitlines():
        raw = raw.strip()
        if not raw:
            continue
        try:
            item = json.loads(raw)
        except Exception:
            continue
        if isinstance(item, dict):
            items.append(item)
    return items


def dump_jsonl(items: List[Dict]) -> str:
    return "\n".join(json.dumps(item, ensure_ascii=False, separators=(",", ":")) for item in items)


def skill_catalog_json() -> str:
    return json.dumps(SKILL_CATALOG, ensure_ascii=False, separators=(",", ":"))


def detect_summary_intent(text: str) -> bool:
    return bool(SUMMARY_INTENT.search((text or "").strip()))


def detect_search_intent(text: str) -> bool:
    value = (text or "").strip()
    return bool(SEARCH_INTENT.search(value)) and not bool(SEARCH_UNSUPPORTED.search(value))


def detect_date_intent(text: str) -> bool:
    value = (text or "").strip()
    has_relative = bool(DATE_RELATIVE.search(value))
    has_core = bool(DATE_CORE.search(value))
    return has_core or (has_relative and has_core)


def detect_delegate_intent(text: str) -> bool:
    return bool(DELEGATE_INTENT.search((text or "").strip()))


def assess_search_query(text: str) -> Dict:
    value = (text or "").strip()
    if not value:
        return {"can_handle": False, "status": "unsupported", "grounded": False, "confidence": 0.0, "reason": "缺少待检索的问题。"}
    if SEARCH_UNSUPPORTED.search(value):
        return {
            "can_handle": False,
            "status": "unsupported",
            "grounded": False,
            "confidence": 0.0,
            "reason": "当前搜索技能未接入实时天气等在线数据源。",
        }
    if detect_search_intent(value):
        return {
            "can_handle": True,
            "status": "limited",
            "grounded": False,
            "confidence": 0.72,
            "reason": "该问题需要检索/背景信息，但当前示例仅提供有限的检索建议能力。",
        }
    return {"can_handle": False, "status": "unsupported", "grounded": False, "confidence": 0.0, "reason": "该问题不属于搜索检索技能范围。"}


def assess_summary_query(text: str) -> Dict:
    if detect_summary_intent(text):
        return {"can_handle": True, "status": "ok", "grounded": True, "confidence": 0.9, "reason": "该问题明确要求总结、概括或复盘已有内容。"}
    return {"can_handle": False, "status": "unsupported", "grounded": False, "confidence": 0.0, "reason": "该问题不属于总结归纳技能范围。"}


def assess_date_query(text: str) -> Dict:
    if detect_date_intent(text):
        return {"can_handle": True, "status": "ok", "grounded": True, "confidence": 0.92, "reason": "该问题包含明确的日期/时间语义。"}
    return {"can_handle": False, "status": "unsupported", "grounded": False, "confidence": 0.0, "reason": "该问题不属于日期时间技能范围。"}


def assess_delegate_query(text: str) -> Dict:
    if detect_delegate_intent(text):
        return {"can_handle": True, "status": "ok", "grounded": False, "confidence": 0.82, "reason": "该问题需要方案制定、任务拆解或协调建议。"}
    return {"can_handle": False, "status": "unsupported", "grounded": False, "confidence": 0.0, "reason": "该问题不属于委托规划技能范围。"}


def is_greeting(text: str) -> bool:
    return bool(GREETING_PATTERN.match((text or "").strip()))


def matched_skills(user_text: str) -> List[str]:
    text = user_text or ""
    matches: List[str] = []
    if detect_search_intent(text):
        matches.append("skill_search")
    if detect_summary_intent(text):
        matches.append("skill_summary")
    if detect_date_intent(text):
        matches.append("skill_date")
    if detect_delegate_intent(text):
        matches.append("skill_delegate")
    return matches


def assess_skill(skill: str, user_text: str) -> Dict:
    if skill == "skill_search":
        return assess_search_query(user_text)
    if skill == "skill_summary":
        return assess_summary_query(user_text)
    if skill == "skill_date":
        return assess_date_query(user_text)
    if skill == "skill_delegate":
        return assess_delegate_query(user_text)
    return {"can_handle": False, "status": "unsupported", "grounded": False, "confidence": 0.0, "reason": "未知技能。"}


def skill_from_llm(raw_text: str) -> List[str]:
    hits = []
    text = raw_text or ""
    for skill in SKILL_META:
        if skill in text:
            hits.append(skill)
    blob = parse_json(raw_text, None)
    if isinstance(blob, dict):
        candidate = blob.get("skills") or blob.get("selected_skills") or blob.get("items") or []
        if isinstance(candidate, list):
            for entry in candidate:
                if isinstance(entry, str) and entry in SKILL_META and entry not in hits:
                    hits.append(entry)
                elif isinstance(entry, dict):
                    skill = entry.get("skill")
                    if skill in SKILL_META and skill not in hits:
                        hits.append(skill)
    return hits


def base_scores(user_text: str) -> Dict[str, int]:
    text = user_text or ""
    scores = {skill: 10 for skill in SKILL_META}
    if detect_search_intent(text):
        scores["skill_search"] += 60
    if detect_summary_intent(text):
        scores["skill_summary"] += 65
    if detect_date_intent(text):
        scores["skill_date"] += 70
    if detect_delegate_intent(text):
        scores["skill_delegate"] += 55
    return scores


def apply_dispatcher_path(scores: Dict[str, int], path_name: str) -> None:
    if path_name == "semantic":
        scores["skill_search"] += 10
        scores["skill_summary"] += 10
    elif path_name == "temporal":
        scores["skill_date"] += 25
        scores["skill_search"] += 5
    elif path_name == "planner":
        scores["skill_delegate"] += 20
        scores["skill_summary"] += 15


def choose_dispatcher_items(session_id: str, user_text: str, path_name: str, llm_text: str) -> List[Dict]:
    if is_greeting(user_text):
        return []
    scores = base_scores(user_text)
    direct_matches = set(matched_skills(user_text))
    apply_dispatcher_path(scores, path_name)
    for skill in skill_from_llm(llm_text):
        if not direct_matches or skill in direct_matches:
            scores[skill] += 35
    ordered = sorted(scores.items(), key=lambda item: item[1], reverse=True)
    chosen = [item for item in ordered if item[1] >= 55 and (not direct_matches or item[0] in direct_matches)][:3]
    if not chosen:
        if direct_matches:
            chosen = [item for item in ordered if item[0] in direct_matches][:1]
        elif ordered and ordered[0][1] >= 75:
            chosen = ordered[:1]
    items = []
    for rank, (skill, score) in enumerate(chosen, start=1):
        assessment = assess_skill(skill, user_text)
        if not assessment.get("can_handle"):
            continue
        meta = SKILL_META[skill]
        items.append(
            {
                "session_id": session_id,
                "skill": skill,
                "workflow_id": meta["workflow_id"],
                "priority": score,
                "status": "dispatch_proposed",
                "reason": assessment.get("reason") or f"{path_name}路径认为该技能可处理当前请求",
                "source_path": path_name,
                "rank": rank,
            }
        )
    return items


def merge_dispatcher_lists(session_id: str, blobs: List[str]) -> List[Dict]:
    merged: Dict[str, Dict] = {}
    for blob in blobs:
        for item in parse_jsonl(blob):
            skill = item.get("skill")
            if skill not in SKILL_META:
                continue
            current = merged.get(skill)
            if current is None or int(item.get("priority", 0)) > int(current.get("priority", 0)):
                merged[skill] = dict(item)
            else:
                current["reason"] = current.get("reason", "") + "；" + item.get("reason", "")
                current["source_path"] = current.get("source_path", "") + "," + item.get("source_path", "")
    ordered = sorted(merged.values(), key=lambda item: int(item.get("priority", 0)), reverse=True)
    for idx, item in enumerate(ordered, start=1):
        item["session_id"] = item.get("session_id") or session_id
        item["rank"] = idx
        item["status"] = "dispatch_selected"
    return ordered


def choose_coordinator_items(session_id: str, path_name: str, selected_blob: str, partial_blob: str, llm_text: str) -> List[Dict]:
    selected = parse_jsonl(selected_blob)
    partial = {
        item.get("skill"): item
        for item in parse_jsonl(partial_blob)
        if item.get("skill") and item.get("can_handle") and item.get("status") != "unsupported"
    }
    llm_hits = set(skill_from_llm(llm_text))
    decisions: List[Dict] = []
    started = 0
    for idx, item in enumerate(sorted(selected, key=lambda ent: int(ent.get("priority", 0)), reverse=True), start=1):
        skill = item.get("skill")
        if skill not in SKILL_META:
            continue
        action = "waiting"
        reason = f"{path_name}路径暂未建议立即执行"
        if skill in partial:
            action = "done"
            reason = f"{path_name}路径识别到该技能已有执行结果"
        elif path_name == "result_aware" and partial:
            if skill == "skill_summary" or skill in llm_hits:
                action = "start"
                reason = "已有部分结果，建议进入总结/整合阶段"
        elif path_name == "dependency":
            if skill in ("skill_search", "skill_date") and started == 0:
                action = "start"
                reason = "该技能可直接提供基础事实或时间信息"
                started += 1
            elif skill == "skill_summary" and partial:
                action = "start"
                reason = "已有结果可供总结"
        elif path_name == "finalizer":
            if idx <= 2 or skill in llm_hits:
                action = "start"
                reason = "最终编排路径建议优先执行该技能"
        if skill in llm_hits and action == "waiting":
            action = "start"
            reason = "模型输出显式提到了该技能"
        decisions.append(
            {
                "session_id": item.get("session_id") or session_id,
                "skill": skill,
                "workflow_id": item.get("workflow_id") or SKILL_META[skill]["workflow_id"],
                "priority": int(item.get("priority", 0)),
                "status": action,
                "reason": reason,
                "source_path": path_name,
                "rank": idx,
            }
        )
    return decisions


def merge_coordinator_lists(session_id: str, selected_blob: str, partial_blob: str, blobs: List[str]) -> Dict:
    selected = parse_jsonl(selected_blob)
    partial_items = [item for item in parse_jsonl(partial_blob) if item.get("skill")]
    partial = {
        item.get("skill"): item for item in partial_items if item.get("can_handle") and item.get("status") != "unsupported"
    }
    merged: Dict[str, Dict] = {}
    for blob in blobs:
        for item in parse_jsonl(blob):
            skill = item.get("skill")
            if skill not in SKILL_META:
                continue
            current = merged.get(skill)
            if current is None:
                merged[skill] = dict(item)
                continue
            lhs = ACTION_ORDER.get(item.get("status", "waiting"), 0)
            rhs = ACTION_ORDER.get(current.get("status", "waiting"), 0)
            if lhs > rhs or (lhs == rhs and int(item.get("priority", 0)) > int(current.get("priority", 0))):
                merged[skill] = dict(item)
            else:
                current["reason"] = current.get("reason", "") + "；" + item.get("reason", "")
    for item in selected:
        skill = item.get("skill")
        if skill in SKILL_META and skill not in merged:
            merged[skill] = {
                "session_id": item.get("session_id") or session_id,
                "skill": skill,
                "workflow_id": item.get("workflow_id") or SKILL_META[skill]["workflow_id"],
                "priority": int(item.get("priority", 0)),
                "status": "waiting",
                "reason": "保留 dispatcher 选择结果，等待后续执行。",
                "rank": int(item.get("rank", 0)),
            }
    ordered = sorted(
        merged.values(),
        key=lambda item: (ACTION_ORDER.get(item.get("status", "waiting"), 0), int(item.get("priority", 0))),
        reverse=True,
    )
    plan = []
    for idx, item in enumerate(ordered, start=1):
        status = item.get("status", "waiting")
        action = "waiting"
        if status == "done":
            action = "done"
        elif status == "start":
            action = "start"
        elif status == "skip":
            action = "skip"
        plan.append(
            {
                "task_id": f"{session_id}_{item['skill']}_{idx}",
                "session_id": item.get("session_id") or session_id,
                "skill": item["skill"],
                "workflow_id": item.get("workflow_id") or SKILL_META[item["skill"]]["workflow_id"],
                "action": action,
                "status": status,
                "priority": int(item.get("priority", 0)),
                "reason": item.get("reason", ""),
                "rank": idx,
            }
        )
    final_response = ""
    if partial_items:
        summary_results = []
        fragments = []
        limitation_reasons = []
        for item in partial_items:
            skill = item.get("skill", "")
            result = item.get("result", "")
            can_handle = bool(item.get("can_handle"))
            status = item.get("status", "")
            grounded = bool(item.get("grounded"))
            if not can_handle or status == "unsupported":
                if item.get("reason"):
                    limitation_reasons.append(item["reason"])
                continue
            if skill == "skill_summary":
                summary_results.append(result)
            if not result:
                continue
            title = SKILL_META.get(skill, {}).get("title", skill)
            prefix = f"【{title}】"
            fragments.append(prefix + result if grounded else prefix + "有限支持：" + result)
        if summary_results:
            final_response = summary_results[-1]
        elif len(fragments) == 1:
            final_response = fragments[0].split("】", 1)[-1]
        elif fragments:
            final_response = "我结合当前可用技能结果给出回复：" + "；".join(fragments)
        elif limitation_reasons:
            final_response = "当前可用技能暂不支持直接回答该问题：" + limitation_reasons[-1]
    return {
        "selected_skills": dump_jsonl(plan),
        "orchestration_plan": json.dumps({"orchestration_plan": plan}, ensure_ascii=False, separators=(",", ":")),
        "final_response": final_response,
        "coordinator_done": True,
    }


def build_search_result(query: str) -> Dict:
    assessment = assess_search_query(query)
    result = ""
    if assessment["can_handle"]:
        if "新闻" in query or "热点" in query:
            result = f"可执行有限检索支持：建议优先查看近 7 天内与“{query}”相关的权威新闻摘要。"
        else:
            result = f"可执行有限检索支持：建议围绕“{query}”补充权威来源、时间范围和事实核验。"
    return {
        "search_result": result,
        "answer_text": result,
        "can_handle": assessment["can_handle"],
        "status": assessment["status"],
        "grounded": assessment["grounded"],
        "confidence": assessment["confidence"],
        "reason": assessment["reason"],
    }


def build_summary_result(text: str, partial_blob: str, history_json: str) -> Dict:
    assessment = assess_summary_query(text)
    result = ""
    if assessment["can_handle"] and partial_blob.strip():
        lines = []
        for item in parse_jsonl(partial_blob):
            if item.get("can_handle") and item.get("result"):
                lines.append(item["result"])
        if lines:
            result = "已完成阶段性总结：" + "；".join(lines[:3])
    if assessment["can_handle"] and not result and history_json.strip():
        history = parse_json(history_json, [])
        contents = []
        for item in history:
            if isinstance(item, dict):
                content = (item.get("content") or "").strip()
                if content and content != text:
                    contents.append(content)
        if contents:
            result = f"上条消息主要内容是：{contents[-1]}"
    return {
        "summary_result": result,
        "answer_text": result,
        "can_handle": assessment["can_handle"] and bool(result),
        "status": "ok" if result else "unsupported",
        "grounded": bool(result),
        "confidence": 0.9 if result else 0.0,
        "reason": assessment["reason"] if not result else "总结技能已基于历史或中间结果生成摘要。",
    }


def find_weekday(text: str):
    for key, value in WEEKDAY.items():
        if key in text:
            return value, key
    return None, ""


def answer_for_relative_week(text: str):
    if ("周" not in text and "星期" not in text) or ("几号" not in text and "几月几号" not in text):
        return None
    matched = re.search(r"(本|下+)周", text)
    if not matched:
        return None
    token = matched.group(1)
    weeks_ahead = 0 if token == "本" else len(token)
    weekday, weekday_text = find_weekday(text)
    if weekday is None:
        return None
    today = dt.date.today()
    monday = today - dt.timedelta(days=today.weekday())
    target = monday + dt.timedelta(days=7 * weeks_ahead + weekday)
    prefix = "本周" if weeks_ahead == 0 else ("下" * weeks_ahead + "周")
    return {
        "matched": True,
        "date": target.isoformat(),
        "answer": f"{prefix}{weekday_text[-1]}是{target.month}月{target.day}日（周{['一', '二', '三', '四', '五', '六', '日'][weekday]}）。",
    }


def answer_for_month_nth_week(text: str):
    if "第" not in text or "周" not in text or ("几号" not in text and "几月几号" not in text):
        return None
    weekday, weekday_text = find_weekday(text)
    if weekday is None:
        return None
    matched = re.search(r"(\d{1,2})月第([一二三四五六\d])周", text)
    if not matched:
        return None
    month = int(matched.group(1))
    nth_raw = matched.group(2)
    nth = int(nth_raw) if nth_raw.isdigit() else CN_NUM.get(nth_raw)
    if not nth or month < 1 or month > 12:
        return None
    today = dt.date.today()
    cal = calendar.Calendar(firstweekday=0)

    def resolve(year: int):
        weeks = cal.monthdayscalendar(year, month)
        count = 0
        for week in weeks:
            if week[weekday] != 0:
                count += 1
                if count == nth:
                    return dt.date(year, month, week[weekday])
        return None

    target = resolve(today.year)
    if target is None or target < today:
        target = resolve(today.year + 1)
    if target is None:
        return None
    return {
        "matched": True,
        "date": target.isoformat(),
        "answer": f"{month}月第{nth}周{weekday_text}是{target.month}月{target.day}日（周{['一', '二', '三', '四', '五', '六', '日'][weekday]}）。",
    }


def parse_date_query(text: str):
    for handler in (answer_for_month_nth_week, answer_for_relative_week):
        result = handler(text)
        if result:
            return result
    return None


def current_date_answer() -> str:
    now = dt.datetime.now(dt.timezone.utc)
    today = dt.date.today()
    weekday_names = ["一", "二", "三", "四", "五", "六", "日"]
    now_text = now.isoformat(timespec="seconds").replace("+00:00", "Z")
    return f"当前UTC时间：{now_text}；本地日期：{today.isoformat()}（周{weekday_names[today.weekday()]}）；7天后：{(today + dt.timedelta(days=7)).isoformat()}。"


def build_date_result(text: str) -> Dict:
    assessment = assess_date_query(text)
    answer = ""
    parsed = parse_date_query(text) if text else None
    if parsed and parsed.get("matched"):
        answer = parsed["answer"]
    if not answer and assessment["can_handle"]:
        answer = current_date_answer()
    return {
        "date_result": answer,
        "answer_text": answer,
        "status": "ok" if answer else assessment["status"],
        "can_handle": bool(answer),
        "grounded": bool(answer),
        "confidence": assessment["confidence"] if answer else 0.0,
        "reason": "日期时间技能已生成结果。" if answer else assessment["reason"],
    }


def build_delegate_result(text: str) -> Dict:
    assessment = assess_delegate_query(text)
    result = ""
    if assessment["can_handle"]:
        result = "建议按以下步骤推进：1）澄清目标与约束；2）拆分关键任务；3）按优先级执行并在每一步复盘风险。"
    return {
        "delegate_result": result,
        "answer_text": result,
        "can_handle": bool(result),
        "status": "ok" if result else assessment["status"],
        "grounded": False,
        "confidence": assessment["confidence"] if result else 0.0,
        "reason": "委托规划技能已生成执行建议。" if result else assessment["reason"],
    }
