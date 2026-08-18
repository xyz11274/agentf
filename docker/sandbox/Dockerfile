# AgentFoundry 沙箱镜像（DockerSandbox + e2b Sandbox 共用）
# 对齐 lld.md T070 + plan.md §11.2 最小细度
# 基础镜像：python:3.12-slim（最小化，约 120MB）
FROM python:3.12-slim

# 沙箱内 HTTP server 端口（固定，宿主机映射时映射到 0:8080 自动分配）
EXPOSE 8080

# 安装最小依赖（curl 用于健康检查）
RUN apt-get update && apt-get install -y --no-install-recommends \
    curl \
    && rm -rf /var/lib/apt/lists/*

# 沙箱内 HTTP server 安装路径
# 宿主机侧 docker run 时通过 -v 映射宿主机 http_server 到容器内 /opt/sandbox/http_server/
# 也支持 COPY 方式（镜像构建时嵌入）
ARG HTTP_SERVER_SRC=/docker/sandbox/http_server
COPY ${HTTP_SERVER_SRC} /opt/sandbox/http_server/

# 沙箱内环境初始化脚本（统一依赖安装入口）
# 与 e2b 模板构建共用同一份 sandbox-setup.sh
COPY docker/sandbox/sandbox-setup.sh /opt/sandbox/sandbox-setup.sh
RUN bash /opt/sandbox/sandbox-setup.sh

# 沙箱内工具目录
RUN mkdir -p /opt/sandbox/tools

# 沙箱内工作区挂载点
# 宿主机侧 docker run 时通过 -v 映射宿主机 workdir 到 /workdir/
RUN mkdir -p /workdir

# workdir 物理挂载点（/workdir，下方挂载）与 HTTP server 定位 harness/skills 用的
# AF_SANDBOX_WORKSPACE 环境变量在此同一镜像层固化——单一真相来源。
# 调用方（sandbox_docker.cpp / sandbox_e2b.cpp / conftest.py）显式注入同值亦兼容，
# 但镜像本身不再依赖调用方"记得"注入，消除霰弹式注入点。
# 2026-08-17 review P3：统一契约名（原 SANDBOX_WORKDIR 已废弃，path-translation-convergence.md）。
ENV AF_SANDBOX_WORKSPACE=/workdir

# 健康检查：每 30s 探测 /exec 端点
HEALTHCHECK --interval=30s --timeout=5s --start-period=5s --retries=3 \
    CMD curl -s -X POST http://127.0.0.1:8080/exec \
        -H "Content-Type: application/json" \
        -d '{"tool":"bash","tool_params":{"command":"echo ready"},"opts":{}}' \
        | grep -qE '"exit_code"[[:space:]]*:[[:space:]]*0' || exit 1

# 默认启动 HTTP server
ENTRYPOINT ["python3", "/opt/sandbox/http_server/sandbox_http_server.py", "--port", "8080", "--host", "0.0.0.0"]