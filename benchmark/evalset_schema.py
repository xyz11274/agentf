#!/usr/bin/env python3
"""evalset.v2 统一评测集 schema 定义与校验。

设计目标：归一化三份既有评测数据——
  1. trace_output/v1-l4-coldstart/dataset（trajectory_evolution JSONL，单轮任务）
  2. test_cases/product/*.xlsx（产品多轮多意图用例）
  3. examples/agent/daily-qa-assistant/evals/evalset.json（多轮对话用例）

统一后的 case 为三者的超集；单轮任务视为 turns 长度为 1 的特例。
意图 ground truth 统一为 skill 集合粒度（expected_skills，[] 表示不应 dispatch）。

零第三方依赖（标准库），Python 3.8+。
"""

from __future__ import annotations

SCHEMA_VERSION = "evalset.v2"

# case 必填字段
_CASE_REQUIRED = ("id", "category", "turns")
# turn 必填字段
_TURN_REQUIRED = ("user",)

_VALID_DIFFICULTY = {"easy", "medium", "hard", ""}
_VALID_CHECK_OPS = {"action_count"}


def empty_evalset(name: str = "", description: str = "") -> dict:
    return {
        "name": name,
        "version": "1.0.0",
        "schema_version": SCHEMA_VERSION,
        "description": description,
        "skill_names": [],
        "categories": [],
        "cases": [],
    }


def normalize_turn(turn: dict) -> dict:
    """把单个 turn 归一到 evalset.v2 结构（补默认值，兼容别名）。"""
    # expected_skills 语义：null = 无意图 ground truth（coldstart，意图指标跳过、不进分母）；
    # [] = 负例（不应 dispatch 任何 skill）；非空数组 = 期望 dispatch 的 skill 集合。
    skills = turn.get("expected_skills")
    out = {
        "user": turn.get("user", ""),
        "expected_skills": sorted(skills) if skills is not None else None,
        # 判分标准：coldstart expected_behavior / product 预期结果 / daily-qa rubric 统一入此字段
        "expected_behavior": turn.get("expected_behavior") or turn.get("rubric") or "",
        "must_include": list(turn.get("must_include") or []),
        "must_not_include": list(turn.get("must_not_include") or []),
    }
    for key in ("expected_operation_types", "expected_intents"):
        if key in turn:
            values = turn[key]
            out[key] = list(values) if values is not None else None
    return out


def normalize_case(case: dict) -> dict:
    """把单个 case 归一到 evalset.v2 结构。"""
    out = {
        "id": case["id"],
        "category": case.get("category", "uncategorized"),
        "difficulty": case.get("difficulty", ""),
        "goal": case.get("goal", ""),
        "constraints": list(case.get("constraints") or []),
        "turns": [normalize_turn(t) for t in case.get("turns") or []],
    }
    if case.get("budget"):
        out["budget"] = {
            k: case["budget"][k] for k in ("max_steps", "max_tool_calls", "timeout_ms")
            if k in case["budget"]
        }
    if case.get("checks"):
        out["checks"] = [dict(c) for c in case["checks"]]
    if case.get("final_rubric"):
        out["final_rubric"] = case["final_rubric"]
    if case.get("split"):
        out["split"] = case["split"]
    # 仅迁移用例元数据；显式新字段优先，不影响进化等级或难度。
    if "case_level" in case or "level" in case:
        out["case_level"] = case.get("case_level", case.get("level"))
    if case.get("provenance"):
        out["provenance"] = dict(case["provenance"])
    return out


def normalize_evalset(data: dict) -> dict:
    """归一整个 evalset：case/turn 补默认值，categories/skill_names 去重排序。"""
    out = empty_evalset(data.get("name", ""), data.get("description", ""))
    out["version"] = data.get("version", "1.0.0")
    out["skill_names"] = sorted(set(data.get("skill_names") or []))
    cases = [normalize_case(c) for c in data.get("cases") or []]
    out["cases"] = cases
    declared = set(data.get("categories") or [])
    declared.update(c["category"] for c in cases)
    out["categories"] = sorted(declared)
    return out


def validate_evalset(data: dict) -> list:
    """schema 与 ground truth 校验，返回错误列表（空 = 通过）。

    与 daily-qa 旧校验的差异：category 不再限定固定枚举（三来源类目不同），
    不再要求 ≥3 轮（coldstart 单轮合法），expected_skills 允许缺省（coldstart
    无意图 ground truth，意图指标对其跳过、不计入分母）。
    """
    errors = []
    if not isinstance(data, dict):
        return ["evalset 必须是 JSON 对象"]
    skill_names = set(data.get("skill_names") or [])
    declared_categories = set(data.get("categories") or [])
    cases = data.get("cases")
    if not isinstance(cases, list) or not cases:
        errors.append("cases 缺失或为空")
        return errors

    ids = set()
    for i, case in enumerate(cases):
        cid = case.get("id", f"<index-{i}>")
        for field in _CASE_REQUIRED:
            if not case.get(field):
                errors.append(f"{cid}: 缺少必填字段 '{field}'")
        if case.get("id") in ids:
            errors.append(f"{cid}: case id 重复")
        ids.add(case.get("id"))

        difficulty = case.get("difficulty", "")
        if difficulty not in _VALID_DIFFICULTY:
            errors.append(f"{cid}: difficulty 非法 → {difficulty!r}（可选 easy/medium/hard）")
        if declared_categories and case.get("category") not in declared_categories:
            errors.append(f"{cid}: category '{case.get('category')}' 不在 categories 声明中")

        for j, turn in enumerate(case.get("turns") or [], 1):
            for field in _TURN_REQUIRED:
                if not turn.get(field):
                    errors.append(f"{cid} 第{j}轮: 缺少必填字段 '{field}'")
            skills = turn.get("expected_skills")
            if skills is not None:
                if not isinstance(skills, list):
                    errors.append(f"{cid} 第{j}轮: expected_skills 必须是数组")
                elif skill_names:
                    for skill in skills:
                        if skill not in skill_names:
                            errors.append(f"{cid} 第{j}轮: 未知 skill '{skill}'（不在 skill_names 中）")
            # expected_behavior 允许为空（product 约 1/3 用例无预期结果列），
            # 此时 judge 按通用质量标准评分；既无判分标准又无意图 GT 的轮次
            # 由 count_unscorable_turns 统计为告警，不阻塞校验。

        for check in case.get("checks") or []:
            if check.get("op") not in _VALID_CHECK_OPS:
                errors.append(f"{cid}: 未知 check op '{check.get('op')}'（支持 {sorted(_VALID_CHECK_OPS)}）")
            if check.get("op") == "action_count":
                if not check.get("operation"):
                    errors.append(f"{cid}: action_count 缺少 operation")
                if check.get("min") is None and check.get("max") is None:
                    errors.append(f"{cid}: action_count 的 min/max 至少填写一个")
    return errors


def count_unscorable_turns(data: dict) -> int:
    """统计既无 expected_behavior 又无意图 ground truth 的轮次数（只能通用评分）。"""
    n = 0
    for case in data.get("cases") or []:
        for turn in case.get("turns") or []:
            if not (turn.get("expected_behavior") or turn.get("rubric")) \
                    and turn.get("expected_skills") is None:
                n += 1
    return n
