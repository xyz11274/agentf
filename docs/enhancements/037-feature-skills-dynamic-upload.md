# 详细设计: AgentFlow Skills 动态上传接口

> **文档级别**: L4 技术设计
> **日期**: 2026-06-20
> **状态**: 待审阅
> **关联**: `.sisyphus/plans/skill-upload-api-design.md`（架构设计）

---

## 1. 背景与动机

### 1.1 问题陈述

AgentFlow 框架的部署最小单元是 **agent package**（固定契约）。当前 agent package 的所有内容（agent.json、workflow JSON、node 脚本、agent.md）在部署时固定，运行时不可变更。

实际业务中，**SKILL.md 技能包**（Claude Code 生态格式）需要灵活更新——新增技能、更新提示词、替换废弃技能——而不应重新部署整个 agent package。

### 1.2 设计目标

- 外部系统（CI/CD、管理平台）可通过接口向**已部署的 agent package** 动态注入/更新 skills
- Skills 可热替换，无需重启 agent 或重新部署 package
- 格式对齐 Claude Code 生态（SKILL.md + YAML frontmatter）
- 校验最小化：只确保每个 skill 目录包含 SKILL.md

### 1.3 非目标

- 不做 skills 的版本管理
- 不做 skills 的跨节点自动分发（由集成方通过 NFS/镜像/git 处理）
- 不做 SKILL.md 内容的深度校验（frontmatter 字段完整性等）
- 不涉及 canonical 源机制

---

## 2. 架构概述

### 2.1 Agent Package 构成

```
Agent Package（部署最小单元）
│
├── 静态部分（部署时固定，不可变）:
│   ├── agent.json          # Agent 拓扑定义
│   ├── package.json        # 包元数据
│   ├── workflow_XX.json    # 工作流定义
│   ├── nodes/              # 可执行节点（.py/.sh/.so）
│   └── agent.md            # Agent 系统提示/LLM 配置
│
└── 动态部分（运行时可热替换）:
    └── harness/            # Harness Engineering 命名空间
        ├── skills/         # SKILL.md 技能包目录
        │   ├── skill-a/
        │   │   ├── SKILL.md
        │   │   └── references/
        │   └── skill-b/
        │       └── SKILL.md
        └── tools/          # 工具定义（预留给未来扩展）
```

### 2.2 接口在系统中的位置

```
外部系统 (CI/CD / 管理平台)
    │
    │ JSON-RPC 2.0 over TCP (端口 17891) 或 UDS
    │ 方法: agent.upload_skills / agent.list_skills / agent.remove_skill
    ▼
agentflowd (daemon)
    │
    ├─ RpcHandler (已有 + 新增 3 个方法)
    │   ├─ 定位 agent → AgentRegistry::find(agent_id)
    │   ├─ 获取 package_path → AgentRegistry::get_package_path(agent_id)
    │   ├─ Base64 解码 → zip 解压 → 校验 SKILL.md → 写入 harness/skills/ 目录
    │   └─ 返回结果
    │
    └─ Agent Package (已部署, 文件系统)
        └── harness/skills/  ← 上传写入的目标目录
```

### 2.3 与现有机制的关系

| 现有机制 | 关系 |
|---------|------|
| `agent.load` / `agent.unload` | 不变——加载/卸载整个 agent package |
| `agent.upload_skills` (新增) | 在已加载的 agent package 上操作 harness/skills 子目录 |
| `enable_hot_reload` | 不直接关联——skills 热替换通过文件覆盖实现，agent 的 LLM node 下次调用时读取新内容 |
| Agent package 格式 | 不变——harness/skills/ 只是约定的新增目录 |

---

## 3. 详细 API 规范

### 3.1 `agent.upload_skills`

向指定 agent package 的 `harness/skills/` 目录上传一个或多个 SKILL.md 技能包。

#### 请求

```json
{
  "jsonrpc": "2.0",
  "method": "agent.upload_skills",
  "params": {
    "agent_id": "harness-agent-001",
    "skills_data": "<base64-encoded-zip>",
    "force": false
  },
  "id": 1
}
```

| 参数 | 类型 | 必填 | 默认值 | 约束 | 说明 |
|------|------|------|--------|------|------|
| `agent_id` | string | 是 | — | 非空 | 目标 agent 的 ID，必须是已通过 `agent.load` 加载的 agent |
| `skills_data` | string | 是 | — | 非空, 解码后 ≤ 100MB | Base64 编码的 zip 文件内容 |
| `force` | bool | 否 | `false` | — | 同名 skill 已存在时是否覆盖 |

#### zip 包结构

zip 文件解压后，根目录下每个子目录代表一个 skill：

```
skills-upload.zip 解压后:
├── skill-a/              # 子目录名 = skill 名称
│   ├── SKILL.md          # 必需 — Claude Code 生态格式
│   ├── references/       # 可选 — 参考文档
│   ├── checklists/       # 可选 — 检查清单
│   └── scripts/          # 可选 — 辅助脚本
├── skill-b/
│   └── SKILL.md
└── skill-c/
    ├── SKILL.md
    └── scripts/
```

#### 成功响应

```json
{
  "jsonrpc": "2.0",
  "result": {
    "agent_id": "harness-agent-001",
    "package_path": "/opt/agentflow/packages/harness-agent",
    "uploaded_skills": ["skill-a", "skill-b"],
    "skipped_skills": [
      {"name": "skill-c", "reason": "already_exists"}
    ],
    "failed_skills": [
      {"name": "skill-d", "reason": "missing_skill_md"}
    ]
  },
  "id": 1
}
```

| 响应字段 | 类型 | 说明 |
|---------|------|------|
| `agent_id` | string | 确认操作的 agent ID |
| `package_path` | string | agent package 的绝对路径 |
| `uploaded_skills` | string[] | 成功上传的 skill 名称列表 |
| `skipped_skills` | object[] | 被跳过的 skill（含原因） |
| `failed_skills` | object[] | 校验失败的 skill（含原因） |

#### 错误响应

```json
{
  "jsonrpc": "2.0",
  "error": {
    "code": -2,
    "message": "Agent not found: harness-agent-001"
  },
  "id": 1
}
```

| 错误码 | HTTP 等价 | 含义 | 触发条件 |
|--------|----------|------|---------|
| -32602 | 400 | Invalid params | `agent_id` 或 `skills_data` 缺失/为空 |
| -2 | 404 | Agent not found | `agent_id` 对应的 agent 未加载 |
| -1 | 422 | Operation failed | 操作失败（Base64 解码失败 / zip 解压失败 / 全部 skill 校验失败 / 磁盘 I/O 错误）— 具体原因通过 message 区分 |
| -5 | 409 | Conflict | 所有 skill 同名且 `force=false` |

> **错误码规范说明**：本接口错误码遵循 daemon 层现有惯例——`rpc_handler.cpp` 中无集中错误码定义，直接使用整数传递给 `build_error(code, message, id)`。`-1` 作为通用失败码覆盖多种场景（Base64 解码、zip 解压、校验、I/O），通过 `message` 字符串区分具体原因。新增 `-5` 表示 skill 冲突（框架未用此编号）。详见 §8.3 技术债务记录。

> **注意**: 部分成功是允许的——如果 zip 内含 3 个 skill，其中 1 个缺 SKILL.md，另外 2 个正常上传，返回成功响应（`failed_skills` 含失败的，`uploaded_skills` 含成功的）。只有当**所有** skill 都失败时才返回错误。

### 3.2 `agent.list_skills`

列出指定 agent package 中已有的 skills。

#### 请求

```json
{
  "jsonrpc": "2.0",
  "method": "agent.list_skills",
  "params": {
    "agent_id": "harness-agent-001"
  },
  "id": 1
}
```

#### 成功响应

```json
{
  "jsonrpc": "2.0",
  "result": {
    "agent_id": "harness-agent-001",
    "skills": [
      {
        "name": "skill-a",
        "path": "/opt/agentflow/packages/harness-agent/harness/skills/skill-a",
        "has_skill_md": true
      },
      {
        "name": "skill-b",
        "path": "/opt/agentflow/packages/harness-agent/harness/skills/skill-b",
        "has_skill_md": true
      }
    ]
  },
  "id": 1
}
```

如果 agent package 没有 `harness/skills/` 目录，返回空数组。

### 3.3 `agent.remove_skill`

删除指定 agent package 中的一个 skill。

#### 请求

```json
{
  "jsonrpc": "2.0",
  "method": "agent.remove_skill",
  "params": {
    "agent_id": "harness-agent-001",
    "skill_name": "skill-a"
  },
  "id": 1
}
```

#### 成功响应

```json
{
  "jsonrpc": "2.0",
  "result": {
    "success": true,
    "removed": "skill-a",
    "path": "/opt/agentflow/packages/harness-agent/harness/skills/skill-a"
  },
  "id": 1
}
```

#### 错误码

| 错误码 | 含义 |
|--------|------|
| -32602 | `agent_id` 或 `skill_name` 缺失 |
| -2 | Agent not found |
| -6 | Skill not found（`harness/skills/{skill_name}/` 不存在） |

---

## 4. 处理流程详述

### 4.1 `agent.upload_skills` 完整流程

```
┌─────────────────────────────────────────────────────────────┐
│ Step 1: 参数校验                                              │
│  - agent_id 非空?                                            │
│  - skills_data 非空?                                         │
│  - skills_data 解码后长度 ≤ 100MB?                            │
│  - 否 → 返回 -32602                                          │
└─────────────────────────┬───────────────────────────────────┘
                          │
                          ▼
┌─────────────────────────────────────────────────────────────┐
│ Step 2: 定位 Agent                                            │
│  - registry_.find(agent_id)                                  │
│  - 找到 → 获取 entry, 继续                                    │
│  - 未找到 → 返回 -2                                           │
└─────────────────────────┬───────────────────────────────────┘
                          │
                          ▼
┌─────────────────────────────────────────────────────────────┐
│ Step 3: 获取 package_path                                     │
│  - package_path = entry.package_path                         │
│  - 例: /opt/agentflow/packages/harness-agent                 │
└─────────────────────────┬───────────────────────────────────┘
                          │
                          ▼
┌─────────────────────────────────────────────────────────────┐
│ Step 4: Base64 解码                                           │
│  - 解码 skills_data → raw_bytes                              │
│  - 失败 → 返回 -3                                             │
│  - 写入临时文件: /tmp/agentflow_skills_{pid}_{ts}.zip        │
└─────────────────────────┬───────────────────────────────────┘
                          │
                          ▼
┌─────────────────────────────────────────────────────────────┐
│ Step 5: Zip 解压                                              │
│  - 创建临时目录: /tmp/agentflow_skills_extract_{pid}_{ts}/   │
│  - 用 miniz/zlib 解压 zip 到临时目录                          │
│  - 失败 → 清理 → 返回 -4                                      │
│  - 路径安全检查: 拒绝含 "../" 或绝对路径的条目                │
└─────────────────────────┬───────────────────────────────────┘
                          │
                          ▼
┌─────────────────────────────────────────────────────────────┐
│ Step 6: 遍历 & 校验 skill 目录                                │
│  - 列出解压根目录下的所有子目录                               │
│  - 对每个子目录 {name}/:                                      │
│    - 检查 {name}/SKILL.md 存在且是文件                        │
│    - 存在 → 加入 valid_skills 列表                           │
│    - 不存在 → 加入 failed_skills (reason: "missing_skill_md")│
└─────────────────────────┬───────────────────────────────────┘
                          │
                          ▼
┌─────────────────────────────────────────────────────────────┐
│ Step 7: 冲突检查                                              │
│  - 对每个 valid skill {name}:                                │
│    - 检查 {package_path}/harness/skills/{name}/ 是否已存在            │
│    - 已存在 + force=false → 加入 skipped_skills              │
│    - 已存在 + force=true → 备份为 {name}.bak.{ts}            │
│    - 不存在 → 准备写入                                       │
└─────────────────────────┬───────────────────────────────────┘
                          │
                          ▼
┌─────────────────────────────────────────────────────────────┐
│ Step 8: 写入 skills 目录                                      │
│  - 确保 {package_path}/harness/skills/ 目录存在                       │
│    (std::filesystem::create_directories)                     │
│  - 对每个待写入的 skill:                                      │
│    - 原子 mv: /tmp/.../extract/{name}/ →                      │
│              {package_path}/harness/skills/{name}/                    │
│  - 记录到 uploaded_skills 列表                               │
└─────────────────────────┬───────────────────────────────────┘
                          │
                          ▼
┌─────────────────────────────────────────────────────────────┐
│ Step 9: 清理 & 响应                                            │
│  - 删除临时 zip 文件                                          │
│  - 删除临时解压目录                                           │
│  - 如果 uploaded_skills 为空 且 failed+skipped 全满:         │
│    → 返回错误 -1 或 -5                                        │
│  - 否则 → 返回成功响应 (含 uploaded/skipped/failed 列表)     │
└─────────────────────────────────────────────────────────────┘
```

### 4.2 `agent.list_skills` 流程

```
1. 参数校验: agent_id 非空
2. registry_.find(agent_id) → 获取 package_path
3. skills_dir = {package_path}/harness/skills/
4. 如果 skills_dir 不存在 → 返回空数组
5. 遍历 skills_dir 下的每个子目录:
   - name = 子目录名
   - has_skill_md = (子目录/SKILL.md 存在)
   - 加入 skills 列表
6. 返回 skills 列表
```

### 4.3 `agent.remove_skill` 流程

```
1. 参数校验: agent_id 非空, skill_name 非空
2. registry_.find(agent_id) → 获取 package_path
3. skill_dir = {package_path}/harness/skills/{skill_name}/
4. 如果 skill_dir 不存在 → 返回 -6
5. std::filesystem::remove_all(skill_dir)
6. 返回成功
```

---

## 5. 校验规则

### 5.1 Skill 目录校验

**唯一校验项**: 每个 skill 子目录包含 `SKILL.md` 文件。

```cpp
bool validate_skill_directory(const std::filesystem::path& skill_dir) {
    std::filesystem::path skill_md = skill_dir / "SKILL.md";
    return std::filesystem::exists(skill_md) &&
           std::filesystem::is_regular_file(skill_md);
}
```

### 5.2 不校验的内容

以下内容由 agent 运行时自行渐进加载，上传接口不做校验：

| 内容 | 说明 |
|------|------|
| `references/` 子目录 | 参考文档，按需加载 |
| `checklists/` 子目录 | 检查清单，按需加载 |
| `scripts/` 子目录 | 辅助脚本，按需加载 |
| `evals/` 子目录 | 评测案例，按需加载 |
| SKILL.md 的 YAML frontmatter | 字段完整性由 agent 自行判断 |
| skill name 与 frontmatter name 一致性 | 不强制 |

### 5.3 路径安全

zip 解压时必须检查每个条目的路径：

```cpp
bool is_path_safe(const std::string& entry_path) {
    // 拒绝绝对路径
    if (entry_path[0] == '/') return false;
    // 拒绝路径穿越
    if (entry_path.find("..") != std::string::npos) return false;
    return true;
}
```

---

## 6. 热替换机制

### 6.1 Skills 如何被 Agent 使用

基于 harness-agent 示例的实现（`llm_think_and_act.py`）：

1. Agent 的 LLM node 启动时读取 `agent.md` 中的 `skills` 列表
2. 对每个声明的 skill，读取 `{package_path}/harness/skills/{name}/SKILL.md`
3. 将 SKILL.md 内容追加到 system prompt
4. LLM 根据 system prompt 中的 skill 内容决定调用哪些工具

### 6.2 热替换策略

| 场景 | 机制 | 生效时机 |
|------|------|---------|
| 新增 skill | 写入 `{package_path}/harness/skills/{new}/SKILL.md` | 下次 LLM 调用时（如果 agent.md 声明了该 skill） |
| 更新 skill | 覆盖 `{package_path}/harness/skills/{name}/SKILL.md` | 下次 LLM 调用时 |
| 删除 skill | 删除 `{package_path}/harness/skills/{name}/` | 下次 LLM 调用时（如果 agent.md 仍引用则缺失） |

**关键点**: Skills 的热替换不需要重启 agent 或重新加载 package。Agent 的 LLM node 在**每次 LLM 调用**时重新读取 skills 目录内容。

### 6.3 与 `enable_hot_reload` 的区别

| 机制 | 对象 | 触发方式 |
|------|------|---------|
| `enable_hot_reload` | MMNode 的可执行文件（.so/.py/.sh） | Worker 级能力，默认关闭；节点显式启用后监控文件变化 |
| Skills 热替换 | SKILL.md 文件 | 无主动监控，LLM node 每次调用时读取 |

两者独立运作，互不影响。Agent package 归档更新则是第三条路径：Bridge 在磁盘更新完成后
通过 daemon `agent.unload → agent.load` 替换已加载实例，同样不依赖 `enable_hot_reload`。

---

## 7. 数据模型

### 7.1 AgentRegistry 扩展

```cpp
// include/internal/daemon/agent_registry.h

class AgentRegistry {
public:
    // ... 已有方法 ...

    // 新增: 获取 agent package 的文件系统路径
    std::string get_package_path(const std::string& agent_id) const;

private:
    struct AgentEntry {
        std::string agent_id;
        std::shared_ptr<framework::MMAgent> agent;
        std::string package_path;  // 已有字段
        std::string name;
    };
    // ...
};
```

### 7.2 RpcHandler 扩展

```cpp
// include/internal/daemon/rpc_handler.h

class RpcHandler {
public:
    // ... 已有方法 ...

    // 新增: Skills 管理
    std::string handle_agent_upload_skills(const std::string& params, int64_t id);
    std::string handle_agent_list_skills(const std::string& params, int64_t id);
    std::string handle_agent_remove_skill(const std::string& params, int64_t id);

private:
    // ... 已有成员 ...

    // 新增: 辅助方法
    bool decode_base64(const std::string& input, std::vector<uint8_t>& output);
    bool extract_zip(const std::vector<uint8_t>& zip_data,
                     const std::filesystem::path& dest_dir,
                     std::string& error_msg);
    bool validate_skill_directory(const std::filesystem::path& skill_dir);
};
```

---

## 8. 错误处理

### 8.1 错误分类

| 类别 | 错误码 | 处理方式 | 示例 |
|------|--------|---------|------|
| 参数错误 | `-32602` | 立即返回 JSON-RPC error | agent_id 为空 |
| Agent 不存在 | `-2` | 返回 error | agent_id 未加载 |
| 操作失败 | `-1` | 返回 error，message 区分具体原因 | Base64 损坏 / zip 损坏 / 全部 skill 缺 SKILL.md / 磁盘满 |
| 冲突 | `-5` | 返回 error | 所有 skill 同名且 force=false |
| 部分成功 | — | 返回 success（uploaded/skipped/failed 三元组） | zip 内 3 个 skill，1 个缺 SKILL.md，2 个正常 |

> **`-1` 的 message 规范**：`-1` 是 daemon 层的通用失败码。对于本接口，以下场景均返回 `-1`，通过 message 区分：
> ```
> -1, "Base64 decode failed: invalid character at offset 42"
> -1, "Zip extract failed: corrupted archive"
> -1, "All skills failed validation: 3 directories, 0 have SKILL.md"
> -1, "Failed to write skill: disk full"
> ```

### 8.2 临时文件清理保证

无论成功或失败，必须清理：
- `/tmp/agentflow_skills_{pid}_{ts}.zip`
- `/tmp/agentflow_skills_extract_{pid}_{ts}/`

使用 RAII 模式确保清理：

```cpp
struct TempDirGuard {
    std::filesystem::path path;
    ~TempDirGuard() {
        if (!path.empty()) {
            std::error_code ec;
            std::filesystem::remove_all(path, ec);
        }
    }
};
```

### 8.3 错误码规范与现状（技术债务记录）

#### 框架现状

daemon 层（`rpc_handler.cpp`）是框架中**唯一没有集中错误码定义的模块**。所有 RPC 错误码直接使用 magic number：

```cpp
// 现状 — 无任何定义，裸整数
build_error(-32602, "Invalid params: missing agent_id", id);
build_error(-2,    "Agent not found: " + agent_id, id);
build_error(-1,    "Failed to load agent from: " + package_path, id);
build_error(-3,    "Workflow not found: " + workflow_id, id);
```

**对比—框架其他模块**均使用 `enum` 集中定义：

| 模块 | 文件 | 做法 |
|------|------|------|
| FileStorage | `include/internal/utils/file_storage.h` | `enum StorageError : int { STORAGE_ERROR_IO = 1, ... }` |
| SSL Adapter | `vendor/network/ssl_adapter.h` | `enum class SslError : int { SSL_ERR_OK = 0, ... }` |
| Private Protocol | `vendor/private_protocol.h` | `ERROR_PROTOCOL = 0x0401, ...`（宏） |
| **daemon RPC** | `src/daemon/rpc_handler.cpp` | **无定义，magic number** |

#### 本实现的选择

**不引入集中定义，遵循 daemon 层现有惯例**：

- 理由：不应因一个 feature 修改框架已有模块的代码风格
- 做法：新增的错误码 `-5`（冲突）、`-6`（skill 不存在）与已有的 `-1`/`-2`/`-3` 风格一致——直接使用裸整数，附注释说明含义
- 复用 `-1` + message 区分多种失败场景（Base64、zip、校验、I/O），与现有 `build_error(-1, "...", id)` 模式一致

#### 技术债务

| 编号 | 债务 | 影响范围 | 建议处理 |
|------|------|---------|---------|
| TD-SKILL-UPLOAD-001 | daemon 层错误码散落在 `rpc_handler.cpp` 各处，无集中定义，无类型安全 | `rpc_handler.cpp` 全部 ~34 处 `build_error()` 调用 | 将来统一重构时，提取 `enum RpcError` 到 `include/internal/daemon/rpc_error.h`，并替换所有 magic number。参考 `file_storage.h` 的 `enum StorageError` 模式。 |

---

## 9. 依赖

### 9.1 Zip 解压库

| 选项 | 优势 | 劣势 | 推荐 |
|------|------|------|------|
| **miniz** | MIT, single-file, 零外部依赖 | C 库（但 C++ 项目广泛使用） | ✅ |
| 系统 zlib | 大多数系统自带 | 只处理 gzip, 需自写 zip 解析 | |
| libzip | 功能完整 | 依赖较重 | |

选择 **miniz**：放入 `vendor/miniz/`，通过 `vendor.sh` 管理。

### 9.2 Base64 解码

**复用项目已有实现**，无需手写或引入外部库。

项目在 `FileDataBusBridge` 中已有完整的 Base64 编解码：

| 文件 | 方法 |
|------|------|
| `include/internal/runtime/databus/file_databus_bridge.h:64-65` | `static std::string base64_encode(const std::string&)` / `static std::string base64_decode(const std::string&)` |
| `src/runtime/databus/file_databus_bridge.cpp:327-378` | 完整实现（标准 Base64 + padding 处理） |

在 `rpc_handler.cpp` 中直接调用：

```cpp
#include "runtime/databus/file_databus_bridge.h"

// 在 handle_agent_upload_skills() 中：
std::string raw_data = runtime::FileDataBusBridge::base64_decode(skills_data);
```

---

## 10. 文件系统布局

### 10.1 上传后的目录结构

```
/opt/agentflow/packages/harness-agent/   ← agent package 根目录
├── agent.json                           ← 静态（部署时固定）
├── agent.md                             ← 静态
├── package.json                         ← 静态
├── coordinator_workflow.json            ← 静态
├── dispatcher_pass_through.json         ← 静态
├── nodes/                               ← 静态
│   ├── llm_think_and_act.py
│   └── tool_echo.py
└── harness/
    ├── skills/                            ← 动态（上传接口写入）
    │   ├── test-skill-a/
    │   │   ├── SKILL.md
    │   │   └── references/
    │   └── test-skill-b/
    │       └── SKILL.md
    └── tools/                             ← 预留给未来扩展
```

### 10.2 备份策略

`force=true` 覆盖时，旧 skill 目录备份为：

```
{package_path}/harness/skills/{name}.bak.{timestamp}/
```

备份不自动清理（由运维决定何时清理）。

---

## 11. 多节点部署

### 11.1 当前限制

AgentFlow 框架未内置 Master-Worker 网络通信（INTEGRATION_GUIDE §6.5）。`agent.upload_skills` 只写入**当前节点**上 agent package 的 skills 目录。

### 11.2 多节点同步策略

由集成方选择，上传接口不干预：

| 策略 | 实现 | 适用场景 |
|------|------|---------|
| 共享文件系统 | NFS/cephfs 挂载 `package_path` | 同机房多节点 |
| 每节点独立上传 | 每个节点各自调用 upload_skills | 简单部署 |
| 镜像分发 | 上传后构建新镜像，滚动更新 | K8s 环境 |
| Git pull | skills 作为 git repo | 版本化场景 |

---

## 12. 测试策略

### 12.1 单元测试

| 用例 | 验证点 |
|------|--------|
| `UploadValidSkills` | 合法 zip 上传成功，文件写入正确位置 |
| `UploadInvalidSkill` | 缺 SKILL.md 的 skill 被记录为 failed |
| `UploadForceOverwrite` | force=true 覆盖旧 skill |
| `UploadNoForceConflict` | force=false 时同名 skill 被跳过 |
| `ListSkills` | 返回正确的 skills 列表 |
| `ListSkillsEmpty` | 无 skills 目录时返回空数组 |
| `RemoveSkill` | 删除成功 |
| `RemoveSkillNotFound` | 不存在的 skill 返回 -6 |
| `AgentNotFound` | 不存在的 agent_id 返回 -2 |
| `Base64Decode` | 合法/非法 Base64 处理 |
| `ZipExtract` | 合法/损坏 zip 处理 |
| `PathTraversal` | 含 ../ 的 zip 条目被拒绝 |

### 12.2 E2E 测试

```bash
# 1. 构建 agent package
# 2. 启动 agentflowd
# 3. 创建测试 skill zip
# 4. agent.upload_skills 上传
# 5. agent.list_skills 验证
# 6. agent.remove_skill 验证
# 7. 清理
```

---

## 13. 文件变更清单

| 文件 | 操作 | 预估行数 | 说明 |
|------|------|---------|------|
| `include/internal/daemon/agent_registry.h` | 修改 | +1 | 新增 `get_package_path()` 声明 |
| `src/daemon/agent_registry.cpp` | 修改 | +8 | 实现 `get_package_path()` |
| `include/internal/daemon/rpc_handler.h` | 修改 | +6 | 新增 3 个 handler 声明 + 辅助方法 |
| `src/daemon/rpc_handler.cpp` | 修改 | +250 | 3 个 handler 实现 + Base64 解码 + zip 解压 + 注册 |
| `vendor/miniz/miniz.h` + `miniz.c` | 新建 | — | 第三方 zip 解压库 |
| `CMakeLists.txt` | 修改 | +5 | 编译 miniz.c + 链接 |
| `tests/functional/src/test_skills_upload.cpp` | 新建 | ~150 | 单元测试 |
| `publish/API_SKILLS_UPLOAD_GUIDE.md` | 新建 | ~100 | API 使用文档 |
| `scripts/test_skills_upload_e2e.sh` | 新建 | ~60 | E2E 冒烟脚本 |

**不修改的文件**:
- agent.json / package.json 格式不变
- agent.cpp / package.cpp 不变
- agent_build.sh 不变
- 现有 RPC 方法不变

---

## 14. 调用示例

### 14.1 上传 Skills

```bash
# 准备测试 skill zip
mkdir -p /tmp/skill-a /tmp/skill-b
cat > /tmp/skill-a/SKILL.md << 'EOF'
---
name: skill-a
description: Test skill A
---
# Skill A
This is a test skill.
EOF
cat > /tmp/skill-b/SKILL.md << 'EOF'
---
name: skill-b
description: Test skill B
---
# Skill B
Another test skill.
EOF
cd /tmp && zip -r skills-upload.zip skill-a skill-b

# Base64 编码
DATA=$(base64 /tmp/skills-upload.zip | tr -d '\n')

# 发送 JSON-RPC
echo "{\"jsonrpc\":\"2.0\",\"method\":\"agent.upload_skills\",\
      \"params\":{\"agent_id\":\"harness-agent\",\"skills_data\":\"$DATA\"},\"id\":1}" | \
  nc 127.0.0.1 17891
```

### 14.2 列出 Skills

```bash
echo '{"jsonrpc":"2.0","method":"agent.list_skills","params":{"agent_id":"harness-agent"},"id":1}' | \
  nc 127.0.0.1 17891
```

### 14.3 删除 Skill

```bash
echo '{"jsonrpc":"2.0","method":"agent.remove_skill","params":{"agent_id":"harness-agent","skill_name":"skill-a"},"id":1}' | \
  nc 127.0.0.1 17891
```

---

## 15. 待确认事项

| # | 事项 | 当前决策 | 备注 |
|---|------|---------|------|
| 1 | zip 解压库选择 | miniz (MIT, vendor/) | 已确认 |
| 2 | 上传大小上限 | 100MB | 已确认 |
| 3 | 部分成功是否允许 | 是 | zip 内部分 skill 合法部分不合法时，合法的照常上传 |
| 4 | 备份策略 | force 覆盖时备份为 .bak.{ts} | 不自动清理 |
| 5 | skills 目录不存在时是否自动创建 | 是 | `create_directories({package_path}/harness/skills/)` |
