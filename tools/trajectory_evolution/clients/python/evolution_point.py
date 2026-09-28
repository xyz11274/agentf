# python 客户端：通过统一协议收集执行事件并接收策略建议。
# 传输与业务执行分离；接收回执不代表 Agent 已完成整个任务。
# 打点的输入是建议修改前的请求，执行后必须回传实际输入、输出与状态。
# 取消、超时和采集缺口需要显式处理，不能把缺失证据当成正常完成。

"""本地与 HTTP 客户端共享的执行包装，不要求宿主另写适配器。"""

import asyncio
import json
import uuid


class PointCalls:
    # 初始化打点客户端的本地缺口跟踪状态，避免观测失败被后续成功覆盖。
    def _point_init(self):
        self._point_gaps, self._point_gaps_full = set(), False

    # 补齐协议字段与稳定请求身份，调用方业务输入保持原样。
    def _point_fields(self, fields):
        ref = fields.get("ref") if isinstance(fields, dict) else None
        run = ref.get("run") if isinstance(ref, dict) else None
        if isinstance(run, str) and (run in self._point_gaps or self._point_gaps_full):
            return dict(fields, capture_gap=True)
        return fields

    # 跟踪打点是否降级或结束，将采集缺口传递到最终运行结果。
    def _point_observed(self, fields, result):
        ref = fields.get("ref") if isinstance(fields, dict) else None
        run = ref.get("run") if isinstance(ref, dict) else None
        if isinstance(run, str) and len(run) <= 256:
            if result.get("degraded"):
                if len(self._point_gaps) < 1024:
                    self._point_gaps.add(run)
                else:
                    self._point_gaps_full = True
            elif result.get("closed") or result.get("run_closed"):
                self._point_gaps.discard(run)
        return result

    # 根据引用式或上下文式协议构造结束字段，保持前后打点归属一致。
    def _finish_fields(self, fields, prepared, **outcome):
        identity = (
            {"ref": fields["ref"]}
            if "ref" in fields
            else {"context": prepared["context"]}
        )
        return dict(identity, kind=fields.get("kind", "custom"), **outcome)

    # 包装宿主业务调用：先查询策略，再最多执行一次原业务函数，最后记录结果。
    def call(self, fields, execute, *, result_format="json", **point_options):
        """包装一次既有 JSON 输入输出调用，永不重试业务调用。"""
        prepared = self.point(dict(fields, phase="before"), **point_options)
        actual = prepared.get("input", fields.get("input"))
        if prepared.get("decision") == "reject":
            reason = prepared.get("reason", "policy_rejected")
            result = {"status": "rejected", "reason": reason, "error": reason}
            if result_format == "json_text":
                result = json.dumps(result, ensure_ascii=False)
            self.point(
                self._finish_fields(
                    fields,
                    prepared,
                    phase="after",
                    executed=False,
                    actual_input=actual,
                    output=result,
                ),
                **point_options,
            )
            return result
        try:
            result = execute(actual)
        except BaseException:
            # 异常正文可能包含凭据，原始异常只交给业务调用方；
            # 采集端仅记录未知终态，不记录异常原文。
            self.point(
                self._finish_fields(
                    fields, prepared, phase="abort", status="unknown", output=None
                ),
                **point_options,
            )
            raise
        self.point(
            self._finish_fields(
                fields,
                prepared,
                phase="after",
                executed=True,
                actual_input=actual,
                output=result,
            ),
            **point_options,
        )
        return result

    # 异步包装宿主业务协程；观测失败不能吞掉原业务异常，也不能重试业务调用。
    async def acall(self, fields, execute, *, result_format="json", **point_options):
        """只等待原协程一次；HTTP 观测放到事件循环之外执行。"""
        prepared = await asyncio.to_thread(
            self.point, dict(fields, phase="before"), **point_options
        )
        actual = prepared.get("input", fields.get("input"))
        if prepared.get("decision") == "reject":
            reason = prepared.get("reason", "policy_rejected")
            result = {"status": "rejected", "reason": reason, "error": reason}
            if result_format == "json_text":
                result = json.dumps(result, ensure_ascii=False)
            await asyncio.to_thread(
                self.point,
                self._finish_fields(
                    fields,
                    prepared,
                    phase="after",
                    executed=False,
                    actual_input=actual,
                    output=result,
                ),
                **point_options,
            )
            return result
        try:
            result = await execute(actual)
        except BaseException:
            await asyncio.to_thread(
                self.point,
                self._finish_fields(
                    fields, prepared, phase="abort", status="unknown", output=None
                ),
                **point_options,
            )
            raise
        await asyncio.to_thread(
            self.point,
            self._finish_fields(
                fields,
                prepared,
                phase="after",
                executed=True,
                actual_input=actual,
                output=result,
            ),
            **point_options,
        )
        return result

    # 在原循环条件处取得并提交实际控制决定；返回是否允许继续，而不是另起一套循环。
    def allow_loop(self, fields, **point_options):
        """直接放在既有循环条件处使用，并提交实际采用的决定。"""
        if "ref" in fields:
            return (
                self.point(
                    dict(fields, phase="guard", kind="loop"), **point_options
                ).get("decision", "continue")
                != "stop"
            )
        prepared = self.point(
            dict(fields, phase="before", kind="loop"), **point_options
        )
        decision = prepared.get("decision", "continue")
        self.point(
            self._finish_fields(
                dict(fields, kind="loop"), prepared, phase="after", decision=decision
            ),
            **point_options,
        )
        return decision != "stop"

    # 异步执行循环守卫，沿用同步版本的前后打点和降级约定。
    async def aallow_loop(self, fields, **point_options):
        return await asyncio.to_thread(self.allow_loop, fields, **point_options)

    # 领取并执行可提交波次，再记录宿主确认；确认丢失时标记缺口，不能盲目重放业务提交。
    def _dispatch_wave(self, prepared, execute, result_format, point_options):
        """每个派发批次只领取一次；提交确认不能替代任务完成。"""
        last_output = None
        for _ in range(128):
            if not prepared.get("wave_id"):
                return dict(prepared, output=prepared.get("output", last_output))
            wave_id, plan_ref = prepared["wave_id"], prepared["plan_ref"]

            # 构造当前阶段的事件或打点，保留调用身份与父子关系。
            def event(name, payload, unique=False):
                ref = {
                    "run": plan_ref["run"],
                    "parent": plan_ref["key"],
                    "key": wave_id
                    + ":"
                    + name
                    + (":" + uuid.uuid4().hex if unique else ""),
                }
                return self.point(
                    {
                        "phase": "event",
                        "kind": "dispatch",
                        "name": name,
                        "ref": ref,
                        "input": dict(payload, wave_id=wave_id),
                    },
                    **point_options,
                )

            claimed = event("claim", {}, unique=True)
            if claimed.get("degraded"):
                # 即使回复丢失，运行时也可能已经接受了本次领取。
                # 不得重试业务回调，也不能重新派发整个批次。
                event("abort", {})
                raise RuntimeError("dispatch_claim_uncertain")
            if not claimed.get("execute"):
                return dict(claimed, managed=True)
            try:
                last_output = execute(claimed["input"])
                decoded = (
                    json.loads(last_output)
                    if result_format == "json_text"
                    else last_output
                )
                if not isinstance(decoded, dict):
                    raise ValueError("dispatch_ack_invalid")
                last_output = decoded
            except BaseException:
                event("abort", {})
                raise
            prepared = event("accept", {"output": decoded})
            if prepared.get("degraded"):
                # 提交已经发生；保留宿主确认，并标记采集缺口。
                # 业务回调重放无法修复一个结果不确定的提交确认。
                return dict(prepared, managed=True, pending=True, output=decoded)
        raise RuntimeError("dispatch_wave_limit")

    # 提交经过组件编排的当前派发波次，业务回调只负责实际提交，不等待其他分支完成。
    def dispatch(self, fields, execute, *, result_format="json", **point_options):
        """包装既有异步批量提交入口，等待和依赖状态由包内持久管理。"""
        prepared = self.point(
            dict(fields, phase="before", kind="dispatch"), **point_options
        )
        if prepared.get("degraded") or not prepared.get("managed"):
            if not prepared.get("degraded") and prepared.get("context", {}).get(
                "activity_id"
            ):
                self.point(
                    self._finish_fields(
                        dict(fields, kind="custom"),
                        prepared,
                        phase="abort",
                        status="unknown",
                        output=None,
                        capture_gap=True,
                    ),
                    **point_options,
                )
            return execute(fields.get("input"))
        result = self._dispatch_wave(prepared, execute, result_format, point_options)
        output = result.get("output")
        if output is None:
            raise RuntimeError("dispatch_submission_pending")
        return (
            json.dumps(output, ensure_ascii=False)
            if result_format == "json_text"
            else output
        )

    # 根据已收到的异步结果恢复编排，只有依赖条件满足的后续波次才能提交。
    def resume_dispatch(
        self, fields, execute, *, result_format="json", **point_options
    ):
        """放在宿主现有批次完成入口，在下一次模型调用之前恢复编排。"""
        prepared = self.point(
            dict(fields, phase="event", kind="dispatch", name="complete"),
            **point_options,
        )
        if prepared.get("degraded") or not prepared.get("managed"):
            return dict(prepared, pending=False, completion=fields.get("input"))
        return self._dispatch_wave(prepared, execute, result_format, point_options)
