// c 客户端：通过统一协议收集执行事件并接收策略建议。
// 传输与业务执行分离；接收回执不代表 Agent 已完成整个任务。
// 打点的输入是建议修改前的请求，执行后必须回传实际输入、输出与状态。
// 取消、超时和采集缺口需要显式处理，不能把缺失证据当成正常完成。

#ifndef TRAJECTORY_EVOLUTION_HTTP_CLIENT_H
#define TRAJECTORY_EVOLUTION_HTTP_CLIENT_H
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif

/* 在线程启动前只初始化一次；要求 libcurl 不低于 7.62。 */
int te_initialize(void);
// 释放进程级网络资源，应在所有客户端调用完成后执行。
void te_cleanup(void);

typedef struct te_client {
  const char *url;           /* 使用 HTTPS 源地址；HTTP 仅允许回环地址。 */
  const char *token;         /* 借用的凭据指针；绝不能写入 JSON。 */
  long timeout_ms;           /* 取值 1..300000，包含数据传输耗时。 */
  size_t max_response_bytes; /* 取值 1024..16777216，通常使用 2097152。 */
  unsigned retries;          /* 取值 0..3；重试只能复用完全相同的请求字节。 */
} te_client;

/* request_json 必须含 operation 和持久化 request_id，可用调用方的 JSON 库构造。
 * 成功返回完整的 {ok,result} 信封，由调用方通过 te_free 释放。
 * 失败只返回固定错误码，不回传原始网络或 HTTP 异常正文。
 * 不可变客户端可被多个线程共享，每次调用的输出独立持有。 */
int te_invoke(const te_client *client, const char *request_json, char **response_json,
              long *http_status, const char **error_code);
// 释放由本库返回的字符串，不能使用调用方不匹配的分配器。
void te_free(void *value);
#ifdef __cplusplus
}
#endif
#endif
