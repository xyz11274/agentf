#!/bin/bash
set -euo pipefail
q="${AF_USER_TEXT:-query}"
python3 - <<'PY'
import os
import urllib.parse
import urllib.request
import xml.etree.ElementTree as ET

q = os.environ.get("AF_USER_TEXT", "query").strip()
query = q
if "新闻" in q or "热点" in q:
    query = f"{q} when:7d"
url = "https://news.google.com/rss/search?q=" + urllib.parse.quote(query) + "&hl=zh-CN&gl=CN&ceid=CN:zh-Hans"

try:
    with urllib.request.urlopen(url, timeout=4) as resp:
        data = resp.read()
    root = ET.fromstring(data)
    items = []
    for it in root.findall(".//item")[:5]:
        title = (it.findtext("title") or "").strip()
        if title:
            items.append(title)
    if items:
        print("最近相关信息：" + "；".join(items))
    else:
        print(f"已执行检索，但未返回可解析新闻条目（query={q}）。")
except Exception:
    print(f"已执行联网检索流程，但当前环境无法直接拉取新闻源（query={q}）。")
PY
