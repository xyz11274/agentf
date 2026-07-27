# CoreMind Bridge — C++ platform integration shim

> 模块级开发者 README，设计规范见 [L4 模块设计](../docs/design/architecture/modules/coremind-bridge.md)。

## What

A minimal HTTP service that links against CoreMind's service discovery libraries,
resolves the configured CoreMind services, builds a request-scoped trusted context,
and proxies external task callbacks to the internal Python Bridge.

Architecture:

```text
External request → Sidecar (:23767) → Bridge (:8199) → agentflowd (:17891)
```

## Build

Two build modes via the `COREMIND_ENABLED` CMake option:

### Production (Huawei CI/CD, requires CoreMind C++ SDK)

```bash
# Provide PLATFORM_DIR pointing to the CoreMind .so + headers
cmake -S sidecar \
      -B build-production \
      -DCOREMIND_ENABLED=ON \
      -DPLATFORM_DIR=/path/to/coremind/platform
cmake --build build-production
```

### Local / E2E (stub mode, no proprietary SDK required)

```bash
cmake -S sidecar \
      -B build-local \
      -DCOREMIND_ENABLED=OFF
cmake --build build-local
```

Stub mode reads fake service instances from the `COREMIND_STUB_SERVICES` env var
at runtime — no CoreMind libraries needed. Sufficient for full E2E except
real `MicroserviceManager::QueryInstanceList` SDK behavior.

**Stub 环境变量：**

| 变量 | 说明 | 示例 |
|------|------|------|
| `COREMIND_STUB_SERVICES` | 伪服务实例列表，支持多实例（同一服务名可重复出现） | `IMInferScheduler=10.0.0.5:27321,IMInferScheduler=10.0.0.6:27321` |
| `COREMIND_STUB_RETRY_DELAY_MS` | 模拟 SDK 重试延迟（毫秒），返回结果前等待指定时长 | `COREMIND_STUB_RETRY_DELAY_MS=200` |

The unified entrypoint `./scripts/build.sh <profile>` handles both modes:

| Profile | sidecar mode | SDK required | Output |
|---------|-------------|-------------|--------|
| `production` | `COREMIND_ENABLED=ON` | yes (`PLATFORM_DIR`) | `build-production/coremind_bridge` |
| `dev` | `COREMIND_ENABLED=OFF` (stub) | no | `build-dev/coremind_bridge` |
| `release` | `COREMIND_ENABLED=OFF` (stub) | no | `build-release/coremind_bridge` |

Production stack intentionally ignores dev/release stub binaries — stub永远返回
空服务发现，会立刻触发 `REQUIRED_SERVICE_DISCOVERY_FAILED`，不能服务生产请求。
Local stub sidecar should be started via `scripts/start_sidecar.sh local` separately.

## Run

```bash
# Stub mode: inject fake instances before starting
export COREMIND_STUB_SERVICES="IMInferScheduler=127.0.0.1:27321,KMMService=127.0.0.1:27322"

./coremind_bridge \
  --listen 0.0.0.0:23767 \
  --bridge-url http://127.0.0.1:8199/health
```

## 模型调用路径切换

sidecar 支持两种模型调用路径，由 `--uds-socket` 参数控制：

### 生产路径（CoreMind）

不传 `--uds-socket`，模型调用走 `platform_context.services.IMInferScheduler` 发现地址 → CoreMind 推理服务。

```bash
./scripts/build.sh production
export APPID="xxx"
export COREMIND_REQUIRED_SERVICES="IMInferScheduler,DMContextService,KMMService"
./scripts/start_sidecar.sh production
```

### 本地模拟路径（外部 OpenAI 兼容 API）

传 `--uds-socket`，模型调用走 UDS socket → sidecar 代理 → 外部 API。

```bash
./scripts/build.sh dev
export AF_LLM_API_KEY="sk-xxx"
export AF_LLM_BASE_URL="https://dashscope.aliyuncs.com/compatible-mode/v1"

./coremind_bridge \
  --listen 0.0.0.0:23767 \
  --bridge-url http://127.0.0.1:8199/health \
  --uds-socket /tmp/aisf_plugin.sock
```

`--llm-api-url` 可选，不传时自动从 `AF_LLM_BASE_URL` 环境变量读取。

### 一句话区分

| 路径 | `--uds-socket` | 模型调用目标 |
|------|---------------|-------------|
| 生产（CoreMind） | 不传 | `IMInferScheduler` 发现地址 |
| 本地模拟（外部 API） | 传 | `AF_LLM_BASE_URL` 指定的 OpenAI 兼容 API |

---

Port conventions (unified across dev/test/prod):

| Port | Service | Env var |
|------|---------|---------|
| 23767 | C++ SideCar (external) | `COREMIND_BRIDGE_PORT` |
| 8199 | Python Bridge (loopback) | `PYTHON_BRIDGE_PORT` |
| 17891 | agentflowd | `DAEMON_PORT` |

## API

```
GET  /health                  → {"status":"healthy","coremind":true,"sidecar":true}
GET  /healthcheck             → same response (platform-compatible alias)
GET  /sysagent/v1/scheduleTaskAbility  → capability advertisement
POST /coremind/systemagent/v1/callback → trusted envelope forwarded to the Python Bridge
POST /health/v1/tasks         → alias, forwarded to Bridge
POST /aisf/outfit/v1/tasks    → alias, forwarded to Bridge
GET  /resolve/{service_name}  → single service discovery diagnostic (admin only)
POST /resolve/batch           → batch discovery diagnostic (admin only)
POST /aisf/sysagent/v1/agentInstall/Update/Uninstall → stub 200, no-op
```

`/resolve/*` is retained for controlled diagnostics. The Python Bridge does not
call it on the business path; discovery results flow only inside the trusted
SideCar → Bridge request envelope.

## Further reading

- [L4 模块设计 — 构建与启动](../docs/design/architecture/modules/coremind-bridge.md#6-开发落点)
- [ADR-0005 — CoreMind 服务发现上下文决策](../docs/design/adrs/0005-coremind-service-discovery.md)
- [E2E 测试设计 — TC-brg-019~025 SideCar 用例](../docs/testing/e2e-test-design.md)
