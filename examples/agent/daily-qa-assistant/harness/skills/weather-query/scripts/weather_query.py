#!/usr/bin/env python3
"""weather_query tool — 心知天气 v3 实时天气 + 预报查询。

skill-scoped tool，由 skill_executor 动态加载。
模块契约：TOOL_NAME / TOOL_DESCRIPTION / TOOL_PARAMS / execute(args)。
零第三方依赖：HTTP 走标准库 urllib，代理不可达时回退直连。
"""

import json
import os
import urllib.error
import urllib.parse
import urllib.request

TOOL_NAME = "weather_query"
TOOL_DESCRIPTION = (
    "Query real-time weather and optional multi-day forecast from Seniverse "
    "(心知天气). Pass a city name (Chinese or pinyin) or adcode."
)
TOOL_PARAMS = {
    "type": "object",
    "properties": {
        "location": {
            "type": "string",
            "description": "城市：中文名（东莞）、拼音（dongguan）或 adcode（441900）",
        },
        "days": {
            "type": "integer",
            "minimum": 0,
            "maximum": 3,
            "description": "0=仅实况（默认）；1~3=附带未来 N 天逐日预报",
        },
        "unit": {
            "type": "string",
            "enum": ["c", "f"],
            "description": "温度单位：c 摄氏（默认）/ f 华氏",
        },
    },
    "required": ["location"],
    "additionalProperties": False,
}

API_BASE = "https://api.seniverse.com/v3/weather"

# 与 forge-image-gen 同款：沙箱继承宿主 HTTP(S)_PROXY 时代理可能拒连公网端点，
# 代理失败回退直连（真实 HTTP 4xx/5xx 不重试）。
_DIRECT_OPENER = urllib.request.build_opener(urllib.request.ProxyHandler({}))


def _urlopen(req, timeout):
    try:
        return urllib.request.urlopen(req, timeout=timeout)
    except urllib.error.HTTPError:
        raise
    except urllib.error.URLError:
        fresh = urllib.request.Request(
            req.full_url, data=req.data,
            headers=dict(req.header_items()), method=req.get_method(),
        )
        return _DIRECT_OPENER.open(fresh, timeout=timeout)


def _http_get_json(url, timeout=15):
    req = urllib.request.Request(url, headers={"Accept": "application/json"})
    with _urlopen(req, timeout) as resp:
        return json.loads(resp.read().decode("utf-8"))


def _extract_location(entry):
    loc = entry.get("location") or {}
    name = loc.get("name") or loc.get("path") or ""
    return name


def _fmt_now(now):
    parts = {
        "text": now.get("text"),
        "temperature": now.get("temperature"),
    }
    for key in ("humidity", "wind_direction", "wind_scale", "wind_speed",
                "feels_like", "pressure", "visibility", "code"):
        if now.get(key) not in (None, ""):
            parts[key] = now.get(key)
    return parts


def _fmt_daily(days):
    out = []
    for day in days or []:
        item = {
            "date": day.get("date"),
            "day": day.get("text_day"),
            "night": day.get("text_night"),
            "high_c": day.get("high"),
            "low_c": day.get("low"),
        }
        for key in ("wind_direction_day", "wind_scale_day", "humidity",
                    "precip", "wind_direction_night", "wind_scale_night"):
            if day.get(key) not in (None, ""):
                item[key] = day.get(key)
        out.append(item)
    return out


def execute(args, sandbox_endpoint=None):
    args = args if isinstance(args, dict) else {}
    location = str(args.get("location", "")).strip()
    if not location:
        return {"status": "failed", "error": "location is required"}

    key = (os.environ.get("SENIVERSE_API_KEY")
           or os.environ.get("SNOW_API_KEY", "")).strip()
    if not key:
        return {"status": "failed",
                "error": "SENIVERSE_API_KEY not configured (see .env)"}

    try:
        days = int(args.get("days") or 0)
    except (TypeError, ValueError):
        days = 0
    days = max(0, min(3, days))
    unit = "f" if str(args.get("unit", "c")).strip().lower() == "f" else "c"
    language = str(os.environ.get("SENIVERSE_LANGUAGE", "zh-Hans"))

    result = {"status": "succeeded", "location": location, "unit": unit}

    try:
        now_url = (
            f"{API_BASE}/now.json?" + urllib.parse.urlencode({
                "key": key, "location": location,
                "language": language, "unit": unit,
            })
        )
        data = _http_get_json(now_url)
        results = data.get("results") or []
        if not results:
            return {"status": "failed",
                    "error": f"empty results from seniverse: {data.get('status', '')}"}
        entry = results[0]
        result["location_name"] = _extract_location(entry)
        result["now"] = _fmt_now(entry.get("now") or {})
        result["last_update"] = entry.get("last_update")
    except urllib.error.HTTPError as exc:
        body = exc.read().decode("utf-8", "replace")[:500]
        return {"status": "failed",
                "error": f"seniverse now API HTTP {exc.code}: {body}"}
    except Exception as exc:  # noqa: BLE001 — 统一业务失败出口
        return {"status": "failed", "error": f"weather now query failed: {exc}"}

    if days > 0:
        try:
            daily_url = (
                f"{API_BASE}/daily.json?" + urllib.parse.urlencode({
                    "key": key, "location": location,
                    "language": language, "unit": unit,
                    "start": 0, "days": days,
                })
            )
            data = _http_get_json(daily_url)
            results = data.get("results") or []
            result["forecast"] = _fmt_daily(
                (results[0].get("daily") if results else None) or [])
        except Exception as exc:  # noqa: BLE001 — 实况已成功，预报失败降级
            result["forecast_error"] = str(exc)

    return result
