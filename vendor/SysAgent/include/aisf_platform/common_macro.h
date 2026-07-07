#pragma once

namespace AisfPlat {

#define CHECK_NULL_LOG_ERROR_RETURN(ptr, ret, format, args...)                      \
    do {                                                                            \
        if (ptr == nullptr) {                                                       \
            LOG_ERR("[%s] " #ptr " is nullptr msg:" #format, __FUNCTION__, ##args); \
            return ret;                                                             \
        }                                                                           \
    } while (0)

#define CHECK_NULL_LOG_ERROR_RETURN_VOID(ptr, format, args...)                      \
    do {                                                                            \
        if (ptr == nullptr) {                                                       \
            LOG_ERR("[%s] " #ptr " is nullptr msg:" #format, __FUNCTION__, ##args); \
            return;                                                                 \
        }                                                                           \
    } while (0)

#define CHECK_NULL_LOG_ERROR_CONTINUE(ptr, format, args...)                     \
    if (ptr == nullptr) {                                                       \
        LOG_ERR("[%s] " #ptr " is nullptr msg:" #format, __FUNCTION__, ##args); \
        continue;                                                               \
    }

#define CHECK_TRUE_CONTINUE(isTrue) if (isTrue) { \
        continue;                                 \
    }

#define CHECK_FALSE_CONTINUE(isFalse) if (!(isFalse)) { \
        continue;                                       \
    }

#define CHECK_NULL_RETURN(ptr, ret) do { \
        if (ptr == nullptr) {            \
            return ret;                  \
        }                                \
    } while (0)

#define CHECK_NULL_RETURN_VOID(ptr) do { \
        if (ptr == nullptr) {            \
            return;                      \
        }                                \
    } while (0)

#define CHECK_TRUE_LOG_ERROR_RETURN(isTrue, ret, format, args...)     \
    do {                                                              \
        if (isTrue) {                                                 \
            LOG_ERR("[%s] error msg:" #format, __FUNCTION__, ##args); \
            return ret;                                               \
        }                                                             \
    } while (0)

#define CHECK_TRUE_RETURN(isTrue, ret) do { \
        if (isTrue) {                       \
            return ret;                     \
        }                                   \
    } while (0)

#define CHECK_TRUE_LOG_ERROR_RETURN_VOID(isTrue, format, args...)     \
    do {                                                              \
        if (isTrue) {                                                 \
            LOG_ERR("[%s] error msg:" #format, __FUNCTION__, ##args); \
            return;                                                   \
        }                                                             \
    } while (0)

#define CHECK_TRUE_RETURN_VOID(isTrue) do { \
        if (isTrue) {                       \
            return;                         \
        }                                   \
    } while (0)

#define CHECK_TRUE_LOG_ERROR(isTrue, format, args...)                 \
    do {                                                              \
        if (isTrue) {                                                 \
            LOG_ERR("[%s] error msg:" #format, __FUNCTION__, ##args); \
        }                                                             \
    } while (0)

#define CHECK_FALSE_LOG_ERROR_RETURN(isFalse, ret, format, args...)   \
    do {                                                              \
        if (!(isFalse)) {                                             \
            LOG_ERR("[%s] error msg:" #format, __FUNCTION__, ##args); \
            return ret;                                               \
        }                                                             \
    } while (0)

#define CHECK_FALSE_RETURN(isFalse, ret) do { \
        if (!(isFalse)) {                     \
            return ret;                       \
        }                                     \
    } while (0)

#define CHECK_FALSE_LOG_ERROR_RETURN_VOID(isFalse, format, args...)   \
    do {                                                              \
        if (!(isFalse)) {                                             \
            LOG_ERR("[%s] error msg:" #format, __FUNCTION__, ##args); \
            return;                                                   \
        }                                                             \
    } while (0)

#define CHECK_FALSE_RETURN_VOID(isFalse) do { \
        if (!(isFalse)) {                     \
            return;                           \
        }                                     \
    } while (0)

#define CHECK_FALSE_LOG_ERROR(isFalse, format, args...)               \
    do {                                                              \
        if (!(isFalse)) {                                             \
            LOG_ERR("[%s] error msg:" #format, __FUNCTION__, ##args); \
        }                                                             \
    } while (0)

#define BOOL2STR(b) ((b) ? "true" : "false")

#define DISALLOW_COPY_AND_ASSIGN(TypeName) public:       \
    TypeName(const TypeName & s) = delete;               \
    TypeName &operator = (const TypeName & s) = delete;

#define DECLARE_SINGLETON_NO_CONSTRUCT(TypeName) DISALLOW_COPY_AND_ASSIGN(TypeName) \
    static TypeName& Instance() { static TypeName instance; return instance; };     \
private:                                                                            \
    ~TypeName() = default;

#define DECLARE_SINGLETON(TypeName) DECLARE_SINGLETON_NO_CONSTRUCT(TypeName) \
    TypeName() = default;

#define ENDIAN_SWAP_16(A) ((((unsigned short)(A)&0xff00u) >> 8u) | (((unsigned short)(A)&0x00ffu) << 8u))

#define ENDIAN_SWAP_32(A) ((((unsigned int)(A)&0xff000000u) >> 24u) | (((unsigned int)(A)&0x00ff0000u) >> 8u) | \
        (((unsigned int)(A)&0x0000ff00u) << 8u) | (((unsigned int)(A)&0x000000ffu) << 24u))

#define SAFE_DIVIDE(dividend, divisor) (((divisor) == 0) ? 0 : (dividend) / (divisor))

}