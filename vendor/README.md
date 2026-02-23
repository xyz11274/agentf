# Vendor 依赖与适配器说明

本目录用于管理 AgentFlow 的第三方依赖及其适配器实现。

## 目录结构

```text
vendor/
├── vendor.conf        # 第三方依赖配置（name|url|branch|target_dir）
├── vendor.sh          # 依赖下载与更新脚本
├── README.md          # 本文档
├── adapter/           # 三方适配器实现
│   ├── actor/         # Actor 适配（Protoactor / stub）
│   ├── cache/         # 缓存适配（Redis）
│   ├── dsl/           # JSON 适配（RapidJSON / stub）
│   ├── event/         # 事件循环适配
│   ├── network/       # 网络适配（预留）
│   └── serialization/ # 序列化适配（预留）
└── <downloaded>/      # 下载的第三方源码（已在 .gitignore 中忽略）
```

## `vendor.conf` 配置格式

每行一个依赖，格式如下：

```text
name|url|branch|target_dir
```

当前配置示例：

```text
libevent|https://github.com/libevent/libevent.git|master|libevent
Protoactor-cpp|https://github.com/DanielKernel/Protoactor-cpp.git|main|Protoactor-cpp
RapidJSON|https://github.com/Tencent/rapidjson.git|master|rapidjson
redis|https://github.com/redis/redis.git|unstable|redis
```

## 使用方式

在仓库根目录执行：

```bash
# 下载全部依赖
./vendor/vendor.sh

# 查看配置与安装状态
./vendor/vendor.sh --list

# 更新已下载依赖
./vendor/vendor.sh --update

# 清理 vendor 目录（会删除已下载内容）
./vendor/vendor.sh --clean
```

## 适配器与依赖对应关系

- `adapter/actor/`：Protoactor-cpp（未启用时使用 stub）
- `adapter/dsl/`：RapidJSON（未启用时使用 stub）
- `adapter/cache/`：Redis
- `adapter/event/`：libevent

## 注意事项

- 下载的第三方源码不纳入 Git 版本控制；适配器代码会纳入版本控制。
- 新克隆仓库后，需至少执行一次 `./vendor/vendor.sh`。
- 若需切换分支或重拉依赖，建议先执行 `--list` 检查状态。
