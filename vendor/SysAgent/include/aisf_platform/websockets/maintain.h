#pragma once

#include "aisf_platform/utils/utils.h"
#include "aisf_platform/websockets/define.h"

namespace AisfPlat {
class WsiMaintain final {
DECLARE_SINGLETON(WsiMaintain)

public:
    struct Record {
        string from;
        string to;
        long time{Utils::GetTimeMs()};
        const lws* wsi{nullptr};
        string data;

        Record(string f, string t, const lws* w, const void* d, size_t len) :
            from(std::move(f)), to(std::move(t)), wsi(w)
        {
            CHECK_NULL_RETURN_VOID(d);
            data = string((const char*) d, len);
        }
    };

public:
    static void Initial();

    void AddRecord(const string& from, const string& to, const lws* wsi, const void* data, size_t len);

private:
    static void Report();

private:
    mutex m_lock{};

    bool m_isWsiHistoryEnable{true};

    const size_t MAX_DATA_INFO_NUM = 1024;
    deque<shared_ptr<Record>> m_records{};
};
}