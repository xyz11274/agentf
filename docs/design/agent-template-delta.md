# Agent 四层架构：模版 → pip 包 + 入口文件

> **层级**: L3 · `docs/design/agent-template-delta.md`
> **状态**: 草案 v0.1
> **创建日期**: 2026-07-11

---

## 1. 问题

### 1.1 当前的双源困境

当前有两份几乎相同的代码：

| 位置 | 角色 | 问题 |
|------|------|------|
| `bridge/templates/agent_template/harness_agent/` | 创建新 Agent 的模版 | 与 outfit-assistant 分叉，16 个 nodes 文件全部不同 |
| `examples/agent/outfit-assistant/` | 第一个生产 Agent | 先在 outfit-assistant 试验，再手动同步回模版 |

每次修改通用机制需要：改 outfit-assistant → 测试通过 → 手动同步到 template → 再同步到 outfit-test-agent。三个副本最终分叉，不知道哪个是权威。

### 1.2 根因：文件复制式分发

```
template/harness_agent/nodes/coordinator_decide.py    ← 100 行，权威源
                              ↓ 手动复制
outfit-assistant/nodes/coordinator_decide.py           ← 100 行，容易分叉
                              ↓ 手动复制
outfit-test-agent/nodes/coordinator_decide.py          ← 100 行，容易分叉
```

物理复制导致必然分叉。没有"权威源"的概念。

### 1.3 容器沙箱的约束

未来 Agent 会在容器沙箱中运行，有独立的文件系统，不能通过 `PYTHONPATH` 或 `symlink` 引用宿主机路径。所以运行时引用方案不可行，需要**真正的依赖分发机制**。

---

## 2. 方案：pip 包 + shell wrapper + 入口文件

### 2.1 核心思路

把通用代码打包成 Python 包，通过 `pip install` 安装到 **Agent 包自己的虚拟环境 `.venv`** 中。Agent 包的 `nodes/` 每个节点保留一对文件：**shell wrapper（`.sh`）+ Python 入口文件（`.py`，1 行 import）**。真正实现在 pip 包中。

**为什么需要 shell wrapper**：C++ daemon 的 `exe_loader.cpp` 只支持"读 `PYTHON_PATH` 环境变量 → execve 指定 python → 执行 .py"，**不支持自动检测 Agent 包的 `.venv`、也不支持 per-Agent 切换虚拟环境**。我们不想改 C++ daemon，所以用 shell wrapper 在 daemon 调用 Python 之前做一层包装：

- daemon workflow JSON 声明 `execution_mode: shell_script`，`command: bash nodes/xxx.sh`
- `.sh` wrapper 检测 Agent 包内 `.venv` 是否就绪 → 用 `.venv/bin/python3` 执行 `.py`
- `.venv` 不可用 → fallback 到系统 `python3`（由 Bridge 启动时预装 `agentflow-runtime`，见 §2.2）
- `.py` 入口文件只做 `from agentflow_runtime.xxx import ...`，真正实现在 pip 包

```
pip install agentflow-runtime → 安装到 Agent 包的虚拟环境 .venv
                                        │
                                        ▼
                        ┌─────────────────────────────┐
                        │  Agent 包虚拟环境            │
                        │  my-agent/.venv/             │
                        │  ├─ bin/python3              │
                        │  └─ lib/python3.12/          │
                        │     └─ site-packages/        │
                        │        ├─ agentflow_runtime/ │
                        │        └─ agentflow_adapter/ │
                        └─────────────────────────────┘
                                        │
 agent 包 nodes/                        │
  ├─ coordinator_decide.sh  ← wrapper：检测 .venv，选 python，exec .py
  └─ coordinator_decide.py  ← 入口文件：from agentflow_runtime.* import
                                        │
 daemon 执行时（已支持的机制）：          │
   bash nodes/coordinator_decide.sh     │
     ├─ .venv 就绪 → exec .venv/bin/python3 nodes/coordinator_decide.py
     └ fallback  → exec python3 nodes/coordinator_decide.py（系统预装 agentflow-runtime）
          → Python 解释器从对应 site-packages/ 加载 agentflow_runtime
          → 执行真正的代码
```

**关键**：`pip install` 的 Python 环境 = shell wrapper 选中的 Python 解释器。两者必须一致，否则 import 会失败。`setup.sh` 在 daemon `load_agent` 时自动执行（见 §5.5），保证 `.venv` + pip 包就绪。

| 场景 | Python 环境 | 安装方式 |
|------|------------|---------|
| 本地开发 | Agent 包 `.venv` | `pip install -e packages/agentflow-runtime` |
| 容器沙箱 | 容器内的 Python（`/usr/local/bin/python`） | Dockerfile 中 `pip install agentflow-runtime` |
| 生产部署 | 每个 Agent 的 `.venv` | `setup.sh` 中 `pip install agentflow-runtime`（`load_agent` 时自动跑） |

### 2.2 可用性增强：daemon 默认 Python 预装 + 回退机制

**问题**：`setup.sh` 创建 `.venv` 可能失败（权限不足、磁盘满、网络不通），导致 Agent 无法启动。

**方案**：agentflow 启动时，在 daemon 的默认 Python 解释器中预装 `agentflow-runtime`。如果 Agent 的 `.venv` 不存在或不可用，直接回退到 daemon 默认 Python。

```
Bridge 启动时：
  pip install agentflow-runtime
  → 预装到 Bridge 进程所用的系统 Python 解释器（/usr/bin/python3 或系统 python）
  → 注意：这是 shell wrapper fallback 的目标环境，必须存在；预装失败不阻断 Bridge 启动（只 WARN）

Agent 节点执行时（shell wrapper 逻辑，见 §3.5）：
  1. 检查 Agent 包内 .venv 是否就绪（bin/python3 存在 + sentinel 文件存在）
     ├─ 是 → exec .venv/bin/python3 nodes/xxx.py
     └ 否 → exec python3 nodes/xxx.py（依赖本节 Bridge 预装）
```

**实现状态**（shell wrapper 范式修正后）：

| 组件 | 状态 | 说明 |
|------|------|------|
| Bridge 启动时预装 `agentflow-runtime` 到系统 python3 | **待实现** | Bridge 进程启动阶段执行 `pip install agentflow-runtime`（失败不阻断 Bridge 启动，只 WARN；shell wrapper fallback 时仍可能 ModuleNotFoundError） |
| shell wrapper 检测 `.venv` + fallback | **已有雏形**（outfit-assistant 的 3 份 `.sh`） | 待标准化为 §3.5 的统一实现 |
| `exe_loader.cpp` 的 `PYTHON_PATH` 解析 | **已实现** | 保留给 `python_script` 模式的节点使用；shell wrapper 范式下不走这条路径 |

> **注意**：`exe_loader.cpp` 的 `PYTHON_PATH` 在 shell wrapper 范式下**不再被走 shell_script 模式的节点用到**——daemon 直接 execve `bash nodes/xxx.sh`，由 wrapper 自己选 python。`PYTHON_PATH` 仍保留给 `python_script` 模式的节点使用，不删除。

**旧的 exe_loader.cpp 回退逻辑**（保留给 python_script 模式）：

```cpp
const char* python_path = std::getenv("PYTHON_PATH");
std::string resolved_path = resolve_python_command(python_path);
if(!resolved_path.empty()) {
    python_cmd = resolved_path;       // 使用 .venv 的 Python
} else {
    python_cmd = "python3";           // 回退到默认 Python
}
```

**效果**：
- 简单 Agent（无自定义依赖）：不需要 `.venv`，直接用 daemon 的 Python
- 复杂 Agent（有自定义依赖）：需要 `.venv`，`pip install` 自定义依赖
- 极端情况：`.venv` 创建失败 → 自动回退，Agent 仍可启动

### 2.3 四层架构

```
┌──────────────────────────────────────────────────────────────┐
│                    用户自定义层（Agent 包）                    │
│                                                              │
│  用户只需上传：                                               │
│  ├─ agent.md              ← 业务描述 + 系统提示              │
│  ├─ harness/              ← 业务技能 + 资产                 │
│  └─ .env                  ← 环境变量（与系统默认合并）        │
│                                                              │
│  Bridge 自动生成：                                           │
│  ├─ agent.json            ← 从 template 复制 + 替换字段      │
│  └─ requirements.txt      ← agentflow-runtime, -adapter      │
├──────────────────────────────────────────────────────────────┤
│                入口文件层（Agent Package 包装层）               │
│                由 Bridge 自动生成，用户不可见                  │
│                                                              │
│  nodes/                     ← 每节点一对：.sh wrapper + .py    │
│  ├─ coordinator_decide.sh ← wrapper：检测 .venv，选 python    │
│  ├─ coordinator_decide.py ← from agentflow_runtime.*        │
│  ├─ skill_executor.sh      ← wrapper                          │
│  ├─ skill_executor.py      ← from agentflow_runtime.*        │
│  ├─ dummy_pre_skill.sh     ← wrapper                          │
│  ├─ dummy_pre_skill.py     ← from agentflow_runtime.*        │
│  └─ ...                    ← 共 3 组（.sh + .py），仅 daemon 入口│
│                                                              │
│  作用：让 daemon 的 workflow 解析能找到入口                   │
│  用户不需要知道 agentflow 的执行机制                            │
├──────────────────────────────────────────────────────────────┤
│                  pip 包：agentflow-runtime                     │
│                                                              │
│  通用 Agent 执行引擎（所有 Agent 共享）                       │
│  ├── coordinator_decide.py    ← 协调器主循环                  │
│  ├── skill_executor.py        ← Skill 执行器                  │
│  ├── llm_call.py              ← LLM 调用 + fallback 链        │
│  ├── common/agent_config.py   ← 统一配置入口（AgentConfig）    │
│  ├── prompts/                 ← 通用系统提示模版               │
│  └── harness/tools/           ← 通用工具（bash, read_file 等）  │
│                                                              │
├──────────────────────────────────────────────────────────────┤
│              pip 包：agentflow-runtime（含 adapter 代码）        │
│                                                              │
│  agentflow 框架 ↔ Python 运行时的桥梁                        │
│  ├── databus_client.py        ← DataBus 通信                  │
│  ├── parse_af_input.py        ← 解析 daemon 输入              │
│  └── common/clients/          ← 客户端基类                    │
└──────────────────────────────────────────────────────────────┘
```

### 2.4 各层职责

| 层 | 谁维护 | 用户是否可见 | 修改频率 | 典型变更 |
|----|--------|------------|---------|---------|
| **用户自定义层** | 业务开发者 | ✅ 上传 | 每次业务需求 | 改 `agent.md` 提示词、新增技能 |
| **shell wrapper + 入口文件层** | Bridge 自动生成 | ❌ 不可见 | 框架升级时重新生成 | daemon workflow 协议变更、venv 检测逻辑变更 |
| **agentflow-runtime** | 框架团队 | ❌ pip 包 | 功能迭代 | 改 `coordinator_decide.py` 循环逻辑 |
| **agentflow-runtime**（含 adapter） | 框架团队 | ❌ pip 包 | 框架升级 | 改 `databus_client.py` 协议 |

---

## 3. shell wrapper + 入口文件机制（Agent Package 包装层）

### 3.1 什么是 shell wrapper + 入口文件

每个节点保留**一对文件**：shell wrapper（`.sh`）+ Python 入口文件（`.py`，1 行 import）。两者共同构成 **Agent Package 包装层**——agentflow 框架与 Python 运行时之间的契约边界：

- daemon workflow JSON 声明 `execution_mode: shell_script`，`command: bash nodes/xxx.sh`
- `.sh` wrapper 检测 `.venv` → 选 python → exec `.py`（标准实现见 §3.5）
- `.py` 入口文件只做 `from agentflow_runtime.xxx import ...`，真正实现在 pip 包
- daemon 的 `exe_loader.cpp` 只需支持 shell_script 模式（已支持），**不需要改 C++**
- Bridge 在创建 Agent 时自动生成 `.sh` + `.py` 这对文件
- 用户不需要知道这对文件的存在

**为什么需要 shell wrapper（而非 daemon 直接 execve .py）**：C++ daemon 的 `exe_loader.cpp` 不支持自动检测 Agent 包的 `.venv`、也不支持 per-Agent 切换虚拟环境。shell wrapper 在 daemon 调用 Python 之前做一层包装，自己解决 venv 检测 + fallback。详见 §2.1 的论证。

**当前（100 行代码，直接实现）：**
```python
# nodes/coordinator_decide.py — 当前：100 行实现代码
import os
import sys
from common.agent_config import AgentConfig
# ... 100 行真正的逻辑
def main():
    ...
```

**改成 wrapper + 入口文件后（.sh wrapper + .py 1 行 import，Bridge 自动生成）：**
```bash
# nodes/coordinator_decide.sh — 自动生成，不要手动修改（标准实现见 §3.5）
#!/bin/bash
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PACKAGE_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
VENV_PYTHON="$PACKAGE_DIR/.venv/bin/python3"
AF_SENTINEL="$PACKAGE_DIR/.venv/.agentflow_installed"
if [ -f "$VENV_PYTHON" ] && [ -f "$AF_SENTINEL" ]; then
    exec "$VENV_PYTHON" "$SCRIPT_DIR/coordinator_decide.py" "$@"
fi
exec python3 "$SCRIPT_DIR/coordinator_decide.py" "$@"
```
```python
# nodes/coordinator_decide.py — 自动生成，不要手动修改
from agentflow_runtime.coordinator_decide import main, execute
```

### 3.2 为什么 wrapper + 入口文件是独立的层

wrapper + 入口文件这对不应属于用户自定义层，也不应属于 pip 包层：

| 归属 | 问题 |
|------|------|
| ❌ 用户自定义层 | 用户不需要知道 daemon workflow 机制 |
| ❌ pip 包 | pip 包安装到 `site-packages/`，daemon 找不到 |
| ✅ 独立包装层 | 文件在 Agent 包目录下，daemon 能找到；`.sh` wrapper 做 venv 检测、`.py` 入口只有 1 行 import，Bridge 自动生成 |

### 3.3 wrapper + 入口文件列表

只有 daemon workflow JSON 直接引用的节点需要 wrapper + 入口文件（共 3 组）。库模块通过 `from agentflow_runtime.xxx` 绝对路径导入，不需要入口文件。

| 节点 | wrapper | 入口文件 | 对应 pip 包 |
|------|---------|---------|-----------|
| `coordinator_decide` | `nodes/coordinator_decide.sh` | `nodes/coordinator_decide.py` | `agentflow_runtime.coordinator_decide` |
| `skill_executor` | `nodes/skill_executor.sh` | `nodes/skill_executor.py` | `agentflow_runtime.skill_executor` |
| `dummy_pre_skill` | `nodes/dummy_pre_skill.sh` | `nodes/dummy_pre_skill.py` | `agentflow_runtime.dummy_pre_skill` |

每组 wrapper 标准实现相同（仅 `exec "$SCRIPT_DIR/xxx.py"` 那行的文件名不同），见 §3.5。

### 3.4 入口文件对测试的影响

**测试改为直接导入 `agentflow_runtime`。** 不再依赖入口文件透传：

```python
# 测试代码 — 直接导入 pip 包
import agentflow_runtime.llm_call as MODULE
import agentflow_runtime.round_reminder as MODULE

# 或导入具体函数
from agentflow_runtime.common.agent_config import AgentConfig
```

测试运行时，Python 直接从 `site-packages/agentflow_runtime/` 加载模块，不经过 `nodes/` 目录中的入口文件。

> **原因**：库模块入口文件已被删除（只有 3 个 daemon 执行入口需要文件）。测试通过 `import agentflow_runtime.xxx` 直接访问 pip 包中的实现。

### 3.5 shell wrapper 标准实现

3 个节点的 `.sh` wrapper 逻辑完全相同，唯一差异是 `exec` 那行的 `.py` 文件名。标准实现如下（以 `coordinator_decide` 为例）：

```bash
#!/bin/bash
# Auto-generated shell wrapper — do not modify
# 优先用 Agent 包内 .venv（setup.sh 装好后写 sentinel）；不可用则 fallback 到系统 python3
# 系统 python3 必须已预装 agentflow-runtime（见 §2.2 Bridge 启动时预装）

set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PACKAGE_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

VENV_PYTHON="$PACKAGE_DIR/.venv/bin/python3"
AF_SENTINEL="$PACKAGE_DIR/.venv/.agentflow_installed"

if [ -f "$VENV_PYTHON" ] && [ -f "$AF_SENTINEL" ]; then
    exec "$VENV_PYTHON" "$SCRIPT_DIR/coordinator_decide.py" "$@"
fi

# fallback：系统 python3（依赖 §2.2 daemon 预装 agentflow-runtime）
exec python3 "$SCRIPT_DIR/coordinator_decide.py" "$@"
```

**不变量**：

| 不变量 | 校验 | 违反后果 |
|--------|------|---------|
| `.venv` 就绪 ⟺ sentinel 存在 | `[ -f "$VENV_PYTHON" ] && [ -f "$AF_SENTINEL" ]` | sentinel 缺失 → fallback 系统python3（可能漏用户自定义依赖） |
| sentinel 由 `setup.sh` 装 runtime 后写入 | `setup.sh` 最后 `touch "$AF_SENTINEL"` | sentinel 早写 → wrapper 用了不完整的 .venv，import 失败 |
| wrapper 路径用 `SCRIPT_DIR`/`PACKAGE_DIR` 绝对定位 | `cd "$(dirname "$0")" && pwd` | 相对路径会因 daemon working_dir 切换而失效 |
| `exec` 替换进程而非 fork | `exec` | fork 会留僵尸进程、env 变量丢失 |

**setup.sh 与 sentinel 的契约**：

`setup.sh` 必须在**所有 `pip install` 成功后**才 `touch "$AF_SENTINEL"`。sentinel 是 `.venv` 就绪的原子标记——wrapper 只检测这一个文件就敢用 `.venv`，不需要每次 fork python 做 import 检查。sentinel 早写或漏写都会破坏不变量。

**生成与同步**：Bridge 在创建 Agent 时自动生成 3 份 `.sh`（内容固定，仅文件名不同）。`sync.py` 推送到所有 derived 副本。用户不修改。

### 3.6 SKILL.md 命令的执行链路与 venv 注入

**执行链路**（交叉比对 `skill_executor.py` + `harness/tools/bash.py` 实际代码）：

1. SKILL.md 里写的命令（如 `python3 ${SKILL_DIR}/scripts/outfit_vision.py --image uploads/<file> --mode garment`）**不是裸 exec**
2. coordinator 让 LLM 按 SKILL.md 指示**调 `bash` 工具**，参数 `command: "python3 ${SKILL_DIR}/scripts/xxx.py ..."`
3. `harness/tools/bash.py:execute()` → `subprocess.run(["bash", "-c", command], env=os.environ, cwd=AGENT_PACKAGE_DIR)`
4. bash 工具的 `env=os.environ` 让子进程**继承 skill_executor 的全部环境变量**

**venv 注入不变量**（§3.5 wrapper 的延伸）：

shell wrapper（§3.5）只解决了"skill_executor.py 入口用哪个 python 跑"的问题。但 SKILL.md 调的子命令 `python3 xxx.py` 是从 **PATH** 解析 `python3`——默认是系统 python3，**不是 .venv 的**。用户技能脚本会用错 python、import 不到 .venv 装的依赖。

**方案**：`skill_executor.py` 启动时（模块级，跑一次）检测同一个 sentinel（§3.5），注入 `.venv/bin` 到 `PATH` 前面 + `.venv/lib/python*/site-packages` 到 `PYTHONPATH`。之后所有子工具（bash、read_file、web_fetch）调的子进程用 `subprocess.run(env=os.environ)` 自动继承——用户技能脚本（weather.py / outfit_vision.py）零改动就能用 .venv 的 python + 依赖。

```
skill_executor.py 启动（被 shell wrapper exec 起来）：
  1. 检测 .venv/bin/python3 存在 + sentinel 存在（同 §3.5 wrapper 逻辑）
     ├─ 是 → 注入 .venv/bin 到 PATH 前 + site-packages 到 PYTHONPATH
     └ 否 → 跳过（fallback 系统 python3，依赖 §2.2 Bridge 启动时预装 agentflow-runtime）
  2. 加载用户技能 / 调 bash 工具 → subprocess.run(env=os.environ) 自动继承 venv 环境
  3. SKILL.md 的 "python3 xxx.py" → 解析到 .venv/bin/python3 → 用户技能用 .venv 依赖
```

**不变量**：

| 不变量 | 校验 | 始违后果 |
|--------|------|---------|
| skill_executor 与 wrapper 检测同一 sentinel | 复用 `PACKAGE_DIR/.venv/.agentflow_installed` | sentinel 判断分叉 → wrapper 用 .venv 但子进程用系统 python，依赖缺失 |
| venv 注入幂等（不重复注入） | `if _venv_bin not in PATH.split(os.pathsep)` | 重复注入 → PATH 膨胀，subprocess 启动变慢 |
| 用户技能零改动 | 用户技能脚本不 import 框架包 | 改用户技能 → 破坏"跨平台可复用"不变量 |
| fallback 路径有兜底 | sentinel 缺失时不注入，依赖 §2.2 Bridge 预装 | fallback 分支 import agentflow_runtime 失败 |

**用户技能的跨平台可复用性**（核心约束）：用户技能脚本（`harness/skills/<name>/scripts/*.py`）只 import stdlib + 自己装的依赖（如 pillow），**不 import 框架的 agentflow_runtime**。在本框架跑时，skill_executor 启动注入 venv 让用户技能自动用 .venv 的 python + 依赖；在其他平台跑时，用户技能用该平台自己的 python——零改动可跨平台复用。

> **例外**：`_uds_client`（用户集成的客户端文件，import `common.clients.base`）是用户自带文件，不在本轮范围。用户集成 `_uds_client` 才能用框架内的模型——这是用户主动选择依赖框架，不是框架强制要求。

---

## 4. pip 包结构（单包）

两个包（`agentflow-runtime` + `agentflow-adapter`）最初分开设计，但 `common/` 包跨两个包导致 split package 问题。最终合并为单包 `agentflow-runtime`，降低复杂度。

### 4.1 agentflow-runtime（单包）

```
packages/agentflow-runtime/
├── pyproject.toml
├── src/
│   └── agentflow_runtime/
│       ├── __init__.py
│       ├── coordinator_decide.py
│       ├── skill_executor.py
│       ├── llm_call.py
│       ├── common/
│       │   ├── __init__.py
│       │   └── agent_config.py
│       ├── dummy_pre_skill.py
│       ├── observability.py
│       ├── prompts/
│       │   ├── __init__.py
│       │   ├── common.py
│       │   ├── coordinator.py
│       │   └── skill.py
│       ├── round_reminder.py
│       └── harness/
│           └── tools/
│               ├── bash.py
│               ├── read_file.py
│               └── ...
│
├── tests/                         ← 包的测试（从 template 迁移）
│   ├── test_llm_call_errors.py
│   ├── test_coordinator_compaction.py
│   └── ...
└── README.md
```

```toml
# pyproject.toml for agentflow-runtime
[project]
name = "agentflow-runtime"
version = "0.1.0"
description = "AgentFlow Agent Runtime — generic agent execution engine"
requires-python = ">=3.12"
dependencies = ["openai>=1.0"]

[build-system]
requires = ["setuptools>=75"]
build-backend = "setuptools.build_meta"

[tool.setuptools.packages.find]
where = ["src"]
```

### 4.2 适配器代码位置

`agentflow-adapter` 的代码（`databus_client.py`、`parse_af_input.py`、`common/clients/`）已合并到 `agentflow-runtime` 包中，位于 `src/agentflow_runtime/` 下。详见 §4.1 的目录结构。

---

## 5. Agent 生命周期

### 5.1 创建新 Agent（Upload 场景）

用户通过 Bridge API `POST /api/packages/create-from-template` 上传 Agent：

```
用户上传（multipart/form-data，复用现有 `POST /api/packages/create-from-template`）：
  ┌──────────────────────────────────────────────────────────────┐
  │ file: my-agent.zip                                           │
  │  ├── agent.md              ← 必需：业务描述                  │
  │  ├── harness/skills/       ← 可选：业务技能                 │
  │  ├── .env                  ← 可选：环境变量，与系统默认合并  │
  │  ├── requirements.txt      ← 可选：自定义依赖，与系统默认合并│
  │  └── setup.sh              ← 可选：自定义初始化脚本          │
  │ template: harness_agent     ← 指定模版                      │
  └──────────────────────────────────────────────────────────────┘
```

Bridge 处理流程：

```
1. 验证上传文件
   ├─ 检查扩展名（.zip / .tar.gz）
   ├─ 检查 agent.md 是否存在
   └─ 检查包名是否已存在（409 Conflict）

2. 生成入口文件
   ├─ nodes/coordinator_decide.py  → from agentflow_runtime.coordinator_decide import main, execute
   ├─ nodes/skill_executor.py      → from agentflow_runtime.skill_executor import execute
   ├─ nodes/llm_call.py            → from agentflow_runtime.llm_call import call_llm
   ├─ nodes/common/agent_config.py → from agentflow_runtime.common.agent_config import AgentConfig
   ├─ nodes/common/clients/...     → from agentflow_adapter.xxx import ...
   └─ ...（共 3 个入口文件，见 §3.3）

3. 复制用户文件
   ├─ agent.md          → 覆盖（用户提供）
   ├─ harness/skills/   → 增量（用户新增的技能）
   ├─ .env              → 与系统默认 .env 合并（见 §5.3）
   ├─ requirements.txt  → 与系统默认 requirements.txt 合并（见 §5.4）
   └─ setup.sh          → 覆盖（用户提供，若未上传则使用系统默认 setup.sh）

4. 生成 agent.json
   ├─ 从 template 复制 agent.json 模板
   └─ 替换 name/description 为 agent.md frontmatter 中的值

5. 生成 workflow JSON
   ├─ 从 template 复制 coordinator_workflow.json
   ├─ 从 template 复制 skill_workflow.json
   └─ 重写 working_dir 为当前包路径

6. ★ Bridge 验 setup.sh（驅动执行，幂等）           ← 冝决策：create 后必跑
   └ bash <package_path>/setup.sh
     ├─ 创建 .venv + 装依赖 + 装 agentflow-runtime + 写 sentinel（见 §3.5）
     └ 失败不阻断 create（只 WARN），shell wrapper fallback 系统 python3
```

### 5.2 更新 Agent（Update 场景）

用户通过 Bridge API `PUT /api/packages/{name}/update` 更新已有 Agent：

```
用户上传（multipart/form-data）：
  ┌──────────────────────────────────────────────────────────────┐
  │ file: update.zip                                             │
  │  ├─ agent.md              ← 可选：更新描述                  │
  │  ├─ harness/skills/       ← 可选：新增/修改技能             │
  │  ├─ .env                  ← 可选：更新环境变量，与系统默认合并│
  │  ├─ requirements.txt      ← 可选：自定义依赖，与系统默认合并│
  │  └─ setup.sh              ← 可选：覆盖初始化脚本            │
  └──────────────────────────────────────────────────────────────┘
```

Bridge 处理流程：

```
1. 验证包存在
   └─ 检查 package/{name} 是否存在（404 Not Found）

2. 备份用户文件
   └─ agent.md → agent.md.bak（若更新失败可回滚）

3. 合并用户文件到现有包
   ├─ agent.md          → 覆盖
   ├─ harness/skills/   → 增量合并（同名技能覆盖，新技能新增）
   ├─ .env              → 与系统默认 .env 合并
   ├─ requirements.txt  → 与系统默认 requirements.txt 合并
   └─ setup.sh          → 覆盖（用户提供，若未上传则保留现有）

4. 同步 template 公共部分（merger `_TEMPLATE_ONLY` 覆盖）
   ├─ harness/tools/*          ← template 始终覆盖（框架基础设施）
   ├─ nodes/databus_client.py  ← template 覆盖
   ├─ nodes/llm_call.py        ← template 覆盖
   ├─ nodes/dummy_pre_skill.py ← template 覆盖
   ├─ nodes/parse_af_input.py  ← template 覆盖
   ├─ nodes/*.sh wrapper       ← template 覆盖（§3.5 标准实现，保持 wrapper venv 检测逻辑同步）
   ├─ tests/                   ← template 覆盖
   └ 用户上传匹配 _TEMPLATE_ONLY 的文件被丢弃（防止意外破坏运行时基础设施）
   注：agent.md / package.json / harness/skills/<custom>/ 是用户域，template 不覆盖

5. ★ 若 setup.sh 或 requirements.txt 变更 → Bridge 验 setup.sh（幂等）  ← 冝决策：条件跑
   └ 否则跳过（.venv 已就绪）

6. 运行热重载
   └ 如果 agent 已加载 → 调用 load_agent 重新加载
```

> **merger 同步不变量**（F5 单一真相）：业务 agent 的公共部分（wrapper / setup.sh / harness/tools / nodes 入口）永远来自 template，用户上传的同类文件被 `_TEMPLATE_ONLY` 丢弃。这保证框架升级（如 §3.5 wrapper 逻辑变更、§3.6 venv 注入逻辑变更）通过 update 自动推到所有业务 agent，**不需要手动改业务 agent**。用户域（agent.md / harness/skills/ / .env / tests 之外的技能脚本）不受 template 覆盖。
>
> **`_TEMPLATE_ONLY` 清单完整性保护**：`_reapply_template_files` 开头跑 `_check_template_only_coverage(template_dir)` sanity check，扫描 template 的 `nodes/*.sh` / `*.py` / `*.json`，发现 template 有文件但 `_TEMPLATE_ONLY` 清单缺时 WARN（不阻断）。这防止"在 template 加新公共文件但忘加清单"导致 update 静默不推的无声漂移——漏清单从静默变可见。WARN 不阻断主流程（漏清单只影响"update 不推该文件"，现有功能不破坏）。create 路径也跑此 check，虽语义为准新建 agent 无"漏同步"问题，但 WARN 无害且保持两路径同源。

### 5.3 .env 合并策略

用户上传的 `.env` 需要与系统默认 `.env` 合并。合并规则：

```
系统默认 .env（由 template 提供）：
  AF_LLM_TIMEOUT=120
  AF_LLM_CONNECT_TIMEOUT=15
  AF_LLM_MAX_RETRIES=2

用户上传 .env：
  AF_LLM_API_KEY=sk-xxx
  AF_LLM_BASE_URL=https://api.example.com

合并结果：
  AF_LLM_API_KEY=sk-xxx              ← 用户值（覆盖）
  AF_LLM_BASE_URL=https://api.example.com  ← 用户值（新增）
  AF_LLM_TIMEOUT=120                 ← 系统默认值（保留）
  AF_LLM_CONNECT_TIMEOUT=15          ← 系统默认值（保留）
  AF_LLM_MAX_RETRIES=2               ← 系统默认值（保留）
```

**规则**：用户值优先，同键覆盖，新键新增，系统默认值保留。这是标准的 `dict.update()` 语义。

> **注意**：`.env` 合并发生在 `load_agent` 的 `_inject_agent_env_defaults` 之前。`_inject_agent_env_defaults` 会再次检查空值/占位符并填充。所以最终优先级：用户 .env > 系统默认 .env > agent_defaults 注入。

### 5.4 requirements.txt 与 setup.sh 合并策略

用户上传的 `requirements.txt` 与系统默认合并：

用户上传的 `.env` 需要与系统默认 `.env` 合并。合并规则：
  agentflow-runtime>=0.1.0

用户上传 requirements.txt：
  pillow>=10.0
  requests

合并结果：
  agentflow-runtime>=0.1.0    ← 系统默认（保留，框架必需）
  pillow>=10.0                ← 用户新增
  requests                    ← 用户新增
```

**规则**：系统默认依赖始终保留（框架必需），用户自定义依赖追加。如果用户明确指定了相同包的不同版本，用户版本优先。

`setup.sh` 的处理更简单：如果用户上传了 `setup.sh`，则覆盖系统默认的；否则使用系统默认的。因为 `setup.sh` 是完整的初始化脚本，不适合做行级合并。

**setup.sh 的执行时机**（决策）：`setup.sh` 由 **Bridge 驱动**，在 Agent 生命周期关键节点自动执行，**不需要用户手动跑、daemon load_agent 不跑 setup.sh**。两个触发点：

| 时机 | 必跑 / 条件跑 | 说明 |
|------|-------------|------|
| Bridge `create-from-template` 流程末（见 §5.1） | **必跑** | 新 Agent 必须初始化 .venv + 装依赖 |
| Bridge `update` 流程末（见 §5.2） | **条件跑** | 仅当 setup.sh 或 requirements.txt 变更时跑，幂等 |
| daemon `load_agent` | **不跑** | daemon 不动 setup.sh，只读 agent.md/.env + agent.load |

```
Bridge 验 setup.sh（驅动执行，幂等）：
  bash <package_path>/setup.sh
    ├─ 创建 .venv + 装依赖 + 装 agentflow-runtime + 写 sentinel
    └ 失败不阻断 Bridge 流程（只 WARN），shell wrapper fallback 到系统 python3
```

> **注意**：setup.sh 改由 Bridge 驱动后，用户上传带自定义依赖的技能也能正常执行——依赖在 Agent 创建/更新时就装好了，不需要用户手动跑、也不依赖 daemon。setup.sh 失败时（权限/磁盘/网络）Bridge 不阻断流程，shell wrapper fallback 到系统 python3（已由 Bridge 启动时预装 agentflow-runtime，见 §2.2），Agent 仍可启动但用户自定义依赖不可用。

### 5.5 加载 Agent（Runtime）

```
load_agent（daemon 入口）：
  1. 读取 agent.md → 解析 frontmatter
  2. 注入 .env 默认配置（_inject_agent_env_defaults）
  3. 调用 daemon agent.load(package_path)

> **setup.sh 不在此流程**：setup.sh 由 Bridge 在 create/update 流程末验动（见 §5.1/§5.4），daemon load_agent 不跑 setup.sh。load_agent 时 .venv 与 sentinel 应已就绪——若未就绪，shell wrapper fallback 到系统 python3（已由 Bridge 启动时预装 agentflow-runtime）。

daemon 执行 workflow（shell wrapper 范式）：
  1. 读 workflow JSON → execution_mode=shell_script, command="bash nodes/xxx.sh"
  2. execve bash <agent>/nodes/xxx.sh
  3. shell wrapper 检测 .venv + sentinel（见 §3.5）→ 选 python（.venv 或 fallback 系统）
  4. exec <选中 python> <agent>/nodes/xxx.py
  5. Python 入口文件（1 行 import）import pip 包中的真正代码
  6. 真正代码执行
```

### 5.6 升级模版

```
框架团队修改 agentflow-runtime：
  1. 改 packages/agentflow-runtime/src/ 中的代码
  2. 更新版本号：0.1.0 → 0.2.0
  3. 发布到 PyPI 或私有仓库

Agent 升级：
  1. 改 requirements.txt: agentflow-runtime>=0.2.0
  2. pip install --upgrade agentflow-runtime
  3. 重启即可
```

---

## 6. 迁移路径

### Phase 1：创建 pip 包

1. 创建 `packages/agentflow-runtime/`
2. 写 `pyproject.toml`
3. 把 `nodes/` 中的代码移到 `src/agentflow_runtime/` 和 `src/agentflow_adapter/`
4. 验证 `pip install -e` 可安装

### Phase 2：生成 shell wrapper + 入口文件

1. 创建生成 wrapper + 入口文件的脚本（一对 `.sh` + `.py`，见 §3.5 标准实现）
2. 为 `outfit-assistant` 生成 3 组 wrapper + 入口文件
3. 验证测试通过（import 路径不变，wrapper fallback 到系统 python3 也能跑）
4. 同样的操作用于 `outfit-test-agent` 和 template

### Phase 3：清理旧副本

1. 从 `outfit-assistant/nodes/` 中删除已迁移的实现代码（保留 wrapper + 入口文件）
2. 从 `template/harness_agent/nodes/` 中删除实现代码（`template/` 目录保留，作为 agent.json 模板和 wrapper/入口文件模板源）
3. 所有 Agent 的 `nodes/` 只保留 wrapper + 入口文件

### Phase 4：发布

1. 配置 CI 自动构建和发布 pip 包
2. 实现 Bridge 启动时预装 agentflow-runtime 到系统 python3（§2.2）
3. 实现 Bridge create/update 流程末验 setup.sh（§5.1/§5.2）
4. 更新 Agent 创建流程（`create_agent_package` 只生成 wrapper + 入口文件）
5. 更新文档

---

## 8. 开发与运维流程

### 8.1 本地开发流程

**当前方式：**
```
改 outfit-assistant/nodes/coordinator_decide.py → 直接生效 → 运行测试
```

**新架构：**
```
改 packages/agentflow-runtime/src/agentflow_runtime/coordinator_decide.py
  → pip install -e packages/agentflow-runtime（editable mode）
  → 改源码即时生效（setuptools 的 editable mode 通过 .pth 文件链接源码目录）
  → 运行测试（import 路径不变，wrapper + 入口文件透传）
```

**关键变化**：开发者改的是 `packages/` 下的源码，而不是 `outfit-assistant/nodes/`。`pip install -e` 后代码变更即时生效，不需要手动复制。

### 8.2 生产环境热修复

**方式一（推荐）：直接修改已安装的 pip 包源码**

pip 安装后的 `.py` 源文件保存在 `site-packages/agentflow_runtime/` 中，可以直接编辑：

```bash
# 1. 找到已安装的包位置
python3 -c "import agentflow_runtime; print(agentflow_runtime.__file__)"
# → /path/to/.venv/lib/python3.12/site-packages/agentflow_runtime/__init__.py

# 2. 直接编辑源码
vim /path/to/.venv/lib/python3.12/site-packages/agentflow_runtime/coordinator_decide.py

# 3. 下次节点执行时自动生效（Python 每次 import 都读 .py 文件）
```

**优点**：不需要知道 wrapper + 入口文件机制，不需要重新打包，修改后即刻生效。

**恢复方式**：重新 `pip install agentflow-runtime==新版本` 覆盖即可。

**方式二：重新打包发布**

```bash
# 修复 packages/agentflow-runtime/src/ 中的源码
# 重新构建并安装
cd packages/agentflow-runtime
pip install --force-reinstall .

# 或发布到 PyPI 后
pip install --upgrade agentflow-runtime
```

**方式三：入口文件覆盖（兜底）**

当无法修改 `site-packages/` 时（如只读文件系统），利用入口文件层的覆盖机制：

```python
# nodes/coordinator_decide.py — 热修复，临时覆盖 pip 包版本
import os, sys
from common.agent_config import AgentConfig
# ... 修复后的完整代码 ...
```

Python import 优先级：`nodes/` 中的文件 > `site-packages/`。修复验证后合并回 pip 包，删除覆盖文件即可恢复。

### 8.3 各场景对比

| 场景 | 当前方式 | 新架构 |
|------|---------|--------|
| 本地开发 | 改 `outfit-assistant/nodes/` | 改 `packages/agentflow-runtime/src/`，`pip install -e` 即时生效 |
| 测试 | `pytest tests/` | 同左，import 路径不变 |
| 生产部署 | 部署整个 `outfit-assistant/` | 部署 Agent 包（入口文件 + agent.md + harness）+ pip 包 |
| 生产热修复 | 直接改 `nodes/` 文件 | wrapper/入口文件替换为完整代码（覆盖机制），或重新发布 pip 包 |
| 框架升级 | 手动同步所有 Agent 副本 | `pip install --upgrade agentflow-runtime` |

| 场景 | 处理 |
|------|------|
| Agent 需要覆盖运行时行为 | 在 `nodes/` 中放同名文件，Python import 优先找到它（wrapper/入口文件被覆盖） |
| Agent 需要扩展运行时类 | 在 `nodes/` 中 import 通用类，子类化后覆盖 |
| 本地开发 | `pip install -e packages/agentflow-runtime` 即时生效 |
| 容器沙箱 | Dockerfile 中 `pip install agentflow-runtime`，入口文件随 Agent 包一起拷贝 |
| 多版本共存 | `requirements.txt` 锁定版本号，不同 Agent 可用不同版本 |
| 工具发现路径变更 | `harness/tools/` 移入 pip 包后，`load_skill.py` 的发现路径需要同步更新 | Phase 2 |
| 测试不通过 | 测试 import 路径不变，wrapper + 入口文件透传，不需要改测试 |