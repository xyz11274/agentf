# 16 · L1–L3：经验注入、动作约束和循环控制

[文档首页](README.md)

## 本章目标

理解增强如何真正进入模型输入、动作派发和下一轮决策，以及 applied 怎样被核验。

## 设计思路

模型提供经验内容，部署配置授权策略，宿主在执行边界应用决定并提交回执。每个动作与循环分别维护状态。

![L1：怎样才算经验实际应用](figures/16-applied.svg)

读图：按实际发送的模型输入核验 applied 回执。 [放大 SVG](figures/16-applied.svg)。


![L2 与 L3 的作用范围](figures/16-controls.svg)

读图：动作与循环规则通过部署配置授权，在对应边界执行。 [放大 SVG](figures/16-controls.svg)。

## 策略如何作用于 L1、L2、L3

### L1：将获准经验绑定到实际模型输入

处理发生在 `activity.begin(kind=model)`：

1. 查询当前任务和状态相关的经验。
2. 只保留 memory_id 与 content_revision 同时匹配批准项的经验。
3. 用结构化 JSON 放入经验模板，逐条检查 `max_context_bytes`；放不下的条目跳过。
4. 外面加带 policy_id 的经验边界标记，用于识别与回执验证。
5. 根据宿主 `binding.pointer` 指定的 JSON 位置注入，返回增强后的参数。

`src/request_binding.cpp` 支持两种绑定模式：

- `text`：追加到指定字符串。
- `system_message`：追加到指定消息数组的 system 内容；没有适合位置时创建 system 消息。

宿主语言和模型 SDK 可自行选择，通过 binding 描述输入位置。
将返回的 input 用于实际发送，并在结束时回报 actual_input。

结束时提供 `actual_input` 和 `executed`。`PolicyRuntime::verify` 重新计算同一次查询应有的增强内容，检查是否真的允许发送、
是否有调用标识、增强内容是否出现在实际绑定位置，以及出现次数等完整性要求。
通过后 `advice.applied` 回执为 applied；有决定但未观察到真实应用为 failed；无增强决定为 rejected。
这些状态用于记录策略应用，业务结果另行保存。
模型请求中的经验内容通过 actual_input 核验，业务效果由后续任务评测衡量。

### L2：逐动作过滤，不重写宿主并行调度

L2 按部署配置的动作规则进行确定性判定。
每个动作先默认为 allow，再按 scope 与 operation 精确匹配规则：

- `deny=true`：拒绝该动作。
- `required_arguments`：任一指定顶层参数缺失或为空则拒绝。
- 空值包括 null、未设置、空字符串、空数组、空对象；数字 0 或 false 不因为是假值就视为缺失。
- 同一动作首条实际触发的拒绝规则确定 reason/rule_id。

L2 不自动重排序、串行化或取消兄弟动作，也不把一个动作拒绝改写成整个任务失败。
通过的动作仍由宿主按原来的串并行关系执行。
回执核对被拒绝的 action_id 确实出现在 skipped 集合，并没有出现在 executed 集合。
回执对照控制决定与实际执行情况；业务工具的返回状态单独记录。

### L3：有状态的循环收敛控制

每个 loop 保留独立的 `LoopProgress`：执行流、scope、policy_id、轮次、状态指纹、连续无进展次数、重规划次数和上次决定。
不会使用一个全局计数器让其他并行 loop 相互影响。

通用边界在 `loop.check` 对 state 做稳定 JSON 序列化并计算 SHA-256。
“无进展”只判断指纹相同，不理解业务意义。state 应包含稳定、与进度有关的事实，例如已完成子任务、已有素材、待满足条件。
state 使用影响业务进展的稳定字段，排除无关时间戳或随机 ID，并随真实进展更新。

核心状态机为：

```text
若该 loop 仍有未结束活动：continue，不在此时 stop/replan
若有效等级低于 L3：continue
若重复提交同一 iteration：状态指纹必须一致，不再次累计
否则：
    fingerprint 与前次相同且存在前次轮次 → no_progress += 1
    fingerprint 改变                       → no_progress = 0

    max_iterations > 0 且 iteration > max_iterations → stop
    否则 max_no_progress > 0 且 no_progress >= max_no_progress：
        允许 replan 且 replans < max_replans → replan，replans += 1，no_progress = 0
        否则                               → stop
    其他                                   → continue
```

max_iterations=12 时，第 13 次下一轮检查触发上限。
max_no_progress=3 时，首次状态建立基准，之后连续三次相同状态达到门槛。
规则按 scope 精确匹配，仅执行已配置阈值的限制项。
已经 stop 的通用 loop 不允许继续 check 或派发新活动；需要新的独立执行时应使用新的循环身份。

`loop.check` 给建议，`loop.commit` 回报宿主实际采用的决定。
stop 按宿主回报核验；replan 在后续模型活动中核对重规划上下文是否进入请求。
并发模型调用分别验证各自的 actual_input。
stop 作用于对应 loop；其他循环和在途操作继续由宿主调度，副作用通过业务接口处理。

## 配置规则与学习建议的边界

L1 可使用通过审核的经验正文；L2 action_rules 和 L3 loop_rules 是部署方显式配置的结构化规则。
动作和循环控制使用显式配置的机器规则，经验建议通过模型上下文参与决策。
启用 GEPA 后，后台也可在 `control_learning` 授权范围内生成结构化规则；生成规则附带来源经验 revision，
经过真实任务评测后随完整策略发布。手工基础规则仍保留，生成循环限制只能收紧基础边界。
经验通过模型上下文影响选择，动作与循环规则通过确定性控制生效；rule_id 和回执记录具体机制。

多个并发动作各自返回 allow/reject、各自报告 executed 和终态。拒绝一个动作不改写宿主已有依赖图。
各并发循环使用独立 loop_id、计数和状态。

## 回执如何判定与排错

- 模型：actual_input 必须是实际发出的请求，且保留增强后的绑定位置；仅返回给宿主不算应用。
- 动作：报告是否执行、返回结果以及对 allow/reject 决定的实际处理。
- 循环：check 是建议/授权边界，commit 是宿主真实采纳；未落实的 control 阻止正常 run.end。
- applied 表示宿主上报的执行数据通过策略应用核验。
- 无匹配经验时返回 no_match，模型使用原输入继续执行。

暂停开关和失败降级不追回已发生业务副作用。要全系统紧急停用需要宿主协调所有进程；
set_enabled 控制当前原生客户端，多实例部署分别管理开关。

## 实现定位

- `src/request_binding.cpp`
- `src/policy_runtime.cpp::PolicyRuntime::decide / observe`
- `src/boundary_runtime.cpp`
- `proto/runtime_policy.proto`
- `tests/test_policy_runtime.cpp`

---

[文档首页](README.md) · [上一章：记忆检索、策略快照与版本](15-policy-snapshots.md) · [下一章：自动学习触发、批处理与积压控制](17-learning-scheduler.md)
