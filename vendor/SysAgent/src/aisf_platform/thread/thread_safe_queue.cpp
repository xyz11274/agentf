#include "framework_common/utils/file_util.h"
#include "aisf_platform/thread/thread_safe_queue.h"
#include <cstdint>

namespace AisfPlat {
using namespace std;
using namespace StringUtils;

ThreadSafeQueueBase::ThreadSafeQueueBase(string n, bool enableMetric) : name(std::move(n)), isEnableMetric(enableMetric)
{
    instances.first.lock();
    instances.second.emplace(this);
    instances.first.unlock();
}

ThreadSafeQueueBase::~ThreadSafeQueueBase()
{
    instances.first.lock();
    instances.second.erase(this);
    instances.first.unlock();
}

void ThreadSafeQueueBase::SucceedInQueue(const ThreadSafeQueueMsgBase& item)
{
    CHECK_FALSE_RETURN_VOID(isEnableMetric);
    auto& metric = metrics[item.inQueTimeMinPosInHour];

    ++metric.inQueueCount;
    metric.inQueueSize += item.Size();

    ++metric.succeedCount;
    metric.succeedSize += item.Size();
}

void ThreadSafeQueueBase::FailedInQueue(const ThreadSafeQueueMsgBase& item)
{
    CHECK_FALSE_RETURN_VOID(isEnableMetric);
    auto& metric = metrics[item.inQueTimeMinPosInHour];

    ++metric.inQueueCount;
    metric.inQueueSize += item.Size();

    ++metric.failCount;
    metric.failSize += item.Size();
}

void ThreadSafeQueueBase::OutQueue(const ThreadSafeQueueMsgBase& item)
{
    CHECK_FALSE_RETURN_VOID(isEnableMetric);
    auto nowNano = Utils::GetTimeNano();
    CHECK_TRUE_RETURN_VOID(nowNano < item.inQueTimeNano); // 防止后续减反转

    auto& metric = metrics[item.inQueTimeMinPosInHour];

    ++metric.outQueueCount;
    metric.outQueueSize += item.Size();

    auto cost = static_cast<uint64_t>(nowNano - item.inQueTimeNano);
    metric.outWaitTotalNano += cost;
    if (cost > metric.maxOutQueueNano) {
        metric.maxOutQueueNano = cost;
    }
}

void ThreadSafeQueueBase::MakeAllReport()
{
    CHECK_FALSE_RETURN_VOID(CMFrm::Utils::CheckFileExist("/tmp/writeThreadSafeQueue"));
    CHECK_FALSE_RETURN_VOID(remove("/tmp/writeThreadSafeQueue") == 0);

    instances.first.lock();
    auto ins = instances.second;
    instances.first.unlock();

    stringstream ss;
    for (auto one : ins) {
        CHECK_TRUE_CONTINUE(one == nullptr)
        one->Report(ss);
        ss << endl;
    }

    auto content = ss.str();
    FileUtils::WriteTextFile("/tmp/ThreadSafeQueue.txt", content.c_str(), content.size());
}

void ThreadSafeQueueBase::Report(stringstream& ss)
{
    ss << "ThreadSafeQueue:" << name << ", create @ " << Utils::MillisecondToString(createTimeMs);
    ChildReport(ss);
    ss << endl;

    if (!isEnableMetric) {
        ss << "\t\tNot enable queue metric!!!" << endl;
        return;
    }

    ss << Metric::HEADER;
    time_t now = time(nullptr);
    SafeString buffer{__FILE_NAME__, __LINE__, BUFFER_SIZE_4KB};

    // 偏移一个小时
    time_t oneHourAgo = now - SECOND_IN_MINUTE * MINUTE_IN_HOUR;
    string yearMonthDayHourStr = Utils::TimeToString(oneHourAgo, "%Y-%m-%dT%H");
    int startPos = int(now / SECOND_IN_MINUTE + 1) % MINUTE_IN_HOUR;
    for (int pos = startPos; pos < MINUTE_IN_HOUR; ++pos) {
        auto& metric = metrics[pos];
        Format(buffer.Get(), buffer.Size(), Metric::LINE, yearMonthDayHourStr.c_str(), pos, metric.inQueueCount,
            Utils::BytesToString(metric.inQueueSize).c_str(), metric.succeedCount,
            Utils::BytesToString(metric.succeedSize).c_str(), metric.failCount,
            Utils::BytesToString(metric.failSize).c_str(), metric.outQueueCount,
            Utils::BytesToString(metric.outQueueSize).c_str(),
            Utils::TimeDurationToStr(SAFE_DIVIDE(metric.outWaitTotalNano, metric.outQueueCount)).c_str(),
            Utils::TimeDurationToStr(metric.maxOutQueueNano).c_str());
        ss << buffer.Data();
    }

    yearMonthDayHourStr = Utils::TimeToString(now, "%Y-%m-%dT%H");
    for (int pos = 0; pos < MINUTE_IN_HOUR && pos < startPos; ++pos) {
        auto& metric = metrics[pos];
        Format(buffer.Get(), buffer.Size(), Metric::LINE, yearMonthDayHourStr.c_str(), pos, metric.inQueueCount,
            Utils::BytesToString(metric.inQueueSize).c_str(), metric.succeedCount,
            Utils::BytesToString(metric.succeedSize).c_str(), metric.failCount,
            Utils::BytesToString(metric.failSize).c_str(), metric.outQueueCount,
            Utils::BytesToString(metric.outQueueSize).c_str(),
            Utils::TimeDurationToStr(SAFE_DIVIDE(metric.outWaitTotalNano, metric.outQueueCount)).c_str(),
            Utils::TimeDurationToStr(metric.maxOutQueueNano).c_str());
        ss << buffer.Data();
    }
    ss << endl;
}
} // namespace AisfPlat