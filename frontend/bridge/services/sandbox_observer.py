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


# ── 异常类型（对齐 L4 契约 §4）────────────────────────────────────


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
            info = {}
            with contextlib.suppress(Exception):  # noqa: BLE001 — get_info 失败不阻断列表
                info = sb.get_info() if hasattr(sb, "get_info") else {}

            created_at = self._extract_created_at(info)
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
            info = {}
            with contextlib.suppress(Exception):  # noqa: BLE001
                info = sb.get_info() if hasattr(sb, "get_info") else {}
            created_at = self._extract_created_at(info)
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
        """能力矩阵：三模式统一支持 list/kill/prune（E1/E2 后 local/docker 不再是只读）。

        local/docker 全部依赖 daemon RPC 通道（sandbox.list / sandbox.destroy），
        daemon 离线时调用会返回 502 ProviderError——能力矩阵表示"模式支持"，
        运行期可用性由路由层错误映射体现。
        """
        return {"list": True, "kill": True, "prune": True, "files": True}

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


__all__ = [
    "SandboxInfo",
    "KillResult",
    "PruneResult",
    "ExecResult",
    "SandboxObserver",
    "SandboxObserverError",
    "SandboxNotFound",
    "UnsupportedAction",
    "ProviderError",
    "ExecUnavailable",
    "E2BProvider",
    "LocalProvider",
]
