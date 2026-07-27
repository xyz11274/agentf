# 生产配置文件

本目录存放不同环境的 agentflowd daemon 配置。

## 用法

### agentflowd daemon

```bash
# 启动 daemon 时通过 --config 指定配置文件
./build/agentflowd --config configs/prod/agentflowd.json

# start_daemon.sh 目前不自动加载 --config，需要手动追加：
./scripts/start_daemon.sh --profile production   # 构建后手动加 --config
```

或者通过环境变量覆盖个别字段：

```bash
export AF_DATAREF_REDIS_URL=redis://prod-redis:6379
export AF_DATAREF_CAPACITY_MB=2048
export AF_LOG_LEVEL=warn
```

### Bridge

Bridge 的配置在 `frontend/bridge/config.yaml`，生产环境建议：

```bash
# 通过环境变量覆盖默认值
export BRIDGE_LISTEN=0.0.0.0:8199
export DAEMON_PORT=17891
```

## 配置说明

| 字段 | 生产值 | 含义 |
|------|--------|------|
| `server.listen` | `0.0.0.0:17891` | TCP 监听地址（与 dev/test 端口统一） |
| `runtime.worker_threads` | 8 | Worker 线程池大小 |
| `dataref_bridge.capacity_mb` | 1024 | 总线 B 容量 |
| `daemon.drain_timeout_ms` | 60000 | 优雅关闭等待时间 |
| `trace.flow_dir` | `logs/traces` | 流式 trace 输出目录 |
