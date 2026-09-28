// 稳定 C ABI：为任意语言提供进程内调用、状态查询和资源释放接口。
// 跨语言分配的结果必须通过配套释放函数归还；异常不能越过 C 边界。
// 公开协议使用 UTF-8 JSON，调用方不依赖 C++ 对象布局。
// 此处是对外声明，具体实现位于 src；修改接口时需考虑现有调用方与持久化版本。

#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 任何支持原生互操作的语言均可使用稳定 UTF-8 JSON C 接口。创建和关闭仅用于启动/收尾；invoke 不等待外部 I/O。所有返回字符串由本库分配，使用 te_local_free 释放。 */
typedef struct te_local_handle te_local_handle;
// 初始化本地运行时句柄，加载配置并建立后台工作资源。
te_local_handle *te_local_open(const char *config, const char *bearer, const char *options_json,
                               char **error);
// 执行有界本地热路径请求，不等待远程模型或磁盘评测完成。
char *te_local_invoke(te_local_handle *handle, const char *request, size_t length);
/* 独立进程使用相同打点信封；timeout_ms=0 表示 20 毫秒，仅尝试一次、不重试。失败返回原始输入和降级上下文；完整 {ok,result} 信封使用 te_local_free 释放。 */
char *te_remote_point(const char *origin, const char *bearer, const char *request, size_t length,
                      unsigned timeout_ms);
/* 只供阻塞式管理使用，不能放在 Agent 执行路径。 */
char *te_local_manage(te_local_handle *handle, const char *request, size_t length);
// 返回本地运行时的队列与采集指标，供运维观察。
char *te_local_metrics(te_local_handle *handle);
// 启停本地附加能力，保留宿主独立运行的默认行为。
void te_local_set_enabled(te_local_handle *handle, int enabled);
// 等待本地待持久化工作完成，仅用于维护和收尾。
void te_local_drain(te_local_handle *handle);
// 关闭本地运行时并释放句柄，调用方须保证不再发起新调用。
void te_local_close(te_local_handle *handle);
// 释放本地库返回的字符串，保证分配与释放来自同一库。
void te_local_free(char *value);

#ifdef __cplusplus
}
#endif
