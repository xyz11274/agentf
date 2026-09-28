// c 客户端：通过统一协议收集执行事件并接收策略建议。
// 传输与业务执行分离；接收回执不代表 Agent 已完成整个任务。
// 打点的输入是建议修改前的请求，执行后必须回传实际输入、输出与状态。
// 取消、超时和采集缺口需要显式处理，不能把缺失证据当成正常完成。

#ifndef TRAJECTORY_EVOLUTION_POINTS_CLIENT_H
#define TRAJECTORY_EVOLUTION_POINTS_CLIENT_H

#include "evolution.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct te_local_handle te_local_handle;
typedef struct te_points_handle te_points_handle;

/* 传输回调返回借用的完整 {ok,result} 信封；组件在回调返回前消费它。
 * 返回 NULL 表示 observer_unavailable。 */
typedef const char *(*te_points_transport)(const char *request_json, void *context);
/* 业务结果只借用到 execute 返回；非零返回码保存到 business_code，不能被观测失败替换。 */
typedef int (*te_points_execute)(const char *input_json, void *context, const char **output_json);

typedef struct te_points_call_result {
  char *output_json;
  char *receipt_json;
  int executed;
  int rejected;
  int business_code;
} te_points_call_result;

/* 回调上下文和本地句柄的生命周期必须长于打点句柄；HTTP 构造函数会复制地址、令牌和限制。 */
te_points_handle *te_points_open_callback(te_points_transport transport, void *context);
// 用 HTTP 客户端配置创建打点句柄，复制必要配置而不借用短命对象。
te_points_handle *te_points_open_http(const te_client *client);
// 用本地原生运行时创建打点句柄，共享同一协议和低延迟语义。
te_points_handle *te_points_open_local(te_local_handle *local);
// 销毁打点句柄；宿主必须先停止所有仍持有该句柄的调用。
void te_points_close(te_points_handle *handle);

/* fields_json 是不含 operation/request_id 的 JSON 对象；打点失败时保持业务放行并返回降级结果。
 * 所有返回字符串通过 te_points_free 释放。 */
int te_points_point(te_points_handle *handle, const char *fields_json, char **result_json,
                    const char **error_code);
// 在策略查询与结束记录之间最多执行一次业务回调，保留业务返回码。
int te_points_call(te_points_handle *handle, const char *fields_json, te_points_execute execute,
                   void *context, te_points_call_result *result, const char **error_code);
// 通过 C 接口取得并提交循环守卫结果，不接管宿主循环体。
int te_points_allow_loop(te_points_handle *handle, const char *fields_json, int *allowed,
                         char **receipt_json, const char **error_code);

/* 显式采集缺口与隔离评测入口使用同一传输层；capture_fields 可以提供打点 ref，
 * 或为严格 capture.gap 操作提供 run_id/reason。 */
int te_points_capture_gap(te_points_handle *handle, const char *capture_fields_json,
                          char **receipt_json, const char **error_code);
// 通过 C 接口接收隔离评测下一轮任务，复用用户原有业务入口。
int te_points_evaluation_next(te_points_handle *handle, const char *consumer, size_t index,
                              char **packet_json, const char **error_code);
// 向评测器报告入口失败，只传固定错误信息。
int te_points_evaluation_fail(te_points_handle *handle, const char *consumer, char **result_json,
                              const char **error_code);

// 释放业务包装结果中由本库分配的各缓冲区。
void te_points_call_result_free(te_points_call_result *result);
// 释放打点接口返回的字符串，避免跨分配器释放。
void te_points_free(void *value);

#ifdef __cplusplus
}
#endif
#endif
