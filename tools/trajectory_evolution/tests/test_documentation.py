# test_documentation 回归验证：检查包内协议、边界条件与失败处理。
# 本地桩用于重现确定性场景，不代表真实 Agent 或远程模型的业务能力。
# 测试应同时验证正常路径与拒绝路径，避免错误数据被当成成功结果。

"""离线检查源码或安装后的手册，不依赖网络和可选绘图工具。"""

import json
from pathlib import Path
import re
import sys
from urllib.parse import unquote, urlsplit
import xml.etree.ElementTree as ET


ROOT = (
    Path(sys.argv[1]).resolve()
    if len(sys.argv) > 1
    else Path(__file__).resolve().parents[1]
)


# 测试辅助函数：outside_code。
# 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
def outside_code(text):
    lines = []
    fence = None
    for line in text.splitlines():
        marker = re.match(r"^\s*(`{3,}|~{3,})", line)
        if marker:
            current = marker.group(1)[0]
            if fence is None:
                fence = current
            elif fence == current:
                fence = None
            continue
        if fence is None:
            lines.append(line)
    assert fence is None, "unclosed Markdown code fence"
    return "\n".join(lines)


# 测试辅助函数：anchors。
# 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
def anchors(path):
    text = outside_code(path.read_text(encoding="utf-8"))
    result = set(re.findall(r'<a\s+id="([^"]+)"', text))
    counts = {}
    for heading in re.findall(r"^#{1,6}\s+(.+)$", text, re.M):
        slug = re.sub(r"[^\w\s-]", "", heading.lower()).replace(" ", "-")
        count = counts.get(slug, 0)
        counts[slug] = count + 1
        result.add(slug + ("-" + str(count) if count else ""))
    return result


# 回归用例：main。
# 关键断言：assert chapters, "chapter handbook missing"；此处只验证测试构造的场景。
def main():
    documents = [
        ROOT / "README.md",
        ROOT / "ALGORITHMS.md",
        *sorted((ROOT / "docs").rglob("*.md")),
    ]
    chapters = sorted((ROOT / "docs").glob("[0-9][0-9]-*.md"))
    assert chapters, "chapter handbook missing"
    links = 0
    for path in documents:
        text = outside_code(path.read_text(encoding="utf-8"))
        for destination in re.findall(r"\]\(([^\s)]+)(?:\s+\"[^\"]*\")?\)", text):
            parsed = urlsplit(destination)
            if parsed.scheme or parsed.netloc:
                continue
            target = (
                (path.parent / unquote(parsed.path)).resolve() if parsed.path else path
            )
            assert target.exists(), (path, "missing link", destination)
            assert target == ROOT or ROOT in target.parents, (
                path,
                "link outside distributable package",
                destination,
            )
            if parsed.fragment and target.suffix == ".md":
                assert unquote(parsed.fragment) in anchors(target), (
                    path,
                    "missing anchor",
                    destination,
                )
            links += 1
        if path in chapters:
            for heading in ("## 本章目标", "## 设计思路", "## 实现定位"):
                assert heading in text, (path, "required section missing", heading)
            assert len(re.findall(r"!\[[^\]]*\]\(figures/[^)]+\.svg\)", text)) >= 2, (
                path,
                "chapter figures missing",
            )

    namespace = "{http://www.w3.org/2000/svg}"
    figures = sorted((ROOT / "docs/figures").glob("*.svg"))
    assert figures, "SVG figures missing"
    for path in figures:
        root = ET.parse(path).getroot()
        assert (
            root.tag == namespace + "svg" and root.find(namespace + "title") is not None
        ), path
        assert (
            root.find(namespace + "desc") is not None
            and root.attrib.get("role") == "img"
        ), path
        caption = root.find(namespace + "text")
        if caption is not None and "data-max-width" in caption.attrib:
            visible = re.sub(r"\s+", "", "".join(caption.itertext()))
            title = re.sub(
                r"\s+", "", "".join(root.find(namespace + "title").itertext())
            )
            assert visible == title, (path, "visible title lost text during wrapping")
        _, _, width, height = map(float, root.attrib["viewBox"].split())
        for element in root.iter():
            assert element.tag not in (
                namespace + "script",
                namespace + "foreignObject",
            ), path
            for key, value in element.attrib.items():
                assert not key.lower().startswith("on"), (path, key)
                if key.endswith("href"):
                    assert value.startswith("#"), (path, "external SVG dependency")
            if element.tag == namespace + "rect":
                x, y = float(element.get("x", 0)), float(element.get("y", 0))
                w, h = float(element.get("width", 0)), float(element.get("height", 0))
                assert (
                    min(x, y, w, h) >= 0 and x + w <= width + 1 and y + h <= height + 1
                ), (
                    path,
                    "rectangle outside canvas",
                )
    print(
        json.dumps(
            {
                "chapters": len(chapters),
                "documents": len(documents),
                "figures": len(figures),
                "local_links": links,
            }
        )
    )


if __name__ == "__main__":
    main()
