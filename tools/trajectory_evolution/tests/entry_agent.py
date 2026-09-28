# entry_agent 回归验证：检查包内协议、边界条件与失败处理。
# 本地桩用于重现确定性场景，不代表真实 Agent 或远程模型的业务能力。
# 测试应同时验证正常路径与拒绝路径，避免错误数据被当成成功结果。

"""同步和异步接收入口的固定回归宿主，使用包内入口绑定。
这是确定性协议测试，不是业务能力基准或真实实验驱动器。"""

import asyncio
import ctypes
import json
import os
from pathlib import Path
import subprocess
import sys
import time

SOURCE = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(SOURCE / "clients/python"))
from evolution import Evolution
from evolution_entry import evaluate, aevaluate

client = Evolution(
    "https://deployment.invalid", "deployment-placeholder-credential", retries=0
)
if "--native" in sys.argv:
    from evolution_local import LocalEvolution

    native_client = LocalEvolution(
        sys.argv[sys.argv.index("--native") + 1],
        "unused.json",
        "deployment-placeholder-credential",
    )

    class NativePoints:
        # 提交框架无关打点，必要时在同一边界先取得策略决定，再记录实际执行结果。
        def point(self, fields, **_):
            return native_client.point(fields)

    client = NativePoints()


# 测试辅助函数：receive。
# 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
def receive(message):
    assert set(message) == {
        "text",
        "user",
        "conversation",
        "key",
        "turn",
        "initial",
        "history",
    }
    turn = message["turn"]
    if "--initial-file" in sys.argv:
        initial_file = Path(os.environ["TRAJECTORY_EVOLUTION_TRIAL_INITIAL_STATE"])
        initial = json.loads(initial_file.read_text())
        assert initial == {"value": 20}
        assert initial_file.stat().st_mode & 0o777 == 0o600
        message["initial"] = initial
    assert len(message["history"]) == turn * 2 + 1
    if turn:
        assert message["history"][-2]["role"] == "assistant"
        assert message["history"][-2]["content"] == "20"
    if "--error" in sys.argv:
        raise ValueError(os.environ.get("EVAL_TEST_API_KEY", "secret"))
    if "--no-run" in sys.argv:
        return "accepted"
    ref = {"run": message["key"]}
    if turn and "--late-previous-failure" in sys.argv:
        previous = {"run": message["key"].rsplit("-", 1)[0] + "-" + str(turn - 1)}
        client.point(
            dict(
                phase="after",
                kind="run",
                ref=previous,
                output={"api_key": "never-store-this-value"},
            ),
            timeout_ms=5000,
        )
    if turn and "--rejected-start" in sys.argv:
        client.point(
            dict(
                phase="before",
                kind="run",
                ref=ref,
                input={"api_key": os.environ["EVAL_TEST_API_KEY"]},
            ),
            timeout_ms=5000,
        )
        return "accepted"
    if turn and "--missing-start-gap" in sys.argv:
        client.point(dict(phase="gap", kind="run", ref=ref), timeout_ms=5000)
        return "accepted"
    run = client.point(
        dict(
            phase="before",
            kind="run",
            ref=ref,
            conversation_id=message["conversation"],
            input=message["text"],
        ),
        timeout_ms=5000,
    )
    assert not run.get("degraded"), run
    if turn and "--open-gap" in sys.argv:
        client.point(dict(phase="gap", kind="run", ref=ref), timeout_ms=5000)
        return "accepted"
    if turn and "--rejected-child" in sys.argv:
        client.point(
            dict(
                phase="before",
                kind="model",
                ref=dict(ref, key="rejected"),
                input={"api_key": os.environ["EVAL_TEST_API_KEY"]},
            ),
            timeout_ms=5000,
        )
        return "accepted"
    if "--extra-run" in sys.argv:
        client.point(
            dict(
                phase="before",
                kind="run",
                ref={"run": message["key"] + "-extra"},
                input="extra",
            ),
            timeout_ms=5000,
        )
    # 后台工作线程接收同一条现有消息，不接收整个 Agent 对象。
    if "--dispatch" in sys.argv and "--worker" not in sys.argv:
        process = subprocess.Popen(
            [sys.executable, "-B", __file__, "--worker"],
            stdin=subprocess.PIPE,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        process.stdin.write(json.dumps(message).encode())
        process.stdin.close()
        return "accepted"
    if "--worker" in sys.argv:
        time.sleep(0.25)
    call = client.point(
        dict(
            phase="before",
            kind="model",
            ref=dict(ref, key="model"),
            name="test-model",
            input=message["text"],
        ),
        timeout_ms=5000,
    )
    client.point(
        dict(
            phase="after",
            kind="model",
            ref=dict(ref, key="model"),
            executed=True,
            actual_input=call["input"],
            output=20,
            metrics={"input_tokens": 5, "output_tokens": 1},
        ),
        timeout_ms=5000,
    )
    # 工具预览补查回归：结果中段才有完整证据，最终答复不复制这段结果。
    if "--long-tool" in sys.argv or "--huge-tool" in sys.argv:
        tool_ref = dict(ref, key="preview-tool")
        tool_input = {"query": "验收查询"}
        client.point(
            dict(phase="before", kind="action", ref=tool_ref,
                 name="preview-tool", input=tool_input),
            timeout_ms=5000,
        )
        padding = 45000 if "--huge-tool" in sys.argv else 9000
        client.point(
            dict(phase="after", kind="action", ref=tool_ref, executed=True,
                 actual_input=tool_input,
                 output={"text": "x" * padding + "中段验收证据完整" + "y" * padding}),
            timeout_ms=5000,
        )
    if "--missing-end" in sys.argv:
        return "accepted"
    output = {
        "answer": "20",
        "state": {"value": message["initial"]["value"]},
        "turn": turn,
    }
    # 单轮裁剪回归专用：有效证据位于中段，头尾预览都无法看见。
    if "--long-answer" in sys.argv or "--huge-answer" in sys.argv:
        padding = 45000 if "--huge-answer" in sys.argv else 9000
        output["answer"] = "x" * padding + "中段验收证据完整" + "y" * padding
    ended = client.point(
        dict(
            phase="after",
            kind="run",
            ref=ref,
            output=output,
            business_status="failed"
            if "--business-failure" in sys.argv
            else "succeeded",
        ),
        timeout_ms=5000,
    )
    assert not ended.get("degraded"), ended
    return "accepted"


# 测试辅助函数：receive_async。
# 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
async def receive_async(message):
    loop = asyncio.get_running_loop()
    await asyncio.sleep(0.01)
    assert asyncio.get_running_loop() is loop
    return receive(message)


if "--worker" in sys.argv:
    # 复用已经开始的运行，重复开始打点使用稳定引用身份。
    receive(json.load(sys.stdin))
elif "--async" in sys.argv:
    asyncio.run(aevaluate(receive_async))
elif "--cabi" in sys.argv:
    native = ctypes.CDLL(sys.argv[sys.argv.index("--cabi") + 1])
    callback_type = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_char_p, ctypes.c_void_p)

    # 测试辅助函数：callback。
    # 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
    def callback(value, _):
        try:
            receive(json.loads(value))
            return 0
        except Exception:
            return -1

    native.te_evaluate.argtypes = [callback_type, ctypes.c_void_p]
    assert native.te_evaluate(callback_type(callback), None) == 1
else:
    evaluate(receive)
if "--stay-alive" in sys.argv:
    time.sleep(30)
