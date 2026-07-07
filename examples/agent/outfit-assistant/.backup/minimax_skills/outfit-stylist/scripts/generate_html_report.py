#!/usr/bin/env python3
"""
generate_html_report.py — 穿搭指南 HTML 报告生成器
用法:
  python3 generate_html_report.py \
      --title "显高穿搭指南" \
      --subtitle "个子不高也能穿出好比例" \
      --out /path/to/report.html
也可调用 build_report(sections: dict, meta: dict) -> str
"""
import argparse
import html
from datetime import datetime
from pathlib import Path


def esc(s: str) -> str:
    return html.escape(str(s)) if s else ""


def build_report(sections: dict, meta: dict) -> str:
    """Build HTML report.

    sections = {
      "principles": [{"title":..., "reason":...}, ...],
      "tops":       [{"title":..., "reason":..., "tag":"推荐"|"避免"}, ...],
      "bottoms":    [...],
      "shoes":      [...],
      "styling":    [...],
      "lookbooks":  [{"name":..., "items": [...], "reason":..., "effect":...}],
      "pitfalls":   [{"title":..., "reason":...}],
    }
    meta = {
      "title": "...",
      "subtitle": "...",
      "city": "...",
      "weather": "...",
      "user_facts": "...",
      "updated": "...",
    }
    """
    m = meta
    css = """
    :root{
      --bg:#faf8f4; --paper:#ffffff; --ink:#2a2a2a; --muted:#6b6b6b;
      --accent:#c97b4e; --accent-soft:#fbeede; --good:#3e8e5b; --bad:#c0563f;
      --line:#ece6dd;
    }
    *{box-sizing:border-box}
    body{margin:0;background:var(--bg);color:var(--ink);
         font-family:-apple-system,"PingFang SC","Hiragino Sans GB","Microsoft YaHei",sans-serif;
         line-height:1.7;-webkit-font-smoothing:antialiased}
    .wrap{max-width:820px;margin:40px auto;padding:0 24px}
    .hero{background:linear-gradient(135deg,#fff7ee 0%,#fbeede 100%);
          border-radius:18px;padding:36px 32px;margin-bottom:32px;
          border:1px solid var(--line);position:relative;overflow:hidden}
    .hero::after{content:"";position:absolute;right:-40px;top:-40px;
                 width:180px;height:180px;border-radius:50%;
                 background:radial-gradient(circle,rgba(201,123,78,.18),transparent 70%)}
    .hero h1{font-size:32px;margin:0 0 8px;letter-spacing:.5px}
    .hero .sub{font-size:15px;color:var(--muted);margin:0}
    .meta{display:grid;grid-template-columns:repeat(3,1fr);gap:14px;margin-top:20px;font-size:13px}
    .meta div{background:rgba(255,255,255,.6);border-radius:10px;padding:10px 14px;border:1px solid var(--line)}
    .meta b{color:var(--accent);display:block;font-size:11px;text-transform:uppercase;letter-spacing:1px;margin-bottom:2px}
    h2{font-size:22px;margin:48px 0 18px;padding-bottom:8px;border-bottom:2px solid var(--accent-soft);
       display:flex;align-items:center;gap:10px}
    h2 .num{background:var(--accent);color:#fff;width:28px;height:28px;border-radius:50%;
            display:inline-flex;align-items:center;justify-content:center;font-size:14px}
    h3{font-size:16px;margin:0 0 6px}
    .card{background:var(--paper);border:1px solid var(--line);border-radius:14px;
          padding:20px 22px;margin-bottom:14px;transition:.2s}
    .card.good{border-left:4px solid var(--good)}
    .card.bad{border-left:4px solid var(--bad)}
    .card.neutral{border-left:4px solid var(--accent)}
    .reason{color:var(--muted);font-size:14px;margin:6px 0 0}
    .tag{display:inline-block;font-size:11px;padding:2px 8px;border-radius:10px;
         margin-left:8px;vertical-align:middle;font-weight:600}
    .tag.good{background:#e6f3eb;color:var(--good)}
    .tag.bad{background:#fbe8e3;color:var(--bad)}
    .tag.tip{background:var(--accent-soft);color:var(--accent)}
    .grid2{display:grid;grid-template-columns:1fr 1fr;gap:14px}
    .lookbook{background:var(--paper);border:1px solid var(--line);border-radius:14px;
              padding:22px;margin-bottom:16px;position:relative}
    .lookbook h3{margin-right:90px}
    .lookbook .effect{position:absolute;right:22px;top:22px;background:var(--accent);
                      color:#fff;padding:4px 10px;border-radius:20px;font-size:12px;font-weight:600}
    .lookbook ul{margin:10px 0 0;padding-left:20px}
    .lookbook li{margin-bottom:4px;font-size:14px}
    .lookbook .reason{margin-top:12px;padding-top:12px;border-top:1px dashed var(--line);
                      font-style:italic}
    .footer{margin:60px 0 30px;text-align:center;color:var(--muted);font-size:12px}
    .emoji{font-style:normal}
    @media(max-width:600px){.meta,.grid2{grid-template-columns:1fr}.hero{padding:24px}}
    """
    parts = []
    parts.append(f"<!DOCTYPE html><html lang='zh-CN'><head><meta charset='UTF-8'>")
    parts.append(f"<meta name='viewport' content='width=device-width,initial-scale=1'>")
    parts.append(f"<title>{esc(m.get('title','穿搭指南'))}</title><style>{css}</style></head><body><div class='wrap'>")

    # Hero
    parts.append(f"<div class='hero'><h1>{esc(m.get('title',''))}</h1>")
    parts.append(f"<p class='sub'>{esc(m.get('subtitle',''))}</p>")
    parts.append(f"<div class='meta'>")
    parts.append(f"<div><b>所在城市</b>{esc(m.get('city','—'))}</div>")
    parts.append(f"<div><b>当前天气</b>{esc(m.get('weather','—'))}</div>")
    parts.append(f"<div><b>更新时间</b>{esc(m.get('updated', datetime.now().strftime('%Y-%m-%d')))}</div>")
    parts.append(f"</div></div>")

    # User facts (optional)
    if m.get("user_facts"):
        parts.append(f"<div class='card neutral'><h3>👤 你的画像</h3><p class='reason'>{esc(m['user_facts'])}</p></div>")

    def render_section(idx, title, items, default_class="neutral"):
        if not items:
            return ""
        out = [f"<h2><span class='num'>{idx}</span>{esc(title)}</h2>"]
        for it in items:
            cls = it.get("class") or default_class
            tag = it.get("tag")
            tag_html = f"<span class='tag {esc(tag.get('cls',''))}'>{esc(tag.get('text',''))}</span>" if tag else ""
            out.append(f"<div class='card {cls}'><h3>{esc(it.get('title',''))}{tag_html}</h3>")
            if it.get("reason"):
                out.append(f"<p class='reason'>{esc(it['reason'])}</p>")
            out.append("</div>")
        return "\n".join(out)

    parts.append(render_section(1, "整体原则 · 黄金比例法则", sections.get("principles")))
    parts.append(render_section(2, "上衣选择", sections.get("tops")))
    parts.append(render_section(3, "下装选择", sections.get("bottoms")))
    parts.append(render_section(4, "鞋履技巧", sections.get("shoes")))
    parts.append(render_section(5, "搭配技巧 · 配饰 & 包袋", sections.get("styling")))
    parts.append(render_section(6, "显高雷区 · 这些千万别碰", sections.get("pitfalls"), default_class="bad"))

    # Lookbooks
    lbs = sections.get("lookbooks") or []
    if lbs:
        parts.append("<h2><span class='num'>7</span>3 套即抄即用搭配</h2>")
        for lb in lbs:
            parts.append(f"<div class='lookbook'>")
            parts.append(f"<h3>{esc(lb.get('name',''))}</h3>")
            if lb.get("effect"):
                parts.append(f"<span class='effect'>{esc(lb['effect'])}</span>")
            if lb.get("items"):
                parts.append("<ul>")
                for item in lb["items"]:
                    parts.append(f"<li>{esc(item)}</li>")
                parts.append("</ul>")
            if lb.get("reason"):
                parts.append(f"<p class='reason'>💡 {esc(lb['reason'])}</p>")
            parts.append("</div>")

    parts.append(f"<div class='footer'>穿搭顾问 · outfit-stylist · {esc(m.get('updated',''))}<br>"
                 f"本报告基于你的档案自动生成,可截图或长按保存。</div>")
    parts.append("</div></body></html>")
    return "\n".join(parts)


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--title", default="穿搭指南")
    p.add_argument("--subtitle", default="")
    p.add_argument("--out", required=True)
    p.add_argument("--data", help="optional JSON file with sections + meta")
    args = p.parse_args()

    if args.data:
        import json
        with open(args.data, encoding="utf-8") as f:
            payload = json.load(f)
        meta = payload.get("meta", {})
        meta.setdefault("title", args.title)
        meta.setdefault("subtitle", args.subtitle)
        sections = payload.get("sections", {})
        html_str = build_report(sections, meta)
    else:
        # Demo
        meta = {"title": args.title, "subtitle": args.subtitle}
        sections = {"principles": [{"title": "上短下长", "reason": "提高腰线,视觉上半身缩短、下半身拉长"}]}
        html_str = build_report(sections, meta)

    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(html_str, encoding="utf-8")
    print(f"✓ HTML 报告已生成: {out}  ({len(html_str)} chars)")


if __name__ == "__main__":
    main()