"""pytest 骨架——bash.py 沙箱代理改造后的测试。

按 test-design.md 的 TC 编号组织，docstring 首行含 TC ID。
这些测试在实现期由 Red-Green-Refactor 驱动：
- Red 阶段：先写测试，确认失败（bash.py 未改造）
- Green 阶段：改造 bash.py 让测试通过
- Refactor 阶段：优化实现
"""
import pytest


def test_dangerous_command_rejected():
    """TC-SBOX-020: 危险命令 rm -rf / 宿主侧拒绝含命令名。

    业务规则:
    - B2: 危险命令前缀在宿主侧第一道防线即拒绝，返回错误含命令名

    错误模型:
    - 危险命令泄漏——检查遗漏或顺序错误导致 rm -rf / 泄漏到沙箱内执行

    失败意味着:
    - 危险命令在沙箱内执行，可能误删沙箱内文件甚至逃逸到宿主机
    """
    # TODO 实现期: 调 bash.py sandbox_exec 执行 "rm -rf /"
    # 预期: 返回错误含 "rm -rf /" 命令名，不执行
    pytest.skip("实现期驱动——bash.py 改造后填充")


def test_sudo_forkbomb_rejected():
    """TC-SBOX-021: sudo/fork bomb 伪装命中拒绝。

    业务规则:
    - B2: 危险命令前缀拒绝（sudo / fork bomb）

    错误模型:
    - 危险命令泄漏——伪装形式未命中检查前缀

    失败意味着:
    - sudo 命令在沙箱内执行提权，或 fork bomb 耗尽沙箱资源
    """
    # TODO 实现期: 调 bash.py 执行 "sudo rm" 和 ":(){ :|:& };:"
    # 预期: 两者均被拒绝含命令名
    pytest.skip("实现期驱动——bash.py 改造后填充")


def test_contract_exec_result_shape():
    """TC-SBOX-060: 契约测试——bash.py 期望沙箱内 server 返回 ExecResult。

    Consumer: bash.py
    Provider: sandbox_http_server.py
    接口: POST /exec

    契约内容:
    - 请求: tool/bash + tool_params + opts
    - 响应: ExecResult JSON（exit_code/stdout/stderr/truncated/duration_ms）

    保护的业务意义:
    - bash.py 实际使用的 ExecResult 字段不被 server 变更破坏

    失败意味着:
    - sandbox_http_server.py 接口变更导致 bash.py 无法解析响应
    """
    # TODO 实现期: Mock sandbox_http_server.py 返回 ExecResult JSON
    # 预期: bash.py 能解析 exit_code/stdout/stderr/truncated/duration_ms
    pytest.skip("实现期驱动——契约测试骨架")
