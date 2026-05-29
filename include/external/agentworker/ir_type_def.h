#ifndef MMAGENTGRAPHENGINE_IR_TYPE_DEF_H
#define MMAGENTGRAPHENGINE_IR_TYPE_DEF_H

#include <string>
#include <any>
#include <vector>
#include <thread>
#include <map>

namespace mmagentgraphengine {
    enum class ErrorCode {
        SUCCESS,                // 成功
        INVALID_ARGUMENT,       // 无效参数
        INVALID_VALUE,          // 无效取值
        INVALID_FILE,           // 无效文件
        INVALID_STATE,          // 无效状态
        INVALID_OPERATION,      // 无效操作
        FILE_NOT_FOUND,         // 文件不存在
        FLOW_NOT_FOUND,         // 流程不存在
        FLOW_UNAVAILABLE,       // 流程不可用
        MEMBER_NOT_FOUND,       // 成员不存在
        PERMISSION_DENIED,      // 权限不足
        TIMEOUT,                // 超时
        UNINITIALIZED,          // 未初始化
        PARSE_ERROR,            // 解析失败
        UNKNOWN_ERROR,          // 未知错误
        TYPE_ERROR,             // 类型错误
        PROCESS_BUSY,           // 程序繁忙
        COMPONENT_RUN_FAIL,     // Component执行失败
        COMPONENT_LOAD_FAIL,    // Component加载失败
        BUTT,
        UNSUPPORTED_ERROR       // 不支持
    };

    enum class ValueType {
        STRING,     // 字符串
        INT,        // 有符号整型
        FLOAT,      // 特指Double类型
        BOOLEAN,    // bool
        LIST,       // 列表
        MAP,        // 字典
        BUTT
    };

    enum class LogLevel {
        DEBUG,
        INFO,
        WARNING,
        ERROR,
    };

    struct Value {
        std::any value;
        ValueType valueType;
    };

    class Context {
    public:
        // 日志打印
        virtual void Log(LogLevel level, std::string file, int line, std::string fmt, ...) = 0;
        // 打点
        virtual void Dot(std::string kpiName,
                         std::string traceId,
                         std::string operation,
                         uint64_t duration,
                         std::string extraInfo) = 0;

        // 获取KpiName，kpiName由业务侧定义并注册
        virtual std::string GetKpiName() = 0;

        // 获取TaskId
        virtual std::string GetTaskId() = 0;

        // 获取请求Id
        virtual int64_t GetRequestId() = 0;

        virtual ~Context() = default;
    };

    using ValueList = std::vector<Value>;
    using ValueMap = std::map<std::string, Value>;
}

#endif // MMAGENTGRAPHENGINE_IR_TYPE_DEF_H