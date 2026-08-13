"""零依赖 .env 文件解析器 — 替代 python-dotenv，避免沙箱内装不上导致业务断链。

业务背景：沙箱 netns 内 DNS 不可达（详见 docs/bugs/20260811_sandbox_dns_unreachable.md），
pip 装不上 python-dotenv；而 .env 文件声明了 DASHSCOPE_API_KEY/AF_LLM_API_KEY 等业务
必需的 API key/endpoint，dotenv 装不上 = .env 没加载 = os.environ 拿不到 key = 业务崩。
本模块用 stdlib 实现 .env 解析与 os.environ 注入，消除"装不上 dotenv → 业务断链"风险。

对齐 python-dotenv `load_dotenv(p, override=True)` 的真实行为：
- 逐行解析 `KEY=VALUE`
- 跳过注释行（`#` 起头）和空行
- VALUE 两端的单引号/双引号剥离（保留引号内原值，含 `#` 不当注释）
- `override=True` = os.environ 中已存在的 key 也覆盖
- 不支持 export 前缀、不展开变量引用（dotenv 默认也不展开，除非 read_from_env）
"""
import os
from pathlib import Path


def load_env_file(path: str | os.PathLike, *, override: bool = True) -> int:
    """解析 .env 文件并注入 os.environ，返回成功注入的变量数。

    Args:
        path: .env 文件路径
        override: True = 已存在的 env 也覆盖（对齐 load_dotenv(override=True)）；
                  False = 已存在则跳过

    文件不存在或解析失败时静默返回 0（对齐 dotenv：文件不存在不抛错）。
    解析规则：
        - `KEY=VALUE`        → 去首尾空白后注入
        - `KEY="VALUE"`      → 去双引号，引号内 `#` 不当注释
        - `KEY='VALUE'`      → 去单引号，引号内 `#` 不当注释
        - `# 注释`           → 整行跳过
        - `KEY=VALUE # 注释` → 行内注释剥离（VALUE 两端无引号时）
        - 空行 / `export KEY=VALUE` → 跳过 export 前缀后照常解析
    """
    p = Path(path)
    if not p.is_file():
        return 0

    count = 0
    try:
        for raw in p.read_text(encoding="utf-8").splitlines():
            line = raw.strip()
            # 跳空行和注释行
            if not line or line.startswith("#"):
                continue
            # 剥离可选的 `export ` 前缀（dotenv 兼容）
            if line.startswith("export ") or line.startswith("export\t"):
                line = line[7:].lstrip()
            # 必须含 `=`，无 `=` 的行跳过（不是有效的 KEY=VALUE）
            if "=" not in line:
                continue

            key, _, value = line.partition("=")
            key = key.strip()
            if not key:
                continue

            # VALUE 处理：引号包裹则去引号（引号内 `#` 保留），否则剥行内注释
            value = _strip_value(value)

            if override or key not in os.environ:
                os.environ[key] = value
                count += 1
    except OSError as exc:
        # 文件读失败静默（对齐 dotenv：不抛，只日志），调用方按 count=0 处理
        import sys
        print(f"[env_loader] WARNING: read failed: {exc}", file=sys.stderr)
        return 0

    return count


def _strip_value(raw: str) -> str:
    """剥 VALUE 的引号/行内注释，对齐 dotenv 解析规则。"""
    v = raw.strip()
    if not v:
        return ""
    # 双引号包裹：去引号，引号内原值保留（含 #）
    if v.startswith('"') and v.endswith('"') and len(v) >= 2:
        return v[1:-1]
    # 单引号包裹：去引号，引号内原值保留
    if v.startswith("'") and v.endswith("'") and len(v) >= 2:
        return v[1:-1]
    # 无引号：剥行内注释（`VALUE # comment` → `VALUE`），注意 `#` 必须前后有空白才当注释
    # dotenv 行为：`KEY=VALUE#frag` 不当注释（# 紧贴 VALUE），`KEY=VALUE # frag` 当注释
    if " # " in v:
        v = v.split(" # ", 1)[0].rstrip()
    elif v.endswith(" #") or v.endswith("\t#"):
        v = v[:-2].rstrip()
    return v
