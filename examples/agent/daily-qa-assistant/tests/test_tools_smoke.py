#!/usr/bin/env python3
"""冒烟测试：真实调用各 skill 工具（依赖 .env 中的 key 与外网连通）。

用法：
    cd examples/agent/daily-qa-assistant
    set -a; . ./.env; set +a
    python3 tests/test_tools_smoke.py            # 全部
    python3 tests/test_tools_smoke.py weather    # 只跑 weather
"""

import importlib.util
import json
import os
import sys
from pathlib import Path

PKG = Path(__file__).resolve().parents[1]

CASES = ["weather", "geocode", "poi", "bus", "search", "image"]


def load(skill, name):
    path = PKG / "harness" / "skills" / skill / "scripts" / f"{name}.py"
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def run(label, fn):
    print(f"\n===== {label} =====")
    try:
        result = fn()
    except Exception as exc:  # noqa: BLE001
        result = {"status": "exception", "error": str(exc)}
    print(json.dumps(result, ensure_ascii=False, indent=2)[:1500])
    return result


def main():
    # .env 兜底加载（无 python-dotenv 时走简单解析）
    env_file = PKG / ".env"
    if env_file.exists():
        for line in env_file.read_text(encoding="utf-8").splitlines():
            line = line.strip()
            if line and not line.startswith("#") and "=" in line:
                k, _, v = line.partition("=")
                os.environ.setdefault(k.strip(), v.strip().strip('"'))

    os.environ.setdefault("AF_AGENT_PACKAGE_DIR", str(PKG))
    selected = sys.argv[1:] or CASES

    if "weather" in selected:
        m = load("weather-query", "weather_query")
        run("weather_query 东莞 days=2",
            lambda: m.execute({"location": "东莞", "days": 2}))

    if "geocode" in selected:
        m = load("amap-nearby", "geocode")
        run("geocode 东莞松山湖", lambda: m.execute({"address": "东莞松山湖"}))

    if "poi" in selected:
        m = load("amap-nearby", "nearby_poi_search")
        run("nearby_poi_search 松山湖 川菜",
            lambda: m.execute({"address": "东莞松山湖", "keywords": "川菜",
                               "radius": 3000, "offset": 3}))

    if "bus" in selected:
        m = load("amap-nearby", "bus_route_query")
        run("bus_route_query 东莞 华为站",
            lambda: m.execute({"city": "东莞", "keywords": "华为站"}))

    if "search" in selected:
        m = load("web-search", "web_search")
        run("web_search 今日新闻要点",
            lambda: m.execute({"query": "今天国内有什么重要新闻", "count": 3}))

    if "image" in selected:
        m = load("image-gen", "generate_image")
        run("generate_image 测试图",
            lambda: m.execute({"prompt": "一只简笔画风格的橘猫",
                               "output": "assets/smoke-test.png"}))


if __name__ == "__main__":
    main()
