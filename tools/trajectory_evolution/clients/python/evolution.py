# python 客户端：通过统一协议收集执行事件并接收策略建议。
# 传输与业务执行分离；接收回执不代表 Agent 已完成整个任务。
# 打点的输入是建议修改前的请求，执行后必须回传实际输入、输出与状态。
# 取消、超时和采集缺口需要显式处理，不能把缺失证据当成正常完成。

"""覆盖 L0–L3 打点的 HTTP/JSON 客户端，仅使用 Python 标准库。"""

import http.client
import json
import logging
import os
import re
import time
import uuid
from urllib.parse import urlsplit

try:
    from evolution_point import PointCalls
except ModuleNotFoundError:
    # 同时支持按文件路径直接加载已经安装的客户端。
    import importlib.util
    from pathlib import Path

    _spec = importlib.util.spec_from_file_location(
        "_evolution_point", Path(__file__).with_name("evolution_point.py")
    )
    _module = importlib.util.module_from_spec(_spec)
    _spec.loader.exec_module(_module)
    PointCalls = _module.PointCalls


class EvolutionError(Exception):
    # 初始化 __init__ 的状态和资源；输入参数的校验与所有权在构造阶段确定。
    # 对象创建后复用既有配置，实际执行与收尾分别由后续接口负责。
    def __init__(self, code, status=0):
        super().__init__(code)
        self.status = status


class Evolution(PointCalls):
    # 初始化 __init__ 的状态和资源；输入参数的校验与所有权在构造阶段确定。
    # 对象创建后复用既有配置，实际执行与收尾分别由后续接口负责。
    def __init__(
        self, url, token, timeout_ms=7000, max_response_bytes=2097152, retries=1
    ):
        self._point_init()
        if os.environ.get("TRAJECTORY_EVOLUTION_TRIAL_URL") and os.environ.get(
            "TRAJECTORY_EVOLUTION_TRIAL_TOKEN"
        ):
            url, token = (
                os.environ["TRAJECTORY_EVOLUTION_TRIAL_URL"],
                os.environ["TRAJECTORY_EVOLUTION_TRIAL_TOKEN"],
            )
        endpoint = urlsplit(url)
        if (
            endpoint.username
            or endpoint.password
            or endpoint.query
            or endpoint.fragment
            or endpoint.path not in ("", "/")
            or not endpoint.hostname
            or (
                endpoint.scheme != "https"
                and not (
                    endpoint.scheme == "http"
                    and endpoint.hostname in ("localhost", "127.0.0.1", "::1")
                )
            )
        ):
            raise EvolutionError("https_or_loopback_url_required")
        if (
            not isinstance(token, str)
            or len(token) < 24
            or any(ord(c) < 33 or ord(c) > 126 for c in token)
        ):
            raise EvolutionError("token_invalid")
        if (
            type(timeout_ms) is not int
            or not 100 <= timeout_ms <= 300000
            or type(max_response_bytes) is not int
            or not 1024 <= max_response_bytes <= 16777216
            or type(retries) is not int
            or not 0 <= retries <= 3
        ):
            raise EvolutionError("client_limits_invalid")
        self._endpoint, self._token = endpoint, token
        self._timeout, self._max_bytes, self._retries = (
            timeout_ms / 1000,
            max_response_bytes,
            retries,
        )

    # 执行一次组件协议调用，检查响应信封并保持请求身份一致。
    def invoke(self, operation, fields, request_id=None):
        """返回结果或固定错误码；只能重试同一个协议请求，不能重试业务操作。"""
        return self._invoke(operation, fields, request_id, self._timeout, self._retries)

    # 提交框架无关打点，必要时在同一边界先取得策略决定，再记录实际执行结果。
    def point(self, fields, timeout_ms=20, request_id=None):
        """一次有界打点尝试；传输失败时保留业务输入并标记降级。"""
        request_id = request_id or uuid.uuid4().hex
        try:
            fields = self._point_fields(fields)
            if type(timeout_ms) is not int or not 1 <= timeout_ms <= 5000:
                raise ValueError("point_timeout_invalid")
            result = self._invoke("point", fields, request_id, timeout_ms / 1000, 0)
            if fields.get("kind") == "run" and result.get("degraded"):
                code = result.get("reason", "point_capture_degraded")
                if not isinstance(code, str) or not re.fullmatch(r"[a-z_]{1,96}", code):
                    code = "point_capture_degraded"
                try:
                    logging.getLogger(__name__).warning(
                        "run point capture degraded: %s", code
                    )
                except Exception:
                    pass
            return self._point_observed(fields, result)
        except Exception as error:
            fields = fields if isinstance(fields, dict) else {}
            context = (
                dict(fields["context"])
                if isinstance(fields.get("context"), dict)
                else {}
            )
            kind = fields.get("kind", context.get("kind", "custom"))
            if kind == "run":
                # 只记录固定传输错误码，不记录业务输入或异常正文。
                code = (
                    str(error)
                    if isinstance(error, EvolutionError)
                    else "observer_unavailable"
                )
                if not re.fullmatch(r"[a-z_]{1,96}", code):
                    code = "observer_unavailable"
                try:
                    logging.getLogger(__name__).warning(
                        "run point capture degraded: %s", code
                    )
                except Exception:
                    pass
            context.update(kind=kind, degraded=True, caused_by=[])
            context.setdefault("run_id", "unobserved-" + request_id)
            return self._point_observed(
                fields,
                {
                    "context": context,
                    "input": fields.get("input"),
                    "degraded": True,
                    "capture_complete": False,
                    "policy_applied": False,
                    "decision": "continue" if kind == "loop" else "allow",
                    "closed": False,
                    "ended": False,
                    "reason": "observer_unavailable",
                },
            )

    # 执行底层协议传输，复用请求身份与总期限，不能扩展为业务回调重试。
    def _invoke(self, operation, fields, request_id, timeout, retries):
        if not isinstance(fields, dict):
            raise EvolutionError("object_required")
        try:
            body = json.dumps(
                {
                    **fields,
                    "operation": operation,
                    "request_id": request_id or uuid.uuid4().hex,
                },
                ensure_ascii=False,
                allow_nan=False,
            ).encode("utf-8")
        except (TypeError, ValueError, UnicodeError):
            raise EvolutionError("request_invalid") from None
        if len(body) > 2097152:
            raise EvolutionError("request_too_large")
        for attempt in range(retries + 1):
            connection = None
            try:
                deadline = time.monotonic() + timeout
                kind = (
                    http.client.HTTPSConnection
                    if self._endpoint.scheme == "https"
                    else http.client.HTTPConnection
                )
                # 直接连接：不使用环境代理，也不跟随重定向。
                connection = kind(
                    self._endpoint.hostname, self._endpoint.port, timeout=timeout
                )
                connection.connect()
                transport = connection.sock
                transport.settimeout(max(0.001, deadline - time.monotonic()))
                connection.request(
                    "POST",
                    "/v1/invoke",
                    body=body,
                    headers={
                        "Content-Type": "application/json",
                        "Authorization": "Bearer " + self._token,
                    },
                )
                transport.settimeout(max(0.001, deadline - time.monotonic()))
                response = connection.getresponse()
                content = bytearray()
                while not response.isclosed():
                    remaining = deadline - time.monotonic()
                    if remaining <= 0:
                        raise EvolutionError("observer_unavailable")
                    transport.settimeout(remaining)
                    part = response.read1(
                        min(65536, self._max_bytes + 1 - len(content))
                    )
                    if not part:
                        break
                    content.extend(part)
                    if len(content) > self._max_bytes:
                        raise EvolutionError("response_too_large")
                try:
                    payload = json.loads(content.decode("utf-8"))
                except (ValueError, UnicodeError):
                    raise EvolutionError("response_invalid", response.status) from None
                if not isinstance(payload, dict):
                    raise EvolutionError("response_invalid", response.status)
                if not 200 <= response.status < 300 or payload.get("ok") is not True:
                    code = payload.get("error", "rpc_failed")
                    code = (
                        code
                        if isinstance(code, str) and re.fullmatch(r"[a-z_]{1,96}", code)
                        else "rpc_failed"
                    )
                    raise EvolutionError(code, response.status)
                if not isinstance(payload.get("result"), dict):
                    raise EvolutionError("response_invalid", response.status)
                return payload["result"]
            except Exception as error:
                failure = (
                    error
                    if isinstance(error, EvolutionError)
                    else EvolutionError("observer_unavailable")
                )
                if attempt == retries or 0 < failure.status < 500:
                    raise failure from None
            finally:
                if connection:
                    connection.close()
