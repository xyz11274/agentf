# CoreMind Bridge — C++ platform integration shim

## What

A minimal HTTP service that links against CoreMind's service discovery libraries,
resolves the configured CoreMind services, builds a request-scoped trusted context,
and proxies external task callbacks to the internal Python Bridge.

## Build

**Prerequisites**: CoreMind `.so` + headers (from Huawei internal CI/CD cache).

```bash
# In HealthAgent CI/CD environment (or any env with CoreMind libs):
cd HealthAgent

# Option A: Build standalone
cmake -S frontend/bridge/coremind_bridge \
      -B build/coremind_bridge \
      -DPLATFORM_DIR=$(pwd)/build/platform
cmake --build build/coremind_bridge

# Local/E2E build without proprietary CoreMind libraries
cmake -S frontend/bridge/coremind_bridge \
      -B build/coremind_bridge-local \
      -DCOREMIND_ENABLED=OFF
cmake --build build/coremind_bridge-local

# Option B: Add as subdirectory to SysAgent's CMakeLists.txt
# add_subdirectory(../frontend/bridge/coremind_bridge coremind_bridge_build)
```

## Run

```bash
./coremind_bridge \
  --listen 127.0.0.1:23767 \
  --bridge-url http://127.0.0.1:8199
```

## API

```
GET  /health                  → {"status":"healthy","coremind":true,"sidecar":true}
GET  /healthcheck             → same response (platform-compatible alias)
GET  /resolve/{service_name}  → {"service":"IMInferScheduler","instances":[{"ip":"...","port":27321}],"status":"ok"}
POST /resolve/batch           → {"services":{...},"status":"ok"}
POST /coremind/systemagent/v1/callback → trusted envelope forwarded to the Python Bridge
```

`/resolve/*` is retained for controlled diagnostics. The Python Bridge does not
call it on the business path; discovery results flow only inside the trusted
SideCar → Bridge request envelope.
