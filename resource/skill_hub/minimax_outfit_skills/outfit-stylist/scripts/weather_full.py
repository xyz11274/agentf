#!/usr/bin/env python3
"""Comprehensive weather query for one city + one date (default: tomorrow)."""
import json, sys, urllib.parse, urllib.request
from datetime import date, timedelta

GEO = "https://geocoding-api.open-meteo.com/v1/search"
FORECAST = "https://api.open-meteo.com/v1/forecast"
AIR = "https://air-quality-api.open-meteo.com/v1/air-quality"

def _get(url, params):
    qs = urllib.parse.urlencode(params)
    req = urllib.request.Request(f"{url}?{qs}", headers={"User-Agent": "agentfoundry-outfit/1.0"})
    with urllib.request.urlopen(req, timeout=20) as r:
        return json.loads(r.read().decode("utf-8"))

def main():
    city = sys.argv[1] if len(sys.argv) > 1 else "北京"
    target = (date.today() + timedelta(days=1)).isoformat()

    geo = _get(GEO, {"name": city, "count": 1, "language": "zh", "format": "json"})
    loc = geo["results"][0]
    lat, lon = loc["latitude"], loc["longitude"]

    fc = _get(FORECAST, {
        "latitude": lat, "longitude": lon, "timezone": "auto", "forecast_days": 7,
        "daily": ",".join([
            "temperature_2m_max","temperature_2m_min","apparent_temperature_max","apparent_temperature_min",
            "precipitation_sum","precipitation_probability_max","rain_sum","showers_sum","snowfall_sum",
            "weather_code","wind_speed_10m_max","wind_gusts_10m_max","wind_direction_10m_dominant",
            "uv_index_max","sunrise","sunset","relative_humidity_2m_mean","surface_pressure_mean"
        ]),
        "hourly": ",".join([
            "temperature_2m","precipitation","precipitation_probability","weather_code",
            "wind_speed_10m","wind_gusts_10m","wind_direction_10m","relative_humidity_2m","uv_index"
        ]),
    })

    aq = _get(AIR, {
        "latitude": lat, "longitude": lon, "timezone": "auto", "forecast_days": 7,
        "hourly": ",".join([
            "pm10","pm2_5","carbon_monoxide","nitrogen_dioxide","sulphur_dioxide","ozone",
            "european_aqi","us_aqi"
        ]),
    })

    days = fc["daily"]
    idx = days["time"].index(target)
    daily = {k: days[k][idx] for k in days.keys()}

    hrs = fc["hourly"]
    h_idx = [i for i, t in enumerate(hrs["time"]) if t.startswith(target)]
    def _hslice(key):
        return [hrs[key][i] for i in h_idx]
    hourly = {
        "time": _hslice("time"),
        "temp": [round(x,1) if x is not None else None for x in _hslice("temperature_2m")],
        "precip_mm": _hslice("precipitation"),
        "precip_prob_pct": _hslice("precipitation_probability"),
        "weather_code": _hslice("weather_code"),
        "wind_kmh": _hslice("wind_speed_10m"),
        "gust_kmh": _hslice("wind_gusts_10m"),
        "wind_dir_deg": _hslice("wind_direction_10m"),
        "humidity_pct": _hslice("relative_humidity_2m"),
        "uv_index": _hslice("uv_index"),
    }
    peak_precip = max(hourly["precip_mm"], key=lambda x: (x is None, x)) if hourly["precip_mm"] else 0
    peak_idx = hourly["precip_mm"].index(peak_precip) if peak_precip and peak_precip > 0 else None

    aqhrs = aq["hourly"]
    aq_idx = [i for i, t in enumerate(aqhrs["time"]) if t.startswith(target)]
    def _aq_h(k):
        return [aqhrs[k][i] for i in aq_idx]
    def _mean(vals):
        clean = [v for v in vals if v is not None]
        return round(sum(clean)/len(clean), 1) if clean else None
    def _max(vals):
        clean = [v for v in vals if v is not None]
        return round(max(clean), 1) if clean else None
    air = {
        "pm2_5_mean": _mean(_aq_h("pm2_5")),
        "pm2_5_max": _max(_aq_h("pm2_5")),
        "pm10_mean": _mean(_aq_h("pm10")),
        "pm10_max": _max(_aq_h("pm10")),
        "no2_mean": _mean(_aq_h("nitrogen_dioxide")),
        "so2_mean": _mean(_aq_h("sulphur_dioxide")),
        "o3_max": _max(_aq_h("ozone")),
        "co_mean": _mean(_aq_h("carbon_monoxide")),
        "eu_aqi_max": _max(_aq_h("european_aqi")),
        "us_aqi_max": _max(_aq_h("us_aqi")),
    }

    out = {
        "query": {"city": loc.get("name"), "region": loc.get("admin1"), "country": loc.get("country"),
                  "latitude": lat, "longitude": lon, "date": target},
        "daily": {
            "temp_max": daily["temperature_2m_max"],
            "temp_min": daily["temperature_2m_min"],
            "feels_like_max": daily["apparent_temperature_max"],
            "feels_like_min": daily["apparent_temperature_min"],
            "weather_code": daily["weather_code"],
            "precip_sum_mm": daily["precipitation_sum"],
            "precip_prob_max_pct": daily["precipitation_probability_max"],
            "rain_sum_mm": daily["rain_sum"],
            "showers_sum_mm": daily["showers_sum"],
            "snowfall_cm": daily["snowfall_sum"],
            "wind_speed_max_kmh": daily["wind_speed_10m_max"],
            "wind_gust_max_kmh": daily["wind_gusts_10m_max"],
            "wind_dir_dominant_deg": daily["wind_direction_10m_dominant"],
            "uv_index_max": daily["uv_index_max"],
            "humidity_mean_pct": daily["relative_humidity_2m_mean"],
            "pressure_mean_hpa": daily["surface_pressure_mean"],
            "sunrise": daily["sunrise"],
            "sunset": daily["sunset"],
        },
        "peak_precip_hour": hourly["time"][peak_idx] if peak_idx is not None else None,
        "peak_precip_mm": peak_precip if peak_idx is not None else 0,
        "air_quality": air,
        "source": "Open-Meteo forecast + air-quality",
    }
    print(json.dumps(out, ensure_ascii=False, indent=2))

if __name__ == "__main__":
    main()