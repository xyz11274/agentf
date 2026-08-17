"""TDD 测试：e2b_sync.py upload 幂等——第二次调用不重复 write_files 已存在文件。

对应设计文档 `docs/design/features/agent-package/agent-package-structure.md` §4.2.1
（2026-08-15 决策）：upload() 上传前用 `_list_remote_files` 列出远端已有文件，
跳过已存在文件（内容寻址 sha256[:16] 文件名天然支持"同名即同内容"），只上传新增。

RED 阶段：当前 upload() 无远端存在性检查 → test_upload_second_call_skips_existing_files 失败。
"""

from __future__ import annotations

import sys
from pathlib import Path
from unittest.mock import MagicMock, patch

import pytest

# 项目根 = packages/agentflow-runtime/tests/ 的 parents[3]（tests → agentflow-runtime → packages → 根）
_SANDBOX_DIR = Path(__file__).resolve().parents[3] / "src" / "sandbox"
if str(_SANDBOX_DIR) not in sys.path:
    sys.path.insert(0, str(_SANDBOX_DIR))

# ── 模块级导入 + 共享 Sandbox mock（code-review P2 修复）────────────
# 原 fixture 依赖"首次 import 时机"：e2b_sync 模块级 `from e2b_code_interpreter import
# Sandbox` 只在首次 import 时执行，Python 缓存模块后独立 fixture 的 mock 不再生效。
# 现改为模块级一次性导入并 patch Sandbox 为共享 mock 类——所有测试共用同一个 mock，
# fixture 只切换 connect.return_value，彻底消除执行顺序依赖。
#
# 注意：不能用 `patch.dict("sys.modules", ...)` 包住 import——patch.dict 退出时会
# pop 掉 patch 期间新增的 key（包括 e2b_sync 自身），导致测试内 import 重新加载真实
# 模块、Sandbox 绑定被覆盖（2026-08-15 实测根因）。直接 import + 强制覆盖最稳。
import e2b_sync  # noqa: E402  # 测试环境 SDK 存在（integration 同款用法）

_SANDBOX_CLS = MagicMock()  # 共享 Sandbox mock 类
e2b_sync.Sandbox = _SANDBOX_CLS


class _Entry:
    """模拟 sb.files.list 返回的远端文件条目。"""

    def __init__(self, path: str, is_file: bool = True) -> None:
        self.path = path
        self._is_file = is_file

    def is_file(self) -> bool:
        return self._is_file

    def is_dir(self) -> bool:
        return not self._is_file


@pytest.fixture(autouse=True)
def _api_key(monkeypatch: pytest.MonkeyPatch) -> None:
    """upload() 入口 _check_api_key 要求 E2B_API_KEY——测试注入，避免提前 sys.exit(2)。"""
    monkeypatch.setenv("E2B_API_KEY", "test-key-12345")


@pytest.fixture
def fake_sandbox() -> MagicMock:
    """Mock e2b Sandbox——files 系列接口可断言调用。"""
    sb = MagicMock()
    sb.files.make_dir = MagicMock()
    sb.files.write_files = MagicMock()
    sb.files.list = MagicMock(return_value=[])  # 默认远端为空
    return sb


@pytest.fixture
def mock_e2b(fake_sandbox: MagicMock) -> MagicMock:
    """共享模块级 Sandbox mock——connect 指向当前测试的 fake_sandbox。

    code-review P2 修复：模块顶部已 ``e2b_sync.Sandbox = _SANDBOX_CLS``（共享 mock 类），
    不依赖首次 import 时机。fixture 只把当前测试的 fake_sandbox 接到共享类的 connect 上，
    各测试互不干扰。
    """
    _SANDBOX_CLS.connect.return_value = fake_sandbox
    return fake_sandbox


def _make_upload_dir(tmp_path: Path) -> Path:
    """构造含两个内容寻址文件的本地 upload 目录。"""
    local = tmp_path / "upload"
    local.mkdir()
    (local / "abc1234567890ab.png").write_bytes(b"data-one")
    (local / "def1234567890cd.png").write_bytes(b"data-two")
    return local


def test_upload_second_call_skips_existing_files(tmp_path, mock_e2b) -> None:
    """A1: 远端已有同内容文件时，第二次 upload 不重复 write_files（当前失败——无存在性检查）。"""
    import e2b_sync

    local = _make_upload_dir(tmp_path)
    remote_dir = "/home/user/workspace/upload"

    # 第一次上传：远端为空 → 全量 write_files
    mock_e2b.files.list.return_value = []
    e2b_sync.upload("sbox-1", str(local), remote_dir)
    first_write_calls = mock_e2b.files.write_files.call_count
    assert first_write_calls >= 1, "首次上传应至少一次 write_files"

    # 第二次上传：远端已有全部文件（同名 = 同内容，内容寻址）→ 不应重复 write_files
    mock_e2b.files.write_files.reset_mock()
    mock_e2b.files.list.return_value = [
        _Entry(f"{remote_dir}/abc1234567890ab.png"),
        _Entry(f"{remote_dir}/def1234567890cd.png"),
    ]
    e2b_sync.upload("sbox-1", str(local), remote_dir)
    assert mock_e2b.files.write_files.call_count == 0, \
        f"远端已存在全部文件，不应重复 write_files（实际 {mock_e2b.files.write_files.call_count} 次）"


def test_upload_first_call_uploads_all(tmp_path, mock_e2b) -> None:
    """A2: 远端为空时首次 upload 全量 write_files（已有行为，锁回归）。"""
    import e2b_sync

    local = _make_upload_dir(tmp_path)
    remote_dir = "/home/user/workspace/upload"

    mock_e2b.files.list.return_value = []
    e2b_sync.upload("sbox-1", str(local), remote_dir)
    assert mock_e2b.files.write_files.called, "远端为空时首次上传应调用 write_files"
    # write_files 收到的 batch 应含本地两个文件（内容寻址文件名）
    all_batches = [call.args[0] for call in mock_e2b.files.write_files.call_args_list]
    all_paths = [item["path"] for batch in all_batches for item in batch]
    assert any("abc1234567890ab.png" in p for p in all_paths), f"缺 abc 文件: {all_paths}"
    assert any("def1234567890cd.png" in p for p in all_paths), f"缺 def 文件: {all_paths}"


def test_upload_reuploads_missing_file(tmp_path, mock_e2b) -> None:
    """A3: 远端部分文件丢失（如沙箱回收/清理）→ 第二次 upload 只重传丢失的文件。

    幂等不变量的另一面：远端已存在 → 跳过（A1）；远端丢失 → 必须重传（A3）——
    否则文件丢失后永远补不回来，违反 F3 I-1（不丢失业务数据）。
    """
    import e2b_sync

    local = _make_upload_dir(tmp_path)
    remote_dir = "/home/user/workspace/upload"

    # 第一次上传：远端为空 → 全量
    mock_e2b.files.list.return_value = []
    e2b_sync.upload("sbox-1", str(local), remote_dir)

    # 第二次上传：远端只剩 abc（def 丢失）→ 必须重传 def，且不重传 abc
    mock_e2b.files.write_files.reset_mock()
    mock_e2b.files.list.return_value = [
        _Entry(f"{remote_dir}/abc1234567890ab.png"),  # 仍在
        # def1234567890cd.png 丢失（沙箱回收/清理）
    ]
    e2b_sync.upload("sbox-1", str(local), remote_dir)

    assert mock_e2b.files.write_files.called, \
        "远端有文件丢失时必须重传（不能静默跳过）"
    all_batches = [call.args[0] for call in mock_e2b.files.write_files.call_args_list]
    all_paths = [item["path"] for batch in all_batches for item in batch]
    assert any("def1234567890cd.png" in p for p in all_paths), \
        f"丢失的 def 文件未重传: {all_paths}"
    assert not any("abc1234567890ab.png" in p for p in all_paths), \
        f"未丢失的 abc 文件不应重传: {all_paths}"
