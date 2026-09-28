#!/usr/bin/env python3
"""数据集格式转换器：三份既有评测数据 → evalset.v2。

支持输入（按扩展名/内容自动识别）：
  - .jsonl  coldstart（trajectory_evolution dataset，每行一个单轮 task）
  - .xlsx   product 用例（测试步骤/预期结果/预期意图 编号语料，stdlib zipfile 解析）
  - .json   daily-qa evalset（或已是 evalset.v2 的文件，幂等归一）
  - .json   hotpotqa 原始样本列表（含 question/answer/context，自动识别）

用法：
  python3 converters.py INPUT [INPUT...] -o OUT.json [--map domain_skill_map.json]

多个输入合并为一份 evalset.v2：cases 拼接、skill_names 取并集、id 冲突时加来源前缀。
零第三方依赖（标准库），Python 3.8+。
"""

from __future__ import annotations

import argparse
import json
import re
import sys
import zipfile
import xml.etree.ElementTree as ET
from pathlib import Path

from evalset_schema import SCHEMA_VERSION, empty_evalset, normalize_evalset

_M = "{http://schemas.openxmlformats.org/spreadsheetml/2006/main}"

# 原表用例的智能等级 → difficulty；与进化能力 L0–L4 无关。
_LEVEL_TO_DIFFICULTY = {"L1": "easy", "L2": "medium", "L3": "hard"}


# ── xlsx 读取（stdlib，仅取第一个 sheet） ─────────────────────────

def read_xlsx_rows(path: Path) -> list:
    """返回 [header, row1, row2, ...]，每行是字符串列表（按列对齐）。"""
    z = zipfile.ZipFile(path)
    shared = []
    if "xl/sharedStrings.xml" in z.namelist():
        root = ET.fromstring(z.read("xl/sharedStrings.xml"))
        for si in root.findall(f"{_M}si"):
            shared.append("".join(t.text or "" for t in si.iter(f"{_M}t")))
    sheets = sorted(n for n in z.namelist() if re.fullmatch(r"xl/worksheets/sheet\d+\.xml", n))
    if not sheets:
        return []
    root = ET.fromstring(z.read(sheets[0]))
    rows = []
    for row in root.iter(f"{_M}row"):
        cells = {}
        max_col = -1
        for c in row.findall(f"{_M}c"):
            ref = c.get("r", "")
            col = 0
            for ch in ref:
                if ch.isalpha():
                    col = col * 26 + (ord(ch.upper()) - 64)
                else:
                    break
            col -= 1
            v = c.find(f"{_M}v")
            inline = c.find(f"{_M}is")
            if v is not None and v.text is not None:
                text = shared[int(v.text)] if c.get("t") == "s" else v.text
            elif inline is not None:
                text = "".join(t.text or "" for t in inline.iter(f"{_M}t"))
            else:
                text = ""
            cells[col] = text
            max_col = max(max_col, col)
        rows.append([cells.get(i, "") for i in range(max_col + 1)])
    return rows


# ── 编号语料拆分 ──────────────────────────────────────────────────

_STEP_MARK = re.compile(r"(?<!\d)(\d{1,2})\s*[.、．](?!\d)\s*")


def split_steps(text: str) -> list:
    """把 '1. xxx\\n2. yyy' 或行内 '1. xxx。2. yyy' 形式的编号语料拆成步骤列表。

    无明显编号时整体作为单步返回。
    """
    text = (text or "").strip()
    if not text:
        return []
    candidates = list(_STEP_MARK.finditer(text))
    # 接受无换行的连续编号，但排除小数、流量规格等普通数字。
    if not candidates or candidates[0].group(1) != "1" or text[:candidates[0].start()].strip(" `\n\r\t"):
        return [text]
    marks = [candidates[0]]
    for candidate in candidates[1:]:
        if int(candidate.group(1)) == len(marks) + 1:
            marks.append(candidate)
    steps = []
    for i, m in enumerate(marks):
        end = marks[i + 1].start() if i + 1 < len(marks) else len(text)
        step = text[m.end():end].strip().strip("`").strip()
        # 空预期也占据一轮，不能删除后让后续答案错位。
        steps.append(step)
    return steps


# 编号后的空白只允许空格/制表符，不能吞掉换行并把下一轮编号当成标签。
_INTENT_LINE = re.compile(r"(\d{1,2})\s*[.、．][ \t]*([^\n`]*)")


def parse_intents(text: str) -> dict:
    """解析 '预期意图' 列：'1.QA,观影\\n2.QA' → {1: ['QA','观影'], 2: ['QA']}。

    某轮编号后为空（如 '1.'）→ 该轮无意图标注（None 语义，由调用方处理）。
    """
    intents = {}
    for m in _INTENT_LINE.finditer(text or ""):
        turn_no = int(m.group(1))
        raw = m.group(2).strip()
        domains = [d.strip() for d in re.split(r"[,，、/]+", raw) if d.strip()]
        intents[turn_no] = domains
    return intents


# ── coldstart JSONL → evalset.v2 ──────────────────────────────────

def convert_coldstart(path: Path) -> dict:
    evalset = empty_evalset(
        name=f"coldstart:{path.parent.name}",
        description=f"trajectory_evolution coldstart 数据集转换（来源 {path}）",
    )
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            task = json.loads(line)
            case = {
                "id": task["task_id"],
                "category": task.get("category", "uncategorized"),
                "difficulty": task.get("difficulty", ""),
                "goal": "",
                "constraints": task.get("constraints") or [],
                "turns": [{
                    "user": task.get("task_input", ""),
                    "expected_skills": None,  # coldstart 无意图 ground truth
                    "expected_behavior": task.get("expected_behavior", ""),
                }],
            }
            if task.get("budget"):
                case["budget"] = task["budget"]
            if task.get("split"):
                case["split"] = task["split"]
            case["provenance"] = {
                "kind": "synthetic",
                "source": str(path),
                "task_version": task.get("task_version", ""),
                "group_id": task.get("group_id", ""),
                "review_status": task.get("review_status", ""),
                **(task.get("provenance") or {}),
            }
            evalset["cases"].append(case)
    return evalset


# ── product xlsx → evalset.v2 ─────────────────────────────────────

def convert_product(path: Path, domain_map: dict, warnings: list) -> dict:
    rows = read_xlsx_rows(path)
    evalset = empty_evalset(
        name=f"product:{path.stem}",
        description=f"产品用例转换（来源 {path}）",
    )
    if len(rows) < 2:
        return evalset
    header = [h.strip() for h in rows[0]]

    def col(name):
        return header.index(name) if name in header else None

    c_id = col("用例编号")
    c_title = col("用例标题")
    c_pre = col("预置条件（可选）")
    c_scenes = [col(f"S{i}场景") for i in range(1, 4)] + [col("S4场景（可选）")]
    c_level = col("智能等级")
    c_steps = col("测试步骤（语料）")
    c_expect = col("预期结果")
    c_intent = col("预期意图")
    c_operation = col("预期操作类型")
    c_voice = col("声纹ID")
    c_device = col("设备类型")
    if c_id is None or c_steps is None:
        warnings.append(f"{path.name}: 缺少 用例编号/测试步骤（语料） 列，跳过")
        return evalset

    skill_names = set()
    for row in rows[1:]:
        def cell(idx):
            return row[idx].strip() if idx is not None and idx < len(row) else ""

        case_id = cell(c_id)
        if not case_id:
            continue
        steps = split_steps(cell(c_steps))
        if not steps:
            warnings.append(f"{case_id}: 测试步骤为空，跳过")
            continue
        expects = split_steps(cell(c_expect))
        intents = parse_intents(cell(c_intent))
        operations = parse_intents(cell(c_operation))

        scenarios = [cell(i) for i in c_scenes if i is not None and cell(i)]
        level = cell(c_level)
        title = cell(c_title)
        precondition = cell(c_pre)

        turns = []
        for idx, step in enumerate(steps, 1):
            domains = intents.get(idx)  # None = 该轮无意图标注
            if not domains:
                # xlsx 中 '1.' 空标注 = 未标注（不是负例），置 None 避免被计入负例准确率
                expected_skills = None
            else:
                expected_skills = []
                for d in domains:
                    if d in domain_map:
                        expected_skills.extend(domain_map[d])
                        skill_names.update(domain_map[d])
                    else:
                        expected_skills.append(d)  # 未映射垂域原样保留
                        skill_names.add(d)
                        warnings.append(f"{case_id} 第{idx}轮: 垂域 '{d}' 未在映射表中，原样保留为 skill 名")
            turns.append({
                "user": step,
                "expected_skills": sorted(set(expected_skills)) if expected_skills is not None else None,
                "expected_behavior": expects[idx - 1] if idx - 1 < len(expects) else "",
                "expected_intents": domains or None,
                "expected_operation_types": operations.get(idx),
            })

        goal = title
        if precondition and precondition != "无":
            goal = f"{title}（预置条件：{precondition}）"
        case = {
            "id": case_id,
            "category": scenarios[0] if scenarios else path.stem,
            "difficulty": _LEVEL_TO_DIFFICULTY.get(level, ""),
            "goal": goal,
            "constraints": [],
            "turns": turns,
            "case_level": level,
            "provenance": {
                "kind": "product",
                "original_case_id": case_id,
                "precondition": precondition,
                "source": str(path),
                "title": title,
                "scenarios": scenarios,
                "voiceprint_id": cell(c_voice),
                "device_type": cell(c_device),
            },
        }
        evalset["cases"].append(case)

    for turn_skills in skill_names:
        evalset["skill_names"].append(turn_skills)
    return evalset


# ── hotpotqa 原始 JSON → evalset.v2 ────────────────────────────────

def convert_hotpotqa(path: Path, data: list) -> dict:
    """HotpotQA 样本列表（_id/question/answer/context/supporting_facts/type/level）
    → evalset.v2 单轮 case，split 由文件名推断（*train* → train，*dev* → val）。

    每轮提问沿用 EvoAgentX benchmark_and_evaluation.py 的拼接格式：
      Context: Title: {标题}\nText: {句子平铺}\n\nTitle: ...\n\nQuestion: {q}\nAnswer:
    （单个 Context: 前缀，各段 "Title/Text" 块以空行连接，拼到 Question 前面）。
    答案入 expected_behavior 作判分标准；HotpotQA 无意图标注，
    expected_skills 置 None（意图指标跳过）。
    """
    stem = path.stem.lower()
    split = "train" if "train" in stem else "val" if "dev" in stem else ""
    evalset = empty_evalset(
        name=f"hotpotqa:{path.stem}",
        description=f"HotpotQA 转换（来源 {path}，多跳问答，段落平铺为 Context）",
    )
    for item in data:
        qid = item["_id"]
        # EvoAgentX 方式：各段 "Title: ...\nText: ..." 平铺后以空行连接，
        # 整体冠以单个 "Context: " 前缀，再接 "Question: ...\nAnswer:"
        blocks = ["Title: {}\nText: {}".format(para[0], " ".join(para[1]))
                  for para in item.get("context") or []]
        user = "Context: {}\n\nQuestion: {}\nAnswer:".format(
            "\n\n".join(blocks), item["question"])

        level = item.get("level", "")
        case = {
            "id": f"hotpotqa-{split}-{qid}",
            "category": f"hotpotqa-{item.get('type', 'unknown')}",
            "difficulty": level if level in ("easy", "medium", "hard") else "",
            "goal": "多跳阅读理解：在给定证据段落中回答问题，答案需与标准答案一致。",
            "constraints": ["仅依据 Context 中的段落作答，不得依赖外部检索或编造事实。"],
            "turns": [{
                "user": user,
                "expected_skills": None,
                "expected_behavior": f"标准答案：{item['answer']}",
                "must_include": [],
                "must_not_include": [],
            }],
            "provenance": {
                "kind": "opensource",
                "source": str(path),
                "hotpot_id": qid,
                "type": item.get("type", ""),
                "level": level,
                "answer": item["answer"],
                "supporting_facts": [
                    [t, s] for t, s in item.get("supporting_facts") or []
                ],
            },
        }
        if split:
            case["split"] = split
        evalset["cases"].append(case)
    return evalset


def _looks_like_hotpotqa(data) -> bool:
    return (isinstance(data, list) and bool(data) and isinstance(data[0], dict)
            and "question" in data[0] and "answer" in data[0]
            and isinstance(data[0].get("context"), list))


# ── daily-qa evalset.json → evalset.v2 ────────────────────────────

def convert_daily_qa(path: Path) -> dict:
    with open(path, encoding="utf-8") as f:
        data = json.load(f)
    # rubric → expected_behavior 由 evalset_schema.normalize_turn 兼容处理
    evalset = empty_evalset(
        name=data.get("name", f"daily-qa:{path.stem}"),
        description=data.get("description", f"daily-qa 评测集转换（来源 {path}）"),
    )
    evalset["version"] = data.get("version", "1.0.0")
    evalset["skill_names"] = list(data.get("skill_names") or [])
    evalset["categories"] = list(data.get("categories") or [])
    for case in data.get("cases") or []:
        out_case = {
            "id": case["id"],
            "category": case.get("category", "uncategorized"),
            "difficulty": case.get("difficulty", ""),
            "goal": case.get("goal", ""),
            "constraints": case.get("constraints") or [],
            "turns": case.get("turns") or [],
            "provenance": {"kind": "manual", "source": str(path)},
        }
        for key in ("budget", "checks", "final_rubric", "split"):
            if case.get(key):
                out_case[key] = case[key]
        # 兼容旧用例字段；对外统一输出 case_level，不与运行时 level 混用。
        if "case_level" in case or "level" in case:
            out_case["case_level"] = case.get("case_level", case.get("level"))
        evalset["cases"].append(out_case)
    return evalset


# ── 入口 ──────────────────────────────────────────────────────────

def load_domain_map(path: Path) -> dict:
    if not path or not path.exists():
        return {}
    with open(path, encoding="utf-8") as f:
        data = json.load(f)
    return {k: v for k, v in data.items() if not k.startswith("_") and isinstance(v, list)}


def convert_one(path: Path, domain_map: dict, warnings: list) -> dict:
    suffix = path.suffix.lower()
    if suffix == ".jsonl":
        return convert_coldstart(path)
    if suffix == ".xlsx":
        return convert_product(path, domain_map, warnings)
    if suffix == ".json":
        with open(path, encoding="utf-8") as f:
            data = json.load(f)
        if _looks_like_hotpotqa(data):
            return convert_hotpotqa(path, data)
        return convert_daily_qa(path)
    raise ValueError(f"无法识别的输入格式: {path}")


def merge_evalsets(evalsets: list) -> dict:
    merged = empty_evalset(name="unified-evalset", description="多来源合并的 evalset.v2")
    seen = {}
    for es in evalsets:
        merged["skill_names"] = sorted(set(merged["skill_names"]) | set(es.get("skill_names") or []))
        for case in es.get("cases") or []:
            cid = case["id"]
            if cid in seen:
                new_id = f"{es['name']}:{cid}"
                case = dict(case, id=new_id)
                cid = new_id
            seen[cid] = True
            merged["cases"].append(case)
    return merged


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description="数据集格式转换器：coldstart/product/daily-qa → evalset.v2")
    parser.add_argument("inputs", nargs="+", help="输入文件（.jsonl / .xlsx / .json）")
    parser.add_argument("-o", "--output", required=True, help="输出 evalset.v2 JSON 路径")
    parser.add_argument("--map", default=str(Path(__file__).resolve().parent / "domain_skill_map.json"),
                        help="垂域→skill 映射表（默认同目录 domain_skill_map.json）")
    args = parser.parse_args(argv)

    domain_map = load_domain_map(Path(args.map))
    warnings: list = []
    evalsets = []
    for raw in args.inputs:
        path = Path(raw)
        if not path.exists():
            print(f"✗ 输入不存在: {path}", file=sys.stderr)
            return 2
        evalsets.append(convert_one(path, domain_map, warnings))

    merged = normalize_evalset(merge_evalsets(evalsets))
    merged["schema_version"] = SCHEMA_VERSION
    out = Path(args.output)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(merged, ensure_ascii=False, indent=2), encoding="utf-8")

    n_turns = sum(len(c["turns"]) for c in merged["cases"])
    n_intent = sum(1 for c in merged["cases"] for t in c["turns"] if t["expected_skills"] is not None)
    print(f"✓ 转换完成 → {out}")
    print(f"  case: {len(merged['cases'])}  turn: {n_turns}  含意图GT的turn: {n_intent}  "
          f"skill_names: {merged['skill_names']}")
    if warnings:
        print(f"  ⚠ {len(warnings)} 条告警（前 10 条）:")
        for w in warnings[:10]:
            print(f"    - {w}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
