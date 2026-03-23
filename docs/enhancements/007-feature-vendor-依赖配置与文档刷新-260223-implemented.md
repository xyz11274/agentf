## Vendor 依赖配置与文档刷新（2026-02-23）

- `vendor/vendor.conf` 补充两项依赖配置：
  - `Protoactor-cpp|https://github.com/DanielKernel/Protoactor-cpp.git|main|Protoactor-cpp`
  - `RapidJSON|https://github.com/Tencent/rapidjson.git|master|rapidjson`
- `vendor/README.md` 已刷新并中文化，统一说明目录结构、配置格式、`vendor.sh` 用法和适配器依赖映射关系。
- 构建与测试验证：
  - `./build.sh build` 通过；
  - `./build.sh test` 出现既有 HotReload 并行抖动；
  - `ctest --test-dir build --output-on-failure -j 1` 串行回归 10/10 通过。
