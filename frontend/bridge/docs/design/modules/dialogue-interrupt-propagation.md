# Dialogue 中断信号向执行节点的传播设计

**层级**: L4 技术设计（`docs/design/modules/`）
**状态**: 草案
**日期**: 2026-07-13
**关联**: `dialogue-serial-lock-and-cancel.md`（串行锁与取消机制，2026-07-04 已固化）、中止信号失效排查（2026-07-13）

---

## 1. 背景与问题

### 1.1 问题现象

`dialogue-serial-lock-and-cancel.md` §3.4 声明的 `handle_dialogue_interrupt` 已在 daemon 侧完整实现（`agent.cpp:1095-1176`），Bridge 侧 `DIALOGUE-INTERRUPT` 下发链路也已接通（`chat.py:405-411`、`chat_rest.py:108-115`）。**但从 daemon 到 skill / coordinator 子进程的最后一环缺失**——中断信号无法传导到实际执行 LLM 调用的 Python 子进程。

用户点击"终止"后，前端立即显示"已停止"（Bridge 收到 `final=true` 的 cancel 通知），但后台 skill 子进程仍在继续 `call_llm` → tool_call → 下一轮，直到 `node_timeout_sec` 或 `max_rounds` 自然耗尽。**这是一条假成功路径**，违反 F1 数据安全与 F3 不变量 I-1（用户感知与实际状态背离）。

### 1.2 根因

| 层 | 已实现 | 缺失 |
|----|--------|------|
| 前端 → Bridge | `stop` 帧 / `cancel` REST / SSE 断流兜底 | — |
| Bridge → daemon | `agent_send_control("DIALOGUE-INTERRUPT")` | — |
| daemon C++ `handle_dialogue_interrupt` | `task->interrupt()` 改状态、enqueue INTERRUPTED、enqueue cancel 通知、`cleanup_dialogue_data` | **未向子进程通信中断信号** |
| daemon C++ `task->interrupt()` | 改 `state_ = INTERRUPTED` | **只改 C++ 内存状态，无 syscall 级打断；不持有也不杀子进程 PID** |
| skill_executor.py LLM while-loop | 超时 / max_rounds / tool 死循环检测 | **不轮询任何中断信号** |
| coordinator_decide.py dispatch loop | 同上 | **不轮询任何中断信号** |

**关键事实**（2026-07-13 核对）：

- `task->interrupt()`（`task.cpp:72-80`）仅 `state_ = TaskState::INTERRUPTED`，无 `kill`/`SIGTERM`/`pthread_cancel`。
- `stop_message_threads()`（`task.cpp:154-172`）是 `join()`——等线程自己结束，非强制 kill；且这些线程是 task 内部消息队列轮询线程，**不是 skill 子进程的执行线程**。
- daemon 与 skill 子进程之间**唯一通信通道是 DataBus**（UDS socket）。子进程不主动轮询，daemon 就毫无办法。
- `skill_executor.py:execute` 的 LLM while-loop（568-1069）每轮顶部只检查 `elapsed > node_timeout_sec`，**无任何 DataBus 中断轮询**。

### 1.3 不做的事

- **不给 daemon 加 kill 子进程能力**：跨平台 PID 管理、孤儿进程回收、半终止状态一致性风险高，且 daemon 框架定位不持有子进程生命周期。
- **不做单轮 LLM call 内部取消**（HTTP abort / SSE cancel）：第一阶段只堵"假成功"，轮间隙打断已足够；单轮内取消是第二阶段优化。
- **不改 `task->interrupt()` 语义**：它作为 C++ 状态机标记仍然有效，用于 coordinator 收尾判断；本设计只补"子进程能读到"的通道。

---

## 2. 设计目标

1. **中断信号能传导到子进程**：daemon 收到 `DIALOGUE-INTERRUPT` 后，skill_executor / coordinator_decide 子进程能在**下一轮 LLM call 之前**检测到并主动退出。
2. **不误伤正常完成**：dialogue 正常完成时的 cleanup 不得让在跑的子进程误判为"被中断"。
3. **flag 生命周期自洽**：中断 flag 的写入与清理不引入新的时序矛盾或死锁。
4. **不改 daemon → 子进程的通信通道**：继续用 DataBus，不引入新通道（信号、共享内存等）。
5. **可回归验证**：新增单元测试 + E2E 覆盖"中断后子进程在下一轮间隙退出"。

---

## 3. 设计方案

### 3.1 核心机制：两阶段中断 + global sys flag

中断flag 放在 **global sys 命名空间**（不带 dialogue 前缀），原因：

- dialogue-scoped key（`/session/{sid}/sys/{did}/{key}`）会被 `cleanup_dialogue_data` 的 `remove_by_prefix` 一次性抹掉——flag 如果放这里，**会被自己紧接着的 cleanup 清掉**，子进程轮询时已读不到。
- global sys key（`/session/{sid}/sys/{key}`）不在 dialogue 级 cleanup 范围内，只能由 session 级清理兜底——天然避开"flag 何时清"的时序矛盾。

### 3.2 daemon 侧：handle_dialogue_interrupt 写 flag

在 `agent.cpp:handle_dialogue_interrupt` 的 **cleanup 之前**插入 flag 写入：

```cpp
// agent.cpp handle_dialogue_interrupt，cleanup_dialogue_data 之前：
//   写 global sys flag（不dialogue 前缀，不会被紧接着的 cleanup 清掉）
std::string interrupt_key = "/session/" + session->get_session_id() +
                            "/sys/interrupt:" + dialogue_id;
agent.publish_data(interrupt_key, DataVariant("true"));
Logger::instance().info_f("[CONTROL] DIALOGUE-INTERRUPT set flag key=%s", interrupt_key.c_str());

// 然后继续既有的 cleanup_dialogue_data（清 dialogue-scoped 子树）
BuiltinDispatcher::cleanup_dialogue_data(*this, session->get_session_id(), dialogue_id);
```

**关键不变量**：

- **I-INT-1**：flag 必须在 cleanup **之前**写入。如果 cleanup 先于 flag，子进程可能在 cleanup 后、flag 写入前的窗口期轮询到"数据空 + flag 空"，误判为"正常完成"。
- **I-INT-2**：flag key 必须在 global sys 命名空间（`/sys/interrupt:{did}`），不能在 dialogue-scoped（`/sys/{did}/interrupt`）。
- **I-INT-8**：flag 写入（daemon 侧 `agent.publish_data` → `runtime_databus_->put`）与 flag 读取（子进程侧 `databus.get_global` → SDK `sys().get`）必须落在同一物理 DataBus 实例上。两者都最终连到同一个 UDS DataBus 服务进程，故天然满足；但落地时必须用 E2E 验证跨侧读写一致（见 §5.3 `test_flag_cross_side_visible_e2e`）。
  - 证据：`agent.cpp:3160-3166` `publish_data` 调 `runtime_databus_->put(key, value)`；`databus_client.py:138-151` `get_global` 调 `self._client.sys().get(key)`。两条路径的 key 都不带 `/session/{sid}/` 前缀自动补全，调用方必须传完整 key。daemon 侧示例用 `"/session/" + session_id + "/sys/interrupt:" + dialogue_id`（与 `agent.cpp:385` 同款完整 key 形态），子进程侧 SDK 的 `sys()` 内部自动补 `/session/{sid}/sys/` 前缀，故子进程只需传 `"interrupt:{did}"`。

### 3.3 子进程侧：循环顶部轮询 flag

#### 3.3.1 skill_executor.py

在 `execute()` 的 `for round_i in range(max_rounds)` 循环顶部、`node_timeout_sec` 检查之后、`call_llm` 之前插入：

```python
# skill_executor.py execute() 循环顶部：
#   检查 daemon 下发的中断 flag（global sys 命名空间）
if databus.is_connected():
    try:
        interrupt_flag = databus.get_global(f"interrupt:{dialogue_id}")
        if interrupt_flag == "true":
            emit_event("skill_executor.interrupted", {
                "round": round_i + 1,
                "skill_name": skill_name,
                "elapsed_s": round(time.time() - _turn_start, 1),
            }, level="WARN")
            print(f"[flow]   ⚠️  interrupted by DIALOGUE-INTERRUPT at round={round_i+1}", file=sys.stderr)
            # 写 interrupted 结果给 coordinator，让它走正常 cleanup
            output = {
                "skill_result": "对话已被取消",
                "status": "interrupted",
                "skill_name": skill_name,
                "rounds_used": round_i + 1,
                "artifacts_json": json.dumps(generated_artifacts, ensure_ascii=False),
            }
            if databus.is_connected():
                databus.put("skill_result:skill-runner",
                            json.dumps(output, ensure_ascii=True))
            print(json.dumps(output, ensure_ascii=False))
            return
    except Exception as exc:
        # DataBus 抖动不阻断主流程（守 §3.7 trace 路径失败模式）
        print(f"[flow]   WARN: interrupt flag poll failed: {exc}", file=sys.stderr)
```

#### 3.3.2 coordinator_decide.py

coordinator 的 dispatch loop 同样要加轮询。在每次 `call_llm` 之前检查同一 flag：

```python
# coordinator_decide.py execute() 的 dispatch loop 内、call_llm 之前：
if databus.is_connected():
    try:
        interrupt_flag = databus.get_global(f"interrupt:{dialogue_id}")
        if interrupt_flag == "true":
            # 写一个 interrupted 最终响应给 Bridge，让它正常断流
            ...
            return
    except Exception:
        pass
```

### 3.4 flag 的清理时机

flag 是 global sys key，**不参与 dialogue 级 cleanup**（`cleanup_dialogue_data` 只清 `/sys/{did}/` 前缀，flag 在 `/sys/interrupt:{did}` 不带前缀）。它的清理走两条兜底：

| 时机 | 机制 | 文件位置 |
|------|------|---------|
| session aging 归还 | `session_cleanup_callback` → `clear_session_data_if_unused` → `remove_by_prefix("/session/{sid}/")` | `agent.cpp:598, 3608` |
| agent stop | `cleanup_resources` → 清整个 agent 的 DataBus | `agent.cpp:3115, 3285` |

**dialogue_id 是 uuid-based 唯一值**（`sysagent/server.py:428-431`、`chat.py` 的 dialogue_id 生成），即使 flag 残留到 session aging，也不会被同 client 的下一个 dialogue 误读——下一个 dialogue 用新的 did。

**不变量**：

- **I-INT-3**：flag 不在 dialogue 级 cleanup 范围内，只由 session 级清理兜底。
- **I-INT-4**：dialogue_id 唯一性保证 flag 残留无害（不会被新 dialogue 误读）。

### 3.5 与现有 cleanup 的关系澄清

| cleanup 触发点 | 是否清 flag | 原因 |
|---------------|------------|------|
| `handle_dialogue_interrupt` 中断路径 | ❌ 不清 | flag 在 global sys，cleanup 只清 `/sys/{did}/` 前缀 |
| dialogue 正常完成（`agent.cpp:2272, 2357, 2415` 等） | ❌ 不清 | 同上 |
| session aging 超时归还 | ✅ 清 | `remove_by_prefix("/session/{sid}/")` 覆盖整个 session 树 |
| agent stop | ✅ 清 | `cleanup_resources` 清全部 |

**这正好避开了"flag 何时清"的时序矛盾**——flag 不参与 dialogue 级清理，不存在"flag 先清还是数据先清"的问题。

### 3.6 为什么不用"探测数据消失"作为隐式信号

讨论中考虑过让子进程轮询 `/sys/{did}/launched_skills` 等数据是否消失，消失则判定被中断。**否决，原因**：

1. ~~正常完成也 cleanup 会误判~~（**撤回**）：经时序核对，正常完成路径的 cleanup（`agent.cpp:2357, 2415`）发生在 coordinator 收完最终响应、enqueue final 之后——此时 skill 与 coordinator 子进程均已退出，cleanup 不可能误伤在跑的子进程。此条原否决理由不成立。
2. **DataBus 抖动**：UDS 重连、瞬间状态都会让 `get` 返回 None，不等于"被清"。需配合幂等校验（连续 N 轮空才认定）才安全，引入额外轮次延迟与状态变量，违反 KISS（F6）。
3. **混合方案在共因失效模式下无兜底价值**：flag 写入失败的场景是 DataBus 进程崩溃或 UDS 断连——此场景下 cleanup 同样会失败、数据也不会空、探测兜底同样读不到。两条路在同一失效模式下一起失效，混合方案无独立兜底意义。
4. **信号语义混乱**：flag = "daemon 明确说中断了"（显式），数据空 = "可能被清了也可能抖了"（隐式）。混在一起子进程逻辑要判断"flag 命中 OR 数据空 N 轮"——两套条件分支，后续维护负担。

显式 flag + 两阶段（先 flag 后 cleanup）已能覆盖中断传导需求；真正的兜底是 `node_timeout_sec` 硬超时（`skill_executor.py:681`），不需要再加一层隐式探测。

---

## 4. 不变量

| ID | 不变量 | 验证方式 |
|----|--------|---------|
| I-INT-1 | flag 写入必须早于同 dialogue 的 cleanup | 单元测试：mock handle_dialogue_interrupt，断言 publish_data 早于 cleanup_dialogue_data |
| I-INT-2 | flag key 必须在 global sys 命名空间（`/sys/interrupt:{did}`） | 单元测试：断言 key 不含 dialogue 前缀 |
| I-INT-3 | flag 不被 dialogue 级 cleanup 清除 | 单元测试：cleanup 后 flag 仍在 |
| I-INT-4 | flag 残留不被新 dialogue 误读（dialogue_id 唯一性） | 现有 dialogue_id 生成逻辑 + 单元测试 |
| I-INT-5 | 子进程轮询 flag 失败（DataBus 抖动）不阻断主流程 | 单元测试：mock get_global 抛异常，断言主循环继续 |
| I-INT-6 | 子进程检测到 flag 后必须写 interrupted 结果给 coordinator，让 coordinator 走正常 cleanup（非凭空消失） | E2E：中断后 coordinator 收到 interrupted 状态 |
| I-INT-7 | 中断后串行锁必然释放（沿用 I-1，由 cleanup → release_dialogue_lock） | 沿用现有 E2E `test_dialogue_cancel_e2e` |
| I-INT-8 | flag 写入与读取落在同一物理 DataBus 实例，跨侧读写一致 | E2E `test_flag_cross_side_visible_e2e`：daemon 写 flag 后子进程能读到 |

---

## 5. 改动清单

### 5.1 daemon C++

| 文件 | 改动 |
|------|------|
| `src/agent/agent.cpp` | `handle_dialogue_interrupt` 在 `cleanup_dialogue_data` 之前插入 `publish_data("/sys/interrupt:{did}", "true")` |

### 5.2 Python runtime

| 文件 | 改动 |
|------|------|
| `packages/agentflow-runtime/src/agentflow_runtime/skill_executor.py` | `execute()` 的 LLM while-loop 顶部加 `databus.get_global("interrupt:{did}")` 轮询，命中则写 interrupted 结果 + return |
| `packages/agentflow-runtime/src/agentflow_runtime/coordinator_decide.py` | `execute()` 的 dispatch loop 内 `call_llm` 之前加同款轮询，命中则写 interrupted 最终响应 + return |

### 5.3 测试

| 测试 | 覆盖 | 类型 |
|------|------|------|
| `test_handle_dialogue_interrupt_sets_flag` | I-INT-1, I-INT-2 | C++ 单元（gtest） |
| `test_flag_survives_dialogue_cleanup` | I-INT-3 | C++ 单元（gtest） |
| `test_skill_executor_interrupt_poll` | I-INT-5, I-INT-6 | Python 单元（mock DataBus） |
| `test_flag_cross_side_visible_e2e` | I-INT-8：daemon 写 flag 后子进程通过 SDK sys() 能读到同一 flag | E2E（新增） |
| `test_dialogue_cancel_propagates_e2e` | 全链路：前端 stop → 子进程在下一轮间隙退出 | E2E（沿用 `test_dialogue_cancel_e2e` 扩展） |

### 5.4 文档同步

| 文件 | 改动 |
|------|------|
| `dialogue-serial-lock-and-cancel.md` | §3.5.1 补"flag 写入"步骤；§3.4.3 补"子进程轮询"说明；引用本文档 |
| 本文 | 状态从"草案"→"已固化"在落地后更新 |

---

## 6. 时序图

### 6.1 正常完成路径（无 flag）

```
skill LLM call → ... → coordinator 收最终响应 → cleanup_dialogue_data（清 /sys/{did}/）
                                                    ↓
                                              flag 不存在，子进程不轮询
                                              session aging 时清整棵树（兜底）
```

### 6.2 中断路径

```
前端 stop → Bridge DIALOGUE-INTERRUPT → daemon handle_dialogue_interrupt
  ├─ ① publish_data("/sys/interrupt:{did}", "true")   ← 新增
  ├─ ② cleanup_dialogue_data（清 /sys/{did}/）         ← 既有
  ├─ ③ enqueue cancel notification final=true           ← 既有
  └ ④ release_dialogue_lock                             ← 既有

skill 子进程下一轮顶部：
  poll /sys/interrupt:{did} → "true" → 写 interrupted 结果 → return

coordinator 子进程下一轮顶部：
  poll /sys/interrupt:{did} → "true" → 写 interrupted 最终响应 → return

flag 残留至 session aging 超时归还时清理（兜底）
```

### 6.3 关键时序约束

- **flag 写入 → cleanup**：必须 flag 先。否则窗口期子进程轮询到"数据空 + flag 空"，误判正常完成。
- **cleanup → cancel 通知**：cleanup 先（清掉 skill_result 等中间态），再发 final 响应给 Bridge。
- **flag → session aging**：flag 不立即清，残留到 session aging。dialogue_id 唯一性保证无害。

---

## 7. 已知限制

1. **轮间隙打断，非单轮内取消**：当前这一轮的 `call_llm` 已发出 HTTP 请求，要等响应回来才能在下一轮顶部检查 flag。最坏情况：用户点终止时 skill 刚开始一轮 60 秒的 LLM call，要等 60 秒后才真正停止。**第二阶段优化**：给 `call_llm` 加 `stream=True` + 客户端 abort，让单轮 LLM 调用也能被打断。
2. **flag 轮询开销**：每轮一次 UDS `get_global`，微秒级，相比 LLM call 秒级可忽略。
3. **DataBus 断连时子进程读不到 flag**：`get_global` 抛异常时子进程继续主流程（I-INT-5），不阻断——但此时中断信号确实丢失。兜底是 `node_timeout_sec` 硬超时。

---

## 8. 验收标准

1. daemon 收到 `DIALOGUE-INTERRUPT` 后，flag key `/session/{sid}/sys/interrupt:{did}` 值为 `"true"`，且早于 cleanup 写入。
2. flag 不被 `cleanup_dialogue_data` 清除（dialogue 级 cleanup 后 flag 仍在）。
3. flag 被 session aging / agent stop 清理（session 级 cleanup 后 flag 消失）。
4. skill_executor 在 flag 命中后写 `status=interrupted` 的 skill_result 并 return，不再调 `call_llm`。
5. coordinator_decide 在 flag 命中后写 interrupted 最终响应并 return，不再 dispatch 新 skill。
6. E2E：前端 stop 后，skill 子进程在当前轮 LLM call 返回后的下一轮顶部退出（≤ node_timeout_sec）。
7. 全量回归：C++ 61+ 单元、Bridge 260+ 单元、Python runtime 单元、E2E 全通过。

---

## 9. 与 `dialogue-serial-lock-and-cancel.md` 的关系

本文是 `dialogue-serial-lock-and-cancel.md` §3.4-3.5 的**补充实现层**：前者定义了"中断机制"（daemon C++ 状态机 + Bridge 下发），本文定义了"中断信号向子进程的传播"（DataBus flag + 子进程轮询）。两者共同构成完整的中断链路。

引用关系：

- `dialogue-serial-lock-and-cancel.md` §3.4.3 `handle_dialogue_interrupt` → 本文 §3.2 flag 写入
- `dialogue-serial-lock-and-cancel.md` §3.5.1 Bridge stop 联动 → 本文 §6.2 中断路径全链路
- 本文不变量 I-INT-1..8 与前者的 I-1..5 共存，不冲突
