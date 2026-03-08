#!/usr/bin/env python3
import json
import re
import sys
from typing import Dict, List

from skill_contracts import (
    assess_date_query,
    assess_delegate_query,
    assess_search_query,
    assess_summary_query,
    detect_date_intent,
    detect_delegate_intent,
    detect_search_intent,
    detect_summary_intent,
)

SKILL_CATALOG = [
    {
        "skill": "skill_search",
        "workflow": "skill-search-workflow",
        "title": "搜索检索",
        "description": "检索最近信息、事实线索和外部背景，适合新闻、热点、资料搜集类问题。",
    },
    {
        "skill": "skill_summary",
        "workflow": "skill-summary-workflow",
        "title": "总结归纳",
        "description": "整理已有信息并输出结构化总结，适合复述、概览、提炼要点类问题。",
    },
    {
        "skill": "skill_date",
        "workflow": "skill-date-workflow",
        "title": "日期时间",
        "description": "解析相对日期、星期、时间安排等信息，适合时间推理相关问题。",
    },
    {
        "skill": "skill_delegate",
        "workflow": "skill-delegate-workflow",
        "title": "委托规划",
        "description": "给出执行方案、步骤编排和多任务协调建议，适合计划、分解、方案制定类问题。",
    },
]
SKILL_META = {item["skill"]: item for item in SKILL_CATALOG}
ACTION_ORDER = {"done": 3, "start": 2, "waiting": 1, "skip": 0}
GREETING_PATTERN = re.compile(
    r"^\s*(?:hi|hello|hey|你好|您好|嗨|哈喽|早上好|中午好|下午好|晚上好|在吗)[!！,.。?？\s]*$",
    re.IGNORECASE)


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


def output(data: Dict):
    print(json.dumps(data, ensure_ascii=False))


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
    return {
        "can_handle": False,
        "status": "unsupported",
        "grounded": False,
        "confidence": 0.0,
        "reason": "未知技能。",
    }


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


def apply_dispatcher_path(scores: Dict[str, int], path_name: str):
    if path_name == "semantic":
        scores["skill_search"] += 10
        scores["skill_summary"] += 10
    elif path_name == "temporal":
        scores["skill_date"] += 25
        scores["skill_search"] += 5
    elif path_name == "planner":
        scores["skill_delegate"] += 20
        scores["skill_summary"] += 15


def choose_dispatcher_items(session_id: str, user_text: str, path_name: str,
                            llm_text: str) -> List[Dict]:
    if is_greeting(user_text):
        return []
    scores = base_scores(user_text)
    direct_matches = set(matched_skills(user_text))
    apply_dispatcher_path(scores, path_name)
    for skill in skill_from_llm(llm_text):
        if not direct_matches or skill in direct_matches:
            scores[skill] += 35
    ordered = sorted(scores.items(), key=lambda item: item[1], reverse=True)
    chosen = [item for item in ordered if item[1] >= 55 and
              (not direct_matches or item[0] in direct_matches)][:3]
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
        items.append({
            "session_id": session_id,
            "skill": skill,
            "workflow_id": meta["workflow"],
            "priority": score,
            "status": "dispatch_proposed",
            "reason": assessment.get("reason") or f"{path_name}路径认为该技能可处理当前请求",
            "source_path": path_name,
            "rank": rank,
        })
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
                current["source_path"] = current.get("source_path", "") + "," + item.get(
                    "source_path", "")
    ordered = sorted(merged.values(), key=lambda item: int(item.get("priority", 0)), reverse=True)
    for idx, item in enumerate(ordered, start=1):
        item["session_id"] = item.get("session_id") or session_id
        item["rank"] = idx
        item["status"] = "dispatch_selected"
    return ordered


def choose_coordinator_items(session_id: str, path_name: str, selected_blob: str,
                             partial_blob: str, llm_text: str) -> List[Dict]:
    selected = parse_jsonl(selected_blob)
    partial = {
        item.get("skill"): item for item in parse_jsonl(partial_blob)
        if item.get("skill") and item.get("can_handle") and item.get("status") != "unsupported"
    }
    llm_hits = set(skill_from_llm(llm_text))
    decisions: List[Dict] = []
    started = 0
    for idx, item in enumerate(
            sorted(selected, key=lambda ent: int(ent.get("priority", 0)), reverse=True), start=1):
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
        decisions.append({
            "session_id": item.get("session_id") or session_id,
            "skill": skill,
            "workflow_id": item.get("workflow_id") or SKILL_META[skill]["workflow"],
            "priority": int(item.get("priority", 0)),
            "status": action,
            "reason": reason,
            "source_path": path_name,
            "rank": idx,
        })
    return decisions


def merge_coordinator_lists(session_id: str, selected_blob: str, partial_blob: str,
                            blobs: List[str]) -> Dict:
    selected = parse_jsonl(selected_blob)
    partial_items = [
        item for item in parse_jsonl(partial_blob)
        if item.get("skill")
    ]
    partial = {
        item.get("skill"): item for item in partial_items
        if item.get("can_handle") and item.get("status") != "unsupported"
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
            if lhs > rhs or (lhs == rhs
                             and int(item.get("priority", 0)) > int(current.get("priority", 0))):
                merged[skill] = dict(item)
            else:
                current["reason"] = current.get("reason", "") + "；" + item.get("reason", "")
    for item in selected:
        skill = item.get("skill")
        if skill in SKILL_META and skill not in merged:
            merged[skill] = {
                "session_id": item.get("session_id") or session_id,
                "skill": skill,
                "workflow_id": item.get("workflow_id") or SKILL_META[skill]["workflow"],
                "priority": int(item.get("priority", 0)),
                "status": "waiting",
                "reason": "未被任一路径提升，先保持等待",
                "source_path": "fallback",
            }
    ordered = sorted(merged.values(),
                     key=lambda item: (ACTION_ORDER.get(item.get("status", "waiting"), 0),
                                       int(item.get("priority", 0))),
                     reverse=True)
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
        plan.append({
            "task_id": f"{session_id}_{item['skill']}_{idx}",
            "session_id": item.get("session_id") or session_id,
            "skill": item["skill"],
            "workflow_id": item.get("workflow_id") or SKILL_META[item["skill"]]["workflow"],
            "action": action,
            "status": status,
            "priority": int(item.get("priority", 0)),
            "reason": item.get("reason", ""),
            "rank": idx,
        })
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
            if grounded:
                fragments.append(prefix + result)
            else:
                fragments.append(prefix + "有限支持：" + result)
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
        "orchestration_plan": json.dumps({"orchestration_plan": plan},
                                         ensure_ascii=False,
                                         separators=(",", ":")),
        "final_response": final_response,
    }


def main(argv: List[str]):
    if len(argv) < 2:
        raise SystemExit("usage: skill_plan_tool.py <command> ...")
    cmd = argv[1]
    if cmd == "dispatcher-path":
        _, _, session_id, user_text, path_name, llm_text = argv
        output({
            "candidate_list":
            dump_jsonl(choose_dispatcher_items(session_id, user_text, path_name, llm_text))
        })
        return
    if cmd == "merge-dispatcher":
        session_id = argv[2]
        output({
            "selected_skills": dump_jsonl(merge_dispatcher_lists(session_id, argv[3:])),
            "dispatcher_done": True
        })
        return
    if cmd == "coordinator-path":
        _, _, session_id, path_name, selected_blob, partial_blob, llm_text = argv
        output({
            "candidate_list":
            dump_jsonl(
                choose_coordinator_items(session_id, path_name, selected_blob, partial_blob,
                                         llm_text))
        })
        return
    if cmd == "merge-coordinator":
        session_id, selected_blob, partial_blob = argv[2], argv[3], argv[4]
        merged = merge_coordinator_lists(session_id, selected_blob, partial_blob, argv[5:])
        merged["coordinator_done"] = True
        output(merged)
        return
    raise SystemExit(f"unknown command: {cmd}")


if __name__ == "__main__":
    main(sys.argv)
