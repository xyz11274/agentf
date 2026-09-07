#!/usr/bin/env python3
"""bus_route_query tool — 高德公交站点/线路查询（bus/stopname）。

skill-scoped tool，由 skill_executor 动态加载。
模块契约：TOOL_NAME / TOOL_DESCRIPTION / TOOL_PARAMS / execute(args)。
零第三方依赖：HTTP 走标准库 urllib，代理不可达时回退直连。

city 参数接受城市中文名或 adcode：中文名通过 data/city_code.json 映射为
adcode（高德 bus/stopname 要求 city 为城市编码），映射表缺失时原样透传。
"""

import json
import os
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path

TOOL_NAME = "bus_route_query"
TOOL_DESCRIPTION = (
    "Query bus stops and their serving bus lines by station/line keyword in "
    "a Chinese city via AMap bus/stopname API."
)
TOOL_PARAMS = {
    "type": "object",
    "properties": {
        "city": {
            "type": "string",
            "description": "城市：中文名（深圳）或 adcode（440300）",
        },
        "keywords": {
            "type": "string",
            "description": "站点名或线路名关键字（如 华为站 / M462）",
        },
    },
    "required": ["city", "keywords"],
    "additionalProperties": False,
}

BUS_STOPNAME_URL = "https://restapi.amap.com/v3/bus/stopname"

# city_code.json 与本脚本同包：harness/skills/amap-nearby/data/city_code.json
_CITY_CODE_FILE = Path(__file__).resolve().parents[1] / "data" / "city_code.json"

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


def _amap_key():
    return (os.environ.get("AMAP_API_KEY")
            or os.environ.get("AMAP_KEY", "")).strip()


def _load_city_codes():
    """返回 {城市名: adcode}；映射表缺失/损坏时返回空 dict（原样透传 city）。"""
    try:
        raw = json.loads(_CITY_CODE_FILE.read_text(encoding="utf-8"))
    except Exception:  # noqa: BLE001 — 映射表是增强能力，不是硬依赖
        return {}
    codes = {}
    for province in raw if isinstance(raw, list) else []:
        for city in province.get("city") or []:
            name = str(city.get("name", "")).strip()
            adcode = str(city.get("adcode", "")).strip()
            if name and adcode:
                codes[name] = adcode
    return codes


def _fmt_busstop(stop):
    item = {
        "name": stop.get("name"),
        "id": stop.get("id"),
        "location": stop.get("location"),
        "adcode": stop.get("adcode"),
    }
    lines = stop.get("buslines") or stop.get("lines") or []
    if isinstance(lines, list):
        item["buslines"] = [
            line if isinstance(line, str) else {
                "name": line.get("name"),
                "id": line.get("id"),
                "type": line.get("type"),
            }
            for line in lines
        ]
    return item


def execute(args, sandbox_endpoint=None):
    args = args if isinstance(args, dict) else {}
    city_input = str(args.get("city", "")).strip()
    keywords = str(args.get("keywords", "")).strip()
    if not city_input or not keywords:
        return {"status": "failed",
                "error": "city and keywords are required"}

    key = _amap_key()
    if not key:
        return {"status": "failed",
                "error": "AMAP_API_KEY not configured (see .env)"}

    city = city_input
    if not city_input.isdigit():
        codes = _load_city_codes()
        city = codes.get(city_input, city_input)

    params = {"key": key, "city": city, "keywords": keywords}
    try:
        req = urllib.request.Request(
            f"{BUS_STOPNAME_URL}?{urllib.parse.urlencode(params)}",
            headers={"Accept": "application/json"},
        )
        with _urlopen(req, 15) as resp:
            data = json.loads(resp.read().decode("utf-8"))
    except urllib.error.HTTPError as exc:
        body = exc.read().decode("utf-8", "replace")[:500]
        return {"status": "failed",
                "error": f"amap bus/stopname HTTP {exc.code}: {body}"}
    except Exception as exc:  # noqa: BLE001
        return {"status": "failed", "error": f"bus query failed: {exc}"}

    if str(data.get("status")) != "1":
        return {"status": "failed",
                "error": f"amap bus/stopname error: info={data.get('info')} "
                         f"infocode={data.get('infocode')}"}

    # 响应结构因版本而异（busstops / buslines 等键），无法识别时原样透传
    payload_keys = [k for k in data.keys()
                    if k not in ("status", "info", "infocode", "count",
                                 "request_id", "key")]
    result = {
        "status": "succeeded",
        "city_input": city_input,
        "city": city,
        "keywords": keywords,
    }
    if "busstops" in data:
        result["busstops"] = [_fmt_busstop(s) for s in (data["busstops"] or [])]
    else:
        result["raw"] = {k: data[k] for k in payload_keys}
    return result
