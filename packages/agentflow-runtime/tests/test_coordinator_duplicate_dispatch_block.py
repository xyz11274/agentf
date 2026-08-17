"""回归测试：F2 重复 dispatch 拦截（_is_duplicate_dispatch）。

根因（2026-08-13 实测日志 agentflowd.log 21:45:40）：
  coordinator_decide.py 拦截条件误用函数对象 `_completed_skill_name`（import 的
  函数，未加括号调用），字符串 == 函数对象 恒为 False → F2 拦截永不触发 →
  第一次 skill 完成后 LLM 偶发二次 dispatch 同一 skill 时，整段 skill 被重复
  执行（~18s 浪费）。

2026-08-13 升级（用户要求）：
  "重复 skill 调度不能只看 skill 名字，还要同时看输入 prompt"——判定升级为
  双维度：**同名 skill 且同 prompt** 才算重复；同名但 prompt 不同（新任务指令 /
  用户补充信息后重跑）→ 放行。last_prompt 由 handle_run_skills_call 在每次
  dispatch 时写入 DataBus（key=last_prompt:{dialogue_id}:{skill_name}），
  re-trigger 时 coordinator 读取对比。

本测试锁定 _is_duplicate_dispatch 的不变量：
  ① 同名 + 同 prompt（无 force）→ 拦截（True）
  ② 同名 + 不同 prompt（新任务）→ 放行
  ③ 首次触发（completed_skill 为空）→ 不拦截
  ④ force=true（显式逃生口，主动重排）→ 不拦截
  ⑤ 不同 skill → 不拦截
  ⑥ last_prompt 缺失（读取失败/首次记录）→ 保守按"同名即拦截"处理
"""

from __future__ import annotations

import sys
from pathlib import Path

PROJECT_ROOT = Path(__file__).resolve().parents[2]
PKG_SRC = PROJECT_ROOT / "packages" / "agentflow-runtime" / "src"
if str(PKG_SRC) not in sys.path:
    sys.path.insert(0, str(PKG_SRC))


def _load():
    from agentflow_runtime import coordinator_decide as cd
    return cd


def test_duplicate_dispatch_same_skill_same_prompt_blocked():
    """重复 dispatch 刚完成的同一 skill 且 prompt 相同（无 force）→ 拦截。"""
    cd = _load()
    skill_names = [{"name": "outfit-stylist", "prompt": "推荐一套穿搭并返回HTML"}]
    assert cd._is_duplicate_dispatch(
        "outfit-stylist", skill_names, False, "推荐一套穿搭并返回HTML"
    ) is True


def test_same_skill_different_prompt_allowed():
    """同名 skill 但 prompt 不同（新任务指令）→ 放行，不拦截。

    2026-08-13 双维度升级的核心：LLM 在 skill 完成后，若带着新的任务指令
    （如用户补充了性别/风格信息要求重新生成）再次 dispatch 同一 skill，
    不应被当成"重复调度"拦截。
    """
    cd = _load()
    skill_names = [{"name": "outfit-stylist", "prompt": "用户是男性，通勤风格，生成HTML推荐"}]
    assert cd._is_duplicate_dispatch(
        "outfit-stylist", skill_names, False, "推荐一套穿搭并返回HTML"
    ) is False


def test_duplicate_dispatch_string_form_skill_name():
    """skill_names 中的 skill 也可能是字符串形式（兼容两种形态）。"""
    cd = _load()
    skill_names = ["outfit-stylist"]
    assert cd._is_duplicate_dispatch("outfit-stylist", skill_names, False) is True


def test_first_trigger_not_blocked():
    """首次触发（completed_skill 为空）→ 不拦截。"""
    cd = _load()
    skill_names = [{"name": "outfit-stylist", "prompt": "首次调度"}]
    assert cd._is_duplicate_dispatch("", skill_names, False) is False


def test_force_true_escape_hatch_not_blocked():
    """force=true（显式逃生口，主动重排）→ 不拦截。"""
    cd = _load()
    skill_names = [{"name": "outfit-stylist", "prompt": "用户不满意要求重跑", "force": True}]
    assert cd._is_duplicate_dispatch("outfit-stylist", skill_names, True) is False


def test_force_true_in_args_not_blocked():
    """args 级 force=true 同样放行。"""
    cd = _load()
    skill_names = [{"name": "outfit-stylist", "prompt": "重跑"}]
    assert cd._is_duplicate_dispatch("outfit-stylist", skill_names, True) is False


def test_different_skill_not_blocked():
    """dispatch 的 skill 与刚完成的 skill 不同 → 不拦截。"""
    cd = _load()
    skill_names = [{"name": "forge-image-gen", "prompt": "生成效果图"}]
    assert cd._is_duplicate_dispatch("outfit-stylist", skill_names, False) is False


def test_last_prompt_missing_conservative_block():
    """last_prompt 缺失（读取失败/首次记录不存在）→ 保守按"同名即拦截"处理。

    确保降级路径不引入重复执行回归：宁可多拦一次（让 LLM 基于 tool result
    重新决策），也不放行潜在的重复 skill 执行。
    """
    cd = _load()
    skill_names = [{"name": "outfit-stylist", "prompt": "推荐穿搭"}]
    assert cd._is_duplicate_dispatch("outfit-stylist", skill_names, False, None) is True
