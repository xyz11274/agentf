"""可信任务上下文：隔离并发分支，不把内部任务标识交给模型填写。"""
from contextlib import contextmanager
from contextvars import ContextVar
import os
import hashlib

_TASK = ContextVar("business_task", default="")


def current_task():
    return _TASK.get()


def worker_task():
    # 同名 skill 可被多次派发；dispatch key 比 skill 名或进程 PID 更稳定。
    return os.environ.get("AF_EXEC_SKILL_KEY") or os.environ.get("AF_SELF_TASK_ID", "")


def receipt_key(skill):
    """回执同样按分支隔离，防止不同任务使用相同 request_id 时互相重放。"""
    task = current_task()
    return skill + ':' + hashlib.sha256(task.encode()).hexdigest() if task else skill


@contextmanager
def task_context(task):
    token = _TASK.set(task or "")
    try:
        yield
    finally:
        _TASK.reset(token)
