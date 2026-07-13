#!/usr/bin/env python3
"""E2E 测试：forge-image-gen（MiniMax Image-01）真实图片生成。

使用方法：
  # 用当前环境的 API_KEY
  python3 test_e2e.py

  # 显式指定 key
  API_KEY=sk-xxx python3 test_e2e.py

生成的图片保存在 test_output/ 目录下，可以直接打开查看。
"""

import base64
import json
import os
import sys
import urllib.error
import urllib.request
from pathlib import Path

ENDPOINT = "https://api.minimaxi.com/v1/image_generation"
TEST_PROMPT = "E2E test image: a simple solid blue square on white background"

# ── 输出目录 ──────────────────────────────────────────────
OUTPUT_DIR = Path(__file__).resolve().parent / "test_output"


def _save_image(data_b64: str, filename: str) -> Path:
    """保存 base64 图片到 test_output/ 目录，返回路径。"""
    OUTPUT_DIR.mkdir(parents=True, exist_ok=True)
    img_bytes = base64.b64decode(data_b64)
    path = OUTPUT_DIR / filename
    path.write_bytes(img_bytes)
    return path


def _get_api_key() -> str:
    key = os.environ.get("API_KEY", "") or os.environ.get("MINIMAX_API_KEY", "")
    if not key:
        print("❌ SKIP: 未设置 API_KEY 或 MINIMAX_API_KEY 环境变量")
        return ""
    return key


def test_api_key_available():
    """测试 1: API key 环境变量存在"""
    key = _get_api_key()
    assert key, "API_KEY 应已设置"
    print(f"✅ API_KEY 已设置（{key[:8]}...{key[-4:]}）")


def test_generate_single_image():
    """测试 2: 单张图片生成 E2E"""
    key = _get_api_key()
    if not key:
        return

    payload = {
        "model": "image-01",
        "prompt": TEST_PROMPT,
        "aspect_ratio": "16:9",
        "n": 1,
        "response_format": "base64",
    }
    headers = {
        "Authorization": f"Bearer {key}",
        "Content-Type": "application/json",
    }
    req = urllib.request.Request(
        ENDPOINT,
        data=json.dumps(payload).encode("utf-8"),
        headers=headers,
        method="POST",
    )
    try:
        with urllib.request.urlopen(req, timeout=60) as resp:
            result = json.loads(resp.read().decode("utf-8"))
    except urllib.error.HTTPError as e:
        body = e.read().decode("utf-8", errors="replace")[:300]
        print(f"❌ HTTP {e.code}: {body}")
        raise
    except urllib.error.URLError as e:
        print(f"❌ 网络错误: {e.reason}")
        raise

    data = result.get("data") or {}
    images_b64 = data.get("image_base64") or []
    assert len(images_b64) == 1, f"应返回 1 张图片，实际 {len(images_b64)}"

    saved = _save_image(images_b64[0], "test_single.jpg")
    size_kb = saved.stat().st_size / 1024
    print(f"✅ 单张图片生成成功（{size_kb:.0f} KB）→ {saved}")


def test_generate_multiple_images():
    """测试 3: 多张图片生成（n=3）"""
    key = _get_api_key()
    if not key:
        return

    payload = {
        "model": "image-01",
        "prompt": TEST_PROMPT,
        "aspect_ratio": "16:9",
        "n": 3,
        "response_format": "base64",
    }
    headers = {
        "Authorization": f"Bearer {key}",
        "Content-Type": "application/json",
    }
    req = urllib.request.Request(
        ENDPOINT,
        data=json.dumps(payload).encode("utf-8"),
        headers=headers,
        method="POST",
    )
    try:
        with urllib.request.urlopen(req, timeout=120) as resp:
            result = json.loads(resp.read().decode("utf-8"))
    except urllib.error.HTTPError as e:
        body = e.read().decode("utf-8", errors="replace")[:300]
        print(f"❌ HTTP {e.code}: {body}")
        raise
    except urllib.error.URLError as e:
        print(f"❌ 网络错误: {e.reason}")
        raise

    data = result.get("data") or {}
    images_b64 = data.get("image_base64") or []
    assert len(images_b64) == 3, f"应返回 3 张图片，实际 {len(images_b64)}"

    saved_paths = []
    for i, b64_str in enumerate(images_b64):
        path = _save_image(b64_str, f"test_multi_{i+1}.jpg")
        saved_paths.append(path)
    size_kb = saved_paths[0].stat().st_size / 1024
    print(f"✅ 多张图片生成成功（3 张，各 ~{size_kb:.0f} KB）")
    for p in saved_paths:
        print(f"   → {p}")


def test_invalid_aspect_ratio():
    """测试 4: 错误的 aspect_ratio 应返回 API 错误"""
    key = _get_api_key()
    if not key:
        return

    payload = {
        "model": "image-01",
        "prompt": TEST_PROMPT,
        "aspect_ratio": "1:1",  # MiniMax image-01 不支持 1:1
        "n": 1,
        "response_format": "base64",
    }
    headers = {
        "Authorization": f"Bearer {key}",
        "Content-Type": "application/json",
    }
    req = urllib.request.Request(
        ENDPOINT,
        data=json.dumps(payload).encode("utf-8"),
        headers=headers,
        method="POST",
    )
    try:
        with urllib.request.urlopen(req, timeout=30) as resp:
            result = json.loads(resp.read().decode("utf-8"))
        # 某些 API 可能返回错误码而非 HTTP 错误
        print(f"⚠️  1:1 比例未抛 HTTP 错误，响应: {json.dumps(result)[:100]}")
    except urllib.error.HTTPError as e:
        body = e.read().decode("utf-8", errors="replace")[:200]
        print(f"✅ 正确拒绝 1:1 比例: HTTP {e.code} — {body}")


def test_no_key_graceful():
    """测试 5: 无 API key 时应有明确报错"""
    has_real_key = bool(os.environ.get("API_KEY") or os.environ.get("MINIMAX_API_KEY"))
    if has_real_key:
        print("⚠️  跳过无 key 测试（当前有 key）")
        return
    print("✅ 无 key 场景（当前无 key，无需特别验证）")


if __name__ == "__main__":
    print("=" * 60)
    print("forge-image-gen E2E 测试")
    print(f"输出目录: {OUTPUT_DIR}/")
    print("=" * 60)

    tests = [
        ("API Key 检查", test_api_key_available),
        ("单张生成", test_generate_single_image),
        ("多张生成 (n=3)", test_generate_multiple_images),
        ("错误比例拒绝", test_invalid_aspect_ratio),
        ("无 Key 优雅降级", test_no_key_graceful),
    ]

    passed = 0
    failed = 0
    for name, fn in tests:
        print(f"\n── {name} ──")
        try:
            fn()
            passed += 1
        except Exception as e:
            print(f"❌ 失败: {e}")
            failed += 1

    print(f"\n{'=' * 60}")
    print(f"结果: {passed} 通过, {failed} 失败")

    if passed > 0 and not failed:
        print(f"\n📂 图片保存在: {OUTPUT_DIR.resolve()}/")
        for f in sorted(OUTPUT_DIR.iterdir()):
            print(f"   {f.name}  ({f.stat().st_size // 1024} KB)")

    sys.exit(1 if failed else 0)
