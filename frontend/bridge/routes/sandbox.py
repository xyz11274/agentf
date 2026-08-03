"""AgentFlow Bridge — sandbox management routes (local dev observability).

提供沙箱列表/详情/销毁/过期清理的 REST 端点。路由层只依赖
:class:`~services.sandbox_observer.SandboxObserver` 统一接口，不感知
provider 细节（e2b SDK / daemon 日志 / 目录扫描）。

设计 ref: ``docs/design/features/sandbox-management.md`` +
``docs/design/architecture/modules/sandbox-management-api.md``。
"""

from __future__ import annotations

import logging
from typing import Any

from fastapi import APIRouter, HTTPException, Request
from pydantic import BaseModel

from services.sandbox_observer import (
    ExecUnavailable,
    ProviderError,
    SandboxNotFound,
    SandboxObserver,
    UnsupportedAction,
)

logger = logging.getLogger(__name__)

router = APIRouter(prefix="/api/sandbox", tags=["sandbox"])


def _get_observer(request: Request) -> SandboxObserver:
    """从 app.state 读取 SandboxObserver（lifespan 注入）。

    未注入时抛 500（正常启动路径必然注入；此分支仅防御误用）。
    """
    observer = getattr(request.app.state, "sandbox_observer", None)
    if observer is None:
        raise HTTPException(status_code=500, detail="INTERNAL: SandboxObserver not initialized")
    return observer


def _ok(data: Any) -> dict[str, Any]:
    """标准成功信封（对齐 agents.py 约定）。"""
    return {"success": True, "data": data, "error": None}


def _map_observer_error(exc: Exception) -> HTTPException:
    """将 observer 异常映射为 HTTP 状态（对齐 L4 契约 §3）。"""
    if isinstance(exc, SandboxNotFound):
        return HTTPException(status_code=404, detail=str(exc))
    if isinstance(exc, UnsupportedAction):
        return HTTPException(status_code=400, detail=str(exc))
    if isinstance(exc, ExecUnavailable):
        logger.error("sandbox exec unavailable: %s", exc)
        return HTTPException(status_code=502, detail=str(exc))
    if isinstance(exc, ProviderError):
        logger.error("sandbox provider error: %s", exc)
        return HTTPException(status_code=502, detail=str(exc))
    logger.error("sandbox route unexpected error: %s", exc, exc_info=True)
    return HTTPException(status_code=500, detail=str(exc))


@router.get("/overview", name="sandbox_overview")
async def sandbox_overview(request: Request) -> dict[str, Any]:
    """``GET /api/sandbox/overview`` — 模式 + 能力矩阵 + 沙箱计数。"""
    observer = _get_observer(request)
    try:
        sandboxes = await observer.list_sandboxes()
    except Exception as exc:  # noqa: BLE001 — 统一错误映射
        raise _map_observer_error(exc) from exc

    counts: dict[str, int] = {"total": len(sandboxes), "running": 0, "stopped": 0, "unknown": 0}
    for sb in sandboxes:
        state = sb.state.upper()
        if state == "RUNNING":
            counts["running"] += 1
        elif state == "STOPPED":
            counts["stopped"] += 1
        else:
            counts["unknown"] += 1

    return _ok(
        {
            "mode": observer.mode,
            "capabilities": observer.capabilities(),
            "counts": counts,
            "config": {},  # 不暴露配置细节；能力矩阵已足够前端决策
            "warning": None,
        }
    )


@router.get("/list", name="sandbox_list")
async def sandbox_list(
    request: Request,
    state: str | None = None,
    mode: str | None = None,
) -> dict[str, Any]:
    """``GET /api/sandbox/list`` — 沙箱列表。

    查询参数：
    - ``state``: 过滤状态（running/stopped/recycling/unknown），默认全部
    - ``mode``: 强制指定 provider（e2b/local/docker），默认取配置
    """
    observer = _get_observer(request)
    try:
        sandboxes = await observer.list_sandboxes(state=state, mode=mode)
    except Exception as exc:  # noqa: BLE001
        raise _map_observer_error(exc) from exc
    return _ok({"sandboxes": [s.to_dict() for s in sandboxes], "total": len(sandboxes), "warning": None})


@router.get("/{sandbox_id}", name="sandbox_detail")
async def sandbox_detail(request: Request, sandbox_id: str) -> dict[str, Any]:
    """``GET /api/sandbox/{sandbox_id}`` — 单沙箱详情。"""
    observer = _get_observer(request)
    try:
        sb = await observer.get_sandbox(sandbox_id)
    except Exception as exc:  # noqa: BLE001
        raise _map_observer_error(exc) from exc
    return _ok(sb.to_dict())


@router.post("/{sandbox_id}/kill", name="sandbox_kill")
async def sandbox_kill(request: Request, sandbox_id: str) -> dict[str, Any]:
    """``POST /api/sandbox/{sandbox_id}/kill`` — 销毁沙箱（仅 e2b 模式）。"""
    observer = _get_observer(request)
    try:
        result = await observer.kill_sandbox(sandbox_id)
    except Exception as exc:  # noqa: BLE001
        raise _map_observer_error(exc) from exc
    return _ok(result.to_dict())


@router.post("/prune", name="sandbox_prune")
async def sandbox_prune(
    request: Request,
    older_minutes: int,
    dry_run: bool = True,
) -> dict[str, Any]:
    """``POST /api/sandbox/prune?older_minutes=&dry_run=`` — 过期清理（仅 e2b 模式）。"""
    if older_minutes <= 0:
        raise HTTPException(status_code=400, detail="older_minutes must be > 0")
    observer = _get_observer(request)
    try:
        result = await observer.prune_stale(older_minutes=older_minutes, dry_run=dry_run)
    except Exception as exc:  # noqa: BLE001
        raise _map_observer_error(exc) from exc
    return _ok(result.to_dict())


class _ExecBody(BaseModel):
    command: str
    timeout_ms: int = 30000
    cwd: str | None = None  # 工作目录（绝对路径）；跨命令 cwd 延续由前端会话内持有并逐条透传


@router.post("/{sandbox_id}/exec", name="sandbox_exec")
async def sandbox_exec(request: Request, sandbox_id: str, body: _ExecBody) -> dict[str, Any]:
    """``POST /api/sandbox/{sandbox_id}/exec`` — webshell 命令执行（单条，完整输出）。

    模式通道：
    - e2b: E2B SDK ``commands.run``（cwd 透传给 SDK 一等参数）
    - local/docker: 沙箱内 HTTP server ``POST /exec``（tool=bash，沙箱侧 is_dangerous 防护）
    """
    command = body.command.strip()
    if not command:
        raise HTTPException(status_code=400, detail="command must not be empty")
    cwd = body.cwd.strip() if body.cwd else None
    observer = _get_observer(request)
    try:
        result = await observer.exec_command(
            sandbox_id=sandbox_id, command=command, timeout_ms=body.timeout_ms, cwd=cwd
        )
    except Exception as exc:  # noqa: BLE001
        raise _map_observer_error(exc) from exc
    return _ok(result.to_dict())


__all__ = ["router"]
