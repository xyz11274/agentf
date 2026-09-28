# python 客户端：通过统一协议收集执行事件并接收策略建议。
# 原生入口复用包内 C ABI；注意句柄生命周期与跨语言内存释放。
# 打点的输入是建议修改前的请求，执行后必须回传实际输入、输出与状态。
# 取消、超时和采集缺口需要显式处理，不能把缺失证据当成正常完成。

"""进程内原生 L0–L3 客户端，不进行 HTTP、子进程、重试或隐式阻塞刷新。"""

import ctypes
import json
import os
import uuid

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


class LocalEvolution(PointCalls):
    # 初始化 __init__ 的状态和资源；输入参数的校验与所有权在构造阶段确定。
    # 对象创建后复用既有配置，实际执行与收尾分别由后续接口负责。
    def __init__(self, library, config, token, **options):
        self._point_init()
        # 在应用启动、业务请求到来之前初始化一次。
        self._pid = os.getpid()
        self._native = ctypes.CDLL(os.fspath(library))
        self._native.te_local_open.argtypes = [
            ctypes.c_char_p,
            ctypes.c_char_p,
            ctypes.c_char_p,
            ctypes.POINTER(ctypes.c_void_p),
        ]
        self._native.te_local_open.restype = ctypes.c_void_p
        self._native.te_local_invoke.argtypes = [
            ctypes.c_void_p,
            ctypes.c_char_p,
            ctypes.c_size_t,
        ]
        self._native.te_local_invoke.restype = ctypes.c_void_p
        self._native.te_local_metrics.argtypes = [ctypes.c_void_p]
        self._native.te_local_metrics.restype = ctypes.c_void_p
        self._native.te_local_drain.argtypes = [ctypes.c_void_p]
        self._native.te_local_close.argtypes = [ctypes.c_void_p]
        self._native.te_local_set_enabled.argtypes = [ctypes.c_void_p, ctypes.c_int]
        for name in ("te_local_drain", "te_local_close", "te_local_set_enabled"):
            getattr(self._native, name).restype = None
        self._native.te_local_free.argtypes = [ctypes.c_void_p]
        self._native.te_local_free.restype = None
        self._limit = options.get("max_request_bytes", 65536)
        error = ctypes.c_void_p()
        self._handle = self._native.te_local_open(
            os.fsencode(config),
            token.encode("utf-8"),
            json.dumps(options, allow_nan=False).encode("utf-8"),
            ctypes.byref(error),
        )
        if error.value:
            self._native.te_local_free(error)
        if not self._handle:
            raise RuntimeError("local_initialization_failed")

    # 读取并释放原生返回值，使用库提供的释放函数。
    def _value(self, pointer):
        if not pointer:
            raise RuntimeError("local_unavailable")
        try:
            return json.loads(ctypes.string_at(pointer).decode("utf-8"))
        finally:
            self._native.te_local_free(pointer)

    # 在编码前检查对象规模，避免巨大业务输入阻塞打点热路径。
    def _fits(self, value, budget, depth=0):
        # JSON 编码前限制遍历规模，避免复制巨大媒体输入。
        if depth > 32 or budget < 0:
            return -1
        if isinstance(value, str):
            return budget - len(value) * 6 - 2
        if isinstance(value, dict):
            for key, item in value.items():
                if not isinstance(key, str):
                    return -1
                budget = self._fits(key, budget - 2, depth + 1)
                budget = self._fits(item, budget, depth + 1)
                if budget < 0:
                    return -1
            return budget - 2
        if isinstance(value, (tuple, list)):
            for item in value:
                budget = self._fits(item, budget - 1, depth + 1)
                if budget < 0:
                    return -1
            return budget - 2
        if value is None or isinstance(value, (bool, float, int)):
            return budget - 32
        return -1

    # 执行一次组件协议调用，检查响应信封并保持请求身份一致。
    def invoke(self, operation, fields, request_id=None):
        fields = fields if isinstance(fields, dict) else {}
        request_id = request_id or uuid.uuid4().hex
        fallback = {
            "degraded": True,
            "reason": "local_unavailable",
            "capture_complete": False,
            "policy_applied": False,
            "input": fields.get("input"),
            "event_id": "",
        }
        if operation == "point":
            context = (
                dict(fields["context"])
                if isinstance(fields.get("context"), dict)
                else {}
            )
            kind = fields.get("kind", context.get("kind", "custom"))
            context.update(kind=kind, degraded=True, caused_by=[])
            context.setdefault("run_id", "unobserved-" + request_id)
            fallback.update(
                context=context,
                decision="continue" if kind == "loop" else "allow",
                closed=False,
                ended=False,
            )
        if operation == "run.begin":
            fallback["run_id"] = "unobserved-" + request_id
        if operation == "activity.begin":
            fallback.update(activity_id="unobserved-" + request_id, decision="allow")
            fallback["context"] = {
                "run_id": fields.get("run_id"),
                "activity_id": fallback["activity_id"],
                "degraded": True,
            }
        if operation == "loop.check":
            fallback.update(
                control_id="unobserved-" + request_id, decision="continue", context=""
            )
        if operation in ("activity.end", "activity.abort", "loop.commit"):
            fallback["ended"] = False
        if operation in ("run.end", "run.abort"):
            fallback.update(closed=False, evaluation_status="quarantined")
        try:
            if not self._handle or os.getpid() != self._pid:
                return fallback
            packet = {**fields, "operation": operation, "request_id": request_id}
            if self._fits(packet, self._limit) < 0:
                raise ValueError("local_request_too_large")
            body = json.dumps(packet, ensure_ascii=False, allow_nan=False).encode(
                "utf-8"
            )
            response = self._value(
                self._native.te_local_invoke(self._handle, body, len(body))
            )
            if response.get("ok") is not True:
                raise ValueError("local_unavailable")
            result = response["result"]
            return {**fallback, **result} if result.get("degraded") else result
        except Exception:
            # 即使本地编码失败，被跳过的记录也必须使所属运行带上采集缺口标记。
            try:
                if self._handle and os.getpid() == self._pid:
                    gap = {
                        "operation": "capture.gap",
                        "request_id": uuid.uuid4().hex,
                        "run_id": fields.get(
                            "run_id", (fields.get("context") or {}).get("run_id")
                        ),
                        "reason": "observer_unavailable",
                    }
                    if operation == "point" and isinstance(fields.get("ref"), dict):
                        gap = {
                            "operation": "point",
                            "request_id": uuid.uuid4().hex,
                            "kind": "run",
                            "phase": "gap",
                            "ref": {"run": fields["ref"].get("run")},
                        }
                    body = json.dumps(gap, allow_nan=False).encode("utf-8")
                    self._value(
                        self._native.te_local_invoke(self._handle, body, len(body))
                    )
            except Exception:
                pass
            return fallback

    # 提交框架无关打点，必要时在同一边界先取得策略决定，再记录实际执行结果。
    def point(self, fields, request_id=None):
        fields = self._point_fields(fields)
        return self._point_observed(fields, self.invoke("point", fields, request_id))

    # 读取组件自身的采集和排队指标，不把观测指标当作业务任务得分。
    def metrics(self):
        return (
            self._value(self._native.te_local_metrics(self._handle))
            if self._handle and os.getpid() == self._pid
            else {}
        )

    # 切换运行时是否启用采集或注入；关闭时仍按协议保持安全降级。
    def set_enabled(self, enabled):
        if self._handle and os.getpid() == self._pid:
            self._native.te_local_set_enabled(self._handle, int(bool(enabled)))

    # 等待当前待持久化工作完成，用于维护或关闭；不要放在主 Agent 每个调用的热路径。
    def drain(self):
        """阻塞式维护操作，不能在模型、工具或运行边界调用。"""
        if self._handle and os.getpid() == self._pid:
            self._native.te_local_drain(self._handle)

    # 执行评测等阻塞式管理操作，与主 Agent 热路径分离。
    def manage(self, operation, fields=None, request_id=None):
        """阻塞式评测管理入口，应在 Agent 业务边界之外调用。"""
        if not self._handle or os.getpid() != self._pid:
            raise RuntimeError("local_unavailable")
        packet = {
            **(fields or {}),
            "operation": operation,
            "request_id": request_id or uuid.uuid4().hex,
        }
        if self._fits(packet, self._limit) < 0:
            raise ValueError("local_request_too_large")
        body = json.dumps(packet, ensure_ascii=False, allow_nan=False).encode("utf-8")
        call = self._native.te_local_manage
        call.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_size_t]
        call.restype = ctypes.c_void_p
        result = self._value(call(self._handle, body, len(body)))
        if not result.get("ok"):
            raise RuntimeError(result.get("error", "local_management_unavailable"))
        return result["result"]

    # 关闭资源并等待本实现要求的在途操作完成，重复关闭按协议处理。
    def close(self):
        """只用于应用关闭；先停止调用方，再排空缓冲并等待后台退出。"""
        handle, self._handle = self._handle, None
        if handle and os.getpid() == self._pid:
            self._native.te_local_close(handle)
