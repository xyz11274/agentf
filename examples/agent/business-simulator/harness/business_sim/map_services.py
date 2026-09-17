"""Credential-free public geocoding and road routing adapters."""
from __future__ import annotations

from datetime import datetime, timezone
from urllib.parse import quote, urlencode

from .live_information import fetch_json, result

NOMINATIM = "https://nominatim.openstreetmap.org"
OSRM = "https://router.project-osrm.org"


def _coordinates(value: str | None, field: str):
    if not value:
        return None
    try:
        lon, lat = [float(part.strip()) for part in value.split(",", 1)]
    except (ValueError, AttributeError):
        raise ValueError(f"{field} 需要 WGS84 经度,纬度")
    if not -180 <= lon <= 180 or not -90 <= lat <= 90:
        raise ValueError(f"{field} 经纬度超出范围")
    return lon, lat


def geocode_text(location: str, city: str | None = None):
    if not location or location in ("当前位置", "我的位置", "这里"):
        return None, result("公共地图不能读取用户当前位置，请提供起点地址或 WGS84 坐标。",
                            status="needs_input", missing_fields=["origin", "origin_coordinates_wgs84"])
    query = " ".join(filter(None, (city, location)))
    url = NOMINATIM + "/search?" + urlencode({"q": query, "format": "jsonv2", "limit": 5,
                                               "accept-language": "zh-CN", "addressdetails": 1})
    rows = fetch_json(url)
    candidates = [{"name": row.get("name") or row.get("display_name", "").split(",")[0],
                   "display_name": row.get("display_name"), "longitude": float(row["lon"]),
                   "latitude": float(row["lat"]), "type": row.get("type"),
                   "importance": row.get("importance")} for row in rows if row.get("lon") and row.get("lat")]
    if not candidates:
        return None, result("开放地图没有找到该地点。", {"query": query, "candidates": []}, status="not_found",
                            source="OpenStreetMap Nominatim")
    return candidates, None


def geocode(args: dict) -> dict:
    candidates, error = geocode_text(args.get("location") or args.get("query", ""), args.get("city"))
    if error:
        return error
    return result("已从开放地图取得真实地理编码候选；请按完整地址核对。",
                  {"candidates": candidates, "coordinate_system": "WGS84",
                   "retrieved_at": datetime.now(timezone.utc).isoformat(),
                   "sources": [{"title": "OpenStreetMap Nominatim", "url": NOMINATIM}]},
                  source="OpenStreetMap Nominatim")


def _resolve(args: dict, prefix: str):
    coordinates = _coordinates(args.get(prefix + "_coordinates_wgs84"), prefix + "_coordinates_wgs84")
    if coordinates:
        return {"longitude": coordinates[0], "latitude": coordinates[1],
                "display_name": args.get(prefix) or "provided coordinates"}, None
    candidates, error = geocode_text(args.get(prefix, ""), args.get("city"))
    if error:
        return None, error
    return candidates[0], None


def route(args: dict) -> dict:
    if args.get("travel_mode", "driving") != "driving":
        return result("当前公共 OSRM 连接只验收驾车路线。", status="unsupported",
                      missing_capabilities=["walking_or_cycling_router"])
    origin, error = _resolve(args, "origin")
    if error:
        return error
    destination, error = _resolve(args, "destination")
    if error:
        return error
    points = f'{origin["longitude"]},{origin["latitude"]};{destination["longitude"]},{destination["latitude"]}'
    url = OSRM + "/route/v1/driving/" + quote(points, safe=",;") + "?" + urlencode({
        "alternatives": "true", "steps": "true", "overview": "false", "annotations": "false"})
    payload = fetch_json(url)
    if payload.get("code") != "Ok" or not payload.get("routes"):
        return result("公共路线服务没有返回可用驾车路线。", {"provider_response": payload.get("code")},
                      status="not_found", source="OSRM")
    routes = []
    for index, item in enumerate(payload["routes"][:3], 1):
        legs = item.get("legs", [])
        steps = []
        for leg in legs:
            for step in leg.get("steps", []):
                maneuver = step.get("maneuver", {})
                steps.append({"instruction": " ".join(filter(None, (maneuver.get("type"), maneuver.get("modifier"), step.get("name")))),
                              "distance_m": round(step.get("distance", 0), 1),
                              "duration_seconds": round(step.get("duration", 0), 1)})
        routes.append({"index": index, "distance_m": round(item["distance"], 1),
                       "duration_seconds": round(item["duration"], 1), "steps": steps})
    routes.sort(key=lambda item: item["duration_seconds"])
    return result("已从公共道路网络计算真实路线；该服务不含实时拥堵，不能称为实时避堵。",
                  {"origin": origin, "destination": destination, "routes": routes,
                   "selected_route": routes[0], "coordinate_system": "WGS84",
                   "traffic_data": False, "route_calculated": True,
                   "retrieved_at": datetime.now(timezone.utc).isoformat(),
                   "sources": [{"title": "OpenStreetMap", "url": NOMINATIM},
                               {"title": "OSRM", "url": OSRM}]},
                  source="OpenStreetMap/OSRM", execution="calculated")
