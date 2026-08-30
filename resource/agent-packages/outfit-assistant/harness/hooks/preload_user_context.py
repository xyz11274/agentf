"""preload_user_context hook — 确定性预扫描用户上下文并注入 system prompt。

垂域 hook（outfit-assistant）：在 skill 首次模型调用前（round == 1），把用户上下文
（档案 / 历史 / 风格参考 / 目录树）确定性收集后**追加到已有 system prompt**，
省去 LLM 首轮决策调用 preload_user_context 工具的那一轮模型调用（实测 4-5 轮
LLM × ~8.5s/轮 → 首轮即带上下文）。

设计决策（用户 2026-08-14 确认）：
- 注入方式：不追加消息，直接在已有 system prompt（ctx.messages 中 role=system）上追加。
- 收集逻辑：hook 独立实现（与 harness/tools/preload_user_context.py 解耦，各司其职——
  工具保留 location 参数化天气能力；hook 只做确定性注入）。
- 天气：不含（需要 location 参数，保留给工具按需调用）。
- 工具去留：hook + 工具并存（hook 覆盖确定性注入，工具保留参数化能力 + 向后兼容）。

Hook 规范（§3.2）：HOOK_NAME / HOOK_EVENTS / handle(event, ctx) 双参。
"""

import json
import logging
import os

HOOK_NAME = "preload_user_context"
# 同时监听 skill 与 coordinator 两层的 before_llm_call：
# - skill_executor.before_llm_call：skill 内 LLM 调用前（skill 执行路径）
# - coordinator_v1.before_llm_call：coordinator v1 决策循环（coordinator_decide.py:597）
# - coordinator_v2.before_llm_call：coordinator v2 agent_loop 直接执行（coordinator_agent_loop.py:357）
# v1/v2 是两套差别较大的体系（v1=决策循环+skill 委派；v2=agent_loop 直接执行工具），
# 事件必须区分开——hook 据此知道 LLM 调用来自哪条路径（2026-08-16 用户确认拆分）。
HOOK_EVENTS = [
    "skill_executor.before_llm_call",
    "coordinator_v1.before_llm_call",
    "coordinator_v2.before_llm_call",
]
HOOK_ORDER = 0  # 最先执行：在其它 hook 之前注入，确保首次 LLM 调用前上下文就位

logger = logging.getLogger(__name__)

_MEMORY_DIR = ".agentfoundry/memory/outfit-profile"
_WS_DIRS = (
    ".", "upload", "outputs",
    ".agentfoundry", ".agentfoundry/memory",
    ".agentfoundry/memory/outfit-profile",
    ".agentfoundry/memory/outfit-profile/history",
)


def _collect_context() -> dict:
    """确定性只读收集用户上下文（不含天气）。失败降级返回最小上下文（不阻断）。"""
    try:
        from agentflow_runtime.common.runtime_context import artifact_routing_id
        from agentflow_runtime.executors.factory import create_executor

        executor = create_executor(
            sandbox_endpoint=os.environ.get("AF_SANDBOX_ENDPOINT") or None
        )
        ctx: dict = {"memory_dir": _MEMORY_DIR}

        # 1. 档案 profile.md（存在性 + 内容）
        profile = executor.read_file(f"{_MEMORY_DIR}/profile.md")
        if profile.error:
            ctx["profile_exists"] = False
            ctx["profile_content"] = ""
        else:
            ctx["profile_exists"] = True
            ctx["profile_content"] = (profile.content or "").strip()

        # 2. 历史 history/ 最近 10 条（name 序近似 mtime 序——文件名带时间戳前缀）
        history_entries = executor.list_dir(f"{_MEMORY_DIR}/history/")
        history_names = sorted(
            (e["name"] for e in history_entries if not e.get("is_dir")),
            reverse=True,
        )
        ctx["history_files"] = history_names[:10]

        # 3. 静态风格参考 style-guide.md（预加载，避免 LLM 每轮 read_file 重复读取）
        agent_pkg = os.environ.get("AF_AGENT_PACKAGE_DIR", "")
        sg_paths = []
        if agent_pkg:
            sg_paths.append(f"{agent_pkg}/harness/skills/outfit-stylist/references/style-guide.md")
        sg_paths.append("harness/skills/outfit-stylist/references/style-guide.md")  # fallback
        sg_out = ""
        for sg_path in sg_paths:
            sg = executor.read_file(sg_path)
            if not sg.error and (sg.content or "").strip():
                sg_out = sg.content.strip()
                break
        if sg_out:
            ctx["style_guide"] = sg_out

        # 4. workspace_tree：关键目录浅层树（防 S2/S4 探查轮）
        agent_pkg = os.environ.get("AF_AGENT_PACKAGE_DIR", "")
        uid = os.environ.get("AF_USER_ID", "")
        cid = os.environ.get("AF_CLIENT_ID", "")
        if agent_pkg and uid and cid:
            routing_id = artifact_routing_id(uid, cid)
            workdir = f"{agent_pkg}/workspace/{routing_id}"
        else:
            workdir = os.environ.get("AF_WORKDIR", "") or "."
        tree: dict = {"workdir": workdir}
        for _dir in _WS_DIRS:
            entries = executor.list_dir(f"{workdir}/{_dir}" if _dir != "." else workdir)
            if not entries:
                tree[_dir] = {"exists": False, "empty_or_missing": True}
                continue
            all_files = sorted(e["name"] for e in entries if not e.get("is_dir"))
            all_dirs = sorted(e["name"] for e in entries if e.get("is_dir"))
            entry: dict = {"exists": True, "files": all_files[:20], "dirs": all_dirs[:10]}
            if len(all_files) > len(entry["files"]):
                entry["files_truncated"] = True
                entry["total_files"] = len(all_files)
            if len(all_dirs) > len(entry["dirs"]):
                entry["dirs_truncated"] = True
                entry["total_dirs"] = len(all_dirs)
            # upload 目录透传图片确定性路径（OP-1，与工具版对齐）：executor.list_dir 的
            # entry["path"] 已是当前执行环境可直接使用的路径（Host=宿主机绝对路径；
            # e2b=沙箱内绝对路径），供 outfit_vision --image 直接使用，消除 LLM 拼路径方差。
            if _dir == "upload":
                upload_images = [
                    {"name": e["name"], "path": e["path"]}
                    for e in entries
                    if not e.get("is_dir")
                    and e["name"].lower().endswith((".png", ".jpg", ".jpeg", ".webp"))
                ]
                if upload_images:
                    entry["upload_images"] = upload_images[:20]
            tree[_dir] = entry
        ctx["workspace_tree"] = tree

        # 4.5 env：非动态环境信息（key 可用性 + 技能挂载，与工具版对齐）
        # SKILL.md 原本写死 `[ -n "$DASHSCOPE_API_KEY" ]` 与 `ls ${SKILLS_DIR}/video-gen/...`
        # 探测命令，每次触发一轮额外 LLM tool_call——由 hook 确定性探测一次性注入。
        env_ctx: dict = {}
        key_probe = executor.exec_command(
            '[ -n "$DASHSCOPE_API_KEY" ] && echo KEY_YES || echo KEY_NO'
        )
        env_ctx["dashscope_key_available"] = bool(
            key_probe.stdout and "KEY_YES" in key_probe.stdout
        )
        skills_root = f"{agent_pkg}/harness/skills" if agent_pkg else "harness/skills"
        skill_dirs = {e["name"] for e in executor.list_dir(skills_root) if e.get("is_dir")}
        env_ctx["skills"] = {
            "forge_image_gen": "forge-image-gen" in skill_dirs,
            "video_gen": "video-gen" in skill_dirs,
        }
        ctx["env"] = env_ctx

        # 4.6 最近天气：读 history/ 最新卡片"地点天气"字段（持久化天气复用）
        # 背景（2026-08-16 用户确认 Q2）：weather.py/preload 的实时查询不落盘，但 SKILL.md
        # 第 6 步建档要求 history 卡片记"地点天气"（如"上海，雷阵雨，27~34°C"）——hook
        # 读最近卡片提取该字段注入；若恰巧就是用户需要的天气，LLM 不再调用天气查询。
        try:
            hist_entries = executor.list_dir(f"{_MEMORY_DIR}/history/")
            hist_names = sorted(
                (e["name"] for e in hist_entries if not e.get("is_dir")),
                reverse=True,
            )
            if hist_names:
                latest = hist_names[0]
                card = executor.read_file(f"{_MEMORY_DIR}/history/{latest}")
                if not card.error and card.content:
                    import re as _re
                    # 卡片格式：`- **地点天气**：上海，⛈️ 雷阵雨，27~34°C...`
                    #（SKILL.md 第 6 步建档模板）——`地点天气` 后可跟 `**` 再加冒号
                    m = _re.search(r"地点天气[^\n:]*[：:]\s*(\S.*?)(?:\n|$)", card.content)
                    if m:
                        ctx["weather"] = {
                            "from_history": latest,
                            "text": m.group(1).strip(),
                        }
        except Exception as exc:  # noqa: BLE001 — 天气为可选增强，失败不阻断
            logger.warning("preload_user_context.hook weather-from-history failed: %s", exc)

        # 5. 新用户标记
        ctx["is_new_user"] = not ctx["profile_exists"]
        return ctx
    except Exception as exc:  # noqa: BLE001 — 预扫描失败降级，不阻断 skill
        logger.warning("preload_user_context.hook collect failed: %s", exc)
        return {"memory_dir": _MEMORY_DIR, "error": f"preload_user_context hook failed: {exc}"}


def handle(event, ctx) -> None:
    """before_llm_call 挂载点：首次模型调用前把用户上下文追加到 system prompt。

    只处理 round == 1（第一次 LLM 调用）；后续轮次 system prompt 已带上下文，
    不再重复注入（I-4 幂等）。
    """
    if event.payload.get("round") != 1:
        return None
    if not ctx.messages:
        return None

    # 定位已有 system 消息（通常是 messages[0]）——不追加新消息，直接追加其 content
    system_idx = None
    for i, m in enumerate(ctx.messages):
        if m.get("role") == "system":
            system_idx = i
            break
    if system_idx is None:
        return None

    collected = _collect_context()
    if not collected:
        return None

    hint = (
        "【预加载用户上下文（权威数据，直接使用）】\n"
        + json.dumps(collected, ensure_ascii=False)
        + "\n本段已提供：档案 profile_content / 历史 history_files / 风格参考 style_guide / "
          "目录树 workspace_tree（含 upload_images 上传图确定性路径）/ 环境 env（key 可用性、"
          "技能挂载）/ 新用户标记 is_new_user"
          + (" / 最近天气 weather（来自 history 卡片，注意时效）" if collected.get("weather") else "")
          + "。"
          "**禁止再用 bash/glob/read 探查这些数据**（禁止 ls 记忆库、档案目录、upload/ 上传图存在性、"
          "探测 key、ls 技能目录）——直接基于本段推理。"
    )
    if not collected.get("weather"):
        hint += "天气未在此处提供：如需实时天气，用 preload_user_context 工具传入 location。"
    ctx.messages[system_idx]["content"] = ctx.messages[system_idx]["content"] + "\n\n" + hint
    return None
