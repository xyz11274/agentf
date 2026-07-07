#pragma once

#include "aisf_platform/utils/utils.h"
#include "libwebsockets/libwebsockets.h"

namespace AisfPlat {
class SafeLwsString final {
DISALLOW_COPY_AND_ASSIGN(SafeLwsString)

    const static inline unsigned long long MAGIC = 0xC0FFEE00DEADBEEF;
    const static inline size_t PRE_LEN = sizeof(MAGIC) + LWS_PRE;

public:
    SafeLwsString() = default;

    explicit SafeLwsString(size_t len) { Initial(len); };

    SafeLwsString(const void* content, size_t len) { Write(content, len); };

    ~SafeLwsString() = default;

    bool Write(const void* content, size_t len);

    [[nodiscard]] unsigned char* Data();

    [[nodiscard]] size_t Length() const;

    [[nodiscard]] auto Empty() const { return m_data.Size() <= PRE_LEN; };

    [[nodiscard]] static bool IsSafeLwsString(const void* content);

private:
    bool Initial(size_t len);

private:
    SafeString m_data{__FILE_NAME__, __LINE__};
};

using SafeLwsStringPtr = shared_ptr<SafeLwsString>;
}