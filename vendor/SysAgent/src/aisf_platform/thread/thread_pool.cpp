#include "framework_common/utils/file_util.h"
#include "aisf_platform/thread/thread_pool.h"

namespace AisfPlat {
using namespace std;

ThreadPoolBase::ThreadPoolBase(string name, bool enableMetric) : m_name(std::move(name)), m_isEnableMetric(enableMetric)
{
    instances.first.lock();
    instances.second.emplace(this);
    instances.first.unlock();
}

ThreadPoolBase::~ThreadPoolBase()
{
    instances.first.lock();
    instances.second.erase(this);
    instances.first.unlock();
}

void ThreadPoolBase::MakeAllReport()
{
    CHECK_FALSE_RETURN_VOID(CMFrm::Utils::CheckFileExist("/tmp/writeThreadPool"));
    CHECK_FALSE_RETURN_VOID(remove("/tmp/writeThreadPool") == 0);

    instances.first.lock();
    auto ins = instances.second;
    instances.first.unlock();

    stringstream ss;
    for (auto one : ins) {
        CHECK_TRUE_CONTINUE(one == nullptr)
        ss << "---------------------" << endl;
        one->Report(ss);
        ss << endl;
    }

    auto content = ss.str();
    FileUtils::WriteTextFile("/tmp/ThreadPool.txt", content.c_str(), content.size());
}

void ThreadPoolBase::Report(stringstream& ss)
{
    ss << "ThreadPool:" << m_name << ", create @ " << Utils::MillisecondToString(m_createTimeMs) << endl;
    for (int i = 0; i < MAX_THREAD_COUNT; ++i) {
        CHECK_TRUE_CONTINUE(m_lastProcTimeStamp[i] == 0)
        ss << "\tthread pos:" << i << " last run @ " << Utils::MillisecondToString(m_lastProcTimeStamp[i]) << endl;
    }

    if (!m_isEnableMetric) {
        ss << "\n\t\tNot enable queue metric!!!" << endl;
        return;
    }

    ChildReport(ss);
}
}