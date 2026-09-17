"""Public, live information adapters. No evaluation data or answer templates."""
from __future__ import annotations

from collections import defaultdict
from datetime import datetime, timedelta
import json
import math
import os
import re
from urllib.parse import urlencode
from urllib.request import Request, urlopen
import xml.etree.ElementTree as ET
from zoneinfo import ZoneInfo

# Coordinates are geographic lookup data, not weather fixtures. Broad regions
# deliberately expose their representative points instead of posing as one city.
PLACES = {
    "川西": [("康定", 30.05, 101.96), ("马尔康", 31.90, 102.22), ("理塘", 30.00, 100.27)],
    "江南": [("杭州", 30.27, 120.15), ("苏州", 31.30, 120.59), ("南京", 32.06, 118.80)],
    "北京": [("北京", 39.90, 116.40)], "上海": [("上海", 31.23, 121.47)],
    "成都": [("成都", 30.57, 104.07)], "杭州": [("杭州", 30.27, 120.15)],
    "广州": [("广州", 23.13, 113.26)], "深圳": [("深圳", 22.54, 114.06)],
    "康定": [("康定", 30.05, 101.96)], "理塘": [("理塘", 30.00, 100.27)],
    "南京": [("南京", 32.06, 118.80)], "武汉": [("武汉", 30.59, 114.31)],
    "西安": [("西安", 34.26, 108.94)], "重庆": [("重庆", 29.56, 106.55)],
}


def fetch_json(url, *, payload=None, headers=None, timeout=25):
    body = json.dumps(payload, ensure_ascii=False).encode() if payload is not None else None
    request = Request(url, data=body, headers={"User-Agent": "AgentFlow-Business/1.0", "Content-Type": "application/json", **(headers or {})})
    with urlopen(request, timeout=timeout) as reply:
        return json.load(reply)


def result(text, data=None, status="succeeded", source="local-business", **extra):
    return {"status": status, "simulated": False, "source": source,
            "UserOutputText": text, "UserOutputObject": data or {}, **extra}


def current_time(args):
    zone = args.get("timezone") or os.environ.get("BUSINESS_TIMEZONE", "Asia/Shanghai")
    now = datetime.now(ZoneInfo(zone))
    return result(now.isoformat(), {"datetime": now.isoformat(), "timezone": zone, "date": now.date().isoformat(), "hour": now.hour}, source="server-clock")


def weather(args):
    location = args.get("location", "").strip()
    if not location:
        return result("天气查询需要具体地点；服务器位置不代表用户位置。", status="needs_input", missing_fields=["location"])
    points = PLACES.get(location.removesuffix("市"))
    if not points:
        geo_url = "https://geocoding-api.open-meteo.com/v1/search?" + urlencode({"name": location, "language": "zh", "count": 5})
        rows = fetch_json(geo_url).get("results", [])
        if len(rows) != 1:
            return result("地点未唯一匹配，请提供城市全称或区县。", {"candidates": rows}, status="needs_input", missing_fields=["location"])
        points = [(rows[0]["name"], rows[0]["latitude"], rows[0]["longitude"])]
    today = datetime.now(ZoneInfo("Asia/Shanghai")).date()
    period = args.get("date", "")
    if args.get("start_date"):
        start = datetime.fromisoformat(args["start_date"]).date()
        end = datetime.fromisoformat(args.get("end_date", args["start_date"])).date()
    elif re.fullmatch(r"(上|下|本|这)周[一二三四五六日天]", period):
        shift = {"上":-7,"下":7,"本":0,"这":0}[period[0]]
        index = "一二三四五六日".index(period[-1].replace("天","日"))
        start = end = today - timedelta(days=today.weekday()) + timedelta(days=shift+index)
    elif period in ("周末","本周末","这周末","下周末"):
        shift = 7 if period.startswith("下") else 0
        start = today - timedelta(days=today.weekday()) + timedelta(days=shift+5); end = start + timedelta(days=1)
    elif "下周" in period:
        start = today - timedelta(days=today.weekday()) + timedelta(days=7); end = start + timedelta(days=6)
    elif "上周" in period:
        start = today - timedelta(days=today.weekday()) - timedelta(days=7); end = start + timedelta(days=6)
    elif any(word in period for word in ("本周", "这周")):
        start = today - timedelta(days=today.weekday()); end = start + timedelta(days=6)
    elif period in ("今天", "今日"):
        start = end = today
    elif period in ("明天", "明日"):
        start = end = today + timedelta(days=1)
    elif period in ("后天",):
        start = end = today + timedelta(days=2)
    elif period and re.fullmatch(r"\d{4}-\d{2}-\d{2}", period):
        start = end = datetime.fromisoformat(period).date()
    elif not period or any(word in period for word in ("未来", "一周", "7天", "七天")):
        start = today; end = today + timedelta(days=6)
    else:
        return result("请将查询日期明确为起止日期。", status="needs_input", missing_fields=["start_date", "end_date"])
    if end < start or start.year < 1940 or end > today+timedelta(days=15) or (end-start).days>366:
        return result("日期需要在1940年之后、未来16天以内，单次最多367天。", status="unsupported")
    historical = start < today-timedelta(days=92)
    if historical and end > today-timedelta(days=6):
        return result("长跨度查询请分别查询历史和近期时段，以区分历史再分析与预报。",status="needs_input",missing_fields=["end_date"])
    variables = ["temperature_2m","relative_humidity_2m","wind_speed_10m", "precipitation" if historical else "precipitation_probability"]
    query = {"latitude": ",".join(str(p[1]) for p in points), "longitude": ",".join(str(p[2]) for p in points),
             "hourly": ",".join(variables),
             "timezone": "Asia/Shanghai", "start_date": start.isoformat(), "end_date": end.isoformat()}
    endpoint = "https://archive-api.open-meteo.com/v1/archive" if historical else "https://api.open-meteo.com/v1/forecast"
    url = endpoint+"?"+urlencode(query)
    payload = fetch_json(url)
    payload = payload if isinstance(payload, list) else [payload]
    if len(payload) != len(points):
        raise ValueError("weather provider returned incomplete locations")
    locations = []; all_humidity = []; summaries = []
    for point, item in zip(points, payload):
        hourly = item.get("hourly", {}); days = defaultdict(lambda: defaultdict(list))
        for index, stamp in enumerate(hourly.get("time", [])):
            for variable in variables:
                values = hourly.get(variable, [])
                if index < len(values) and isinstance(values[index],(int,float)) and not isinstance(values[index],bool) and math.isfinite(values[index]):
                    days[stamp[:10]][variable].append(values[index])
        daily = []; local_humidity = []
        for day, values in sorted(days.items()):
            humidity = values["relative_humidity_2m"]
            if not humidity:
                continue
            all_humidity.extend(humidity)
            local_humidity.extend(humidity)
            daily.append({"date": day, "humidity_mean_pct": round(sum(humidity)/len(humidity), 1),
                          "humidity_min_pct": min(humidity), "humidity_max_pct": max(humidity),
                          "temperature_min_c": min(values["temperature_2m"], default=None),
                          "temperature_max_c": max(values["temperature_2m"], default=None),
                          "rain_probability_max_pct": max(values["precipitation_probability"], default=None),
                          "precipitation_sum_mm": round(sum(values["precipitation"]),2) if values["precipitation"] else None,
                          "wind_max_kmh": max(values["wind_speed_10m"], default=None)})
        summaries.append({"name": point[0],
                          "humidity_mean_pct": round(sum(local_humidity)/len(local_humidity), 1) if local_humidity else None,
                          "humidity_samples": len(local_humidity),
                          "temperature_min_c": min((d["temperature_min_c"] for d in daily if d["temperature_min_c"] is not None), default=None),
                          "temperature_max_c": max((d["temperature_max_c"] for d in daily if d["temperature_max_c"] is not None), default=None),
                          "daily_high_min_c": min((d["temperature_max_c"] for d in daily if d["temperature_max_c"] is not None), default=None)})
        locations.append({"name": point[0], "latitude": point[1], "longitude": point[2], "daily": daily})
    if not all_humidity:
        return result("天气接口没有返回可用湿度。", status="failed", source="Open-Meteo")
    mean = round(sum(all_humidity)/len(all_humidity), 1)
    data = {"location": location, "start_date": start.isoformat(), "end_date": end.isoformat(),
            "retrieved_at": datetime.now().astimezone().isoformat(), "location_summaries": summaries, "locations": locations,
            "representative_points_only": len(points) > 1, "humidity_mean_pct": mean,
            "humidity_assessment": {"is_humid": mean >= 70, "threshold_pct": 70,
                                    "basis": "代表点查询时段的平均相对湿度>=70%称偏大，仅为本次描述口径；不是用药或户外安全标准"},
            "sources": [{"title": "Open-Meteo historical reanalysis" if historical else "Open-Meteo forecast", "url": url}], "live_data_available": True,
            "data_kind": "historical_reanalysis_not_original_forecast" if historical else "weather_model_forecast_and_recent_model_estimates"}
    return result(f"{location} {start}至{end}，代表点平均相对湿度{mean}%；逐日湿度、降水与风速见结构化结果；历史再分析不代表当时发布的预报。", data, source="Open-Meteo")


def search(args):
    query = args.get("query", "").strip()
    if not query:
        return result("请提供查询词。", status="needs_input", missing_fields=["query"])
    if os.environ.get("WEBSEARCH_BASE_URL") and os.environ.get("WEBSEARCH_API_KEY"):
        from ._live_search import execute
        live = execute({"query": query})
        if live.get("status") != "succeeded":
            return result("配置的搜索服务调用失败。", status="failed", source="configured-web-search")
        return result(live.get("results", ""), {"results": live.get("results", ""), "sources": live.get("sources", []), "live_data_available": True}, source="configured-web-search")
    url = "https://www.bing.com/search?" + urlencode({"q": query, "format": "rss", "setlang": "zh-hans"})
    with urlopen(Request(url, headers={"User-Agent": "AgentFlow-Business/1.0"}), timeout=25) as reply:
        raw = reply.read(2*1024*1024)
    root = ET.fromstring(raw)
    items = [{"title": node.findtext("title", ""), "url": node.findtext("link", ""),
              "snippet": node.findtext("description", ""), "published": node.findtext("pubDate", "")}
             for node in root.findall("./channel/item")][:8]
    return result("公开网页搜索结果；这些是来源摘要，须核对相关性，不代表已验证事实。",
                  {"query": query, "results": items, "sources": [{"title": r["title"], "url": r["url"]} for r in items],
                   "retrieved_at": datetime.now().astimezone().isoformat(), "live_data_available": True},
                  status="succeeded" if items else "not_found", source="Bing RSS")
