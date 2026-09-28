# python 客户端：通过统一协议收集执行事件并接收策略建议。
# 传输与业务执行分离；接收回执不代表 Agent 已完成整个任务。
# 打点的输入是建议修改前的请求，执行后必须回传实际输入、输出与状态。
# 取消、超时和采集缺口需要显式处理，不能把缺失证据当成正常完成。

"""由配置管理的打点入口，宿主只传递已有身份和业务数据。"""

import json
import os
from pathlib import Path
import re
import threading
import uuid
from urllib.parse import urlsplit
from evolution import Evolution


_SECRET_NAME = re.compile(r"key|token|secret|password|credential|auth", re.I)
_CREDENTIAL = re.compile(
    r"\b(?:sk-|ark-)[A-Za-z0-9_-]{16,}\b|\bAKIA[A-Z0-9]{16}\b|"
    r"\bAIza[A-Za-z0-9_-]{20,}\b|\b(?:gh[pousr]_|github_pat_)[A-Za-z0-9_]{20,}\b|"
    r"\bxox[a-z]-[A-Za-z0-9-]{16,}\b|\bBearer\s+[A-Za-z0-9._~+/-]{16,}|"
    r"(?:api[_ -]?key|access[_ -]?token|secret|password)\s*[:=]\s*[\"']?[A-Za-z0-9._~+/-]{12,}|"
    r"[?&](?:api[_-]?key|access[_-]?token|token|signature|sig|key)=[^&\s\"']{8,}|"
    r"\beyJ[A-Za-z0-9_-]{16,}\.[A-Za-z0-9_-]{8,}\.[A-Za-z0-9_-]{8,}\b|"
    r"-----BEGIN [A-Z ]*PRIVATE KEY-----",
    re.I,
)
_PRIVATE_FIELDS = {
    "authorization",
    "apikey",
    "password",
    "passwd",
    "accesstoken",
    "refreshtoken",
    "clientsecret",
    "privatekey",
    "cookie",
    "setcookie",
    "secret",
    "credentials",
}


# 检查载荷是否包含不得外发的敏感内容，拒绝时只上报最小缺口标记。
def _private_payload(value, public_env_names=()):
    """传输前拒绝敏感采集内容，不修改原始业务数据。"""
    extra = os.environ.get("EVOLUTION_SECRET_ENV_NAMES", "").split(",")
    secrets = tuple(
        v
        for k, v in os.environ.items()
        if len(v) >= 8
        and (k in extra or (k not in public_env_names and _SECRET_NAME.search(k)))
    )
    pending = [value]
    visited = 0
    while pending:
        item = pending.pop()
        visited += 1
        if visited > 100000:
            return True
        if isinstance(item, dict):
            for key, child in item.items():
                if (
                    not isinstance(key, str)
                    or key.lower().replace("-", "").replace("_", "") in _PRIVATE_FIELDS
                ):
                    return True
                pending.extend((key, child))
        elif isinstance(item, (list, tuple)):
            pending.extend(item)
        elif isinstance(item, str):
            # 宿主可能已移除 URL 签名；继续检查剩余文本，
            # 同时仍要在原始文本中匹配已知凭据值。
            inspected = item.replace("<redacted:credential_field>", "")
            if _CREDENTIAL.search(inspected) or any(
                secret in item for secret in secrets
            ):
                return True
    return False


class _LocalPoints(Evolution):
    public_env_names = ()

    # 提交框架无关打点，必要时在同一边界先取得策略决定，再记录实际执行结果。
    def point(self, fields, **options):
        try:
            private = _private_payload(fields, self.public_env_names)
        except Exception:
            private = True
        if not private:
            return super().point(fields, **options)
        # 只有不含业务内容的缺口或中止标记可以越过本地边界。
        # 原始输入和异常只保留给业务调用方。
        fields = fields if isinstance(fields, dict) else {}
        ref = fields.get("ref", {})
        if (
            isinstance(ref, dict)
            and ref.get("run")
            and not _private_payload(ref, self.public_env_names)
        ):
            if fields.get("phase") in ("after", "abort"):
                marker = {
                    "phase": "abort",
                    "kind": fields.get("kind", "custom"),
                    "ref": ref,
                    "output": None,
                    "status": "unknown",
                    "capture_gap": True,
                }
            else:
                marker = {"phase": "gap", "kind": "run", "ref": {"run": ref["run"]}}
            super().point(marker, **options)
        return self._point_observed(
            fields,
            {
                "input": fields.get("input"),
                "decision": "continue" if fields.get("kind") == "loop" else "allow",
                "degraded": True,
                "capture_complete": False,
                "policy_applied": False,
                "reason": "privacy_rejected",
            },
        )


class ConfiguredPoints:
    # 初始化 __init__ 的状态和资源；输入参数的校验与所有权在构造阶段确定。
    # 对象创建后复用既有配置，实际执行与收尾分别由后续接口负责。
    def __init__(self):
        self._path = None
        self._clients = {}
        self._config = {}
        self._lock = threading.Lock()

    # 获取复用的客户端实例，避免每次业务调用重新初始化资源。
    def _client(self, user_id):
        path = os.environ.get("EVOLUTION_POINT_CONFIG", "")
        trial_path = os.environ.get("TRAJECTORY_EVOLUTION_TRIAL_CONFIG", "")
        if trial_path and not (
            os.environ.get("TRAJECTORY_EVOLUTION_TRIAL_URL")
            and os.environ.get("TRAJECTORY_EVOLUTION_TRIAL_TOKEN")
        ):
            # 这个便捷入口仅使用 HTTP；原生评测试验不能退回生产采集端。
            # 原生评测试验应使用原生 SDK。
            return None
        if not path and not trial_path:
            return None
        try:
            with self._lock:
                if self._path != (path, trial_path):
                    self._path = (path, trial_path)
                    self._clients = {}
                    self._config = {}
                    with Path(path or trial_path).open(encoding="utf-8") as source:
                        raw = source.read(65537)
                    if len(raw) > 65536:
                        return None
                    config = json.loads(raw)
                    public_names = config.get("privacy", {}).get("public_env_names", [])
                    if (
                        not isinstance(public_names, list)
                        or len(public_names) > 64
                        or any(
                            not isinstance(name, str)
                            or not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]{0,127}", name)
                            for name in public_names
                        )
                    ):
                        return None
                    if trial_path:
                        with Path(trial_path).open(encoding="utf-8") as source:
                            trial_raw = source.read(65537)
                        if len(trial_raw) > 65536:
                            return None
                        config["principals"] = json.loads(trial_raw)["principals"]
                        config["url"] = os.environ["TRAJECTORY_EVOLUTION_TRIAL_URL"]
                    endpoint = config.get(
                        "url", "http://127.0.0.1:" + str(config.get("port", 8766))
                    )
                    # 隐式应用接线使用本地采集端。
                    # 远程部署则应显式配置 HTTP 客户端。
                    if urlsplit(endpoint).hostname not in (
                        "localhost",
                        "127.0.0.1",
                        "::1",
                    ):
                        return None
                    for principal in config.get("principals", []):
                        token = os.environ.get(principal["token_env"], "")
                        if token:
                            client = _LocalPoints(endpoint, token, retries=0)
                            client.public_env_names = frozenset(public_names)
                            self._clients[principal["user_id"]] = client
                    self._config = config
                if not user_id and len(self._clients) == 1:
                    return next(iter(self._clients.values()))
                return self._clients.get(user_id)
        except Exception:
            return None

    # 提交框架无关打点，必要时在同一边界先取得策略决定，再记录实际执行结果。
    def point(self, fields, *, user_id=""):
        client = self._client(user_id)
        if client is None:
            return {
                "input": fields.get("input"),
                "decision": "continue" if fields.get("kind") == "loop" else "allow",
                "degraded": True,
                "capture_complete": False,
                "enabled": False,
            }
        return client.point(fields, timeout_ms=self._config.get("point_timeout_ms", 20))

    # 包装宿主业务调用：先查询策略，再最多执行一次原业务函数，最后记录结果。
    def call(self, fields, execute, *, user_id="", result_format="json"):
        client = self._client(user_id)
        if client is None:
            return execute(fields.get("input"))
        fields = dict(fields)
        if "ref" in fields and "key" not in fields["ref"]:
            fields["ref"] = dict(fields["ref"], key=uuid.uuid4().hex)
        return client.call(
            fields,
            execute,
            result_format=result_format,
            timeout_ms=self._config.get("point_timeout_ms", 20),
        )

    # 在原循环条件处取得并提交实际控制决定；返回是否允许继续，而不是另起一套循环。
    def allow_loop(self, fields, *, user_id=""):
        client = self._client(user_id)
        return client is None or client.allow_loop(
            fields, timeout_ms=self._config.get("point_timeout_ms", 20)
        )

    # 异步包装宿主业务协程；观测失败不能吞掉原业务异常，也不能重试业务调用。
    async def acall(self, fields, execute, *, user_id="", result_format="json"):
        client = self._client(user_id)
        if client is None:
            return await execute(fields.get("input"))
        fields = dict(fields)
        if "ref" in fields and "key" not in fields["ref"]:
            fields["ref"] = dict(fields["ref"], key=uuid.uuid4().hex)
        return await client.acall(
            fields,
            execute,
            result_format=result_format,
            timeout_ms=self._config.get("point_timeout_ms", 20),
        )

    # 异步执行循环守卫，沿用同步版本的前后打点和降级约定。
    async def aallow_loop(self, fields, *, user_id=""):
        client = self._client(user_id)
        return client is None or await client.aallow_loop(
            fields, timeout_ms=self._config.get("point_timeout_ms", 20)
        )

    # 提交经过组件编排的当前派发波次，业务回调只负责实际提交，不等待其他分支完成。
    def dispatch(self, fields, execute, *, user_id="", result_format="json"):
        client = self._client(user_id)
        if client is None:
            return execute(fields.get("input"))
        return client.dispatch(
            fields,
            execute,
            result_format=result_format,
            timeout_ms=self._config.get("point_timeout_ms", 20),
        )

    # 根据已收到的异步结果恢复编排，只有依赖条件满足的后续波次才能提交。
    def resume_dispatch(self, fields, execute, *, user_id="", result_format="json"):
        client = self._client(user_id)
        if client is None:
            return {"pending": False, "completion": fields.get("input")}
        return client.resume_dispatch(
            fields,
            execute,
            result_format=result_format,
            timeout_ms=self._config.get("point_timeout_ms", 20),
        )


points = ConfiguredPoints()
