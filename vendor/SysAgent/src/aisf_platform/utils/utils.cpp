#include <iomanip>
#include <thread>
#include <filesystem>
#include <unistd.h>
#include <sys/sysinfo.h>

#include "framework_common/utils/net_util.h"
#include "framework_common/utils/time_util.h"

#include "aisf_platform/utils/utils.h"
#include "aisf_platform/common_define.h"

namespace AisfPlat::Utils {
using namespace std;

string JoinIpPort(const string& ip, const string& port)
{
    if (!StringUtils::Contains(ip, ":")) {
        return ip + ":" + port;
    }

    string rst = ip;
    if (!StringUtils::StartsWith(ip, "[")) {
        rst = "[" + rst;
    }
    if (!StringUtils::EndsWith(ip, "]")) {
        rst += "]";
    }

    return rst + ":" + port;
}

string JoinIpPort(const string& ip, unsigned port) { return JoinIpPort(ip, std::to_string(port)); }

long GetTimeMs()
{
    struct timeval tv{};
    if (gettimeofday(&tv, nullptr) == 0) {
        return (tv.tv_sec * MILLI_IN_SECOND + tv.tv_usec / MILLI_IN_SECOND);
    }

    return 0;
}

int64_t GetTimeNano()
{
    auto now = std::chrono::time_point_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now());
    return now.time_since_epoch().count();
}

long GetUpTimeMs()
{
    struct sysinfo sysInfo{};
    if (sysinfo(&sysInfo) != 0) {
        return 0;
    }
    return sysInfo.uptime * MILLI_IN_SECOND;
}

string TimeToString(time_t stamp) { return TimeToString(stamp, "%Y-%m-%d %H:%M:%S"); }

string MillisecondToString(long milliSec)
{
    return TimeToString(milliSec / MILLI_IN_SECOND) + "." + std::to_string(milliSec % MILLI_IN_SECOND);
}

string TimeToString(time_t stamp, const string& format)
{
    string rst;
    struct tm tmStruct{};

    const struct tm* tmPtr = localtime_r(&stamp, &tmStruct);
    if (tmPtr != nullptr) {
        char timeBuffer[BUFFER_SIZE_256B] = {0};
        strftime(timeBuffer, BUFFER_SIZE_256B, format.c_str(), tmPtr);
        rst = timeBuffer;
    }

    return rst;
}

// TimeDurationToStr 格式化时间
string TimeDurationToStr(uint64_t stamp)
{
    const static uint64_t timeStepNum = 1000;
    char buffer[BUFFER_SIZE_128B]{};

    if (stamp < timeStepNum) {
        StringUtils::Format(buffer, BUFFER_SIZE_128B, "%lluns", stamp);
        return buffer;  // 转换为纳秒
    }

    auto decimals = stamp % timeStepNum;
    stamp /= timeStepNum;
    if (stamp < timeStepNum) {
        StringUtils::Format(buffer, BUFFER_SIZE_128B, "%llu.%03lluus", stamp, decimals);
        return buffer;  // 转换为微妙
    }

    decimals = stamp % timeStepNum;
    stamp /= timeStepNum;
    if (stamp < timeStepNum) {
        StringUtils::Format(buffer, BUFFER_SIZE_128B, "%llu.%03llums", stamp, decimals);
        return buffer;  // 转换为毫秒
    }

    decimals = stamp % timeStepNum;
    stamp /= timeStepNum;
    if (stamp < SECOND_IN_MINUTE) {
        StringUtils::Format(buffer, BUFFER_SIZE_128B, "%llu.%03llus", stamp, decimals);
        return buffer;  // 转换为秒
    }

    decimals = stamp % SECOND_IN_MINUTE;
    stamp /= SECOND_IN_MINUTE;
    if (stamp < MINUTE_IN_HOUR) {
        StringUtils::Format(buffer, BUFFER_SIZE_128B, "%llu.%03lluM", stamp, decimals);
        return buffer;  // 转换为分钟
    }

    decimals = stamp % MINUTE_IN_HOUR;
    stamp /= MINUTE_IN_HOUR;
    StringUtils::Format(buffer, BUFFER_SIZE_128B, "%llu.%03lluH", stamp, decimals);
    return buffer;  // 剩下的时间，全部转换为小时
}

string BytesToString(size_t size)
{
    const static size_t BUFFER_LEN = 64;
    const static size_t BYTE_MAX_NUM = 1024;
    const static size_t KB_MAX_NUM = 1024 * BYTE_MAX_NUM;
    const static size_t MB_MAX_NUM = 1024 * KB_MAX_NUM;
    const static size_t GB_MAX_NUM = 1024 * MB_MAX_NUM;
    const static size_t PB_MAX_NUM = 1024 * GB_MAX_NUM;

    char buffer[BUFFER_LEN] = {0};
    double dSize = size;

    if (size < BYTE_MAX_NUM) {
        sprintf_s(buffer, BUFFER_LEN, "%uB", size);
    } else if (size < KB_MAX_NUM) {
        sprintf_s(buffer, BUFFER_LEN, "%.2lfKB", dSize / BYTE_MAX_NUM);
    } else if (size < MB_MAX_NUM) {
        sprintf_s(buffer, BUFFER_LEN, "%.2lfMB", dSize / KB_MAX_NUM);
    } else if (size < GB_MAX_NUM) {
        sprintf_s(buffer, BUFFER_LEN, "%.2lfGB", dSize / MB_MAX_NUM);
    } else if (size < PB_MAX_NUM) {
        sprintf_s(buffer, BUFFER_LEN, "%.2lfTB", dSize / GB_MAX_NUM);
    } else {
        sprintf_s(buffer, BUFFER_LEN, "%.2lfPB", dSize / PB_MAX_NUM);
    }

    return buffer;
}

void SetThreadName(const string& name)
{
    const static int WAIT_MS = 10 * MICRO_IN_MILLI;
    const static int MAX_NAME_LEN = 16;  // 最后一个是结束符
    const static int MAX_TRY_TIMES = 3;

    char buffer[MAX_NAME_LEN] = {0};
    auto tid = pthread_self();
    auto fullName = name;
    fullName = fullName.substr(0, MAX_NAME_LEN - 1);

    for (int i = 0; i < MAX_TRY_TIMES; ++i) {
        (void)pthread_setname_np(tid, fullName.c_str());
        (void)pthread_getname_np(tid, buffer, MAX_NAME_LEN);
        if (fullName == buffer) {
            return;
        }

        usleep(WAIT_MS);
    }

    LOG_ERR("thread name set error:%s", fullName.c_str());
}

string GetBaseIp()
{
    return CMFrm::Utils::GetBasePlaneIp();
}

long GetNProcessNum() { return sysconf(_SC_NPROCESSORS_CONF); }

std::string GetCurrentDate()
{
    time_t now = time(0);
    char buf[80];
    struct tm tstruct;
    tstruct = *localtime(&now);

    // Format: YYYY-MM-DD
    strftime(buf, sizeof(buf), "%Y%m%d", &tstruct);
    return std::string(buf);
}

std::string GenerateUniqueId()
{
    //TODO, 自增ID(需要修正为UUID)
    static int idCounter = 0;
    idCounter++;
    return std::to_string(idCounter);
}

std::string VectorToJsonString(const std::vector<std::string>& vec)
{
    // 创建一个 JSON 数组类型的 Json::Value 对象
    Json::Value jsonArray(Json::arrayValue);

    // 将 vector 中的字符串添加到 JSON 数组中
    for (const auto& str : vec) {
        jsonArray.append(str);
    }

    // 创建一个 Json::StreamWriterBuilder，用于将 Json::Value 转换为字符串
    Json::StreamWriterBuilder writer;
    return Json::writeString(writer, jsonArray);
}

std::string GetCurrentTimestampInCustomFormat()
{
    return CMFrm::Utils::GetCurrentTimeStr("%Y-%m-%dT%H:%M:%SZ");
}

std::string GetThreadName()
{
    char name[32];
    if (pthread_getname_np(pthread_self(), name, sizeof(name)) == 0) {
        return std::string(name);
    } else {
        std::ostringstream oss;
        oss << "Thread-" << std::this_thread::get_id();
        return oss.str();
    }
}
std::string GetCurrentTimeMillisecondStr()
{
    auto curTimestamp = std::chrono::system_clock::now();
    std::time_t curTimestampT = std::chrono::system_clock::to_time_t(curTimestamp);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(curTimestamp.time_since_epoch()).count() % 1000;
    std::stringstream oss;
    oss << std::put_time(std::localtime(&curTimestampT), "%Y-%m-%dT%H:%M:%S");
    oss << "." << std::setw(3) << std::setfill('0') << ms << "Z";
    return std::string(oss.str());
}
}  // namespace AisfPlat::Utils