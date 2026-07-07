#include "framework_common/timermanager/timer_manager.h"
#include "framework_common/utils/file_util.h"
#include "aisf_platform/websockets/maintain.h"

namespace AisfPlat {
using namespace std;

void WsiMaintain::Initial()
{
    auto timer = std::make_shared<CMFrm::Timer::TimerTask>(0, 5000, Report);
    CMFrm::Timer::TimerManager::GetTimerManager()->AddTimerTask(timer);
}

void WsiMaintain::AddRecord(const string& from, const string& to, const lws* wsi, const void* data, size_t len)
{
    CHECK_FALSE_RETURN_VOID(m_isWsiHistoryEnable);
    auto record = std::make_shared<Record>(from, to, wsi, data, len);

    mutex_guard guard(WsiMaintain::Instance().m_lock);
    while(m_records.size() >= MAX_DATA_INFO_NUM) {
        m_records.pop_front();
    }
    m_records.emplace_back(record);
}

void WsiMaintain::Report()
{
#ifndef ACM_DEV_MOD
    WsiMaintain::Instance().m_isWsiHistoryEnable = CMFrm::Utils::CheckFileExist("/tmp/enableWriteWsiHistory");
    CHECK_FALSE_RETURN_VOID(WsiMaintain::Instance().m_isWsiHistoryEnable);
    CHECK_FALSE_RETURN_VOID(CMFrm::Utils::CheckFileExist("/tmp/writeWsiHistory"));
    CHECK_FALSE_RETURN_VOID(remove("/tmp/writeWsiHistory") == 0);
    const static string wsiHistoryPath = "/tmp/WsiHistory.csv";
#else
    CHECK_FALSE_RETURN_VOID(CMFrm::Utils::CheckFileExist("/tmp/writeWsiHistory2"));
    CHECK_FALSE_RETURN_VOID(remove("/tmp/writeWsiHistory2") == 0);
    const static string wsiHistoryPath = "/tmp/WsiHistory2.csv";
#endif

    stringstream rst;
    rst << "Time,WSI,From,To,Data" << endl;
    {
        mutex_guard guard(WsiMaintain::Instance().m_lock);
        for (const auto& one : WsiMaintain::Instance().m_records) {
            CHECK_TRUE_CONTINUE(one == nullptr)

            rst << "'" << Utils::MillisecondToString(one->time) << ",";
            rst << StringUtils::Format(BUFFER_SIZE_64B, "%p", one->wsi) << ",";
            rst << one->from << ",";
            rst << one->to << ",";
            rst << "\"" << StringUtils::Replace(one->data, "\"", "\"\"") << "\"" << endl;
        }
    }

    const auto content = rst.str();
    FileUtils::WriteTextFile(wsiHistoryPath, content.c_str(), content.size());
}

}