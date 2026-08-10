"""Sandbox 观测适配层 — 本地开发可观测性。

按 daemon ``sandbox.mode`` 分派数据源：

- ``e2b``   : e2b SDK 直连（复用 ``scripts/e2b/sandbox_list.py`` 已验证路径：SSL patch /
              envd URL 重写 / Sandbox.list / connect+kill）
- ``local`` : daemon 日志 ``[SANDBOX]`` 事件解析 + workdir/snapshot 目录扫描
- ``docker``: 同 local（本期，未接 docker SDK）

路由层只依赖 :class:`SandboxObserver` 统一接口，不感知 provider 细节（F6 单一职责 +
霰弹式防护）。设计 ref: ``docs/design/features/sandbox-management.md`` 与
``docs/design/architecture/modules/sandbox-management-api.md``。
"""

from __future__ import annotations

import asyncio
import contextlib
import dataclasses
import logging
import os
from dataclasses import dataclass, field
from datetime import UTC, datetime, timedelta
from pathlib import Path
from typing import Any

logger = logging.getLogger("agentflow.bridge.services.sandbox_observer")

# ── 统一数据模型（对齐 L4 契约 §1.1）────────────────────────────────

@dataclass
class SandboxInfo:
    """沙箱统一信息模型（provider 无关）。"""

    sandbox_id: str
    mode: str
    state: str = "UNKNOWN"
    owner: dict[str, str | None] = field(default_factory=dict)  # user_id/client_id/agent_id
    endpoint: str | None = None
    workdir: str | None = None
    template: str | None = None
    created_at: str | None = None
    age_seconds: int | None = None
    source: str = "unknown"
    # RPC 来源：daemon steady_clock 最近活跃时间戳（ms，与 sandbox.list 的
    # server_now_ms 同基准）。local/docker 模式 prune 用 idle 判定闲置超龄；
    # created_at/age_seconds 在 RPC 模式下为 null（I-3：不猜墙钟创建时间）。
    last_active_at_ms: int | None = None

    def to_dict(self) -> dict[str, Any]:
        return dataclasses.asdict(self)


@dataclass
class KillResult:
    """销毁结果（对齐 L4 契约 §1.2）。"""

    sandbox_id: str
    killed: bool = False
    already_gone: bool = False

    def to_dict(self) -> dict[str, Any]:
        return dataclasses.asdict(self)


@dataclass
class PruneResult:
    """过期清理结果（对齐 L4 契约 §1.3）。"""

    dry_run: bool = True
    older_minutes: int = 30
    stale: list[dict[str, Any]] = field(default_factory=list)
    killed: int = 0
    failed: int = 0

    def to_dict(self) -> dict[str, Any]:
        return dataclasses.asdict(self)


@dataclass
class ExecResult:
    """webshell 命令执行结果（对齐 L4 契约 §1.4）。

    不变量（I-1 + F7）：stdout/stderr 完整传递，provider 不得静默截断；
    仅当沙箱侧既有上限（output_max_bytes）触发时置 truncated=true。
    """

    sandbox_id: str
    command: str
    exit_code: int = -1
    stdout: str = ""
    stderr: str = ""
    truncated: bool = False
    duration_ms: int | None = None

    def to_dict(self) -> dict[str, Any]:
        return dataclasses.asdict(self)


@dataclass
class PauseResult:
    """沙箱暂停结果（对齐 daemon sandbox.pause RPC 响应）。"""

    sandbox_id: str
    paused: bool = False  # false = 已暂停，幂等命中

    def to_dict(self) -> dict[str, Any]:
        return dataclasses.asdict(self)


@dataclass
class ResumeResult:
    """沙箱恢复结果（对齐 daemon sandbox.resume RPC 响应）。"""

    sandbox_id: str
    resumed: bool = False  # false = 已运行，幂等命中

    def to_dict(self) -> dict[str, Any]:
        return dataclasses.asdict(self)


@dataclass
class SnapshotResult:
    """快照实体（对齐 daemon sandbox.snapshot / snapshot_list RPC 响应）。"""

    snapshot_id: str
    sandbox_id: str
    created_at: int | None = None  # ms 时间戳
    store_path: str | None = None  # 宿主机持久化路径（E2B 模式为空）
    size_bytes: int | None = None
    sandbox_mode: str | None = None  # ""（Local/Docker）/ "e2b"

    def to_dict(self) -> dict[str, Any]:
        return dataclasses.asdict(self)


@dataclass
class SnapshotDeleteResult:
    """快照单条删除结果（对齐 daemon sandbox.snapshot_delete RPC 响应）。"""

    snapshot_id: str
    deleted: bool = False
    already_gone: bool = False  # 幂等命中（快照不存在）

    def to_dict(self) -> dict[str, Any]:
        return dataclasses.asdict(self)


@dataclass
class TemplateResult:
    """e2b 模板实体（模板创建/列表结果，仅 e2b 模式）。"""

    template_id: str
    name: str
    status: str = "ready"  # ready | building | failed
    cpu_count: int | None = None
    memory_mb: int | None = None
    error: str | None = None  # 构建失败原因（status=failed 时）

    def to_dict(self) -> dict[str, Any]:
        return dataclasses.asdict(self)


class SandboxObserverError(Exception):
    """观测适配层错误基类。"""


class SandboxNotFound(SandboxObserverError):
    """目标沙箱不存在。"""


class UnsupportedAction(SandboxObserverError):
    """当前模式不支持该操作（local/docker 只读）。"""


class ProviderError(SandboxObserverError):
    """provider 底层调用失败（e2b SDK / 日志读取）。"""


class ExecUnavailable(SandboxObserverError):
    """命令执行通道不可用（endpoint 未解析 / e2b SDK 失败）。"""


# ── e2b Provider（SDK 直连，复用 sandbox_list.py 已验证逻辑）────────

# 内部 e2b 部署用 path-based 反向代理（cubeproxy），SDK 构造的 hostname 路由
# （https://{port}-{id}.cube.app）不可达，需重写为 path-based 路由。
# ⚠️ 必须用裸 IP 而非域名——cubeproxy nginx server_name `_` catch-all 不识别
# *.cube.app 的 SNI hostname，带域名的请求被 RST。裸 IP 无 SNI 走 catch-all
# server block，TLS 通过 + path-based 路由生效。
# 详见 docs/bugs/20260723_e2b_sandbox_envd_connection.md 阶段 2-3。
_DEFAULT_ENVD_PORT = 49983
_SANDBOX_TARGET_HOST = "https://218.245.96.148:443"


class E2BProvider:
    """e2b SDK 直连 provider。

    逻辑对齐 ``scripts/e2b/sandbox_list.py``（该脚本是经过真实环境验证的 e2b
    观测实现）：SSL patch（IP 直连跳过证书校验）、envd URL 重写（cubeproxy
    path-based 路由）、``Sandbox.list(query=...)`` 枚举、``connect + kill`` 销毁。
    """

    def __init__(self, api_url: str, api_key: str, ca_cert_path: str) -> None:
        self._api_url = api_url
        self._api_key = api_key
        self._ca_cert_path = ca_cert_path

    # -- 环境准备（幂等） --------------------------------------------
    def _setup_env(self) -> None:
        os.environ.setdefault("E2B_API_URL", self._api_url)
        os.environ.setdefault("E2B_API_KEY", self._api_key)
        if self._ca_cert_path and Path(self._ca_cert_path).is_file():
            os.environ.setdefault("SSL_CERT_FILE", self._ca_cert_path)

    @staticmethod
    def _apply_ssl_patch() -> None:
        """SSL patch：内部 e2b 部署使用 IP 直连，跳过证书验证。"""
        import ssl as _ssl

        if getattr(_ssl, "_agentflow_patched", False):
            return  # idempotent guard

        _orig_create_default_context = _ssl.create_default_context

        def _patched_create_default_context(*args, **kwargs):
            ctx = _orig_create_default_context(*args, **kwargs)
            ctx.check_hostname = False
            ctx.verify_mode = _ssl.CERT_NONE
            return ctx

        _ssl.create_default_context = _patched_create_default_context
        _ssl._agentflow_patched = True  # type: ignore[attr-defined]

    @staticmethod
    def _patch_envd_url(sb) -> None:
        """将 sandbox 的 envd_api_url 从 hostname 路由重写为 path-based 路由。"""
        path = f"{_SANDBOX_TARGET_HOST}/sandbox/{sb.sandbox_id}/{_DEFAULT_ENVD_PORT}"
        with contextlib.suppress(AttributeError):
            sb._SandboxBase__envd_api_url = path  # type: ignore[attr-defined]
        sb._envd_api_url = path  # type: ignore[attr-defined]
        for attr in ("_filesystem", "_commands", "_pty"):
            mod = getattr(sb, attr, None)
            if mod is not None and hasattr(mod, "_envd_api_url"):
                mod._envd_api_url = path  # type: ignore[attr-defined]

    # -- SDK 懒加载（避免 import 失败影响 bridge 启动） ---------------
    def _import_sdk(self):
        try:
            from e2b.sandbox.sandbox_api import SandboxQuery, SandboxState  # type: ignore
            from e2b_code_interpreter import Sandbox  # type: ignore
            return Sandbox, SandboxQuery, SandboxState
        except ImportError as exc:
            raise ProviderError(
                f"e2b SDK not installed (pip install e2b-code-interpreter): {exc}"
            ) from exc

    def _connect_sandbox(self, Sandbox, sandbox_id: str):
        sb = Sandbox.connect(sandbox_id)
        self._patch_envd_url(sb)
        return sb

    def _kill_sandbox(self, Sandbox, sandbox_id: str) -> None:
        s = self._connect_sandbox(Sandbox, sandbox_id)
        s.kill()

    # -- 观测能力 -----------------------------------------------------
    def list_sandboxes(self, state: str | None = None) -> list[SandboxInfo]:
        self._setup_env()
        self._apply_ssl_patch()
        Sandbox, SandboxQuery, SandboxState = self._import_sdk()

        if state:
            state_map = {
                "running": SandboxState.RUNNING,
                "paused": SandboxState.PAUSED,
            }
            states = [state_map[s] for s in [state] if s in state_map]
            query = SandboxQuery(state=states) if states else None
        else:
            query = SandboxQuery(state=[SandboxState.RUNNING])

        try:
            paginator = Sandbox.list(query=query)
            sandboxes = list(paginator.next_items())
        except Exception as exc:  # noqa: BLE001 — 需完整保留错误信息
            logger.error("sandbox_observer.e2b_list failed: %s", exc)
            raise ProviderError(f"e2b list failed: {exc}") from exc

        results: list[SandboxInfo] = []
        for sb in sandboxes:
            sandbox_id = getattr(sb, "sandbox_id", getattr(sb, "id", "unknown"))
            sb_state = getattr(sb, "state", None)
            template = getattr(sb, "template", None)
            # CubeSandbox 适配(2026-08-10):优先读 ListedSandbox.started_at 属性,
            # 不再依赖 get_info()(ListedSandbox 无此方法 → created_at 恒 None)
            created_at = self._extract_sandbox_created_at(sb)
            age_seconds = self._compute_age(created_at)
            results.append(
                SandboxInfo(
                    sandbox_id=str(sandbox_id),
                    mode="e2b",
                    state=str(sb_state) if sb_state else "UNKNOWN",
                    endpoint=self._endpoint_for(sandbox_id),
                    template=str(template) if template else None,
                    created_at=created_at.isoformat() if created_at else None,
                    age_seconds=age_seconds,
                    source="e2b_sdk",
                )
            )
        return results

    def get_sandbox(self, sandbox_id: str) -> SandboxInfo:
        found = [s for s in self.list_sandboxes(state=None) if s.sandbox_id == sandbox_id]
        if not found:
            raise SandboxNotFound(f"Sandbox {sandbox_id} not found (e2b)")
        return found[0]

    def kill_sandbox(self, sandbox_id: str) -> KillResult:
        self._setup_env()
        self._apply_ssl_patch()
        Sandbox, _, _ = self._import_sdk()
        try:
            self._kill_sandbox(Sandbox, sandbox_id)
            logger.info("sandbox_observer.e2b_kill sandbox_id=%s", sandbox_id)
            return KillResult(sandbox_id=sandbox_id, killed=True, already_gone=False)
        except Exception as exc:  # noqa: BLE001
            msg = str(exc).lower()
            if "404" in msg or "not found" in msg or "already" in msg:
                return KillResult(sandbox_id=sandbox_id, killed=False, already_gone=True)
            logger.error("sandbox_observer.e2b_kill failed sandbox_id=%s: %s", sandbox_id, exc)
            raise ProviderError(f"e2b kill {sandbox_id} failed: {exc}") from exc

    def prune_stale(self, older_minutes: int, dry_run: bool = True) -> PruneResult:
        self._setup_env()
        self._apply_ssl_patch()
        Sandbox, SandboxQuery, SandboxState = self._import_sdk()

        cutoff = datetime.now(UTC) - timedelta(minutes=older_minutes)
        try:
            paginator = Sandbox.list(query=SandboxQuery(state=[SandboxState.RUNNING]))
            sandboxes = list(paginator.next_items())
        except Exception as exc:  # noqa: BLE001
            logger.error("sandbox_observer.e2b_prune_list failed: %s", exc)
            raise ProviderError(f"e2b prune list failed: {exc}") from exc

        stale: list[dict[str, Any]] = []
        for sb in sandboxes:
            sandbox_id = getattr(sb, "sandbox_id", getattr(sb, "id", "unknown"))
            # CubeSandbox 适配(2026-08-10):优先直接读 ListedSandbox.started_at 属性,
            # 不再依赖 get_info()(ListedSandbox 无此方法 → created_at 恒 None → 永删 0 个)
            created_at = self._extract_sandbox_created_at(sb)
            if created_at and created_at < cutoff:
                age_minutes = int((datetime.now(UTC) - created_at).total_seconds() // 60)
                stale.append({"sandbox_id": str(sandbox_id), "age_minutes": age_minutes})

        result = PruneResult(dry_run=dry_run, older_minutes=older_minutes, stale=stale)
        if dry_run or not stale:
            return result

        failed = 0
        for item in stale:
            try:
                self._kill_sandbox(Sandbox, item["sandbox_id"])
            except Exception as exc:  # noqa: BLE001
                failed += 1
                logger.warning("sandbox_observer.e2b_prune_kill failed %s: %s", item["sandbox_id"], exc)
        result.killed = len(stale) - failed
        result.failed = failed
        logger.info(
            "sandbox_observer.e2b_prune older_minutes=%d dry_run=%s killed=%d failed=%d",
            older_minutes, dry_run, result.killed, result.failed,
        )
        return result

    def exec_command(self, sandbox_id: str, command: str, timeout_ms: int = 30000, cwd: str | None = None) -> ExecResult:
        """在 e2b 沙箱内执行单条命令（复用 cmd_run 已验证的 commands.run 通道）。

        Args:
            cwd: 工作目录（绝对路径），None 时由沙箱默认 cwd 决定。
                前端在会话内持有当前 cwd 并逐条透传——沙箱本身不维护 webshell
                session 状态（无状态通道，I-4 幂等）。
        """
        self._setup_env()
        self._apply_ssl_patch()
        Sandbox, _, _ = self._import_sdk()

        if not command or not command.strip():
            raise ProviderError("command must not be empty")

        sb = None
        try:
            sb = self._connect_sandbox(Sandbox, sandbox_id)
        except Exception as exc:  # noqa: BLE001
            logger.error("sandbox_observer.e2b_exec_connect failed sandbox_id=%s: %s", sandbox_id, exc)
            raise ProviderError(f"e2b connect {sandbox_id} failed: {exc}") from exc

        try:
            kwargs = {"timeout": timeout_ms / 1000.0}
            if cwd:
                kwargs["cwd"] = cwd
            result = sb.commands.run(command, **kwargs)
        except Exception as exc:  # noqa: BLE001
            logger.error("sandbox_observer.e2b_exec failed sandbox_id=%s cwd=%s: %s", sandbox_id, cwd, exc)
            raise ExecUnavailable(f"e2b exec {sandbox_id} failed: {exc}") from exc

        return ExecResult(
            sandbox_id=sandbox_id,
            command=command,
            exit_code=int(getattr(result, "exit_code", -1) if getattr(result, "exit_code", -1) is not None else -1),
            stdout=getattr(result, "stdout", "") or "",
            stderr=getattr(result, "stderr", "") or "",
            truncated=False,
            duration_ms=None,
        )

    # -- 管理能力（pause/resume/snapshot，SDK 直连通道） -------------
    def create_template(self, name: str, from_template: str = "base",
                        cpu_count: int = 2, memory_mb: int = 1024) -> TemplateResult:
        """构建 e2b 自定义模板（SDK Template.build，同步长耗时）。

        复用 docker/cubesandbox/e2b_template_build.py 的已验证构建逻辑：
        预装 sandbox_http_server.py + sandbox-setup.sh 到 /opt/sandbox/，
        设置 SANDBOX_WORKDIR 环境变量，构建后返回 template_id。

        Args:
            name: 模板名称（唯一，重复构建由 SDK 幂等处理）
            from_template: 基础模板 ID（默认 base）
            cpu_count / memory_mb: 资源配额
        """
        self._setup_env()
        self._apply_ssl_patch()
        try:
            from e2b import Template
        except ImportError as exc:
            raise ProviderError(f"e2b SDK not installed (pip install e2b): {exc}") from exc

        # 读取本地 http_server 与 setup 脚本（对齐 e2b_template_build.py）
        import sys as _sys
        from pathlib import Path as _Path
        script_dir = _Path(__file__).resolve().parents[3] / "docker" / "sandbox"
        http_server = script_dir / "http_server" / "sandbox_http_server.py"
        setup_script = script_dir / "sandbox-setup.sh"
        if not http_server.is_file() or not setup_script.is_file():
            raise ProviderError(
                f"template build assets not found: {http_server} / {setup_script}"
            )

        logger.info(
            "sandbox_observer.e2b_create_template name=%s from_template=%s cpu=%d mem=%dMB",
            name, from_template, cpu_count, memory_mb,
        )
        try:
            builder = (
                Template()
                .from_template(from_template)
                .add_files({
                    "/opt/sandbox/http_server/sandbox_http_server.py": http_server.read_text(),
                    "/opt/sandbox/sandbox-setup.sh": setup_script.read_text(),
                })
                .add_commands([
                    "mkdir -p /opt/sandbox/tools",
                    "bash /opt/sandbox/sandbox-setup.sh",
                ])
                .env_vars({"SANDBOX_WORKDIR": "/workdir"})
            )
            result = Template.build(
                builder, name,
                cpu_count=cpu_count, memory_mb=memory_mb,
            )
        except Exception as exc:  # noqa: BLE001
            logger.error("sandbox_observer.e2b_create_template failed name=%s: %s", name, exc)
            raise ProviderError(f"e2b create_template {name} failed: {exc}") from exc

        template_id = str(getattr(result, "id", getattr(result, "template_id", "")))
        logger.info("sandbox_observer.e2b_create_template done name=%s template_id=%s", name, template_id)
        return TemplateResult(template_id=template_id, name=name, status="ready",
                              cpu_count=cpu_count, memory_mb=memory_mb)

    def list_templates(self) -> list[TemplateResult]:
        """列出已构建的 e2b 模板。

        e2b SDK 无原生模板列表 API——返回本进程已创建的模板（内存记录，
        E2BProvider 实例生命周期内）。跨进程/历史模板无法枚举（SDK 限制），
        由调用方（路由层）在响应中如实标注 warning（I-3 不猜）。
        """
        # 简化：SDK 无 list 方法，返回空清单 + 路由层 warning 提示
        # （本实现不维护模板注册表——模板是平台侧资源，由 e2b 控制台管理）
        return []

    def delete_snapshot(self, snapshot_id: str) -> SnapshotDeleteResult:
        """暂停 e2b 沙箱（SDK pause，keep_memory=True 保留内存快照可快速恢复）。"""
        self._setup_env()
        self._apply_ssl_patch()
        Sandbox, _, _ = self._import_sdk()
        try:
            sb = self._connect_sandbox(Sandbox, sandbox_id)
            paused = sb.pause(keep_memory=True)
            logger.info("sandbox_observer.e2b_pause sandbox_id=%s paused=%s", sandbox_id, paused)
            return PauseResult(sandbox_id=sandbox_id, paused=bool(paused))
        except Exception as exc:  # noqa: BLE001
            msg = str(exc).lower()
            if "404" in msg or "not found" in msg or "already" in msg:
                # 已暂停/已消失：SDK 幂等语义
                logger.info("sandbox_observer.e2b_pause idempotent sandbox_id=%s", sandbox_id)
                return PauseResult(sandbox_id=sandbox_id, paused=False)
            logger.error("sandbox_observer.e2b_pause failed sandbox_id=%s: %s", sandbox_id, exc)
            raise ProviderError(f"e2b pause {sandbox_id} failed: {exc}") from exc

    def resume_sandbox(self, sandbox_id: str) -> ResumeResult:
        """恢复已暂停的 e2b 沙箱（SDK 无显式 resume——connect 唤醒 + is_running 校验）。

        e2b SDK 的 paused sandbox 在重新 connect 时自动唤醒（auto_resume 语义）。
        若平台不支持唤醒（is_running 仍为 False），抛 ProviderError 含可定位原因。
        """
        self._setup_env()
        self._apply_ssl_patch()
        Sandbox, _, _ = self._import_sdk()
        try:
            sb = self._connect_sandbox(Sandbox, sandbox_id)
            running = sb.is_running()
            logger.info("sandbox_observer.e2b_resume sandbox_id=%s running=%s", sandbox_id, running)
            if not running:
                raise ProviderError(
                    f"e2b resume {sandbox_id}: sandbox still not running after connect "
                    "(platform may not support resume)"
                )
            return ResumeResult(sandbox_id=sandbox_id, resumed=True)
        except ProviderError:
            raise
        except Exception as exc:  # noqa: BLE001
            msg = str(exc).lower()
            if "404" in msg or "not found" in msg:
                logger.warning("sandbox_observer.e2b_resume gone sandbox_id=%s", sandbox_id)
                raise SandboxNotFound(f"Sandbox {sandbox_id} not found (e2b)") from exc
            logger.error("sandbox_observer.e2b_resume failed sandbox_id=%s: %s", sandbox_id, exc)
            raise ProviderError(f"e2b resume {sandbox_id} failed: {exc}") from exc

    def create_snapshot(self, sandbox_id: str) -> SnapshotResult:
        """为 e2b 沙箱创建平台侧快照（SDK create_snapshot，filesystem + memory）。"""
        self._setup_env()
        self._apply_ssl_patch()
        Sandbox, _, _ = self._import_sdk()
        try:
            sb = self._connect_sandbox(Sandbox, sandbox_id)
            snap = sb.create_snapshot()
            snap_id = str(getattr(snap, "id", getattr(snap, "snapshot_id", "")))
            logger.info("sandbox_observer.e2b_create_snapshot sandbox_id=%s snapshot_id=%s",
                        sandbox_id, snap_id)
            return SnapshotResult(
                snapshot_id=snap_id,
                sandbox_id=sandbox_id,
                created_at=None,  # e2b SDK 不提供 ms 时间戳（I-3：不猜）
                store_path=None,  # e2b 平台侧存储，无宿主机路径
                size_bytes=None,
                sandbox_mode="e2b",
            )
        except Exception as exc:  # noqa: BLE001
            msg = str(exc).lower()
            if "404" in msg or "not found" in msg:
                raise SandboxNotFound(f"Sandbox {sandbox_id} not found (e2b)") from exc
            logger.error("sandbox_observer.e2b_create_snapshot failed sandbox_id=%s: %s", sandbox_id, exc)
            raise ProviderError(f"e2b create_snapshot {sandbox_id} failed: {exc}") from exc

    def delete_snapshot(self, snapshot_id: str) -> SnapshotDeleteResult:
        """删除 e2b 平台侧快照（SDK delete_snapshot 类方法，幂等）。"""
        self._setup_env()
        self._apply_ssl_patch()
        Sandbox, _, _ = self._import_sdk()
        try:
            deleted = Sandbox.delete_snapshot(snapshot_id)
            logger.info("sandbox_observer.e2b_delete_snapshot snapshot_id=%s deleted=%s",
                        snapshot_id, deleted)
            return SnapshotDeleteResult(snapshot_id=snapshot_id, deleted=bool(deleted), already_gone=not deleted)
        except Exception as exc:  # noqa: BLE001
            msg = str(exc).lower()
            if "404" in msg or "not found" in msg:
                logger.info("sandbox_observer.e2b_delete_snapshot idempotent snapshot_id=%s", snapshot_id)
                return SnapshotDeleteResult(snapshot_id=snapshot_id, deleted=False, already_gone=True)
            logger.error("sandbox_observer.e2b_delete_snapshot failed snapshot_id=%s: %s", snapshot_id, exc)
            raise ProviderError(f"e2b delete_snapshot {snapshot_id} failed: {exc}") from exc

    def list_snapshots(self, sandbox_id: str | None = None) -> list[SnapshotResult]:
        """列出 e2b 平台侧快照（SDK list_snapshots）。sandbox_id 必填（SDK 实例方法）。"""
        self._setup_env()
        self._apply_ssl_patch()
        Sandbox, _, _ = self._import_sdk()
        if not sandbox_id:
            raise ProviderError("e2b list_snapshots requires sandbox_id (SDK instance method)")
        try:
            sb = self._connect_sandbox(Sandbox, sandbox_id)
            paginator = sb.list_snapshots()
            items = list(paginator) if hasattr(paginator, "__iter__") else []
            out: list[SnapshotResult] = []
            for item in items:
                snap_id = str(getattr(item, "id", getattr(item, "snapshot_id", "")))
                if not snap_id:
                    continue
                out.append(SnapshotResult(
                    snapshot_id=snap_id,
                    sandbox_id=sandbox_id,
                    created_at=None,
                    store_path=None,
                    size_bytes=None,
                    sandbox_mode="e2b",
                ))
            logger.info("sandbox_observer.e2b_list_snapshots sandbox_id=%s count=%d", sandbox_id, len(out))
            return out
        except Exception as exc:  # noqa: BLE001
            msg = str(exc).lower()
            if "404" in msg or "not found" in msg:
                raise SandboxNotFound(f"Sandbox {sandbox_id} not found (e2b)") from exc
            logger.error("sandbox_observer.e2b_list_snapshots failed sandbox_id=%s: %s", sandbox_id, exc)
            raise ProviderError(f"e2b list_snapshots {sandbox_id} failed: {exc}") from exc

    # -- helpers -------------------------------------------------------
    @staticmethod
    def _endpoint_for(sandbox_id: str) -> str:
        """计算 path-based 重写后的 envd endpoint（cubeproxy 路由）。"""
        return f"{_SANDBOX_TARGET_HOST}/sandbox/{sandbox_id}/{_DEFAULT_ENVD_PORT}"

    @staticmethod
    def _extract_created_at(info: dict[str, Any]) -> datetime | None:
        for key in ("created_at", "started_at", "timestamp"):
            val = info.get(key)
            if not val:
                continue
            try:
                if isinstance(val, (int, float)):
                    return datetime.fromtimestamp(val, tz=UTC)
                if isinstance(val, str):
                    return datetime.fromisoformat(val.replace("Z", "+00:00"))
            except (ValueError, TypeError):
                continue
        return None

    @staticmethod
    def _extract_sandbox_created_at(sb) -> datetime | None:
        """提取沙箱创建时间:优先直接读对象自带属性,fallback get_info()。

        CubeSandbox 适配(2026-08-10):``Sandbox.list`` 返回的 ``ListedSandbox``
        自带 ``started_at`` 属性(datetime),但**没有** ``get_info`` 方法——
        此前依赖 ``sb.get_info()`` 提取,info 恒为空 dict → created_at 永远 None
        → prune_stale 永不判 stale → 清理过期沙箱永远删 0 个。修复:先读属性,
        属性缺失时再 fallback get_info()(兼容 E2B Cloud 场景)。
        """
        val = getattr(sb, "started_at", None)
        if isinstance(val, datetime):
            return val
        if val:  # str/int 形态兜底
            return E2BProvider._extract_created_at({"started_at": val})
        with contextlib.suppress(Exception):  # noqa: BLE001 — get_info 失败不阻断
            info = sb.get_info() if hasattr(sb, "get_info") else {}
            return E2BProvider._extract_created_at(info)
        return None

    @staticmethod
    def _compute_age(created_at: datetime | None) -> int | None:
        if not created_at:
            return None
        return int((datetime.now(UTC) - created_at).total_seconds())


# ── Local / Docker Provider（daemon RPC 通道，E1 sandbox.list）────────


class LocalProvider:
    """local/docker 模式：经 daemon RPC ``sandbox.list`` 观测沙箱。

    2026-08-02 重构：数据源从"日志解析 + 目录扫描"切换为 daemon RPC
    （E1 ``sandbox.list`` / E2 ``sandbox.destroy``）。daemon 不可达时抛
    ``ProviderError``（502）——不再有 daemon 离线兜底场景（设计意图变更，
    用户确认：去掉 daemon 离线场景，列表以 daemon 内存活跃沙箱为权威）。
    """

    def __init__(self, mode: str, daemon_client) -> None:
        self._mode = mode
        self._daemon_client = daemon_client  # 必填：list/get/exec 全部依赖 RPC

    async def list_sandboxes(self, state: str | None = None) -> list[SandboxInfo]:
        """经 ``sandbox.list`` RPC 枚举 daemon 内存活跃沙箱（真实 state）。"""
        params: dict[str, Any] = {}
        if state:
            params["state"] = state.upper()
        try:
            result = await self._daemon_client.send_request("sandbox.list", params)
        except Exception as exc:  # noqa: BLE001
            logger.error("sandbox_observer.sandbox_list failed mode=%s: %s", self._mode, exc)
            raise ProviderError(f"sandbox.list failed ({self._mode}): {exc}") from exc
        sandboxes = result.get("sandboxes") or []
        server_now_ms = result.get("server_now_ms")
        out: list[SandboxInfo] = []
        for item in sandboxes:
            out.append(self._to_sandbox_info(item, server_now_ms))
        return out

    def _to_sandbox_info(self, item: dict[str, Any], server_now_ms: int | None) -> SandboxInfo:
        owner = item.get("owner") or {}
        last_active = item.get("last_active_at")
        idle_seconds = None
        if server_now_ms is not None and last_active is not None:
            idle_seconds = max(0, int((server_now_ms - last_active) / 1000))
        return SandboxInfo(
            sandbox_id=item["sandbox_id"],
            # mode 优先取 RPC 返回（daemon 配置权威），缺失时兜底 provider 配置
            # （避免 docker 模式被误标为 local）
            mode=item.get("mode") or self._mode,
            state=str(item.get("state", "UNKNOWN")).upper(),
            owner={
                "user_id": owner.get("user_id"),
                "client_id": owner.get("client_id"),
                "agent_id": owner.get("agent_id"),
            },
            endpoint=item.get("endpoint"),
            workdir=item.get("workdir"),
            created_at=None,  # RPC 不提供墙钟创建时间（I-3：不猜）
            age_seconds=idle_seconds,  # 闲置时长（idle = server_now - last_active_at）
            source="daemon_rpc",
            last_active_at_ms=last_active,
        )

    async def get_sandbox(self, sandbox_id: str) -> SandboxInfo:
        found = [s for s in await self.list_sandboxes() if s.sandbox_id == sandbox_id]
        if not found:
            raise SandboxNotFound(f"Sandbox {sandbox_id} not found ({self._mode})")
        return found[0]

    async def exec_command(self, sandbox_id: str, command: str, timeout_ms: int = 30000, cwd: str | None = None) -> ExecResult:
        """在沙箱内执行单条命令：向沙箱内 HTTP server 发 ``POST /exec``（tool=bash）。

        ⚠️ 当前 e2b 场景不通（2026-08-09 确认）：endpoint 是沙箱内 HTTP server 的
        cubeproxy 外部路径（http://host:80/sandbox/{id}/8080），D-001 实测 cubeproxy
        不转发 8080（HTTP 502），e2b 下此通道实际抛 ExecUnavailable。webshell 在
        e2b 场景不可用，仅 Local/Docker 模式（HTTPExecutor 直连沙箱内 HTTP server）
        此通道可用。

        endpoint 来自 ``sandbox.list`` RPC（权威源，不再解析日志）。endpoint
        缺失时抛 ExecUnavailable（L4 呑约 §2.7）。危险命令过滤由沙箱侧
        ``is_dangerous()`` 承担（不重复实现，M2 收敛）。

        Args:
            cwd: 工作目录（绝对路径），None 时由沙箱默认 cwd 决定。
                透传给 opts.working_dir → sandbox_http_server → runtime bash。
                前端在会话内持有当前 cwd 并逐条透传——沙箱本身不维护 webshell
                session 状态（无状态通道，I-4 幂等）。
        """
        if not command or not command.strip():
            raise ProviderError("command must not be empty")

        sb = await self.get_sandbox(sandbox_id)
        endpoint = sb.endpoint
        if not endpoint:
            logger.warning(
                "sandbox_observer.exec_no_endpoint sandbox_id=%s mode=%s", sandbox_id, self._mode
            )
            raise ExecUnavailable(
                f"no endpoint for sandbox {sandbox_id} ({self._mode}); "
                "cannot exec (sandbox.list returned no endpoint)"
            )

        import httpx  # 局部导入：observer 基础路径（list/get）不依赖网络客户端

        url = endpoint.rstrip("/") + "/exec"
        opts: dict = {"timeout_ms": timeout_ms}
        if cwd:
            opts["working_dir"] = cwd
        payload = {
            "tool": "bash",
            "tool_params": {"command": command},
            "opts": opts,
        }
        try:
            resp = httpx.post(url, json=payload, timeout=max(timeout_ms / 1000.0 + 5, 10.0))
            resp.raise_for_status()
            data = resp.json()
        except Exception as exc:  # noqa: BLE001
            logger.error("sandbox_observer.exec_failed sandbox_id=%s url=%s: %s", sandbox_id, url, exc)
            raise ExecUnavailable(f"exec {sandbox_id} failed (url={url}): {exc}") from exc

        return ExecResult(
            sandbox_id=sandbox_id,
            command=command,
            exit_code=int(data.get("exit_code", -1)),
            stdout=data.get("stdout", "") or "",
            stderr=data.get("stderr", "") or "",
            truncated=bool(data.get("truncated", False)),
            duration_ms=data.get("duration_ms"),
        )

    # -- 管理能力（pause/resume/snapshot，经 daemon RPC 通道） -------
    async def pause_sandbox(self, sandbox_id: str) -> PauseResult:
        """经 daemon RPC ``sandbox.pause`` 暂停沙箱（Local/Docker 模式）。"""
        try:
            result = await self._daemon_client.send_request("sandbox.pause", {"sandbox_id": sandbox_id})
        except Exception as exc:  # noqa: BLE001
            logger.error("sandbox_observer.pause failed sandbox_id=%s mode=%s: %s", sandbox_id, self._mode, exc)
            raise ProviderError(f"sandbox.pause {sandbox_id} failed ({self._mode}): {exc}") from exc
        return PauseResult(sandbox_id=sandbox_id, paused=bool(result.get("paused", False)))

    async def resume_sandbox(self, sandbox_id: str) -> ResumeResult:
        """经 daemon RPC ``sandbox.resume`` 恢复沙箱（Local/Docker 模式）。"""
        try:
            result = await self._daemon_client.send_request("sandbox.resume", {"sandbox_id": sandbox_id})
        except Exception as exc:  # noqa: BLE001
            logger.error("sandbox_observer.resume failed sandbox_id=%s mode=%s: %s", sandbox_id, self._mode, exc)
            raise ProviderError(f"sandbox.resume {sandbox_id} failed ({self._mode}): {exc}") from exc
        return ResumeResult(sandbox_id=sandbox_id, resumed=bool(result.get("resumed", False)))

    async def create_snapshot(self, sandbox_id: str) -> SnapshotResult:
        """经 daemon RPC ``sandbox.snapshot`` 生成快照（Local/Docker 模式）。

        Local/Docker：SandboxSnapshots 拷贝 workdir → snapshot_root（含 size_bytes）；
        E2B 模式走 SDK 直连（E2BProvider），不经此路径。
        """
        try:
            result = await self._daemon_client.send_request("sandbox.snapshot", {"sandbox_id": sandbox_id})
        except Exception as exc:  # noqa: BLE001
            logger.error("sandbox_observer.snapshot failed sandbox_id=%s mode=%s: %s", sandbox_id, self._mode, exc)
            raise ProviderError(f"sandbox.snapshot {sandbox_id} failed ({self._mode}): {exc}") from exc
        return SnapshotResult(
            snapshot_id=result.get("snapshot_id", ""),
            sandbox_id=sandbox_id,
            created_at=result.get("created_at"),
            store_path=result.get("store_path"),
            size_bytes=result.get("size_bytes"),
            sandbox_mode=result.get("sandbox_mode"),
        )

    async def delete_snapshot(self, snapshot_id: str) -> SnapshotDeleteResult:
        """经 daemon RPC ``sandbox.snapshot_delete`` 单条删除快照（幂等）。"""
        try:
            result = await self._daemon_client.send_request("sandbox.snapshot_delete", {"snapshot_id": snapshot_id})
        except Exception as exc:  # noqa: BLE001
            logger.error("sandbox_observer.snapshot_delete failed snapshot_id=%s mode=%s: %s", snapshot_id, self._mode, exc)
            raise ProviderError(f"sandbox.snapshot_delete {snapshot_id} failed ({self._mode}): {exc}") from exc
        return SnapshotDeleteResult(
            snapshot_id=snapshot_id,
            deleted=bool(result.get("deleted", False)),
            already_gone=bool(result.get("already_gone", False)),
        )

    async def list_snapshots(self, sandbox_id: str | None = None) -> list[SnapshotResult]:
        """经 daemon RPC ``sandbox.snapshot_list`` 列出快照（可选 sandbox_id 过滤）。"""
        params: dict[str, Any] = {}
        if sandbox_id:
            params["sandbox_id"] = sandbox_id
        try:
            result = await self._daemon_client.send_request("sandbox.snapshot_list", params)
        except Exception as exc:  # noqa: BLE001
            logger.error("sandbox_observer.snapshot_list failed mode=%s: %s", self._mode, exc)
            raise ProviderError(f"sandbox.snapshot_list failed ({self._mode}): {exc}") from exc
        out: list[SnapshotResult] = []
        for item in result.get("snapshots") or []:
            out.append(SnapshotResult(
                snapshot_id=item.get("snapshot_id", ""),
                sandbox_id=item.get("sandbox_id", ""),
                created_at=item.get("created_at"),
                store_path=item.get("store_path"),
                size_bytes=item.get("size_bytes"),
                sandbox_mode=item.get("sandbox_mode"),
            ))
        return out


# ── 统一入口（路由层只依赖此类）───────────────────────────────────


class SandboxObserver:
    """按 daemon sandbox.mode 分派到具体 Provider。

    构造一次（lifespan 注入），路由层只依赖本类的统一接口。
    """

    def __init__(self, settings, daemon_client=None) -> None:
        self._mode = settings.sandbox_mode
        self._daemon_client = daemon_client  # local/docker 模式 kill/prune 的 daemon RPC 通道
        self._provider = self._build_provider(settings)

    def _build_provider(self, settings):
        mode = settings.sandbox_mode
        if mode == "e2b":
            return E2BProvider(
                api_url=settings.sandbox_e2b_api_url,
                api_key=settings.sandbox_e2b_api_key,
                ca_cert_path=settings.sandbox_e2b_ca_cert_path,
            )
        if mode in ("local", "docker"):
            # local/docker：全 RPC 通道（sandbox.list / sandbox.destroy）
            return LocalProvider(mode=mode, daemon_client=self._daemon_client)
        raise ProviderError(f"Unsupported sandbox.mode: {mode}")

    @property
    def mode(self) -> str:
        return self._mode

    def capabilities(self) -> dict[str, bool]:
        """能力矩阵：三模式统一支持 list/kill/prune + pause/snapshot；template 仅 e2b。

        local/docker 全部依赖 daemon RPC 通道（sandbox.list / sandbox.destroy /
        sandbox.pause / sandbox.resume / sandbox.snapshot / sandbox.snapshot_delete /
        sandbox.snapshot_list），daemon 离线时调用会返回 502 ProviderError——
        能力矩阵表示"模式支持"，运行期可用性由路由层错误映射体现。
        template 是 e2b 平台资源（SDK Template.build），local/docker 无模板概念。
        """
        return {"list": True, "kill": True, "prune": True, "files": True,
                "pause": True, "snapshot": True,
                "template": self._mode == "e2b"}

    async def list_sandboxes(
        self, state: str | None = None, mode: str | None = None
    ) -> list[SandboxInfo]:
        provider = self._provider
        if mode and mode != self._mode:
            raise UnsupportedAction(f"mode '{mode}' differs from configured sandbox.mode '{self._mode}'")
        if self._mode == "e2b":
            return await asyncio.to_thread(provider.list_sandboxes, state)
        # local/docker：async RPC 通道（LocalProvider 方法为 async）
        return await provider.list_sandboxes(state)

    async def get_sandbox(self, sandbox_id: str) -> SandboxInfo:
        if self._mode == "e2b":
            return await asyncio.to_thread(self._provider.get_sandbox, sandbox_id)
        return await self._provider.get_sandbox(sandbox_id)

    async def kill_sandbox(self, sandbox_id: str) -> KillResult:
        if self._mode == "e2b":
            return await asyncio.to_thread(self._provider.kill_sandbox, sandbox_id)
        # local/docker：经 daemon RPC sandbox.destroy（幂等，E2）
        return await self._daemon_destroy(sandbox_id)

    async def _daemon_destroy(self, sandbox_id: str) -> KillResult:
        """经 DaemonClient 调 sandbox.destroy（local/docker 模式统一通道）。

        daemon_client 未注入（构造时未传或 daemon 离线）时抛 ProviderError——
        销毁动作必须由 daemon SandboxLifecycle 执行（唯一真相来源，I-5），
        桥接层无法替代（进程 PID / SessionManager 映射均在 C++ 侧）。

        幂等语义（already_gone）由 daemon 在 ``result.already_gone`` 中返回
        （handle_sandbox_destroy 对未命中沙箱返回 success + already_gone=true）；
        这里**不做字符串模糊匹配**——daemon 抛出的任何异常（含 Method not found、
        连接失败）一律映射为 ProviderError，避免把"方法不存在"误判成"已消失"
        造成静默失败（M3：错误必须可定位）。
        """
        if self._daemon_client is None:
            raise ProviderError(
                f"daemon client unavailable; cannot destroy sandbox (mode={self._mode})"
            )
        try:
            result = await self._daemon_client.send_request(
                "sandbox.destroy", {"sandbox_id": sandbox_id}
            )
        except Exception as exc:  # noqa: BLE001
            logger.error(
                "sandbox_observer.daemon_destroy failed sandbox_id=%s: %s", sandbox_id, exc
            )
            raise ProviderError(f"daemon destroy {sandbox_id} failed: {exc}") from exc
        destroyed = bool(result.get("destroyed", False))
        already_gone = bool(result.get("already_gone", False))
        return KillResult(sandbox_id=sandbox_id, killed=destroyed, already_gone=already_gone)

    async def prune_stale(self, older_minutes: int, dry_run: bool = True) -> PruneResult:
        if self._mode == "e2b":
            return await asyncio.to_thread(self._provider.prune_stale, older_minutes, dry_run)
        # local/docker：经 sandbox.list RPC 枚举 → 按闲置时长（last_active_at_ms
        # 相对 server_now_ms）判定超龄 → dry_run 返回列表 / 实际逐个经 daemon RPC
        # 销毁（幂等）。与 daemon GC（gc_destroy_ttl_ms，last_active_at 语义）对齐。
        sandboxes = await self._provider.list_sandboxes(None)
        stale: list[dict[str, Any]] = []
        for sb in sandboxes:
            if sb.age_seconds is None:
                continue  # 无活跃时间戳（last_active_at 缺失）不参与闲置判定
            age_minutes = sb.age_seconds // 60
            if age_minutes >= older_minutes:
                stale.append({"sandbox_id": sb.sandbox_id, "age_minutes": age_minutes})

        result = PruneResult(dry_run=dry_run, older_minutes=older_minutes, stale=stale)
        if dry_run or not stale:
            return result

        failed = 0
        for item in stale:
            try:
                await self._daemon_destroy(item["sandbox_id"])
            except Exception as exc:  # noqa: BLE001
                failed += 1
                logger.warning(
                    "sandbox_observer.daemon_prune_kill failed %s: %s", item["sandbox_id"], exc
                )
        result.killed = len(stale) - failed
        result.failed = failed
        logger.info(
            "sandbox_observer.daemon_prune mode=%s older_minutes=%d dry_run=%s killed=%d failed=%d",
            self._mode, older_minutes, dry_run, result.killed, result.failed,
        )
        return result

    async def exec_command(self, sandbox_id: str, command: str, timeout_ms: int = 30000, cwd: str | None = None) -> ExecResult:
        if self._mode == "e2b":
            return await asyncio.to_thread(self._provider.exec_command, sandbox_id, command, timeout_ms, cwd)
        # local/docker：async RPC 通道（LocalProvider 方法为 async）
        return await self._provider.exec_command(sandbox_id, command, timeout_ms, cwd)

    # -- 管理能力统一接口（pause/resume/snapshot） -------------------
    async def pause_sandbox(self, sandbox_id: str) -> PauseResult:
        """暂停沙箱（B21：RUNNING→STOPPED 可逆）。

        e2b 走 SDK pause；local/docker 经 daemon RPC ``sandbox.pause``。
        """
        if self._mode == "e2b":
            return await asyncio.to_thread(self._provider.pause_sandbox, sandbox_id)
        return await self._provider.pause_sandbox(sandbox_id)

    async def resume_sandbox(self, sandbox_id: str) -> ResumeResult:
        """恢复沙箱（B21：STOPPED→RUNNING 可逆）。

        e2b 走 SDK connect 唤醒（auto_resume 语义，无显式 resume 方法）；
        local/docker 经 daemon RPC ``sandbox.resume``。
        """
        if self._mode == "e2b":
            return await asyncio.to_thread(self._provider.resume_sandbox, sandbox_id)
        return await self._provider.resume_sandbox(sandbox_id)

    async def create_snapshot(self, sandbox_id: str) -> SnapshotResult:
        """生成快照（e2b 走 SDK create_snapshot；local/docker 经 daemon RPC ``sandbox.snapshot``）。"""
        if self._mode == "e2b":
            return await asyncio.to_thread(self._provider.create_snapshot, sandbox_id)
        return await self._provider.create_snapshot(sandbox_id)

    async def delete_snapshot(self, snapshot_id: str) -> SnapshotDeleteResult:
        """单条删除快照（幂等；e2b 走 SDK delete_snapshot；local/docker 经 daemon RPC）。"""
        if self._mode == "e2b":
            return await asyncio.to_thread(self._provider.delete_snapshot, snapshot_id)
        return await self._provider.delete_snapshot(snapshot_id)

    async def list_snapshots(self, sandbox_id: str | None = None) -> list[SnapshotResult]:
        """列出快照（e2b 需 sandbox_id 定位实例；local/docker 可选过滤）。"""
        if self._mode == "e2b":
            return await asyncio.to_thread(self._provider.list_snapshots, sandbox_id)
        return await self._provider.list_snapshots(sandbox_id)

    # -- 模板能力（仅 e2b 模式） --------------------------------------
    async def create_template(self, name: str, from_template: str = "base",
                              cpu_count: int = 2, memory_mb: int = 1024) -> TemplateResult:
        """构建 e2b 模板（SDK Template.build，同步长耗时——前端需 loading 态）。

        local/docker 无模板概念 → UnsupportedAction（400，能力矩阵 template=false）。
        """
        if self._mode != "e2b":
            raise UnsupportedAction(
                f"template creation not supported in {self._mode} mode (e2b only)"
            )
        return await asyncio.to_thread(
            self._provider.create_template, name, from_template, cpu_count, memory_mb
        )

    async def list_templates(self) -> list[TemplateResult]:
        """列出已构建 e2b 模板（SDK 无原生 list API——返回空 + warning 由路由层标注）。"""
        if self._mode != "e2b":
            raise UnsupportedAction(
                f"template listing not supported in {self._mode} mode (e2b only)"
            )
        return await asyncio.to_thread(self._provider.list_templates)


__all__ = [
    "SandboxInfo",
    "KillResult",
    "PruneResult",
    "ExecResult",
    "PauseResult",
    "ResumeResult",
    "SnapshotResult",
    "SnapshotDeleteResult",
    "TemplateResult",
    "SandboxObserver",
    "SandboxObserverError",
    "SandboxNotFound",
    "UnsupportedAction",
    "ProviderError",
    "ExecUnavailable",
    "E2BProvider",
    "LocalProvider",
]
