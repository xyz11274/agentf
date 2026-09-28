# python 客户端：通过统一协议收集执行事件并接收策略建议。
# 传输与业务执行分离；接收回执不代表 Agent 已完成整个任务。
# 打点的输入是建议修改前的请求，执行后必须回传实际输入、输出与状态。
# 取消、超时和采集缺口需要显式处理，不能把缺失证据当成正常完成。

"""绑定既有任务接收入口，仅在组件拥有的隔离试验内启用。"""

import asyncio
import inspect
import os
import time
import uuid


class _Entry:
    # 初始化 __init__ 的状态和资源；输入参数的校验与所有权在构造阶段确定。
    # 对象创建后复用既有配置，实际执行与收尾分别由后续接口负责。
    def __init__(self):
        from evolution import Evolution

        url = os.environ.get("TRAJECTORY_EVOLUTION_TRIAL_URL", "")
        token = os.environ.get("TRAJECTORY_EVOLUTION_TRIAL_TOKEN", "")
        self.client = Evolution(url, token, timeout_ms=5000, retries=0)
        self.consumer = uuid.uuid4().hex

    # 获取下一条允许交付的评测输入，空返回代表当前无可执行任务。
    def next(self, index):
        return self.client.invoke(
            "evaluation.next", {"consumer": self.consumer, "index": index}
        )

    # 返回或提交固定失败诊断，不把业务正文或凭据放入错误消息。
    def fail(self):
        try:
            self.client.invoke("evaluation.fail", {"consumer": self.consumer})
        except Exception:
            pass


# 通过本模块的评测入口处理请求，执行与采集生命周期由对应契约约束。
def evaluate(receive):
    """每轮调用一次既有同步接收函数，普通在线启动返回 False。
    回调只确认交付；真实运行结束打点提供结果并解锁下一轮。"""
    if os.environ.get("TRAJECTORY_EVOLUTION_EVALUATION_ENTRY") != "1":
        return False
    entry = _Entry()
    index = 0
    try:
        while True:
            packet = entry.next(index)
            state = packet.get("state")
            if state == "done":
                return True
            if state == "waiting":
                time.sleep(0.1)
                continue
            if state != "input" or packet.get("index") != index:
                raise RuntimeError("evaluation_entry_response_invalid")
            result = receive(packet["input"])
            if inspect.isawaitable(result):
                if inspect.iscoroutine(result):
                    result.close()
                raise RuntimeError("evaluation_entry_async_receiver_requires_aevaluate")
            index += 1
    except BaseException:
        entry.fail()
        # 不要打印模型提供方错误、凭据或业务异常正文。
        raise RuntimeError("evaluation_entry_failed") from None


# 在原事件循环中异步交付评测输入；宿主实际收尾仍由正常运行结束打点确认。
async def aevaluate(receive):
    """在调用方现有事件循环中执行原接收函数，不新建事件循环。"""
    if os.environ.get("TRAJECTORY_EVOLUTION_EVALUATION_ENTRY") != "1":
        return False
    entry = _Entry()
    index = 0
    try:
        while True:
            packet = await asyncio.to_thread(entry.next, index)
            state = packet.get("state")
            if state == "done":
                return True
            if state == "waiting":
                await asyncio.sleep(0.1)
                continue
            if state != "input" or packet.get("index") != index:
                raise RuntimeError("evaluation_entry_response_invalid")
            result = receive(packet["input"])
            if inspect.isawaitable(result):
                await result
            index += 1
    except BaseException:
        await asyncio.to_thread(entry.fail)
        raise RuntimeError("evaluation_entry_failed") from None
