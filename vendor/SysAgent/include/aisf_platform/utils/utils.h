#pragma once

#include "aisf_platform/parser/json_parser.h"
#include "aisf_platform/string/safe_string.h"
#include "aisf_platform/string/string_buffer.h"
#include "aisf_platform/string/string_ring_buffer.h"
#include "aisf_platform/string/string_utils.h"
#include "aisf_platform/thread/thread_safe_map.h"
#include "file_utils.h"
#include "template_utils.h"

namespace AisfPlat::Utils {
// 组合ip和端口，支持ipv4和ipv6
string JoinIpPort(const string& ip, const string& port);

// 组合ip和端口，支持ipv4和ipv6
string JoinIpPort(const string& ip, unsigned port);

struct IpAddr {
    string ip{};
    uint16_t port{0};

    [[nodiscard]] string ToString() const { return JoinIpPort(ip, port); }
};

// 获取当前系统时间，单位为毫秒
long GetTimeMs();

// 获取当前系统时间，单位为纳秒
int64_t GetTimeNano();

// 获取操作系统的启动时间，单位为毫秒
long GetUpTimeMs();

// 时间转换为字符串，支持自定义格式
string TimeToString(time_t stamp, const string& format);

// 时间转换为字符串，标准格式
string TimeToString(time_t stamp);

// 毫秒时间转换为字符串，标准格式
string MillisecondToString(long milliSec);

// 格式化一个时间戳，此时间单位为纳秒
string TimeDurationToStr(uint64_t stamp);

// 将一个大小输出为字符串，方便阅读
string BytesToString(size_t size);

// 阻塞设置线程名称，重试3次
void SetThreadName(const string& name);

// 获取base平面的IP，即eth1的最后一个IPv4地址，如果获取失败，值是空串
string GetBaseIp();

// 获取当前系统所有的cpu核数
long GetNProcessNum();

std::string GetCurrentDate();

std::string GenerateUniqueId();

std::string VectorToJsonString(const std::vector<std::string>& vec);

std::string GetCurrentTimestampInCustomFormat();

std::string GetThreadName();

std::string GetCurrentTimeMillisecondStr();
}