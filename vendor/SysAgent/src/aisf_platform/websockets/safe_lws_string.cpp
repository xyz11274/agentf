#include "aisf_platform/websockets/safe_lws_string.h"

namespace AisfPlat {

bool SafeLwsString::Write(const void* content, size_t len)
{
    CHECK_NULL_RETURN(content, false);
    CHECK_FALSE_RETURN(Initial(len), false);
    CHECK_FALSE_LOG_ERROR(memcpy_s(m_data.Get() + PRE_LEN, m_data.Size() - PRE_LEN, content, len) == EOK,
                          "memcpy_s error");
    return true;
}

bool SafeLwsString::Initial(size_t len)
{
    CHECK_TRUE_RETURN(len == 0, false);

    m_data.Reset(PRE_LEN + len);
    CHECK_TRUE_RETURN(m_data.Empty(), false);

    auto& magic = *(unsigned long long*) m_data.Get();
    magic = MAGIC; // 写魔术字
    return true;
}

[[nodiscard]] unsigned char* SafeLwsString::Data()
{
    CHECK_TRUE_RETURN(Empty(), nullptr);
    return (unsigned char*) (m_data.Get() + PRE_LEN);
}

[[nodiscard]] size_t SafeLwsString::Length() const
{
    CHECK_TRUE_RETURN(Empty(), 0);
    return m_data.Size() - PRE_LEN;
}

bool SafeLwsString::IsSafeLwsString(const void* content)
{
    CHECK_NULL_RETURN(content, false);
    CHECK_TRUE_RETURN((uintptr_t) content <= (uintptr_t) PRE_LEN, false); // 防止减反转

    auto& magic = *(const unsigned long long*) ((const char*) content - PRE_LEN);
    return magic == MAGIC; // 魔术字对上了
}
}