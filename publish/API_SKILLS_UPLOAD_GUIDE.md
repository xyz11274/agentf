# Skills 动态上传 API 参考

> 文档版本: v0.1
> 生效版本: AgentFlow 0.1.0+

## 概述

本接口允许外部系统动态向已部署的 Agent 注入或更新 SKILL.md 技能包，
无需重新部署整个 Agent Package。
技能包存放在 Agent Package 的 `harness/skills/` 目录下。

## 传输协议

所有接口通过 `agentflowd` daemon 的 JSON-RPC 2.0 端点访问。

- TCP: 默认 `127.0.0.1:7890`
- Unix Domain Socket: 通过配置的 `uds_biz_path`

## 方法

### 1. agent.upload_skills

向指定 Agent 的 `harness/skills/` 目录上传一个或多个 SKILL.md 技能包。

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

**参数说明**:

| 参数 | 类型 | 必填 | 默认 | 说明 |
|------|------|------|------|------|
| agent_id | string | 是 | — | 目标 Agent 的 ID，必须是已加载的 Agent |
| skills_data | string | 是 | — | Base64 编码的 zip 文件，内含一个或多个 skill 目录 |
| force | bool | 否 | false | 同名 skill 已存在时是否覆盖 |

**zip 包格式**: 解压后根目录下每个子目录代表一个 skill，每个 skill 目录必须包含 SKILL.md 文件。

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

#### 错误码

| 错误码 | 含义 | 触发条件 |
|--------|------|---------|
| -32602 | Invalid params | agent_id 或 skills_data 缺失 |
| -2 | Agent not found | agent_id 对应的 Agent 未加载 |
| -1 | Operation failed | Base64解码失败 / zip解压失败 / 所有skill均校验失败 |
| -5 | Conflict | 所有 skill 同名且 force=false |

> 注意：部分成功是允许的——zip 中部分 skill 合法、部分不合法时，合法的正常上传。

### 2. agent.list_skills

列出指定 Agent 已有的 skills。

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
      {"name": "skill-a", "path": "/opt/agentflow/packages/harness-agent/harness/skills/skill-a", "has_skill_md": true},
      {"name": "skill-b", "path": "/opt/agentflow/packages/harness-agent/harness/skills/skill-b", "has_skill_md": false}
    ]
  },
  "id": 1
}
```

### 3. agent.remove_skill

删除指定 Agent 中的一个 skill。

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
| -32602 | agent_id 或 skill_name 缺失 |
| -2 | Agent not found |
| -6 | Skill not found |

## 调用示例

### nc (TCP JSON-RPC)

```bash
# 1. 创建测试 skill
mkdir -p /tmp/my-skill
cat > /tmp/my-skill/SKILL.md << 'EOF'
---
name: my-skill
description: My test skill
---
# My Skill
This is a test skill.
EOF

# 2. 打包并 base64 编码
cd /tmp && zip -r skills-upload.zip my-skill
DATA=$(base64 skills-upload.zip | tr -d '\n')

# 3. 上传
echo "{\"jsonrpc\":\"2.0\",\"method\":\"agent.upload_skills\",\
      \"params\":{\"agent_id\":\"harness-agent\",\"skills_data\":\"$DATA\"},\"id\":1}" | \
  nc 127.0.0.1 7890

# 4. 列出 skills
echo '{"jsonrpc":"2.0","method":"agent.list_skills",\
      "params":{"agent_id":"harness-agent"},"id":1}' | nc 127.0.0.1 7890

# 5. 删除 skill
echo '{"jsonrpc":"2.0","method":"agent.remove_skill",\
      "params":{"agent_id":"harness-agent","skill_name":"my-skill"},"id":1}' | \
  nc 127.0.0.1 7890
```

### Python

```python
import json, socket, base64

def send_rpc(method, params):
    req = json.dumps({"jsonrpc":"2.0","method":method,"params":params,"id":1})
    sock = socket.create_connection(("127.0.0.1", 7890))
    sock.sendall(req.encode())
    resp = b""
    while True:
        chunk = sock.recv(4096)
        if not chunk: break
        resp += chunk
    sock.close()
    return json.loads(resp)

# Upload
with open("skills-upload.zip", "rb") as f:
    data = base64.b64encode(f.read()).decode()
result = send_rpc("agent.upload_skills", {
    "agent_id": "harness-agent",
    "skills_data": data
})
print(json.dumps(result, indent=2))

# List
result = send_rpc("agent.list_skills", {"agent_id": "harness-agent"})
print(json.dumps(result, indent=2))
```

## 多节点部署说明

AgentFlow 框架当前未内置 Master-Worker 网络通信层。
`agent.upload_skills` 只写入当前节点上 Agent Package 的 `harness/skills/` 目录。

多节点场景下的 skills 同步策略：

| 策略 | 说明 |
|------|------|
| 共享文件系统 | NFS/cephfs 挂载 Agent Package 根目录，上传一次所有节点可见 |
| 每节点独立上传 | 每个节点各自调用 upload_skills |
| 镜像分发 | 上传后构建新镜像，滚动更新 |
| Git pull | skills 作为 git repo 管理 |
