#pragma once

#include <string>
#include "aisf_platform/common_define.h"

namespace AisfPlat {
struct SafeStringStat {
    size_t size{0};
    size_t mallocCnt{0};
    size_t freeCnt{0};
};

class SafeString final { // 支持并发申请内存，但是不支持并发调用读写接口
DISALLOW_COPY_AND_ASSIGN(SafeString)

private:
    using Tail = unsigned long long;
    using Head = unsigned long long;

    const static Head HEAD_MAGIC = 0xC0FFEE01C0FFEE02;
    const static int HEAD_LENGTH = sizeof(Head);

    const static Tail TAIL_MAGIC = 0xC0FFEE03C0FFEE04;
    const static int TAIL_LENGTH = sizeof(Tail);
    const static int EMPTY_LENGTH = 1;

public:
    SafeString(string func, int line);

    SafeString(string func, int line, size_t len, bool safe = false);

    ~SafeString();

public:
    // 交换两个SafeString对象的值
    void Swap(SafeString& str);

    // 重置内部内存的长度，默认为非安全模式
    void Reset(size_t newLen, bool isSafe = false);

    // 设置为安全模式，内存释放前，将会写零，用于清理敏感数据
    void SetToSafe();

    // 获取当前安全状态
    [[nodiscard]] bool Safe() const;

    // 收缩内存到指定的长度，最后一个字符填写0，用于适配字符串
    void ShrinkLength(size_t smallerLen);

    // 将内部内存清零
    void Clear();

    // 获取内部内存的可写指针，注意：通过此方式写数据，length不会增加
    char* Get();

    // 获取内部内存的可用长度
    [[nodiscard]] size_t Length() const;

    // 获取内部内存的不可写指针,无数据时返回空串,不会返回空指针
    [[nodiscard]] const char* Data() const;

    // 获取内部内存的可用长度
    [[nodiscard]] size_t Size() const;

    // 获取内部内存是否已经申请
    [[nodiscard]] bool Empty() const;

    // 向内部内存写数据，内部长度length相应的增加
    bool Write(const char* d, size_t len);

    // 获取内部内存的总长度，包括头和尾的魔术字长度
    static size_t TotalLength();

    // 获取SafeString类的申请统计，用于维测
    static auto GetMemoryStat();

private:
    void MemoryStat(bool isAdd);

    void DoMalloc();

    void Free();

    // 监测头和尾，防止写超，要记录日志和报告文件
    void Check() const;

private:
    const string funcName{"null"};
    const int lineNo{0};

    Head* headPos{nullptr};
    Tail* tailPos{nullptr};

    char* freeData{nullptr};
    char* data{nullptr};

    size_t length{0};

    bool safeDestroy{false};

    static atomic_ullong totalLength;
    static mutex statLock;
    static unordered_map<string, unordered_map<int, SafeStringStat>> statistics;
};

using SafeStringPtr = shared_ptr<SafeString>;
}
