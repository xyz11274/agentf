"""Preload user context — 确定性预扫描用户上下文，减少 skill 内 LLM 探索轮次。

背景（2026-08-10，outfit perf 基线优化分析）：
SKILL.md 第 0 步"召回个性化档案（每次先做）"要求 LLM 用 bash 工具逐轮读取
profile/history/天气——这些是确定性只读收集，却走 LLM while-loop 逐轮决策
（实测 skill 内 4-5 轮 LLM × ~8.5s/轮）。本工具把收集抽离为一次确定性预扫描：
skill 首轮前调用一次 → 返回结构化上下文（profile + history + 可选天气）→
LLM 首轮即拿到全部上下文，只做真正的推理（分析/搭配/输出）。

设计原则（M7 三段式）：
- 识别脚本：本工具 = 确定性只读收集（cat/ls/天气查询），无副作用
- 判断留 LLM：收集结果的解读/决策由 LLM 在后续轮次完成
- 与 bash.py 同模式：经 create_executor() 路由（sandbox_endpoint → 沙箱内执行）
- 收敛（2026-08-11）：不再拼 shell 命令（_run_in_sandbox 已删除），改用 executor
  能力级接口（read_file/list_dir）——I-sb-1 执行层统一，工具不感知具体沙箱实现

工具契约（_LocalToolRegistry 约定）：
TOOL_NAME / TOOL_DESCRIPTION / TOOL_PARAMS / execute(tool_input, sandbox_endpoint)
"""
import json
import os
import urllib.request
from pathlib import Path

TOOL_NAME = "preload_user_context"
TOOL_DESCRIPTION = (
    "预扫描用户上下文（档案 profile + 历史 history + 可选天气）——确定性只读收集，"
    "返回结构化 JSON。skill 首轮前调用一次即可拿到全部用户上下文，避免 LLM 逐轮"
    "探索（减少 LLM 交互轮次）。档案路径默认 .agentfoundry/memory/outfit-profile/，"
    "可通过 memory_dir 参数覆盖。天气查询可选（location 参数，Open-Meteo 免 key）。"
    "支持按需加载（load_profile/load_history/load_style_guide/load_workspace/load_env/"
    "load_weather 参数，默认全 true）——只需部分字段时显式设 false 跳过其余，"
    "返回 JSON 只含请求的字段（_hint 同步声明实际返回项），便于 debug 链路追溯"
    "（2026-08-17 优化：LLM 自选要什么，如只补查天气传 load_weather=true 其余 false）。"
)
TOOL_PARAMS = {
    "type": "object",
    "properties": {
        "memory_dir": {
            "type": "string",
            "description": "用户记忆目录（相对沙箱 workdir），默认 .agentfoundry/memory/outfit-profile",
            "default": ".agentfoundry/memory/outfit-profile",
        },
        "location": {
            "type": "string",
            "description": "查天气的地点（可选，如 '北京'/'Shanghai'；缺省跳过天气）",
        },
        "load_profile": {
            "type": "boolean",
            "description": "是否加载档案 profile.md（含 is_new_user 标记），默认 true",
            "default": True,
        },
        "load_history": {
            "type": "boolean",
            "description": "是否加载历史 history/ 最近 10 条，默认 true",
            "default": True,
        },
        "load_style_guide": {
            "type": "boolean",
            "description": "是否加载风格参考 style-guide.md，默认 true",
            "default": True,
        },
        "load_workspace": {
            "type": "boolean",
            "description": "是否加载目录树 workspace_tree（含 upload_images 确定性路径），默认 true",
            "default": True,
        },
        "load_env": {
            "type": "boolean",
            "description": "是否加载环境 env（DASHSCOPE_API_KEY 可用性 + 技能挂载），默认 true",
            "default": True,
        },
        "load_weather": {
            "type": "boolean",
            "description": "是否加载天气（仍需 location；本参数 false 时即使给 location 也不查），默认 true",
            "default": True,
        },
    },
}

# Open-Meteo 免 key 天气接口（SKILL.md 已声明真实查询用 Open-Meteo）
_OPEN_METEO_GEO = "https://geocoding-api.open-meteo.com/v1/search"
_OPEN_METEO_WEATHER = "https://api.open-meteo.com/v1/forecast"

# WMO weather code -> 中文描述（精简版，对齐 weather.py 覆盖主要 code）。
# 评估证据（CHANGE-20260810-010）：v2 轮2 LLM 在 preload 返回天气后仍 bash
# weather.py 补查——根因是 preload 只返回温度/天气码，缺湿度/体感/预报描述；
# 补全 desc 后 LLM 可直接复用，无需补查。
_WMO_DESC = {
    0: "晴", 1: "晴间多云", 2: "多云", 3: "阴",
    45: "雾", 48: "雾凇",
    51: "小毛毛雨", 53: "毛毛雨", 55: "大毛毛雨",
    56: "冻毛毛雨", 57: "强冻毛毛雨",
    61: "小雨", 63: "中雨", 65: "大雨",
    66: "冻雨", 67: "强冻雨",
    71: "小雪", 73: "中雪", 75: "大雪", 77: "雪粒",
    80: "阵雨", 81: "中阵雨", 82: "强阵雨",
    85: "阵雪", 86: "强阵雪",
    95: "雷阵雨", 96: "雷阵雨伴冰雹", 99: "强雷阵雨伴冰雹",
}


def _wmo_desc(code) -> str:
    """WMO weather code → 中文描述；未知 code 返回'未知'。"""
    try:
        return _WMO_DESC.get(int(code), "未知")
    except (TypeError, ValueError):
        return "未知"


def _fetch_weather(location: str) -> dict:
    """Open-Meteo 免 key 查询：geocoding 解析地点 → forecast 取当前天气。失败返回空 dict。"""
    try:
        # 保持系统 CA 校验；私有 CA 通过 SSL_CERT_FILE 配置，不能以
        # 跳过校验换取网络可用性。
        import ssl

        _ssl_ctx = ssl.create_default_context()
        # geocoding 必须带 language=zh，否则中文地点（如"北京"）返回空 results
        # （与 weather.py 成功先例对齐）。实测对照：name=北京&count=1 → 0 条；
        # +language=zh → 1 条。
        _geo_qs = urllib.parse.urlencode(
            {"name": location, "count": 1, "language": "zh", "format": "json"}
        )
        with urllib.request.urlopen(
            f"{_OPEN_METEO_GEO}?{_geo_qs}",
            timeout=5, context=_ssl_ctx,
        ) as r:
            geo = json.loads(r.read().decode())
        results = geo.get("results") or []
        if not results:
            return {"error": f"location not found: {location}"}
        lat, lon = results[0]["latitude"], results[0]["longitude"]
        with urllib.request.urlopen(
            f"{_OPEN_METEO_WEATHER}?latitude={lat}&longitude={lon}"
            "&current=temperature_2m,apparent_temperature,relative_humidity_2m,wind_speed_10m,weather_code"
            "&daily=temperature_2m_max,temperature_2m_min,weather_code"
            "&timezone=auto&forecast_days=5",
            timeout=5, context=_ssl_ctx,
        ) as r:
            w = json.loads(r.read().decode())
        current = w.get("current") or {}
        daily = w.get("daily") or {}
        # 5 日预报：min/max + 天气描述（LLM 可据此做温度分层/防水推理，无需 bash weather.py 补查）
        days = []
        d_times = daily.get("time") or []
        d_max = daily.get("temperature_2m_max") or []
        d_min = daily.get("temperature_2m_min") or []
        d_codes = daily.get("weather_code") or []
        for i, d in enumerate(d_times):
            days.append({
                "date": d,
                "desc": _wmo_desc(d_codes[i] if i < len(d_codes) else None),
                "max_c": round(d_max[i]) if i < len(d_max) and d_max[i] is not None else None,
                "min_c": round(d_min[i]) if i < len(d_min) and d_min[i] is not None else None,
            })
        wcode = current.get("weather_code")
        return {
            "location": location,
            "current": {
                # date = 预报首日(即"今天")——current 不带日期时 LLM 无法确认该数据对应哪一天
                "date": d_times[0] if d_times else None,
                "temp_c": current.get("temperature_2m"),
                "feels_like_c": current.get("apparent_temperature"),
                "humidity": current.get("relative_humidity_2m"),
                "wind_kmh": current.get("wind_speed_10m"),
                "weather_code": wcode,
                "desc": _wmo_desc(wcode),
            },
            "forecast": days,
        }
    except Exception as e:  # noqa: BLE001 — 天气是可选增强，失败不阻断
        return {"error": str(e)}


def execute(tool_input: dict, sandbox_endpoint: str = None) -> str:
    """预扫描用户上下文（确定性只读，无副作用）。

    按需加载（2026-08-17 优化）：load_* 参数控制各字段是否收集，默认全 true
    （向后兼容）；LLM 只需部分字段时显式设 false 跳过其余——返回 JSON 只含
    请求字段，_hint 同步声明实际返回项，便于 debug 链路追溯。
    """
    memory_dir = tool_input.get("memory_dir") or ".agentfoundry/memory/outfit-profile"
    location = tool_input.get("location") or ""
    # 按需加载开关（默认全 true = 现状行为）
    load_profile = tool_input.get("load_profile", True)
    load_history = tool_input.get("load_history", True)
    load_style_guide = tool_input.get("load_style_guide", True)
    load_workspace = tool_input.get("load_workspace", True)
    load_env = tool_input.get("load_env", True)
    load_weather = tool_input.get("load_weather", True)
    try:
        # 能力级 executor 路由（与 bash/read_file 同模式，I-sb-1 执行层统一）：
        # 不再拼 shell 命令——read_file/list_dir 由 executor 翻译路径并在沙箱内执行
        from agentflow_runtime.common.runtime_context import artifact_routing_id
        from agentflow_runtime.executors.factory import create_executor
        executor = create_executor(sandbox_endpoint=sandbox_endpoint)
        ctx: dict = {"memory_dir": memory_dir}

        # 1. 档案 profile.md（存在性 + 内容；is_new_user 依赖 profile_exists，跟随本开关）
        if load_profile:
            profile = executor.read_file(f"{memory_dir}/profile.md")
            if profile.error:
                ctx["profile_exists"] = False
                ctx["profile_content"] = ""
            else:
                ctx["profile_exists"] = True
                ctx["profile_content"] = (profile.content or "").strip()

        # 2. 历史 history/ 最近 10 条（能力级 list_dir）
        # 注：ls -t 的 mtime 排序在 executor 接口下不可得，以 name 排序近似——
        # history 文件名通常带时间戳前缀（如 20260811T100000.md），name 序≈mtime 序。
        if load_history:
            history_entries = executor.list_dir(f"{memory_dir}/history/")
            history_names = sorted(
                (e["name"] for e in history_entries if not e.get("is_dir")),
                reverse=True,
            )
            ctx["history_files"] = history_names[:10]

        # 3. 可选天气（location 参数 + load_weather 开关，双条件缺一不查）
        if location and load_weather:
            ctx["weather"] = _fetch_weather(location)

        # 3.5 静态风格参考 style_guide.md（预加载，避免 LLM 每轮 read_file 重复读取）
        # 优化依据（CHANGE-20260811-026）：v3 日志中轮1/轮2 均 read_file style-guide.md
        #（64 行静态文档），每次触发一轮额外 LLM 决策——预加载后 LLM 首轮即有，省 1 轮/次。
        # 路径：优先 AF_AGENT_PACKAGE_DIR（executor 自动翻译成沙箱路径），fallback 相对路径。
        if load_style_guide:
            agent_pkg = os.environ.get("AF_AGENT_PACKAGE_DIR", "")
            sg_paths = []
            if agent_pkg:
                sg_paths.append(f"{agent_pkg}/harness/skills/outfit-stylist/references/style-guide.md")
            sg_paths.append("harness/skills/outfit-stylist/references/style-guide.md")  # fallback 相对
            sg_out = ""
            for sg_path in sg_paths:
                sg = executor.read_file(sg_path)
                if not sg.error and (sg.content or "").strip():
                    sg_out = sg.content.strip()
                    break
            if sg_out:
                ctx["style_guide"] = sg_out

        # 3.75 workspace_tree：关键目录浅层树（防 S2/S4 探查轮）
        # 优化依据（2026-08-12 UC1 逐轮分析）：S2 轮 LLM 调 bash ls upload/xxx.png
        # 查上传图存在性、S4 轮 bash ls memory/ 查档案目录（DIR_NOT_EXIST）——两者都是
        # 确定性只读信息，preload 一次列出即可覆盖，避免各占一轮 LLM tool_calls。
        # 只列关键目录一层（非递归），避免整个 workspace 递归树膨胀上下文。
        #
        # 工作根 = 用户数据目录 workspace/{artifact_routing_id}：
        #   宿主机视角: {AF_AGENT_PACKAGE_DIR}/workspace/{routing_id}
        #   沙箱视角（经 executor._to_sandbox_path 替换 agent_package_dir 前缀）:
        #     /home/user/workspace/workspace/{routing_id}  ← 真实用户数据层（双层 workspace）
        # 背景（2026-08-12 确证）：e2b 沙箱根是 /home/user/workspace，其下还有
        # workspace/{routing_id}/ 用户层（upload/outputs/memory 都在这层）——
        # 用 AF_WORKDIR 或 "." 解析到沙箱根，列不到真实 upload（S2/S4 会误报缺失）。
        # AF_USER_ID/AF_CLIENT_ID 已由框架注入（rpc_handler → task parameter → worker env）。
        if load_workspace:
            agent_pkg = os.environ.get("AF_AGENT_PACKAGE_DIR", "")
            uid = os.environ.get("AF_USER_ID", "")
            cid = os.environ.get("AF_CLIENT_ID", "")
            if agent_pkg and uid and cid:
                routing_id = artifact_routing_id(uid, cid)
                workdir = f"{agent_pkg}/workspace/{routing_id}"
            else:
                # 兜底：无 package_dir/user/client（如独立脚本调试），退回相对 "."，
                # 依赖 executor 相对路径语义（host: 进程 cwd；e2b: 拼沙箱根）。
                workdir = os.environ.get("AF_WORKDIR", "") or "."
            tree: dict = {"workdir": workdir}
            for _dir in (".", "upload", "outputs",
                         ".agentfoundry", ".agentfoundry/memory",
                         ".agentfoundry/memory/outfit-profile",
                         ".agentfoundry/memory/outfit-profile/history"):
                entries = executor.list_dir(f"{workdir}/{_dir}" if _dir != "." else workdir)
                if not entries:
                    # list_dir 为空无法区分"目录不存在"与"目录存在但空"（executor 无
                    # stat/exists 能力）——不得断言 exists=False（空目录会被误判为不存在），
                    # 用 empty_or_missing 表达"未列出内容"，LLM 据此不误判"无此目录"。
                    tree[_dir] = {"exists": False, "empty_or_missing": True}
                    continue
                all_files = sorted(e["name"] for e in entries if not e.get("is_dir"))
                all_dirs = sorted(e["name"] for e in entries if e.get("is_dir"))
                # F7 preview 模式：截断必须明示（truncated + total），LLM 知道目录还有更多
                files = all_files[:20]  # 每目录最多 20 个文件，控体积
                dirs = all_dirs[:10]
                entry: dict = {"exists": True, "files": files, "dirs": dirs}
                if len(all_files) > len(files):
                    entry["files_truncated"] = True
                    entry["total_files"] = len(all_files)
                if len(all_dirs) > len(dirs):
                    entry["dirs_truncated"] = True
                    entry["total_dirs"] = len(all_dirs)
                # upload 目录额外透传图片确定性路径（OP-1，2026-08-16）：
                # executor.list_dir 的 entry["path"] 已是当前执行环境可直接使用的路径
                #（HostExecutor=宿主机绝对路径；e2b=沙箱内绝对路径），透传给 LLM 供
                # outfit_vision --image 直接使用，消除"LLM 从文件名拼相对路径"的方差
                #（001748 轮2 #5 拼 upload/xxx.png 失败 + find/pwd 探索的根因）。
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

        # 3.8 env：非动态环境信息探测（key 可用性 + 技能挂载，防探索性 bash）
        # 背景（2026-08-16）：SKILL.md 文生图/视频章节原本写死 `[ -n "$DASHSCOPE_API_KEY" ]`
        # 与 `ls ${SKILLS_DIR}/video-gen/...` 探测命令，每次触发一轮额外 LLM tool_call。
        # 这些是非动态环境信息（key 是否注入、技能是否挂载），由本工具确定性探测
        # 一次性返回（M7 识别脚本），LLM 直接基于返回推理，不再 bash 探测。
        if load_env:
            env_ctx: dict = {}
            # key 可用性：executor 在目标执行环境（Host=宿主机 / e2b=沙箱内）探测
            key_probe = executor.exec_command(
                '[ -n "$DASHSCOPE_API_KEY" ] && echo KEY_YES || echo KEY_NO'
            )
            env_ctx["dashscope_key_available"] = bool(
                key_probe.stdout and "KEY_YES" in key_probe.stdout
            )
            # 技能挂载：列 harness/skills 根目录，检查 forge-image-gen / video-gen
            agent_pkg = os.environ.get("AF_AGENT_PACKAGE_DIR", "")
            skills_root = f"{agent_pkg}/harness/skills" if agent_pkg else "harness/skills"
            skill_dirs = {e["name"] for e in executor.list_dir(skills_root) if e.get("is_dir")}
            env_ctx["skills"] = {
                "forge_image_gen": "forge-image-gen" in skill_dirs,
                "video_gen": "video-gen" in skill_dirs,
            }
            ctx["env"] = env_ctx

        # 4. 新用户标记（LLM 可据此走 ask_user 分支，不额外探索）
        # 依赖 profile_exists——仅 load_profile=true 时才有意义，避免误导
        if load_profile:
            ctx["is_new_user"] = not ctx["profile_exists"]

        # 5. _hint：声明本次已返回的字段（防探索性 bash，CHANGE-20260811-042）
        # LLM 看到 _hint 后不得再用 bash 探索/探测/ls 重复获取这些数据。
        # 2026-08-17 按需加载适配：hint 按实际返回项动态拼接——未请求的字段
        #（load_* = false）明确提示"未请求，可再调用本工具补取"，避免误导
        # LLM 以为该字段已返回。
        returned = [k for k in ("profile_content", "history_files", "weather", "style_guide", "workspace_tree", "env") if ctx.get(k)]
        _hint_parts = [
            "本工具已返回以下字段(权威数据,直接使用): " + ", ".join(returned) + "。",
            "**禁止再用 bash 探索/探测/重复获取这些数据**"
            "（禁止 ls 目录、[ -n 探测 key、ls weather.py/scripts/references、查看档案目录、"
            "查 upload/ 上传图存在性）。",
        ]
        if "workspace_tree" in returned:
            _hint_parts.append(
                "workspace_tree 已列出关键目录树（upload/memory/outputs），"
                "上传图存在性与档案目录结构直接看 workspace_tree；"
                "upload/ 内图片的确定性路径见 workspace_tree.upload.upload_images[].path"
                "（executor 已翻译为当前执行环境可直接使用的路径），outfit_vision --image 直接用该 path，"
                "禁止拼相对路径（如 upload/xxx.png）或 find/ls 探测图片位置。"
            )
        else:
            _hint_parts.append(
                "本次未请求 workspace_tree（load_workspace=false）——如需上传图确定性路径/目录树，"
                "可再次调用本工具并传 load_workspace=true。"
            )
        if "env" in returned:
            _hint_parts.append(
                "env 已返回非动态环境信息：dashscope_key_available（DASHSCOPE_API_KEY 是否可用）"
                "与 skills.forge_image_gen / skills.video_gen（文生图/视频技能是否挂载）——"
                "生成图/视频前直接看 env，**禁止再用 bash `[ -n $VAR ]` 探测 key 或 ls 技能目录**。"
            )
        else:
            _hint_parts.append(
                "本次未请求 env（load_env=false）——如需 DASHSCOPE_API_KEY 可用性/技能挂载，"
                "可再次调用本工具并传 load_env=true。"
            )
        ctx["_hint"] = " ".join(_hint_parts)

        return json.dumps(ctx, ensure_ascii=False)
    except Exception as e:  # noqa: BLE001 — 预扫描失败降级：返回最小上下文，不阻断 skill
        return json.dumps(
            {"memory_dir": memory_dir, "error": f"preload_user_context failed: {e}"},
            ensure_ascii=False,
        )
