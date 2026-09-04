#!/usr/bin/env python3
"""geocode tool — 高德地图地址转坐标。

skill-scoped tool，由 skill_executor 动态加载。
模块契约：TOOL_NAME / TOOL_DESCRIPTION / TOOL_PARAMS / execute(args)。
零第三方依赖：HTTP 走标准库 urllib，代理不可达时回退直连。
"""

import json
import os
import urllib.error
import urllib.parse
import urllib.request

TOOL_NAME = "geocode"
TOOL_DESCRIPTION = (
    "Geocode a Chinese address via AMap (高德地理编码): address -> lng/lat "
    "coordinates plus province/city/district metadata."
)
TOOL_PARAMS = {
    "type": "object",
    "properties": {
        "address": {
            "type": "string",
            "description": "地址（如 东莞松山湖 / 深圳市南山区科技园）",
        },
        "city": {
            "type": "string",
            "description": "可选：限定查询城市（城市名或 adcode），提高匹配精度",
        },
    },
    "required": ["address"],
    "additionalProperties": False,
}

GEOCODE_URL = "https://restapi.amap.com/v3/geocode/geo"

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


def http_get_json(url, timeout=15):
    req = urllib.request.Request(url, headers={"Accept": "application/json"})
    with _urlopen(req, timeout) as resp:
        return json.loads(resp.read().decode("utf-8"))


def execute(args):
    args = args if isinstance(args, dict) else {}
    address = str(args.get("address", "")).strip()
    if not address:
        return {"status": "failed", "error": "address is required"}

    key = _amap_key()
    if not key:
        return {"status": "failed",
                "error": "AMAP_API_KEY not configured (see .env)"}

    params = {"address": address, "key": key, "output": "json"}
    city = str(args.get("city", "")).strip()
    if city:
        params["city"] = city

    try:
        data = http_get_json(f"{GEOCODE_URL}?{urllib.parse.urlencode(params)}")
    except urllib.error.HTTPError as exc:
        body = exc.read().decode("utf-8", "replace")[:500]
        return {"status": "failed",
                "error": f"amap geocode HTTP {exc.code}: {body}"}
    except Exception as exc:  # noqa: BLE001 — 统一业务失败出口
        return {"status": "failed", "error": f"geocode failed: {exc}"}

    if str(data.get("status")) != "1":
        return {"status": "failed",
                "error": f"amap geocode error: info={data.get('info')} "
                         f"infocode={data.get('infocode')}"}

    geocodes = data.get("geocodes") or []
    if not geocodes:
        return {"status": "failed",
                "error": f"no geocode result for address: {address}"}

    first = geocodes[0]
    return {
        "status": "succeeded",
        "address": address,
        "formatted_address": first.get("formatted_address"),
        "location": first.get("location"),
        "country": first.get("country"),
        "province": first.get("province"),
        "city": first.get("city") if isinstance(first.get("city"), str) else "",
        "district": first.get("district") if isinstance(first.get("district"), str) else "",
        "level": first.get("level"),
        "candidates": [
            {"formatted_address": g.get("formatted_address"),
             "location": g.get("location"),
             "district": g.get("district") if isinstance(g.get("district"), str) else ""}
            for g in geocodes[1:4]
        ],
    }
