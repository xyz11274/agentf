#!/usr/bin/env python3
"""Fetch REAL current weather + 5-day forecast for a city.

Uses Open-Meteo (free, no API key, Chinese geocoding) via the stdlib only — no
pip install needed in the sandbox. Prints a single JSON object to stdout that the
outfit-stylist skill reads to ground its recommendation (temp / feels_like /
desc / wind decide layering, waterproofing, etc.). Vendored from the `weather`
skill so outfit-stylist is self-contained.

Usage:  python3 weather.py "北京"
        python3 weather.py "Tokyo"
"""
import json
import sys
import urllib.parse
import urllib.request

GEO = "https://geocoding-api.open-meteo.com/v1/search"
FORECAST = "https://api.open-meteo.com/v1/forecast"

# WMO weather code -> (中文描述, emoji)
WMO = {
    0: ("晴", "☀️"),
    1: ("晴间多云", "🌤️"), 2: ("多云", "⛅"), 3: ("阴", "☁️"),
    45: ("雾", "🌫️"), 48: ("雾凇", "🌫️"),
    51: ("小毛毛雨", "🌦️"), 53: ("毛毛雨", "🌦️"), 55: ("大毛毛雨", "🌧️"),
    56: ("冻毛毛雨", "🌧️"), 57: ("强冻毛毛雨", "🌧️"),
    61: ("小雨", "🌦️"), 63: ("中雨", "🌧️"), 65: ("大雨", "🌧️"),
    66: ("冻雨", "🌧️"), 67: ("强冻雨", "🌧️"),
    71: ("小雪", "🌨️"), 73: ("中雪", "🌨️"), 75: ("大雪", "❄️"), 77: ("雪粒", "🌨️"),
    80: ("阵雨", "🌦️"), 81: ("中阵雨", "🌧️"), 82: ("强阵雨", "⛈️"),
    85: ("阵雪", "🌨️"), 86: ("强阵雪", "❄️"),
    95: ("雷阵雨", "⛈️"), 96: ("雷阵雨伴冰雹", "⛈️"), 99: ("强雷阵雨伴冰雹", "⛈️"),
}
WEEKDAYS = ["周一", "周二", "周三", "周四", "周五", "周六", "周日"]


def _get(url, params):
    qs = urllib.parse.urlencode(params)
    req = urllib.request.Request(f"{url}?{qs}", headers={"User-Agent": "agentfoundry-outfit/1.0"})
    with urllib.request.urlopen(req, timeout=15) as r:
        return json.loads(r.read().decode("utf-8"))


def desc(code):
    return WMO.get(int(code), ("未知", "❓"))


def main():
    if len(sys.argv) < 2 or not sys.argv[1].strip():
        print(json.dumps({"error": "缺少城市名。用法: weather.py <城市>"}, ensure_ascii=False))
        return
    city = sys.argv[1].strip()
    try:
        geo = _get(GEO, {"name": city, "count": 1, "language": "zh", "format": "json"})
        results = geo.get("results") or []
        if not results:
            print(json.dumps({"error": f"找不到城市「{city}」"}, ensure_ascii=False))
            return
        loc = results[0]
        lat, lon = loc["latitude"], loc["longitude"]
        name = loc.get("name", city)
        admin = loc.get("admin1", "")
        country = loc.get("country", "")

        fc = _get(FORECAST, {
            "latitude": lat, "longitude": lon,
            "current": "temperature_2m,relative_humidity_2m,apparent_temperature,weather_code,wind_speed_10m,surface_pressure",
            "daily": "temperature_2m_max,temperature_2m_min,weather_code",
            "timezone": "auto", "forecast_days": 5,
        })
        cur = fc.get("current", {})
        cur_desc, cur_emoji = desc(cur.get("weather_code", -1))
        daily = fc.get("daily", {})
        days = []
        dates = daily.get("time", [])
        for i, d in enumerate(dates):
            dd, de = desc(daily.get("weather_code", [])[i])
            # weekday from ISO date
            try:
                import datetime
                wd = WEEKDAYS[datetime.date.fromisoformat(d).weekday()]
            except Exception:
                wd = d
            days.append({
                "date": d, "weekday": wd, "desc": dd, "emoji": de,
                "max": round(daily.get("temperature_2m_max", [])[i]),
                "min": round(daily.get("temperature_2m_min", [])[i]),
            })

        out = {
            "city": name,
            "region": (admin + " · " + country).strip(" ·") if (admin or country) else "",
            "current": {
                "temp": round(cur.get("temperature_2m", 0)),
                "feels_like": round(cur.get("apparent_temperature", 0)),
                "desc": cur_desc, "emoji": cur_emoji,
                "humidity": round(cur.get("relative_humidity_2m", 0)),
                "wind": round(cur.get("wind_speed_10m", 0)),
                "pressure": round(cur.get("surface_pressure", 0)),
            },
            "forecast": days,
            "source": "Open-Meteo",
        }
        print(json.dumps(out, ensure_ascii=False))
    except Exception as e:  # network / parse failure — return a clean error
        print(json.dumps({"error": f"获取天气失败: {e}"}, ensure_ascii=False))


if __name__ == "__main__":
    main()
