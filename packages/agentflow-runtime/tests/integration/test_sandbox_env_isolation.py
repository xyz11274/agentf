"""E2E + mock 测试：沙箱执行环境与宿主机不一致的不变量。

## 背景与设计意图

沙箱透明性（agent 无感）要求 E2BExecutor 把沙箱 workspace 路径翻译回宿主路径，
让 agent 觉得自己在熟悉环境里干活——这是正确的设计。

但沙箱透明性只该屏蔽"路径语义"，不该屏蔽"环境语义"——agent 用环境差异命令
（`uname`、`cat /etc/os-release`、`whoami`、`hostname`）做沙箱自检时，输出必须真实反映沙箱 Linux
环境，不能被宿主 macOS 信号污染。

## 覆盖的不变量

- **I-沙箱隔离**：sandbox_mode=e2b 下 bash 工具实际通过 e2b SDK `commands.run` 路由，
  不走宿主 subprocess.run（mock 测试锁住路由分叉点）
- **I-环境差异保真**：环境差异命令（`uname`、`/etc/os-release` 等）的 stdout 原样返回，
  不被路径翻译 `.replace("/home/user/workspace", host_dir)` 命中——因为输出根本不含该字面量
  （mock 测试锁住翻译逻辑不污染环境信号）
- **I-真实沙箱 Linux**：真实 e2b 沙箱里 `cat /etc/os-release` 返回 Debian Linux，
  宿主 macOS 不存在该文件（real_sandbox marker 真实执行）

## 测试分档

|档|marker|依赖|用途|
|---|---|---|---|
|mock|无|mock e2b_code_interpreter|CI 跑，锁路由 + 翻译边界|
|real|`@pytest.mark.real_sandbox`|真实 E2B_API_KEY + 远端沙箱|真执行，锁 Linux-vs-macOS 真相|

参考：
- ADR 20260724-tools-runtime-refactor.md（E2BExecutor 设计）
- docs/env/e2b_smoke.py（真实沙箱 smoke test 模板）
- 经验：`pwd` 被路径翻译命中→误判宿主机；环境差异命令不被命中→能真相沙箱
"""
import json
import os
import sys
from pathlib import Path
from unittest.mock import MagicMock, patch

import pytest

# ── 生产代码导入 ─────────────────────────────────────────────────────
_TOOLS_DIR = (
    Path(__file__).resolve().parents[2]
    / "src" / "agentflow_runtime" / "harness" / "tools"
)
sys.path.insert(0, str(_TOOLS_DIR))

from agentflow_runtime.executors.factory import create_executor  # noqa: E402
from agentflow_runtime.executors.e2b import E2BExecutor  # noqa: E402
from agentflow_runtime.executors.host import HostExecutor  # noqa: E402


# ── 公共 fixture ─────────────────────────────────────────────────────
VALID_ENDPOINT = (
    "https://218.245.96.148:443/sandbox/"
    "a1b2c3d4-e5f6-7890-abcd-ef1234567890/49983"
)
SANDBOX_ID = "a1b2c3d4-e5f6-7890-abcd-ef1234567890"
HOST_PKG_DIR = "/fake/agent/package"


@pytest.fixture
def mock_sandbox():
    """Mock e2b Sandbox——commands.run 返回空成功，可被单测覆盖。"""
    sb = MagicMock()
    sb.sandbox_id = SANDBOX_ID
    execution = MagicMock()
    execution.stdout = ""
    execution.stderr = ""
    execution.exit_code = 0
    sb.commands.run.return_value = execution
    return sb


@pytest.fixture
def mock_e2b_module(mock_sandbox):
    """Inject fake e2b_code_interpreter into sys.modules."""
    mock_mod = MagicMock()
    mock_mod.Sandbox.connect.return_value = mock_sandbox
    with patch.dict("sys.modules", {"e2b_code_interpreter": mock_mod}):
        yield mock_mod


@pytest.fixture
def e2b_executor(monkeypatch):
    """E2BExecutor with required env，agent_package_dir=HOST_PKG_DIR."""
    monkeypatch.setenv("E2B_API_KEY", "test-key-12345")
    monkeypatch.setenv("AF_AGENT_PACKAGE_DIR", HOST_PKG_DIR)
    monkeypatch.setenv("AF_SANDBOX_MODE", "e2b")
    monkeypatch.setenv("AF_SANDBOX_ENDPOINT", VALID_ENDPOINT)
    return E2BExecutor(endpoint=VALID_ENDPOINT)


@pytest.fixture
def e2b_executor_ready(e2b_executor, mock_e2b_module, mock_sandbox):
    """Executor + mock SDK injected + _download_images stubbed."""
    e2b_executor._download_images = MagicMock()
    return e2b_executor, mock_sandbox


# ═══════════════════════════════════════════════════════════════════
# 档 1：mock 测试 —— 路由 + 翻译边界不变量（CI 可跑，无远端依赖）
# ═══════════════════════════════════════════════════════════════════

# TC-SBOX-ENV-001：sandbox_mode=e2b → create_executor 返回 E2BExecutor 实例
def test_factory_routes_e2b_when_sandbox_mode_set(monkeypatch):
    """TC-SBOX-ENV-001：factory 在 mode=e2b + endpoint 时返回 E2BExecutor。

    业务规则：B1 命令在隔离沙箱内执行——sandbox_mode=e2b 必须路由到 E2BExecutor
    错误模型：路由分叉点错误——mode/endpoint 任一缺失就退化到 HostExecutor
    失败意味着：agent 的 bash 命令在宿主机跑，沙箱隔离失效
    """
    monkeypatch.setenv("E2B_API_KEY", "test-key")
    exe = create_executor(sandbox_mode="e2b", sandbox_endpoint=VALID_ENDPOINT)
    assert isinstance(exe, E2BExecutor), (
        f"mode=e2b 应返回 E2BExecutor，实际 {type(exe).__name__}——沙箱路由失效"
    )


# TC-SBOX-ENV-002：无 endpoint/mode 时退 HostExecutor（向后兼容不变量）
def test_factory_routes_host_when_no_sandbox(monkeypatch):
    """TC-SBOX-ENV-002：无 sandbox_mode/endpoint 时回 HostExecutor。

    业务规则：缺省 sandbox-mode=none → HostExecutor，向后兼容本地执行
    错误模型：缺省路由漂移——无沙箱配置时误连远端
    失败意味着：本地测试误连远端沙箱，或断言沙箱缺省行为漂移
    """
    monkeypatch.delenv("AF_SANDBOX_MODE", raising=False)
    monkeypatch.delenv("AF_SANDBOX_ENDPOINT", raising=False)
    exe = create_executor()
    assert isinstance(exe, HostExecutor), (
        f"无沙箱配置应回 HostExecutor，实际 {type(exe).__name__}"
    )


# TC-SBOX-ENV-003：bash 工具在 sandbox 模式下 → exec_command 调 SDK commands.run
def test_bash_tool_routes_to_sdk_in_sandbox_mode(e2b_executor_ready):
    """TC-SBOX-ENV-003：bash sandbox 模式→SDK commands.run 被调一次。

    业务规则：B1 命令在隔离沙箱内执行——bash 工具必须通过 E2BExecutor→SDK 路由
    错误模型：路由分叉点旁路——bash 内部 if-else 退化到宿主 subprocess
    失败意味着：命令在宿主机跑，agent 误以为在沙箱（沙箱透明性反而掩盖泄漏）
    """
    from bash import execute  # noqa: E402
    # 重用 sandbox 端点注入逻辑——bash.execute 默认 sandbox_endpoint=None 走 HostExecutor
    # 显式透传 endpoint 路由 E2BExecutor
    result = json.loads(
        execute({"command": "uname -s", "timeout": 5}, sandbox_endpoint=VALID_ENDPOINT)
    )
    # 验证：mock sandbox 的 commands.run 被调一次（证明路由到了 SDK，非宿主 subprocess）
    exe, sb = e2b_executor_ready
    # bash.execute 调 create_executor 路由会新建 E2BExecutor；用 sb.commands.run 被调证明走 SDK
    sb.commands.run.assert_called()
    # 失败信号要含可定位线索
    assert "error" not in result or result.get("exit_code") == 0, (
        f"uname -s 路由到沙箱但执行失败：{result}——检查 mock sandbox commands.run 配置"
    )


# TC-SBOX-ENV-004：环境差异命令 uname 输出不被宿主路径翻译污染
def test_environment_diff_command_not_translated(e2b_executor_ready):
    """TC-SBOX-ENV-004：`uname -s` stdout='Linux\n' 原样返回。

    业务规则：沙箱透明性只屏蔽路径语义，不屏蔽环境语义
    错误模型：无差别 .replace() 命中环境信号——`uname`/`os-release` 输出被改写
    失败意味着：agent 无法用环境差异命令做沙箱自检，证据被污染成宿主信号
    """
    exe, sb = e2b_executor_ready
    # 模拟真实沙箱 uname -s 输出——不含 /home/user/workspace 字面量
    sb.commands.run.return_value.stdout = "Linux\n"
    sb.commands.run.return_value.stderr = ""
    sb.commands.run.return_value.exit_code = 0

    result = exe.exec_command("uname -s", timeout=10)
    assert result.stdout == "Linux\n", (
        f"uname stdout 应原样返回 'Linux\\n'，实际 '{result.stdout}'"
        "——翻译逻辑污染了环境信号"
    )
    assert result.exit_code == 0
    assert result.error is None


# TC-SBOX-ENV-005：/etc/os-release 内容（Debian）原样返回
def test_os_release_content_not_translated(e2b_executor_ready):
    """TC-SBOX-ENV-005：`cat /etc/os-release` Debian 内容原样返回。

    业务规则：环境差异命令的 stdout 不该被路径翻译命中
    错误模型：翻译 .replace() 是无差别字符串替换——含 '/home/user/workspace' 子串
    失败意味着：发行版信息被改写，agent 无法从 os-release 真相沙箱是 Linux
    """
    exe, sb = e2b_executor_ready
    # 真实 Debian 13 os-release 内容（不含 /home/user/workspace）
    debian_release = (
        'PRETTY_NAME="Debian GNU/Linux 13 (trixie)"\n'
        'NAME="Debian GNU/Linux"\n'
        'VERSION_ID="13"\n'
        'VERSION="13 (trixie)"\n'
        'ID=debian\n'
    )
    sb.commands.run.return_value.stdout = debian_release
    sb.commands.run.return_value.stderr = ""
    sb.commands.run.return_value.exit_code = 0

    result = exe.exec_command("cat /etc/os-release", timeout=10)
    assert result.stdout == debian_release, (
        f"os-release 应原样返回 Debian 内容，实际被改写为：'{result.stdout}'"
    )
    assert "Debian" in result.stdout, "os-release 含 Debian 字样被丢失"


# TC-SBOX-ENV-006：pwd 输出含 /home/user/workspace 时**会**被翻译（锁反向不变量）
def test_pwd_output_translated_when_contains_workspace(e2b_executor_ready):
    """TC-SBOX-ENV-006：pwd stdout='/home/user/workspace' → 翻译回宿主 agent_package_dir。

    业务规则：沙箱透明性——pwd 输出路径要翻译回宿主，agent 看到熟悉路径（设计意图）
    错误模型：路径翻译失效——agent 看到沙箱路径 → 沙箱透明性被打破
    失败意味着：agent 必须感知沙箱路径才能干活，沙箱无感设计意图破裂

    注：这条测试锁的是"路径翻译**该生效时**生效"——与 TC-SBOX-ENV-004/005 互补，
    共同界定翻译边界：含 workspace 字面量→翻译；不含→原样。
    """
    exe, sb = e2b_executor_ready
    sb.commands.run.return_value.stdout = E2BExecutor.SANDBOX_WORKSPACE + "\n"
    sb.commands.run.return_value.stderr = ""
    sb.commands.run.return_value.exit_code = 0

    result = exe.exec_command("pwd", timeout=10)
    assert result.stdout == HOST_PKG_DIR + "\n", (
        f"pwd 应翻译回宿主路径 '{HOST_PKG_DIR}\\n'，实际 '{result.stdout}'"
        "——路径翻译失效，沙箱透明性被打破"
    )
    assert E2BExecutor.SANDBOX_WORKSPACE not in result.stdout, (
        "pwd stdout 含沙箱原始路径——翻译未生效"
    )


# ═══════════════════════════════════════════════════════════════════
# 档 2：真实 e2b 沙箱 e2e 测试 —— 需 E2B_API_KEY + 远端沙箱
# ═══════════════════════════════════════════════════════════════════

# 真实沙箱凭证——通过环境变量注入，缺则 skip（本地无凭证不阻塞 CI）
# 复用 docs/env/e2b_smoke.py 的自建 e2b 部署约定（全零 key）
_DEFAULT_E2B_API_URL = "http://218.245.96.148:30030"
_DEFAULT_E2B_TEMPLATE = "tpl-cb6b9dcbf5784a1487a75971"
# path-based 路由固定走 443/sandbox/{id}/49983（与 smoke test 一致）
_SANDBOX_PORT = "49983"
_E2B_HOST = "218.245.96.148"


def _real_e2b_api_key():
    return os.environ.get("E2B_API_KEY", "")


@pytest.fixture
def real_sandbox_endpoint():
    """自建真实 e2b 沙箱并返回 daemon 兼容格式的 endpoint。

    返回 endpoint 形如 ``https://host:443/sandbox/{id}/{port}``——与 daemon
    注入到 AF_SANDBOX_ENDPOINT 的格式一致，E2BExecutor 能从中解析 sandbox_id。

    Skip 条件：E2B_API_KEY 缺失（本地无凭证不阻塞 CI）。

    Teardown：测试结束 kill 沙箱，不留孤儿。
    """
    api_key = _real_e2b_api_key()
    if not api_key:
        pytest.skip(
            "未注入 E2B_API_KEY——本地 CI 跑 mock 档，真实档需手动配凭证"
            "（参考 docs/env/e2b_smoke.py）"
        )

    # 配齐 e2b SDK 必要环境——参考 docs/env/e2b_smoke.py
    os.environ.setdefault("E2B_API_URL", _DEFAULT_E2B_API_URL)
    os.environ["E2B_API_KEY"] = api_key
    os.environ.setdefault("CUBE_TEMPLATE_ID", _DEFAULT_E2B_TEMPLATE)

    # SSL 补丁——path-based 路由需要绕过自签证书校验
    import ssl as _ssl
    _orig_ctx = _ssl.create_default_context
    def _patched_ctx(*a, **kw):
        ctx = _orig_ctx(*a, **kw)
        ctx.check_hostname = False
        ctx.verify_mode = _ssl.CERT_NONE
        return ctx
    _ssl.create_default_context = _patched_ctx

    from e2b_code_interpreter import Sandbox

    sandbox = Sandbox.create(
        template=os.environ["CUBE_TEMPLATE_ID"],
        network={"allow_out": ["10.0.0.0/8", "43.0.0.0/8", "218.0.0.0/8"]},
    )
    # path-based 路由 patch（与 smoke test 一致）
    _path = f"https://{_E2B_HOST}:443/sandbox/{sandbox.sandbox_id}/{_SANDBOX_PORT}"
    sandbox._envd_api_url = _path
    for _attr in ("_filesystem", "_commands", "_pty"):
        _mod = getattr(sandbox, _attr, None)
        if _mod and hasattr(_mod, "_envd_api_url"):
            _mod._envd_api_url = _path

    endpoint = _path
    yield endpoint

    # Teardown——不留孤儿沙箱
    try:
        sandbox.kill()
    except Exception:
        pass


@pytest.mark.real_sandbox
def test_real_sandbox_env_differs_from_host(real_sandbox_endpoint):
    """REAL-ENV-001：真实沙箱 cat /etc/os-release 返回 Debian Linux，宿主 macOS 无此文件。

    业务规则：B1 命令在隔离沙箱内执行——沙箱 Linux ≠ 宿主 macOS
    错误模型：路由旁路或沙箱不存在——命令在宿主跑 → cat /etc/os-release 失败
    失败意味着：沙箱未真连，或 endpoint 注入错，或沙箱镜像非 Linux

    运行方式：
        # 设好真实沙箱凭证（参考 docs/env/e2b_smoke.py）
        export E2B_API_KEY=e2b_000000000  # 自建 e2b 部署的全零 key
        uv run pytest packages/agentflow-runtime/tests/integration/test_sandbox_env_isolation.py::test_real_sandbox_env_differs_from_host -m real_sandbox -v
    """
    exe = E2BExecutor(endpoint=real_sandbox_endpoint)
    # 真实调 SDK，timeout=30s（远端沙箱命令实际耗时 ~1-3s）
    result = exe.exec_command("cat /etc/os-release", timeout=30)

    # 失败要含可定位线索——F10 完工判定矩阵"真实执行核心路径一次"
    assert result.error is None, (
        f"真实沙箱 cat /etc/os-release 失败：error={result.error} "
        f"exit_code={result.exit_code} stderr={result.stderr}"
        "——检查 endpoint/API_KEY/SSL 配置"
    )
    # 不变量 I-真实沙箱 Linux：os-release 必含 ID=debian 或 Linux 字样
    assert "debian" in result.stdout.lower() or "linux" in result.stdout.lower(), (
        f"os-release 不含 Linux/Debian 字样——可能宿主 macOS 跑了或沙箱镜像非 Debian："
        f"stdout={result.stdout}"
    )
    # 不变量 I-环境差异：宿主 macOS 无 /etc/os-release，若 stdout 像 macOS 报错即泄漏
    assert "darwin" not in result.stdout.lower(), (
        f"stdout 含 'darwin'——命令在 macOS 宿主跑而非沙箱：stdout={result.stdout}"
    )
    # 不变量 I-边界：exit_code=0
    assert result.exit_code == 0, (
        f"cat /etc/os-release exit_code 非 0：{result.exit_code}——沙箱文件不存在或权限错"
    )


@pytest.mark.real_sandbox
def test_real_sandbox_uname_is_linux_not_darwin(real_sandbox_endpoint):
    """REAL-ENV-002：真实沙箱 uname -s 返回 'Linux'，不是 macOS 'Darwin'。

    互补 REAL-ENV-001——uname 是更轻量的环境差异探针（单字符串）。
    """
    exe = E2BExecutor(endpoint=real_sandbox_endpoint)
    result = exe.exec_command("uname -s", timeout=15)

    assert result.error is None, f"真实沙箱 uname -s 失败：{result}"
    assert result.stdout.strip() == "Linux", (
        f"uname -s 应返回 'Linux'，实际 '{result.stdout.strip()}'"
        "——沙箱镜像非 Linux 或命令在宿主跑"
    )
    assert "Darwin" not in result.stdout, (
        f"uname 含 'Darwin'——命令在 macOS 宿主跑而非沙箱：{result.stdout}"
    )
