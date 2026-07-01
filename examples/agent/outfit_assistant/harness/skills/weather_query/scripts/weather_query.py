"""weather_query tool — query weather via QWeather API.

Returns temperature, humidity, UV index, and dressing advice index.
Requires QWEATHER_API_KEY and QWEATHER_API_HOST environment variables.

API docs: https://dev.qweather.com/docs/api
"""
import gzip
import json
import os
import urllib.parse
import urllib.request

TOOL_NAME = "weather_query"
TOOL_DESCRIPTION = "查询指定城市的实时天气和穿衣指数。返回温度、湿度、紫外线和生活指数。"
TOOL_PARAMS = {
    "type": "object",
    "properties": {
        "city": {
            "type": "string",
            "description": "城市名称（中文或拼音），如 '北京' 或 'beijing'"
        }
    },
    "required": ["city"]
}


def _get_api_key():
    return os.environ.get("QWEATHER_API_KEY", "")


def _get_api_host():
    return os.environ.get("QWEATHER_API_HOST", "")


def _request(path, params):
    """Send authenticated GET request to QWeather API."""
    api_key = _get_api_key()
    api_host = _get_api_host()
    if not api_key or not api_host:
        return None, "QWEATHER_API_KEY 或 QWEATHER_API_HOST 未配置"

    query = urllib.parse.urlencode(params)
    url = f"https://{api_host}{path}?{query}"
    req = urllib.request.Request(url, headers={
        "X-QW-Api-Key": api_key,
        "Accept-Encoding": "gzip",
    })
    try:
        with urllib.request.urlopen(req, timeout=10) as resp:
            raw = resp.read()
            if raw[:2] == b'\x1f\x8b':
                raw = gzip.decompress(raw)
            return json.loads(raw.decode("utf-8")), None
    except Exception as e:
        return None, str(e)


def _geolocation(city):
    """Get city location ID via QWeather Geo API v2."""
    data, err = _request("/geo/v2/city/lookup", {"location": city})
    if err:
        return None, err
    if data.get("code") == "200" and data.get("location"):
        return data["location"][0]["id"], None
    return None, f"城市未找到: {city}"


def _weather_now(location_id):
    """Get current weather via QWeather Weather Now API."""
    data, err = _request("/v7/weather/now", {"location": location_id})
    if err:
        return None, err
    if data.get("code") == "200":
        return data.get("now", {}), None
    return None, f"天气查询失败: {data.get('code')}"


def _indices(location_id, types="3,5"):
    """Get living indices (3=dressing, 5=UV) via QWeather Indices API."""
    data, err = _request("/v7/indices/1d", {"location": location_id, "type": types})
    if err:
        return None, err
    if data.get("code") == "200":
        return data.get("daily", []), None
    return None, f"指数查询失败: {data.get('code')}"


def execute(params):
    city = params.get("city", "")

    if not _get_api_key() or not _get_api_host():
        return json.dumps({
            "error": "QWEATHER_API_KEY 或 QWEATHER_API_HOST 未设置",
            "hint": "请设置和风天气 API key 和 host 环境变量"
        }, ensure_ascii=False)

    try:
        # Step 1: geolocation
        location_id, err = _geolocation(city)
        if err:
            return json.dumps({"error": err, "city": city}, ensure_ascii=False)

        # Step 2: current weather
        now, err = _weather_now(location_id)
        if err:
            return json.dumps({"error": err, "city": city}, ensure_ascii=False)

        # Step 3: living indices (dressing + UV)
        indices_list, err = _indices(location_id, "3,5")
        indices = {}
        if indices_list:
            for idx in indices_list:
                idx_type = idx.get("type")
                if idx_type == "3":
                    indices["dressing"] = {
                        "level": idx.get("level", ""),
                        "category": idx.get("category", ""),
                        "advice": idx.get("text", "")
                    }
                elif idx_type == "5":
                    indices["uv"] = {
                        "level": idx.get("level", ""),
                        "category": idx.get("category", ""),
                        "advice": idx.get("text", "")
                    }

        result = {
            "city": city,
            "temperature": now.get("temp", ""),
            "feels_like": now.get("feelsLike", ""),
            "humidity": now.get("humidity", ""),
            "text": now.get("text", ""),
            "wind": f"{now.get('windDir', '')} {now.get('windScale', '')}级",
            "pressure": now.get("pressure", ""),
            "indices": indices
        }
        return json.dumps(result, ensure_ascii=False)

    except Exception as e:
        return json.dumps({
            "error": f"天气查询异常: {str(e)}",
            "city": city
        }, ensure_ascii=False)
