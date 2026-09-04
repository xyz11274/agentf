#!/usr/bin/env python3
"""nearby_poi_search tool — 高德周边 POI 搜索（place/around）。

skill-scoped tool，由 skill_executor 动态加载。
模块契约：TOOL_NAME / TOOL_DESCRIPTION / TOOL_PARAMS / execute(args)。
零第三方依赖：HTTP 走标准库 urllib，代理不可达时回退直连。
只有 address 没有 location 时，内部先调 geocode 换坐标。
"""

import json
import os
import urllib.error
import urllib.parse
import urllib.request

TOOL_NAME = "nearby_poi_search"
TOOL_DESCRIPTION = (
    "Search nearby POIs (restaurants, shops, hospitals, gas stations, …) "
    "around a coordinate or address via AMap place/around API."
)
TOOL_PARAMS = {
    "type": "object",
    "properties": {
        "location": {
            "type": "string",
            "description": "中心点坐标 \"lng,lat\"（与 address 二选一）",
        },
        "address": {
            "type": "string",
            "description": "中心点地址（与 location 二选一；内部自动 geocode）",
        },
        "keywords": {
            "type": "string",
            "description": "搜索关键字（如 川菜/超市/幼儿园）；keywords/types 至少一个",
        },
        "types": {
            "type": "string",
            "description": "POI 类型代码（如 050000=餐饮服务，050500=中餐厅），多个用 | 分隔",
        },
        "radius": {
            "type": "integer",
            "minimum": 0,
            "maximum": 50000,
            "description": "搜索半径（米），默认 1000",
        },
        "offset": {
            "type": "integer",
            "minimum": 1,
            "maximum": 25,
            "description": "每页条数，默认 10",
        },
        "page": {
            "type": "integer",
            "minimum": 1,
            "maximum": 100,
            "description": "页码，默认 1",
        },
    },
    "additionalProperties": False,
}

AROUND_URL = "https://restapi.amap.com/v3/place/around"
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


def _resolve_location(args, key):
    """location 参数优先；否则用 address geocode。返回 (location, error)。"""
    location = str(args.get("location", "")).strip()
    if location:
        return location, None
    address = str(args.get("address", "")).strip()
    if not address:
        return None, "location or address is required"
    try:
        data = http_get_json(
            f"{GEOCODE_URL}?" + urllib.parse.urlencode(
                {"address": address, "key": key, "output": "json"}))
    except Exception as exc:  # noqa: BLE001
        return None, f"geocode failed for '{address}': {exc}"
    if str(data.get("status")) != "1" or not (data.get("geocodes") or []):
        return None, f"geocode returned no result for '{address}'"
    return data["geocodes"][0].get("location"), None


def _fmt_poi(poi):
    biz = poi.get("biz_ext") or {}
    item = {
        "name": poi.get("name"),
        "address": poi.get("address") if isinstance(poi.get("address"), str) else "",
        "location": poi.get("location"),
        "type": poi.get("type"),
    }
    for key in ("distance", "tel", "id"):
        if poi.get(key):
            item[key] = poi.get(key)
    rating = biz.get("rating") or biz.get("cost")
    if biz.get("rating"):
        item["rating"] = biz["rating"]
    if biz.get("cost"):
        item["avg_cost"] = biz["cost"]
    return item


def execute(args):
    args = args if isinstance(args, dict) else {}
    keywords = str(args.get("keywords", "")).strip()
    types = str(args.get("types", "")).strip()
    if not keywords and not types:
        return {"status": "failed",
                "error": "keywords or types is required (e.g. keywords='川菜')"}

    key = _amap_key()
    if not key:
        return {"status": "failed",
                "error": "AMAP_API_KEY not configured (see .env)"}

    location, err = _resolve_location(args, key)
    if err:
        return {"status": "failed", "error": err}

    def _int(name, default, lo, hi):
        try:
            value = int(args.get(name) or default)
        except (TypeError, ValueError):
            value = default
        return max(lo, min(hi, value))

    params = {
        "key": key,
        "location": location,
        "radius": _int("radius", 1000, 0, 50000),
        "offset": _int("offset", 10, 1, 25),
        "page": _int("page", 1, 1, 100),
        "output": "json",
        "extensions": "all",
    }
    if keywords:
        params["keywords"] = keywords
    if types:
        params["types"] = types

    try:
        data = http_get_json(f"{AROUND_URL}?{urllib.parse.urlencode(params)}")
    except urllib.error.HTTPError as exc:
        body = exc.read().decode("utf-8", "replace")[:500]
        return {"status": "failed",
                "error": f"amap place/around HTTP {exc.code}: {body}"}
    except Exception as exc:  # noqa: BLE001
        return {"status": "failed", "error": f"nearby search failed: {exc}"}

    if str(data.get("status")) != "1":
        return {"status": "failed",
                "error": f"amap place/around error: info={data.get('info')} "
                         f"infocode={data.get('infocode')}"}

    pois = data.get("pois") or []
    return {
        "status": "succeeded",
        "location": location,
        "keywords": keywords or None,
        "radius_m": params["radius"],
        "total": data.get("count"),
        "pois": [_fmt_poi(p) for p in pois],
    }
