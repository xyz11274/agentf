#!/usr/bin/env python3
"""
沙箱测试用图片生成脚本 — 纯本地、不调外部 API。

用 Python 标准库（struct + zlib）生成一个简单的 PNG 测试图片。
图片内容：纯色背景 + 测试文字标记，可用于验证沙箱内工具执行 + 产物收集全链路。
"""

import struct
import zlib
import os
import sys
from pathlib import Path


def create_test_png(output_path: str, width: int = 200, height: int = 150) -> str:
    """生成一张简单的测试 PNG 图片。

    图片内容：
    - 渐变背景（从蓝色到紫色）
    - 白色边框
    - 底部有测试标记像素

    使用 Python 标准库（struct + zlib）手动构造 PNG，不依赖 PIL/Pillow。
    """
    # ── 构造像素数据（RGBA） ──
    raw_data = b""
    for y in range(height):
        # PNG 每行开头加一个 filter byte（0 = None）
        raw_data += b"\x00"
        for x in range(width):
            # 渐变：从左到右从蓝到紫
            r = int(x / width * 200)
            g = int(y / height * 100)
            b_val = 255 - int(x / width * 150)
            a = 255  # 不透明

            # 白色边框
            if x < 4 or x >= width - 4 or y < 4 or y >= height - 4:
                r, g, b_val, a = 255, 255, 255, 255

            # 底部测试标记
            if y > height - 20 and x > 10 and x < width - 10:
                # 交替黑白条纹作为测试标记
                if (x // 8) % 2 == 0:
                    r, g, b_val, a = 0, 0, 0, 255
                else:
                    r, g, b_val, a = 255, 255, 255, 255

            raw_data += struct.pack("BBBB", r, g, b_val, a)

    # ── 构造 PNG 文件 ──
    def _chunk(chunk_type: bytes, data: bytes) -> bytes:
        """构造 PNG chunk。"""
        c = chunk_type + data
        return struct.pack(">I", len(data)) + c + struct.pack(">I", zlib.crc32(c) & 0xFFFFFFFF)

    # PNG signature
    signature = b"\x89PNG\r\n\x1a\n"

    # IHDR chunk
    ihdr_data = struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)  # 8-bit RGBA
    ihdr = _chunk(b"IHDR", ihdr_data)

    # IDAT chunk（压缩像素数据）
    compressed = zlib.compress(raw_data)
    idat = _chunk(b"IDAT", compressed)

    # IEND chunk
    iend = _chunk(b"IEND", b"")

    png_data = signature + ihdr + idat + iend

    # ── 写入文件 ──
    output = Path(output_path)
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_bytes(png_data)

    return str(output.resolve())


def main() -> int:
    """CLI 入口。"""
    import argparse

    parser = argparse.ArgumentParser(description="生成测试图片")
    parser.add_argument("--output", "-o", default="./test_image.png",
                        help="输出路径（默认 ./test_image.png）")
    parser.add_argument("--width", type=int, default=200,
                        help="图片宽度（默认 200）")
    parser.add_argument("--height", type=int, default=150,
                        help="图片高度（默认 150）")
    args = parser.parse_args()

    output_path = create_test_png(args.output, args.width, args.height)
    file_size = os.path.getsize(output_path)

    # 输出结构化结果供 skill_executor 消费
    result = {
        "status": "success",
        "image_path": output_path,
        "file_size": file_size,
        "width": args.width,
        "height": args.height,
        "format": "png",
    }
    import json
    print(json.dumps(result))
    return 0


if __name__ == "__main__":
    sys.exit(main())