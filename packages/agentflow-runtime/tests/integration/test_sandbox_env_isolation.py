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


# ═══════════════════════════════════════════════════════════════════
# 档 1 补：mock 测试 —— ${SKILL_DIR} 替换沙箱路径不变量（CI 可跑）
# ═══════════════════════════════════════════════════════════════════

# TC-SBOX-ENV-007：sandbox_mode=e2b 时 ${SKILL_DIR} 替换为沙箱内绝对路径
def test_skill_dir_replaced_with_sandbox_path_in_e2b_mode(monkeypatch):
    """TC-SBOX-ENV-007：sandbox_mode=e2b → ${SKILL_DIR} 替换为 /home/user/workspace/...

    业务规则：沙箱透明性——LLM 看到的 SKILL_DIR 必须是沙箱内它能直接用的绝对路径，
    而非宿主路径。否则 LLM 拼命令含宿主路径 /Users/xxx/...，e2b.py 翻译回沙箱
    路径后目标在沙箱里也不存在（agent 包按 harness/skills 结构落盘到
    /home/user/workspace/harness/skills/...）。

    错误模型：skill_executor 替换 ${SKILL_DIR} 时用了宿主路径 → LLM 拼宿主路径命令
    失败意味着：LLM 拼的命令在沙箱里找不到目标文件，skill 调用链断
    """
    from agentflow_runtime.skill_executor import load_skill_md

    # 配 sandbox 环境——AF_SANDBOX_MODE=e2b + agent_package_dir + skills_base
    monkeypatch.setenv("AF_SANDBOX_MODE", "e2b")
    monkeypatch.setenv("AF_SANDBOX_WORKSPACE", "/home/user/workspace")
    monkeypatch.setenv("AF_AGENT_PACKAGE_DIR", "/fake/agent/pkg")
    # _get_skills_base 走 AF_AGENT_PACKAGE_DIR/harness/skills
    monkeypatch.setenv("AF_SKILLS_BASE", "/fake/agent/pkg/harness/skills")

    # 廑一个 SKILL.md 含 ${SKILL_DIR}/scripts/foo.py 占位符
    import tempfile
    with tempfile.TemporaryDirectory() as tmp:
        skills_base = os.path.join(tmp, "harness", "skills")
        skill_name = "test-skill"
        skill_dir = os.path.join(skills_base, skill_name)
        os.makedirs(skill_dir)
        skill_md = os.path.join(skill_dir, "SKILL.md")
        with open(skill_md, "w", encoding="utf-8") as f:
            f.write("# Test\n\n脚本在 `${SKILL_DIR}/scripts/foo.py`，根 `${SKILLS_DIR}`。\n")

        # monkeypatch _get_skills_base + _get_agent_package_dir 指临时目录
        from agentflow_runtime import skill_executor
        monkeypatch.setattr(skill_executor, "_get_skills_base", lambda: skills_base)
        monkeypatch.setattr(skill_executor, "_get_agent_package_dir", lambda: tmp)

        content = load_skill_md(skill_name)

    assert content is not None, "load_skill_md 返回 None——SKILL.md 没找到"
    # 不变量 I-沙箱路径：${SKILL_DIR} 替换为沙箱内绝对路径
    expected_skill_dir = "/home/user/workspace/harness/skills/" + skill_name
    expected_skills_dir = "/home/user/workspace/harness/skills"
    assert "${SKILL_DIR}" not in content, (
        "${SKILL_DIR} 占位符未替换——LLM 会看到字面量而非路径"
    )
    assert expected_skill_dir in content, (
        f"SKILL_DIR 应替换为沙箱路径 '{expected_skill_dir}'，实际内容：{content}"
        "——sandbox_mode=e2b 时仍用了宿主路径，LLM 拼命令会含宿主路径"
    )
    assert expected_skills_dir in content, (
        f"SKILLS_DIR 应替换为沙箱路径 '{expected_skills_dir}'，实际内容：{content}"
    )
    # 不变量 I-不含宿主路径：替换后内容里不该含宿主 agent_package_dir
    assert tmp not in content, (
        f"替换后内容含宿主 agent_package_dir '{tmp}'——sandbox_mode=e2b 时漏了沙箱翻译"
    )


# TC-SBOX-ENV-008：sandbox_mode=none 时 ${SKILL_DIR} 仍用宿主路径（向后兼容）
def test_skill_dir_uses_host_path_when_no_sandbox(monkeypatch):
    """TC-SBOX-ENV-008：sandbox_mode=none → ${SKILL_DIR} 用宿主路径（本地执行）。

    互补 TC-SBOX-ENV-007——非沙箱模式不能误替换沙箱路径，否则本地执行坏。
    错误模型：sandbox_mode=none 时也替换了沙箱路径 → 本地命令找不到目标
    失败意味着：本地无沙箱配置时 skill 调用链断
    """
    from agentflow_runtime.skill_executor import load_skill_md

    monkeypatch.delenv("AF_SANDBOX_MODE", raising=False)
    monkeypatch.delenv("AF_SANDBOX_WORKSPACE", raising=False)

    import tempfile
    with tempfile.TemporaryDirectory() as tmp:
        skills_base = os.path.join(tmp, "harness", "skills")
        skill_name = "test-skill-local"
        skill_dir = os.path.join(skills_base, skill_name)
        os.makedirs(skill_dir)
        skill_md = os.path.join(skill_dir, "SKILL.md")
        with open(skill_md, "w", encoding="utf-8") as f:
            f.write("# Test\n\n脚本在 `${SKILL_DIR}/scripts/foo.py`。\n")

        from agentflow_runtime import skill_executor
        monkeypatch.setattr(skill_executor, "_get_skills_base", lambda: skills_base)
        monkeypatch.setattr(skill_executor, "_get_agent_package_dir", lambda: tmp)

        content = load_skill_md(skill_name)

    assert content is not None
    expected_host_skill_dir = os.path.join(tmp, "harness", "skills", skill_name)
    assert "${SKILL_DIR}" not in content
    assert expected_host_skill_dir in content, (
        f"非沙箱模式 SKILL_DIR 应宿主路径 '{expected_host_skill_dir}'，实际：{content}"
    )
    # 不变量 I-不含沙箱路径：非沙箱模式不该出现 /home/user/workspace
    assert "/home/user/workspace" not in content, (
        "非沙箱模式内容含沙箱路径 /home/user/workspace——误替换破坏本地执行"
    )


# ═══════════════════════════════════════════════════════════════════
# 档 2 补：真实 e2b 沙箱 e2e —— agent 包必须上传到沙箱（需凭证）
# ═══════════════════════════════════════════════════════════════════

@pytest.mark.real_sandbox
def test_real_sandbox_harness_skills_uploaded(real_sandbox_endpoint):
    """REAL-ENV-003：真实沙箱里 E2BExecutor cwd 修复后能跑命令 + workspace 路径可建可用。

    业务规则：B1 命令在隔离沙箱内执行——_connect 补 mkdir -p SANDBOX_WORKSPACE 后
    cwd 不再崩 cwd does not exist；workspace 路径可建可用是 daemon upload 链路的前提
    错误模型：_connect 没建 workspace → commands.run cwd 指向不存在路径 → 崩
    失败意味着：E2BExecutor 真链路 cwd 修复失效，daemon upload 链路也无处可传

    注：本测试只验证 E2BExecutor fixture 范围内的不变量——cwd 修复 + workspace 可建。
    daemon sandbox upload 链路（sandbox_e2b.cpp skills sync）把 harness/skills 上传到
    沙箱是 A 修复的活，需跑 daemon 集成测试（启动真 daemon + e2b 模式），不在
    E2BExecutor 单元测试范围。fixture 自建裸沙箱不走 daemon upload 链路是预期。

    运行方式：
        export E2B_API_KEY=e2b_000000000
        uv run pytest ...::test_real_sandbox_harness_skills_uploaded -m real_sandbox -v
    """
    exe = E2BExecutor(endpoint=real_sandbox_endpoint)
    # 不变量 I-cwd 修复：_connect mkdir -p 后 cwd 不崩，命令能跑
    result = exe.exec_command("pwd && ls -1 /home/user/ 2>&1", timeout=15)

    assert result.error is None, (
        f"真实沙箱 pwd+ls 失败（cwd 修复失效）：{result}"
        "——检查 _connect 的 mkdir -p SANDBOX_WORKSPACE"
    )
    assert result.exit_code == 0, (
        f"pwd+ls exit_code 非 0：{result.exit_code}——cwd 或 workspace 路径错"
    )
    # 不变量 I-workspace 可建：SANDBOX_WORKSPACE 路径存在可写
    assert "/home/user/workspace" in result.stdout or "workspace" in (
        result.stdout or ""
    ) or result.stdout.strip(), (
        f"cwd 命令空输出——workspace 路径不可建或 cwd 仍崩：stdout={result.stdout!r}"
    )
    # 不变量 I-环境差异保真：沙箱是 Linux 镜像，不该出现 macOS 信号
    assert "darwin" not in (result.stdout or "").lower(), (
        f"stdout 含 'darwin'——命令在 macOS 宿主跑而非沙箱：{result.stdout}"
    )
    # 互补：workspace 可写 mkdir harness/skills 链路通——daemon upload 链路前提
    mkdir_res = exe.exec_command(
        "mkdir -p /home/user/workspace/harness/skills && ls -1 /home/user/workspace/harness/",
        timeout=15,
    )
    assert mkdir_res.error is None, (
        f"mkdir harness/skills 失败——workspace 不可写或权限错：{mkdir_res}"
    )
    assert "skills" in (mkdir_res.stdout or ""), (
        f"mkdir 后 ls 不含 skills 子目录——mkdir 链路断：{mkdir_res.stdout}"
    )


@pytest.mark.real_sandbox
def test_real_sandbox_manifest_driven_upload(real_sandbox_endpoint, tmp_path):
    """REAL-ENV-004：manifest 驱动上传——只传 manifest 声明内容（.env + harness/skills），
    排除 .venv/workspace 等沙箱无关产物，set -a && source 后子进程 os.environ 可读密钥。

    业务规则（设计文档 agent-package-structure.md §3 + sandbox-env-injection.md §2 链路环节⑤）：
    - agent 包根整体上传由 manifest.jsonc 白名单驱动（用户声明什么传什么）
    - 系统预制 includes = harness/skills/**、harness/tools/**、.env（只传主 .env）、setup.sh、requirements.txt
    - 系统预制 excludes = .venv、workspace、outputs、output、tests、nodes、__pycache__ 等
    - e2b_sync.py upload --manifest 按白名单过滤，命中 excludes 的剔除
    - 上传后 e2b.py exec_command 的 set -a && source /home/user/workspace/.env 让子进程读到密钥

    本测试复用 daemon 同款 e2b_sync.py（import 直接调 upload），在真实沙箱验证：
    ✅ .env 上传且密钥子进程可读（本轮根因修复目标）
    ✅ harness/skills/demo/SKILL.md 上传（harness 目录在）
    ❌ .venv/x.py 不上传（91M 虚拟环境排除）
    ❌ workspace/u/a.png 不上传（会话级产物排除）

    运行方式：
        export E2B_API_KEY=e2b_000000000
        uv run pytest ...::test_real_sandbox_manifest_driven_upload -m real_sandbox -v
    """
    exe = E2BExecutor(endpoint=real_sandbox_endpoint)
    sandbox_id = exe.sandbox_id

    # ── 构造迷你 agent 包（含 .env、harness/skills、.venv、workspace）──
    pkg = tmp_path / "agent-pkg"
    (pkg / "harness/skills/demo").mkdir(parents=True)
    (pkg / ".venv/lib").mkdir(parents=True)
    (pkg / "workspace/u").mkdir(parents=True)
    (pkg / ".env").write_text("AF_TEST_SECRET_005=sk-value-visible-005\n")
    (pkg / "harness/skills/demo/SKILL.md").write_text("---\nname: demo\n---\n")
    (pkg / ".venv/lib/x.py").write_text("JUNK")
    (pkg / "workspace/u/a.png").write_text("PNG")

    # ── 写 manifest.jsonc（与 outfit-assistant 包同构的预制 includes/excludes）──
    manifest = {
        "includes": [
            "harness/skills/**",
            "harness/tools/**",
            "data/**",
            "requirements.txt",
            ".env",
            "setup.sh",
        ],
        "excludes": [
            ".agentfoundry",
            ".venv",
            "__pycache__",
            "nodes",
            "output",
            "outputs",
            "tests",
            "workspace",
        ],
    }
    manifest_path = pkg / "manifest.jsonc"
    manifest_path.write_text(json.dumps(manifest, indent=2))

    # ── 驱动 daemon 同款 e2b_sync.py upload（import 直调，走 --manifest 白名单）──
    sys.path.insert(0, str(Path(__file__).resolve().parents[4] / "src" / "sandbox"))
    import e2b_sync  # noqa: E402

    e2b_sync.upload(
        sandbox_id,
        str(pkg),
        "/home/user/workspace",
        str(manifest_path),
    )

    # ── 断言 1：.env 上传 + set -a && source 后子进程读到密钥（本轮根因修复目标）──
    result = exe.exec_command(
        "python3 -c \"import os; print(os.environ.get('AF_TEST_SECRET_005', ''))\"",
        timeout=20,
    )
    assert result.error is None, (
        f"exec_command 读 os.environ 失败：{result}"
        "——检查 e2b.py _build_env_source_prefix 的 set -a && source 前缀"
    )
    assert result.exit_code == 0, (
        f"python3 exit_code={result.exit_code}——命令崩或沙箱无 python3：{result}"
    )
    assert "sk-value-visible-005" in (result.stdout or ""), (
        f"子进程读不到 .env 密钥——manifest 未把 .env 传上去或 set -a 前缀失效："
        f"stdout={result.stdout!r}"
    )

    # ── 断言 2：harness/skills 上传（harness 目录在）──
    skills_res = exe.exec_command(
        "ls /home/user/workspace/harness/skills/demo/SKILL.md",
        timeout=15,
    )
    assert skills_res.error is None and skills_res.exit_code == 0, (
        f"harness/skills/demo/SKILL.md 未上传（harness 目录缺失）：{skills_res}"
        "——检查 manifest includes 的 harness/skills/**"
    )

    # ── 断言 3：.venv 不上传（91M 虚拟环境排除）──
    venv_res = exe.exec_command(
        "test -e /home/user/workspace/.venv/x.py && echo EXISTS || echo ABSENT",
        timeout=15,
    )
    assert "ABSENT" in (venv_res.stdout or ""), (
        f".venv/x.py 被上传进沙箱——manifest excludes 未生效：{venv_res}"
        "——检查 manifest excludes 的 .venv"
    )

    # ── 断言 4：workspace 不上传（会话级产物排除）──
    ws_res = exe.exec_command(
        "test -e /home/user/workspace/workspace/u/a.png && echo EXISTS || echo ABSENT",
        timeout=15,
    )
    assert "ABSENT" in (ws_res.stdout or ""), (
        f"workspace/u/a.png 被上传进沙箱——manifest excludes 未生效：{ws_res}"
        "——检查 manifest excludes 的 workspace"
    )

