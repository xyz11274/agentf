# local_crash 回归验证：检查包内协议、边界条件与失败处理。
# 本地桩用于重现确定性场景，不代表真实 Agent 或远程模型的业务能力。
# 测试应同时验证正常路径与拒绝路径，避免错误数据被当成成功结果。

"""崩溃测试桩：保存开始标记后直接退出，不执行关闭刷新。"""

import os
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "clients/python"))
from evolution_local import LocalEvolution

client = LocalEvolution(
    sys.argv[1], sys.argv[2], os.environ["INTEGRATION_TEST_A"], background=False
)
result = client.invoke(
    "run.begin", {"input": "producer will be interrupted", "level": "L0"}
)
assert not result["degraded"]
client.drain()
os._exit(0)
