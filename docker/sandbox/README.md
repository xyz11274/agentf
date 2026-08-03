# 沙箱镜像构建与使用

> 与 Dockerfile 同目录，构建前请先阅读。

## 构建镜像

```bash
# 在项目根目录执行
docker build -t agentflow-sandbox:v1 -f docker/sandbox/Dockerfile .
```

## 镜像内容

| 组件 | 说明 |
|------|------|
| **基础镜像** | `python:3.12-slim`（~120MB） |
| **HTTP server** | `/opt/sandbox/http_server/sandbox_http_server.py` — POST /exec 统一入口 |
| **工具目录** | `/opt/sandbox/tools/` |
| **工作区** | `/workdir`（宿主机挂载点） |
| **健康检查** | 每 30s 探测 `/exec` 端点 |
| **默认启动** | `ENTRYPOINT` 自动启动 HTTP server（端口 8080） |

## 标签管理

```bash
# 构建并打标签
docker build -t agentflow-sandbox:v1 -t agentflow-sandbox:latest -f docker/sandbox/Dockerfile .

# 推送到 registry（需先 docker login）
docker tag agentflow-sandbox:v1 registry.example.com/agentflow-sandbox:v1
docker push registry.example.com/agentflow-sandbox:v1
```

## 配置项

| 配置项 | 默认值 | 说明 |
|--------|--------|------|
| `image_name` | `agentflow-sandbox:v1` | daemon 配置中引用，见 `configs/*/agentflowd.json` 的 `sandbox.image_name` |

## 与 e2b 的关系

Docker 沙箱与 e2b 沙箱共用同一代码库（`sandbox_http_server.py`），但镜像构建方式不同：

| 特性 | Docker 沙箱 | e2b 沙箱 |
|------|------------|----------|
| 镜像来源 | 本地 `docker build` | e2b 平台模板（`Sandbox.create(template=...)`） |
| 构建方式 | `docker/sandbox/Dockerfile` | 通过 e2b SDK `Template` API |
| 生命周期 | 宿主机管理 | e2b 平台管理 |

详见 `docs/usage/sandbox.md` §4「e2b 模板创建」。