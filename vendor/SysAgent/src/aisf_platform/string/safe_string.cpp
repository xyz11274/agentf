#include "aisf_platform/utils/utils.h"

namespace AisfPlat {
using namespace std;

atomic_ullong SafeString::totalLength{0};
std::mutex SafeString::statLock;
unordered_map<string, unordered_map<int, SafeStringStat>> SafeString::statistics;

SafeString::SafeString(string func, int line) : funcName(std::move(func)), lineNo(line), length(0), safeDestroy(false)
{
}

SafeString::SafeString(string func, int line, size_t len, bool safe)
    : funcName(std::move(func)), lineNo(line), length(len), safeDestroy(safe)
{
    CHECK_TRUE_RETURN_VOID(length == 0);
    DoMalloc();
}

SafeString::~SafeString()
{
    Free();
}

void SafeString::Swap(SafeString& str)
{
    std::swap(headPos, str.headPos);
    std::swap(tailPos, str.tailPos);
    std::swap(freeData, str.freeData);
    std::swap(data, str.data);
    std::swap(length, str.length);
    std::swap(safeDestroy, str.safeDestroy);
}

void SafeString::Reset(size_t newLen, bool isSafe)
{
    Free();

    length = newLen;
    safeDestroy = isSafe;

    CHECK_TRUE_RETURN_VOID(length == 0);
    DoMalloc();
}

void SafeString::SetToSafe()
{
    safeDestroy = true;
}

bool SafeString::Safe() const
{
    return safeDestroy;
}

// 收缩长度，最后一个字符填写0，适配字符串，但是内存实际长度不变
void SafeString::ShrinkLength(size_t smallerLen)
{
    Check();

    CHECK_TRUE_RETURN_VOID(smallerLen >= length);

    data[smallerLen] = 0;
    length = smallerLen;
}

void SafeString::Clear()
{
    CHECK_TRUE_RETURN_VOID(Empty());

    auto iSecFuncRet = memset_s(data, length + EMPTY_LENGTH, 0, length + EMPTY_LENGTH);
    if (iSecFuncRet != EOK) {
        return;
    }
}

char* SafeString::Get()
{
    Check();
    return data;
}

size_t SafeString::Length() const
{
    Check();
    return length;
}

const char* SafeString::Data() const
{
    Check();
    if (data == nullptr) {
        return "";
    }
    return data;
}

size_t SafeString::Size() const
{
    Check();
    return length;
}

bool SafeString::Empty() const
{
    return data == nullptr || length == 0;
}

bool SafeString::Write(const char* d, size_t len)
{
    CHECK_TRUE_RETURN(((d == nullptr || len == 0)), false);
    CHECK_TRUE_RETURN(len > length, false);

    return memcpy_s(data, length, d, len) == EOK;
}

size_t SafeString::TotalLength()
{
    return totalLength;
}

auto SafeString::GetMemoryStat()
{
    mutex_guard guard(statLock);
    return statistics;
}

void SafeString::MemoryStat(bool isAdd)
{
    auto capacity = (HEAD_LENGTH + length + EMPTY_LENGTH + TAIL_LENGTH);

    mutex_guard guard(statLock);

    auto funcItr = statistics.find(funcName);
    if (funcItr == statistics.end()) {
        statistics[funcName] = unordered_map<int, SafeStringStat>();
        funcItr = statistics.find(funcName);
    }

    auto sizeItr = funcItr->second.find(lineNo);
    if (sizeItr == funcItr->second.end()) {
        funcItr->second[lineNo] = SafeStringStat{};
        sizeItr = funcItr->second.find(lineNo);
    }

    auto& s = sizeItr->second;
    if (isAdd) {
        s.size += capacity;
        s.mallocCnt++;
    } else {
        s.size = ((s.size < capacity) ? 0 : (s.size - capacity));
        s.freeCnt++;
    }
}

void SafeString::DoMalloc()
{
    freeData = (char*) malloc(HEAD_LENGTH + length + EMPTY_LENGTH + TAIL_LENGTH);
    CHECK_NULL_RETURN_VOID(freeData);
    data = freeData + HEAD_LENGTH;
    totalLength += (HEAD_LENGTH + length + EMPTY_LENGTH + TAIL_LENGTH);

    MemoryStat(true);
    Clear();

    headPos = (Head*) freeData;
    *headPos = HEAD_MAGIC; // 头部写一个魔术字，防止被覆写

    tailPos = (Tail*) (data + length + EMPTY_LENGTH);
    *tailPos = TAIL_MAGIC; // 尾巴写一个魔术字，防止写超
}

void SafeString::Free()
{
    CHECK_NULL_RETURN_VOID(freeData);

    MemoryStat(false);
    Check();

    if (safeDestroy) {
        Clear();
    }

    totalLength -= (HEAD_LENGTH + length + EMPTY_LENGTH + TAIL_LENGTH);

    free(freeData);
    freeData = nullptr;
    data = nullptr;

    safeDestroy = false;
    length = 0;
    headPos = nullptr;
    tailPos = nullptr;
}


// 监测头和尾，防止写超，要记录日志和报告文件
void SafeString::Check() const
{
    CHECK_NULL_RETURN_VOID(freeData);

    if (tailPos != nullptr) {
        if (*tailPos != TAIL_MAGIC) {
            PRINT_AND_LOG_ERR("SafeString _stamp_tail");
        }
    }

    if (headPos != nullptr) {
        if (*headPos != HEAD_MAGIC) {
            PRINT_AND_LOG_ERR("SafeString _stamp_head");
        }
    }
}
}
