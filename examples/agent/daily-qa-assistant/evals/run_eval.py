#!/usr/bin/env python3
"""Daily QA Assistant 长程任务评测 runner。

对接 agentflowd（JSON-RPC over TCP），执行 evals/evalset.json 中的多轮对话用例，
通过 sessionlog 轨迹核对每轮 skill.dispatch 与 expected_skills（多意图识别准确率），
结合规则检查与 LLM-as-judge 计算任务执行成功率，输出 JSON 报告。

用法（先按 README 启动 daemon，且 .env 中 AF_SESSIONLOG_ENABLED=1）：
  python3 evals/run_eval.py --port 17892
  python3 evals/run_eval.py --port 17892 --cases LH-001,LH-002 --judge
  python3 evals/run_eval.py --validate-only

零第三方依赖（标准库 urllib/socket），Python 3.8+。
"""

import argparse
import concurrent.futures
import json
import os
import re
import socket
import sys
import threading
import time
import urllib.error
import urllib.request
from datetime import datetime
from pathlib import Path

PKG_DIR = Path(__file__).resolve().parents[1]
REPO_DIR = PKG_DIR.parents[2]
DEFAULT_EVALSET = Path(__file__).resolve().parent / "evalset.json"
VALID_CATEGORIES = {"single-intent", "multi-intent", "cross-turn", "long-horizon", "negative"}

_print_lock = threading.Lock()


def log(msg: str) -> None:
    with _print_lock:
        print(msg, flush=True)


# ── .env 加载（进程环境优先） ─────────────────────────────────────

def load_env(env_file: Path) -> None:
    if not env_file.exists():
        return
    for line in env_file.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        key, _, value = line.partition("=")
        key, value = key.strip(), value.strip().strip('"').strip("'")
        if key and key not in os.environ:
            os.environ[key] = value


# ── JSON-RPC over TCP（一行一请求，\n 分隔） ──────────────────────

class RpcClient:
    def __init__(self, host: str, port: int, timeout: float = 60.0):
        self.host, self.port, self.timeout = host, port, timeout
        self.sock = None
        self.req_id = 0
        self.pushed = []  # daemon 异步推送的 agent.response 通知缓存

    def connect(self) -> None:
        self.sock = socket.create_connection((self.host, self.port), timeout=self.timeout)
        self.sock.settimeout(self.timeout)

    def close(self) -> None:
        if self.sock:
            try:
                self.sock.close()
            except OSError:
                pass
            self.sock = None

    def _recv_line(self) -> str:
        buf = b""
        while True:
            ch = self.sock.recv(1)
            if not ch or ch == b"\n":
                break
            buf += ch
        return buf.decode("utf-8")

    def call(self, method: str, params: dict) -> dict:
        if self.sock is None:
            self.connect()
        self.req_id += 1
        payload = json.dumps({"jsonrpc": "2.0", "method": method, "params": params, "id": self.req_id})
        self.sock.sendall((payload + "\n").encode("utf-8"))
        while True:
            line = self._recv_line()
            try:
                obj = json.loads(line)
            except json.JSONDecodeError:
                continue
            if obj.get("id") == self.req_id:
                if obj.get("error") is not None:
                    raise RuntimeError(f"RPC error on {method}: {obj['error']}")
                return obj.get("result", {})
            # daemon 会把响应以 agent.response 通知异步推送到同一 socket（ResponsePoller），
            # 必须缓存，否则协议错位且 poll 端队列已被消费导致"空完成"
            if obj.get("method") == "agent.response":
                p = obj.get("params")
                if isinstance(p, str):
                    try:
                        p = json.loads(p)
                    except json.JSONDecodeError:
                        p = {}
                if isinstance(p, dict):
                    self.pushed.append(p)

    def reconnect(self) -> None:
        self.close()
        self.connect()


def check_daemon(client: RpcClient, agent_id: str) -> None:
    health = client.call("health", {})
    if health.get("status") != "ok":
        raise RuntimeError(f"daemon health check failed: {health}")
    agents = client.call("agent.list", {}).get("agents", [])
    ids = [a.get("agent_id") for a in agents]
    if agent_id not in ids:
        raise RuntimeError(f"agent '{agent_id}' not found, available: {ids}")


def send_turn(client: RpcClient, agent_id: str, user_id: str, client_id: str,
              dialogue_id: str, content: str) -> dict:
    return client.call("agent.execute", {
        "agent_id": agent_id,
        "user_id": user_id,
        "client_id": client_id,
        "content": content,
        "dialogue_id": dialogue_id,
    })


def drain_stale_responses(client: RpcClient, agent_id: str, max_rounds: int = 6) -> None:
    """排空响应队列中的无主残留（重复轮次/上次运行遗留的 final），避免污染本轮。"""
    for _ in range(max_rounds):
        try:
            result = client.call("agent.poll_response", {"agent_id": agent_id, "timeout_ms": 300})
        except (socket.timeout, OSError):
            try:
                client.reconnect()
            except OSError:
                pass
            return
        client.pushed.clear()  # 排空阶段的一切内容一律丢弃
        if result.get("complete"):
            return
        time.sleep(0.3)


def wait_turn_response(client: RpcClient, deadline: float) -> dict:
    """等待本轮最终回复：纯推送模式（ResponsePoller 会把响应推到本连接），
    出现 final=true 推送即为本轮结束；进度推送只入 texts 不作为完成依据。
    不调用 agent.poll_response —— 它与 poller 在同一队列上竞争消费，会造成轮次间串扰。"""
    texts, final_parts, error_parts = [], [], []

    def absorb(msgs) -> None:
        for msg in msgs:
            content = msg.get("content", "")
            if not content:
                continue
            texts.append(content)
            if msg.get("final"):
                final_parts.append(content)
            if msg.get("error"):
                error_parts.append(content)

    def snapshot(timed_out: bool) -> dict:
        return {"texts": texts, "final_text": "\n".join(final_parts or texts),
                "error": "\n".join(error_parts) if error_parts else "", "timed_out": timed_out}

    if client.sock is not None:
        client.sock.settimeout(5)
    while time.time() < deadline:
        try:
            line = client._recv_line()
        except socket.timeout:
            absorb(client.pushed)
            client.pushed.clear()
            if final_parts:
                return snapshot(False)
            continue
        except OSError:
            return snapshot(True)
        try:
            obj = json.loads(line)
        except json.JSONDecodeError:
            continue
        if obj.get("method") == "agent.response":
            p = obj.get("params")
            if isinstance(p, str):
                try:
                    p = json.loads(p)
                except json.JSONDecodeError:
                    p = {}
            if isinstance(p, dict):
                absorb([p])
                if final_parts:
                    return snapshot(False)
    return snapshot(True)


# ── sessionlog 解析（skill.dispatch 事件） ────────────────────────

def sessionlog_candidates(user_id: str, client_id: str, dialogue_id: str):
    roots = []
    if os.environ.get("AF_SESSIONLOG_DIR"):
        roots.append(Path(os.environ["AF_SESSIONLOG_DIR"]))
    if os.environ.get("AF_TRACE_OUTPUT_DIR"):
        roots.append(Path(os.environ["AF_TRACE_OUTPUT_DIR"]) / "sessionlogs")
    roots.append(PKG_DIR / "trace_output" / "sessionlogs")
    roots.append(REPO_DIR / "trace_output" / "sessionlogs")
    rel = Path("by-user") / user_id / client_id / f"{dialogue_id}.jsonl"
    return [r / rel for r in roots]


def find_sessionlog(user_id: str, client_id: str, dialogue_id: str):
    for path in sessionlog_candidates(user_id, client_id, dialogue_id):
        if path.exists():
            return path
    return None


def read_dispatch_events(path) -> list:
    events = []
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            try:
                event = json.loads(line)
            except json.JSONDecodeError:
                continue
            if event.get("type") == "skill.dispatch":
                events.append(event)
    return events


def dispatch_union(events: list) -> set:
    names = set()
    for event in events:
        names.update(event.get("data", {}).get("skill_names") or [])
    return names


# ── 规则判分 ──────────────────────────────────────────────────────

def rule_check(response: str, turn: dict, expected: set, dispatched: set) -> dict:
    response_l = response.lower()
    must_include = turn.get("must_include") or []
    must_not_include = turn.get("must_not_include") or []
    missing_include = [kw for kw in must_include if kw.lower() not in response_l]
    skills_exact = (dispatched == expected) if dispatched is not None else None
    return {
        "response_nonempty": bool(response.strip()),
        "missing_include": missing_include,
        "hit_not_include": [kw for kw in must_not_include if kw.lower() in response_l],
        "skills_exact": skills_exact,
        "skills_missed": sorted(expected - dispatched) if dispatched is not None else None,
        "skills_extra": sorted(dispatched - expected) if dispatched is not None else None,
    }


# ── LLM-as-judge ─────────────────────────────────────────────────

JUDGE_SYSTEM = "你是严格、公正的中文对话质量测评裁判。只输出一个 JSON 对象，不要输出任何其他内容。"


def build_turn_judge_messages(case: dict, transcript: list, turn: dict, response: str) -> list:
    lines = [f"## 任务背景\n{case.get('goal', '')}", "## 对话记录（按轮次）"]
    for item in transcript[:-1]:
        lines.append(f"[第{item['turn']}轮] 用户: {_clip(item['user'], 600)}")
        lines.append(f"[第{item['turn']}轮] 助手: {_clip(item['response'], 800)}")
    last = transcript[-1]
    lines += [
        f"## 待评分轮次：第 {last['turn']} 轮",
        f"用户输入：{_clip(last['user'], 600)}",
        f"助手回复：{_clip(response, 2000)}",
        f"## 本轮检查点\n{turn.get('rubric', '无')}",
        "- 是否准确识别并完成了本轮用户的核心意图",
        "- 实时信息（天气/地点/公交/新闻/图片）应来自真实工具调用并标注来源，不得编造",
        "- 表达是否自然、结构清晰",
        "## 评分标准（0~5 整数）\n5=完全达成且质量高；4=达成核心目标有轻微瑕疵；3=部分达成；2=基本未达成；1=答非所问；0=错误/编造/拒绝回答。",
        '只输出 JSON：{"score": <0-5 整数>, "reason": "<50字内理由>"}',
    ]
    return [{"role": "system", "content": JUDGE_SYSTEM}, {"role": "user", "content": "\n".join(lines)}]


def build_dialogue_judge_messages(case: dict, transcript: list) -> list:
    lines = [f"## 任务背景\n{case.get('goal', '')}", "## 整段对话记录"]
    for item in transcript:
        lines.append(f"[第{item['turn']}轮] 用户: {_clip(item['user'], 400)}")
        lines.append(f"[第{item['turn']}轮] 助手: {_clip(item['response'], 600)}")
    lines += [
        f"## 整体检查点\n{case.get('final_rubric', '')}",
        "## 评分标准（0~5 整数）\n5=整段对话完美达成任务目标；4=达成但有明显小瑕疵；3=部分轮次未达成；2=多数轮次失败；1=任务链断裂；0=完全失败。",
        '只输出 JSON：{"score": <0-5 整数>, "reason": "<50字内理由>"}',
    ]
    return [{"role": "system", "content": JUDGE_SYSTEM}, {"role": "user", "content": "\n".join(lines)}]


def _clip(text: str, limit: int) -> str:
    text = (text or "").strip()
    return text if len(text) <= limit else text[:limit] + "…[截断]"


def parse_judge_output(text: str):
    text = re.sub(r"```(?:json)?", "", text or "")
    matches = re.findall(r"\{[^{}]*\}", text)
    for raw in reversed(matches):
        try:
            obj = json.loads(raw)
            if isinstance(obj.get("score"), (int, float)):
                score = int(max(0, min(5, int(round(obj["score"])))))
                return score, str(obj.get("reason", ""))[:200]
        except (json.JSONDecodeError, ValueError):
            continue
    return None, ""


def llm_chat(base_url: str, api_key: str, model: str, messages: list, timeout: int = 120) -> str:
    url = base_url.rstrip("/") + "/chat/completions"
    body = json.dumps({"model": model, "messages": messages, "temperature": 0, "stream": False}).encode("utf-8")
    req = urllib.request.Request(url, data=body, method="POST", headers={
        "Content-Type": "application/json", "Authorization": f"Bearer {api_key}"})
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        data = json.loads(resp.read().decode("utf-8"))
    return data["choices"][0]["message"]["content"]


def judge_score(cfg: dict, messages: list):
    for _ in range(3):
        try:
            out = llm_chat(cfg["base_url"], cfg["api_key"], cfg["model"], messages)
            score, reason = parse_judge_output(out)
            if score is not None:
                return score, reason
        except (OSError, KeyError, json.JSONDecodeError, urllib.error.URLError) as exc:  # noqa: BLE001
            reason = f"judge 调用失败: {exc}"
        time.sleep(2)
    return None, reason


# ── 单 case 执行 ──────────────────────────────────────────────────

def run_case(case: dict, cfg: dict) -> dict:
    case_id = case["id"]
    dialogue_id = f"eval-{case_id}-{datetime.now().strftime('%H%M%S')}"
    user_id, client_id = cfg["user_id"], f"eval-{case_id.lower()}"
    client = RpcClient(cfg["host"], cfg["port"], timeout=max(60.0, cfg["turn_timeout"]))
    result = {
        "case_id": case_id, "category": case["category"], "difficulty": case.get("difficulty", ""),
        "goal": case.get("goal", ""), "dialogue_id": dialogue_id, "turns": [],
        "sessionlog_missing": False, "error": "", "case_success": False,
    }
    log(f"▶ {case_id} [{case['category']}] dialogue={dialogue_id} 开始（{len(case['turns'])} 轮）")
    try:
        client.connect()
    except OSError as exc:
        result["error"] = f"连接 daemon 失败: {exc}"
        log(f"✗ {case_id} {result['error']}")
        return result

    dispatched_seen_ids: set = set()
    transcript = []
    try:
        for idx, turn in enumerate(case["turns"], start=1):
            turn_result = {
                "turn": idx, "user": turn["user"],
                "expected_skills": sorted(turn.get("expected_skills") or []),
                "response": "", "progress_texts": [], "timed_out": False, "error": "",
                "dispatched_skills": None, "checks": {}, "rule_pass": False,
                "judge_score": None, "judge_reason": "", "judge_pass": False, "turn_success": False,
            }
            deadline = time.time() + cfg["turn_timeout"]
            try:
                drain_stale_responses(client, cfg["agent_id"])
                send_turn(client, cfg["agent_id"], user_id, client_id, dialogue_id, turn["user"])
                polled = wait_turn_response(client, deadline)
                turn_result["response"] = polled["final_text"]
                turn_result["progress_texts"] = polled["texts"]
                turn_result["timed_out"] = polled["timed_out"]
                if polled["error"]:
                    turn_result["error"] = polled["error"]
            except (OSError, RuntimeError) as exc:
                try:
                    client.reconnect()
                except OSError:
                    result["error"] = f"daemon 连接不可恢复: {exc}"
                    break
                turn_result["error"] = f"本轮执行失败: {exc}"

            log_path = find_sessionlog(user_id, client_id, dialogue_id)
            if log_path:
                events_now = read_dispatch_events(log_path)
                # 轨迹 append-only：按 event_id 水位取本轮新增事件（支持跨轮重复 dispatch 同一 skill）
                new_events = [e for e in events_now if e.get("event_id") not in dispatched_seen_ids]
                dispatched_seen_ids.update(e.get("event_id") for e in new_events)
                turn_result["dispatched_skills"] = sorted(dispatch_union(new_events))
                turn_result["dispatch_events_total"] = len(events_now)
            else:
                result["sessionlog_missing"] = True

            expected = set(turn.get("expected_skills") or [])
            dispatched = set(turn_result["dispatched_skills"]) if turn_result["dispatched_skills"] is not None else None
            checks = rule_check(turn_result["response"], turn, expected, dispatched)
            turn_result["checks"] = checks
            turn_result["rule_pass"] = (
                checks["response_nonempty"] and not turn_result["error"] and not turn_result["timed_out"]
                and not checks["missing_include"] and not checks["hit_not_include"]
                and checks["skills_exact"] is not False
            )

            if cfg["judge"]:
                score, reason = judge_score(cfg["judge_cfg"], build_turn_judge_messages(case, transcript + [
                    {"turn": idx, "user": turn["user"], "response": turn_result["response"]}], turn,
                    turn_result["response"]))
                turn_result["judge_score"], turn_result["judge_reason"] = score, reason
                turn_result["judge_pass"] = score is not None and score >= cfg["judge_threshold"]

            turn_result["turn_success"] = turn_result["rule_pass"] and (
                not cfg["judge"] or turn_result["judge_pass"])
            transcript.append({"turn": idx, "user": turn["user"], "response": turn_result["response"]})
            result["turns"].append(turn_result)
            log(f"  ✓ 轮{idx} [{'成功' if turn_result['turn_success'] else '失败'}] "
                f"skills: 期望={sorted(expected)} 实际={turn_result['dispatched_skills']} "
                f"judge={turn_result['judge_score']}")

        if cfg["judge"] and case.get("final_rubric") and transcript:
            score, reason = judge_score(cfg["judge_cfg"], build_dialogue_judge_messages(case, transcript))
            result["dialogue_judge_score"], result["dialogue_judge_reason"] = score, reason

        result["case_success"] = bool(result["turns"]) and all(t["turn_success"] for t in result["turns"])
        if result["error"]:
            result["case_success"] = False
    finally:
        client.close()

    status = "✓ 成功" if result["case_success"] else "✗ 失败"
    log(f"{status} {case_id}（sessionlog {'缺失' if result['sessionlog_missing'] else '正常'}）")
    return result


# ── 指标汇总 ──────────────────────────────────────────────────────

def compute_metrics(case_results: list) -> dict:
    turns = [t for c in case_results for t in c["turns"]]
    valid = [t for t in turns if t["dispatched_skills"] is not None]
    sessionlog_missing = len(turns) - len(valid)

    def exact(t):
        return set(t["expected_skills"]) == set(t["dispatched_skills"])

    multi = [t for t in valid if len(t["expected_skills"]) >= 2]
    single = [t for t in valid if len(t["expected_skills"]) == 1]
    negative = [t for t in valid if not t["expected_skills"]]
    judged = [t for t in turns if t["judge_score"] is not None]

    skill_names = sorted({s for t in valid for s in t["expected_skills"] + t["dispatched_skills"]})
    per_skill = {}
    for skill in skill_names:
        tp = sum(1 for t in valid if skill in t["expected_skills"] and skill in t["dispatched_skills"])
        fp = sum(1 for t in valid if skill not in t["expected_skills"] and skill in t["dispatched_skills"])
        fn = sum(1 for t in valid if skill in t["expected_skills"] and skill not in t["dispatched_skills"])
        precision = tp / (tp + fp) if tp + fp else None
        recall = tp / (tp + fn) if tp + fn else None
        f1 = (2 * precision * recall / (precision + recall)) if precision and recall else None
        per_skill[skill] = {"precision": precision, "recall": recall, "f1": f1, "tp": tp, "fp": fp, "fn": fn}

    def rate(num, den):
        return round(num / den, 4) if den else None

    summary = {
        "cases_total": len(case_results),
        "cases_success": sum(1 for c in case_results if c["case_success"]),
        "case_success_rate": rate(sum(1 for c in case_results if c["case_success"]), len(case_results)),
        "turns_total": len(turns),
        "turn_success_rate": rate(sum(1 for t in turns if t["turn_success"]), len(turns)),
        "rule_pass_rate": rate(sum(1 for t in turns if t["rule_pass"]), len(turns)),
        "intent_turns": len(valid),
        "sessionlog_missing_turns": sessionlog_missing,
        "intent_exact_accuracy": rate(sum(1 for t in valid if exact(t)), len(valid)),
        "multi_intent_accuracy": rate(sum(1 for t in multi if exact(t)), len(multi)),
        "multi_intent_turns": len(multi),
        "single_intent_accuracy": rate(sum(1 for t in single if exact(t)), len(single)),
        "no_dispatch_accuracy": rate(sum(1 for t in negative if exact(t)), len(negative)),
        "false_dispatch_rate": rate(
            sum(len(set(t["dispatched_skills"]) - set(t["expected_skills"])) for t in valid),
            sum(len(t["dispatched_skills"]) for t in valid) or None),
        "judge_enabled": bool(judged),
        "judge_turns": len(judged),
        "judge_pass_rate": rate(sum(1 for t in judged if t["judge_pass"]), len(judged)),
        "judge_avg_score": rate(sum(t["judge_score"] for t in judged), len(judged)),
        "judge_errors": sum(1 for t in turns if t["judge_score"] is None and t.get("judge_reason", "").startswith("judge")),
    }
    per_category = {}
    for cat in sorted({c["category"] for c in case_results}):
        cat_cases = [c for c in case_results if c["category"] == cat]
        cat_turns = [t for c in cat_cases for t in c["turns"] if t["dispatched_skills"] is not None]
        cat_multi = [t for t in cat_turns if len(t["expected_skills"]) >= 2]
        per_category[cat] = {
            "cases": len(cat_cases),
            "case_success_rate": rate(sum(1 for c in cat_cases if c["case_success"]), len(cat_cases)),
            "turns": len(cat_turns),
            "intent_exact_accuracy": rate(sum(1 for t in cat_turns if exact(t)), len(cat_turns)),
            "multi_intent_accuracy": rate(sum(1 for t in cat_multi if exact(t)), len(cat_multi)),
        }
    return {"summary": summary, "per_category": per_category, "per_skill": per_skill}


# ── 评测集校验 ────────────────────────────────────────────────────

def validate_evalset(data: dict) -> list:
    errors = []
    skill_names = set(data.get("skill_names") or [])
    ids = set()
    for i, case in enumerate(data.get("cases") or []):
        cid = case.get("id", f"<index-{i}>")
        if case.get("id") in ids:
            errors.append(f"{cid}: case id 重复")
        ids.add(case.get("id"))
        if case.get("category") not in VALID_CATEGORIES:
            errors.append(f"{cid}: category 非法 → {case.get('category')}")
        if not case.get("turns"):
            errors.append(f"{cid}: 缺少 turns")
            continue
        if len(case["turns"]) < 3:
            errors.append(f"{cid}: 长程用例应至少 3 轮（当前 {len(case['turns'])}）")
        for j, turn in enumerate(case["turns"], 1):
            if not turn.get("user"):
                errors.append(f"{cid} 第{j}轮: 缺少 user")
            for skill in turn.get("expected_skills") or []:
                if skill not in skill_names:
                    errors.append(f"{cid} 第{j}轮: 未知 skill '{skill}'")
        if case["category"] == "multi-intent" and not any(
                len(t.get("expected_skills") or []) >= 2 for t in case["turns"]):
            errors.append(f"{cid}: multi-intent 用例应至少有 1 轮含 2+ 意图")
        if case["category"] == "negative" and any(t.get("expected_skills") for t in case["turns"]):
            pass  # NG-004 允许末轮真实意图（陷阱设计），不做硬校验
    return errors


# ── main ──────────────────────────────────────────────────────────

def parse_args(argv=None):
    parser = argparse.ArgumentParser(description="Daily QA Assistant 长程任务评测 runner")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=17892)
    parser.add_argument("--agent-id", default="daily-qa-assistant")
    parser.add_argument("--evalset", default=str(DEFAULT_EVALSET))
    parser.add_argument("--cases", default="", help="逗号分隔的 case id 过滤")
    parser.add_argument("--category", default="", help="逗号分隔的 category 过滤")
    parser.add_argument("--user-id", default="eval-user")
    parser.add_argument("--concurrency", type=int, default=1, help="并发 case 数（daemon session 池为 3，建议 ≤3）")
    parser.add_argument("--turn-timeout", type=int, default=240, help="单轮超时秒数")
    parser.add_argument("--judge", dest="judge", action="store_true", default=None,
                        help="启用 LLM-as-judge（默认：有 API key 即启用）")
    parser.add_argument("--no-judge", dest="judge", action="store_false")
    parser.add_argument("--judge-model", default="", help="judge 模型（默认 AF_LLM_MODEL）")
    parser.add_argument("--judge-threshold", type=int, default=4, help="judge 通过阈值（默认 4）")
    parser.add_argument("--output", default="", help="报告输出路径（默认 evals/reports/report-<ts>.json）")
    parser.add_argument("--validate-only", action="store_true", help="仅校验评测集格式后退出")
    parser.add_argument("--list-cases", action="store_true", help="列出用例后退出")
    return parser.parse_args(argv)


def main(argv=None) -> int:
    args = parse_args(argv)
    load_env(PKG_DIR / ".env")

    with open(args.evalset, encoding="utf-8") as f:
        evalset = json.load(f)
    errors = validate_evalset(evalset)
    if errors:
        print("✗ 评测集校验失败：")
        for err in errors:
            print(f"  - {err}")
        return 2
    print(f"✓ 评测集校验通过：{len(evalset['cases'])} 个 case")

    if args.validate_only:
        return 0
    if args.list_cases:
        for case in evalset["cases"]:
            skills = sorted({s for t in case["turns"] for s in t.get("expected_skills") or []} ) or ["-"]
            print(f"  {case['id']:8s} [{case['category']:13s}] {len(case['turns'])} 轮  {','.join(skills)}  {case['goal'][:40]}")
        return 0

    cases = evalset["cases"]
    if args.cases:
        wanted = {c.strip() for c in args.cases.split(",") if c.strip()}
        cases = [c for c in cases if c["id"] in wanted]
        missing = wanted - {c["id"] for c in cases}
        if missing:
            print(f"✗ 未找到用例: {sorted(missing)}")
            return 2
    if args.category:
        cats = {c.strip() for c in args.category.split(",") if c.strip()}
        cases = [c for c in cases if c["category"] in cats]
    if not cases:
        print("✗ 过滤后没有可执行用例")
        return 2

    judge_enabled = args.judge
    judge_cfg = {
        "base_url": os.environ.get("AF_LLM_BASE_URL", ""),
        "api_key": os.environ.get("AF_LLM_API_KEY") or os.environ.get("API_KEY", ""),
        "model": args.judge_model or os.environ.get("AF_LLM_MODEL", ""),
    }
    if judge_enabled is None:
        judge_enabled = bool(judge_cfg["api_key"] and judge_cfg["base_url"])
        if not judge_enabled:
            print("⚠ 未检测到 AF_LLM_API_KEY/AF_LLM_BASE_URL，自动关闭 LLM judge")
    if judge_enabled and not (judge_cfg["api_key"] and judge_cfg["base_url"] and judge_cfg["model"]):
        print("✗ judge 配置不完整（AF_LLM_API_KEY / AF_LLM_BASE_URL / AF_LLM_MODEL）")
        return 2

    cfg = {
        "host": args.host, "port": args.port, "agent_id": args.agent_id,
        "user_id": args.user_id, "turn_timeout": args.turn_timeout,
        "judge": judge_enabled, "judge_cfg": judge_cfg, "judge_threshold": args.judge_threshold,
    }
    print(f"目标: {args.host}:{args.port} agent={args.agent_id} | 用例: {len(cases)} | "
          f"judge={'on(' + judge_cfg.get('model', '') + ')' if judge_enabled else 'off'} | 并发={args.concurrency}")

    probe = RpcClient(args.host, args.port)
    try:
        check_daemon(probe, args.agent_id)
    except (OSError, RuntimeError) as exc:
        print(f"✗ daemon 不可用: {exc}")
        return 2
    finally:
        probe.close()
    print("✓ daemon 连接正常")

    t0 = time.time()
    if args.concurrency <= 1:
        case_results = [run_case(case, cfg) for case in cases]
    else:
        with concurrent.futures.ThreadPoolExecutor(max_workers=args.concurrency) as pool:
            case_results = list(pool.map(lambda c: run_case(c, cfg), cases))
    elapsed = time.time() - t0

    metrics = compute_metrics(case_results)
    report = {
        "meta": {
            "evalset": os.path.abspath(args.evalset), "evalset_version": evalset.get("version"),
            "agent_id": args.agent_id, "host": args.host, "port": args.port,
            "judge_model": judge_cfg.get("model") if judge_enabled else None,
            "judge_threshold": args.judge_threshold if judge_enabled else None,
            "started_at": datetime.fromtimestamp(t0).isoformat(),
            "elapsed_s": round(elapsed, 1),
        },
        **metrics,
        "cases": case_results,
    }
    out_path = Path(args.output) if args.output else (
        Path(__file__).resolve().parent / "reports" / f"report-{datetime.now().strftime('%Y%m%d-%H%M%S')}.json")
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")

    s = metrics["summary"]
    print("\n" + "=" * 64)
    print(f"评测完成（{round(elapsed)}s）→ {out_path}")
    print(f"  任务执行成功率   case: {s['case_success_rate']} ({s['cases_success']}/{s['cases_total']})"
          f"  turn: {s['turn_success_rate']} ({s['turns_total']} 轮)")
    print(f"  多意图识别准确率      : {s['multi_intent_accuracy']} ({s['multi_intent_turns']} 轮 ≥2 意图)")
    print(f"  意图集合完全匹配率    : {s['intent_exact_accuracy']} (单意图 {s['single_intent_accuracy']}"
          f" / 零意图 {s['no_dispatch_accuracy']})")
    print(f"  误分发率              : {s['false_dispatch_rate']}  sessionlog 缺失轮: {s['sessionlog_missing_turns']}")
    if judge_enabled:
        print(f"  judge 通过率/均分     : {s['judge_pass_rate']} / {s['judge_avg_score']} (阈值 {args.judge_threshold}, "
              f"解析失败 {s['judge_errors']})")
    for skill, st in metrics["per_skill"].items():
        print(f"  skill[{skill:12s}] P={st['precision']} R={st['recall']} F1={st['f1']}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
