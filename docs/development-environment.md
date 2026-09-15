# AgentFlow 测试与开发环境

本仓库使用根目录的 `environment.dev.yml` 建立隔离的 Conda 环境 `agentflow-dev`。该环境同时覆盖当前 Python harness、AgentFlow C++ 底座、Protobuf 轨迹序列化，以及后续 Rust 重写所需工具链。

## 创建与进入环境

在仓库根目录执行：

```bash
conda env create -f environment.dev.yml
conda activate agentflow-dev
```

环境文件更新后，可同步已有环境：

```bash
conda env update -n agentflow-dev -f environment.dev.yml --prune
conda activate agentflow-dev
```

`agentflow-runtime` 以 editable 模式安装，因此修改 `packages/agentflow-runtime/src` 后不需要重复安装。

## Python harness 测试

日常本地回归：

```bash
python -m pytest packages/agentflow-runtime/tests -m "not real_sandbox"
```

`real_sandbox` 用例依赖真实的 `E2B_API_KEY` 和 `AF_SANDBOX_ENDPOINT`，本地无凭证时不应纳入默认回归。环境预置了 `NO_PROXY=127.0.0.1,localhost`，避免本机 sandbox 请求误走宿主 HTTP 代理。

## C++ 配置与构建

项目的 RapidJSON 查找逻辑不会自动搜索 Conda 前缀，因此首次配置必须显式传入头文件目录：

```bash
cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
  -DCMAKE_PREFIX_PATH="$CONDA_PREFIX" \
  -DRAPIDJSON_INCLUDE_DIR="$CONDA_PREFIX/include/rapidjson"
cmake --build build --parallel 16
```

当前代码在现代 GCC 下还存在两个与环境无关的源码阻塞：

- `include/internal/workflow/dsl/json_adapter.h` 使用 `uint64_t`，但没有直接包含 `<cstdint>`。
- `src/runtime/runtime.cpp` 使用 `std::remove`，但没有直接包含 `<algorithm>`，因而会被解析成 C 标准库的 `remove(const char*)`。

这些问题应在源码中补齐直接依赖，不建议通过全局 `CXXFLAGS=-include ...` 掩盖。

## 工具链自检

```bash
python --version
pytest --version
cmake --version
ninja --version
protoc --version
rustc --version
cargo --version
```

如需确认 Python 依赖一致性：

```bash
python -m pip check
```
