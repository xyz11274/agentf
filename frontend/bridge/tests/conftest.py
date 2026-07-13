"""conftest — 确保项目根目录在 sys.path 中。

当 pytest 通过 shebang 脚本（#!/venv/bin/pytest）启动时，sys.path[0] 会被设为
脚本所在目录（.venv_unix/bin/）而非当前工作目录，导致 `import client` 等顶层
模块导入失败。此 conftest.py 将项目根目录（frontend/bridge/）插入 sys.path，
确保无论 pytest 以何种方式启动，导入路径都正确。

同时为 E2E 测试客户端进程拦截系统代理环境变量，避免 Clash/V2Ray 等本地代理
劫持发往本地 bridge/daemon（127.0.0.1）的请求导致超时。
"""

import os
import sys
from pathlib import Path

PROJECT_ROOT = Path(__file__).resolve().parent.parent  # tests/../ = frontend/bridge/
PROJECT_ROOT_STR = str(PROJECT_ROOT)

if PROJECT_ROOT_STR not in sys.path:
    sys.path.insert(0, PROJECT_ROOT_STR)

# 删除系统代理环境变量，防止 requests 库在 import 时缓存代理配置导致本地请求被劫持
for _var in ("HTTP_PROXY", "HTTPS_PROXY", "http_proxy", "https_proxy",
             "NO_PROXY", "no_proxy"):
    os.environ.pop(_var, None)
# 然后设置 NO_PROXY 确保本地地址绕过代理（即使有后续代码重新设置代理变量）
os.environ["NO_PROXY"] = "127.0.0.1,localhost,*.local"
os.environ["no_proxy"] = "127.0.0.1,localhost,*.local"