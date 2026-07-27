"""pytest 骨架——sandbox-mode=none 向后兼容回归测试。"""

import pytest


def test_none_mode_full_chain():
    """TC-SBOX-050: sandbox-mode=none 全链路行为与移植前 100% 一致。

    业务规则:
    - B5: 向后兼容硬约束

    错误模型:
    - 向后兼容断裂——某环节引入了沙箱逻辑

    失败意味着:
    - 现有部署回归失败
    """
    pytest.skip("实现期驱动——E2E 回归测试")
