// 公开接口：定义宿主接入所需的类型、配置和调用约定。
// 此处是对外声明，具体实现位于 src；修改接口时需考虑现有调用方与持久化版本。

#pragma once

#include <cstddef>
#include <filesystem>
#include <string>

#include "trajectory_trace.pb.h"

namespace trajectory::evolution {

struct TraceLimits {
  std::size_t max_bytes = 256U * 1024U * 1024U;
  std::size_t max_events = 100000;
};

struct ValidatedTrace {
  v1::TraceRun trace;
  v1::CapabilityReport report;
};

// JSON 使用 Protobuf 字段名，也接受下划线命名。
// 未知字段被拒绝；应用自定义字段放在 attributes。
v1::TraceRun parse_trace_json(const std::string &json, TraceLimits limits = {});
v1::TraceRun read_trace(const std::filesystem::path &path, TraceLimits limits = {});
ValidatedTrace validate_trace(const v1::TraceRun &trace, TraceLimits limits = {});
// 解析轨迹清单及其能力声明，拒绝不符合契约的字段。
v1::CapabilityManifest parse_manifest_json(const std::string &json);
// 根据真实采集与控制条件判断可支持等级，不凭名称宣称能力完整。
v1::CapabilityReport inspect_capabilities(const ValidatedTrace &trace,
                                          const v1::CapabilityManifest &manifest);
v1::CapabilityReport inspect_file(const std::filesystem::path &path,
                                  const v1::CapabilityManifest &manifest, TraceLimits limits = {});
// 将当前结果转换为公开 JSON 表示，内部临时状态不随意暴露。
std::string to_json(const google::protobuf::Message &message, bool pretty = true);
// 判断路径是否属于标准轨迹输入，避免把其他产物误当日志。
bool is_trace_path(const std::filesystem::path &path);

} // 命名空间结束：trajectory::evolution
