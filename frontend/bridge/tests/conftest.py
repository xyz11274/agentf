"""conftest — 确保项目根目录在 sys.path 中。

当 pytest 通过 shebang 脚本（#!/venv/bin/pytest）启动时，sys.path[0] 会被设为
脚本所在目录（.venv_unix/bin/）而非当前工作目录，导致 `import client` 等顶层
模块导入失败。此 conftest.py 将项目根目录（frontend/bridge/）插入 sys.path，
确保无论 pytest 以何种方式启动，导入路径都正确。
"""

import sys
from pathlib import Path

PROJECT_ROOT = Path(__file__).resolve().parent.parent  # tests/../ = frontend/bridge/
PROJECT_ROOT_STR = str(PROJECT_ROOT)

if PROJECT_ROOT_STR not in sys.path:
    sys.path.insert(0, PROJECT_ROOT_STR)
