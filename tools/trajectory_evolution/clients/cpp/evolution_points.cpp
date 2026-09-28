// cpp 客户端：通过统一协议收集执行事件并接收策略建议。
// 传输与业务执行分离；接收回执不代表 Agent 已完成整个任务。
// 打点的输入是建议修改前的请求，执行后必须回传实际输入、输出与状态。
// 取消、超时和采集缺口需要显式处理，不能把缺失证据当成正常完成。

#include "evolution_points.hpp"
#include "../c/evolution_points.h"

#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>

using trajectory_evolution_http::Error;
using trajectory_evolution_http::Points;

struct te_points_handle {
  std::unique_ptr<Points> points;
};

namespace {
thread_local char last_error[97];

// 复制跨接口传递的数据，避免返回值引用即将释放的临时缓冲区。
char *copy(const std::string &value) {
  auto *result = static_cast<char *>(std::malloc(value.size() + 1));
  if (!result)
    return nullptr;
  std::memcpy(result, value.c_str(), value.size() + 1);
  return result;
}

// 构造固定错误结果，避免把输入、凭据或原始异常放入诊断。
int failed(const char *code, const char **error) {
  bool valid = code != nullptr;
  std::size_t size = 0;
  if (valid)
    for (; code[size] != '\0' && size <= 96; ++size) {
      const auto c = static_cast<unsigned char>(code[size]);
      if (!((c >= 'a' && c <= 'z') || c == '_')) {
        valid = false;
        break;
      }
    }
  valid = valid && size > 0 && size <= 96 && code[size] == '\0';
  const char *value = valid ? code : "client_failed";
  size = std::strlen(value);
  std::memcpy(last_error, value, size);
  last_error[size] = '\0';
  if (error)
    *error = last_error;
  return -1;
}

// 构造固定错误结果，避免把输入、凭据或原始异常放入诊断。
int failed(const std::exception &error, const char **code) {
  return failed(error.what(), code);
}

// 复制或提取当前调用输出，保持跨语言边界的所有权约定。
int output(const std::string &value, char **result, const char **error) {
  if (!result)
    return failed("output_required", error);
  *result = copy(value);
  return *result ? 0 : failed("allocation_failed", error);
}

struct BusinessFailure {
  int code;
};
} // 结束当前匿名命名空间。

extern "C" {

// 用现有传输回调创建打点句柄，不要求宿主实现框架专用适配文件。
te_points_handle *te_points_open_callback(te_points_transport transport, void *context) {
  if (!transport)
    return nullptr;
  try {
    auto handle = std::make_unique<te_points_handle>();
    handle->points = std::make_unique<Points>([transport, context](const std::string &request) {
      const char *result = transport(request.c_str(), context);
      if (!result)
        throw Error("observer_unavailable", 0);
      return std::string(result);
    });
    return handle.release();
  } catch (...) {
    return nullptr;
  }
}

// 用 HTTP 客户端配置创建打点句柄，复制必要配置而不借用短命对象。
te_points_handle *te_points_open_http(const te_client *client) {
  if (!client || !client->url || !client->token)
    return nullptr;
  try {
    auto handle = std::make_unique<te_points_handle>();
    handle->points = std::make_unique<Points>(Points::http(*client));
    return handle.release();
  } catch (...) {
    return nullptr;
  }
}

// 用本地原生运行时创建打点句柄，共享同一协议和低延迟语义。
te_points_handle *te_points_open_local(te_local_handle *local) {
  if (!local)
    return nullptr;
  try {
    auto handle = std::make_unique<te_points_handle>();
    handle->points = std::make_unique<Points>(Points::local(local));
    return handle.release();
  } catch (...) {
    return nullptr;
  }
}

// 销毁打点句柄；宿主必须先停止所有仍持有该句柄的调用。
void te_points_close(te_points_handle *handle) {
  delete handle;
}

// 通过 C 接口提交单个打点，返回字符串须由匹配的释放接口回收。
int te_points_point(te_points_handle *handle, const char *fields, char **result,
                    const char **error) {
  if (result)
    *result = nullptr;
  if (error)
    *error = nullptr;
  if (!handle || !handle->points || !fields)
    return failed("client_invalid", error);
  try {
    return output(handle->points->point(fields), result, error);
  } catch (const std::exception &failure) {
    return failed(failure, error);
  } catch (...) {
    return failed("client_failed", error);
  }
}

// 在策略查询与结束记录之间最多执行一次业务回调，保留业务返回码。
int te_points_call(te_points_handle *handle, const char *fields, te_points_execute execute,
                   void *context, te_points_call_result *result, const char **error) {
  if (error)
    *error = nullptr;
  if (!result)
    return failed("output_required", error);
  std::memset(result, 0, sizeof(*result));
  if (!handle || !handle->points || !fields || !execute)
    return failed("client_invalid", error);
  int business_code = 0;
  std::string abort_receipt;
  try {
    const auto called = handle->points->call(
        fields,
        [&](const std::string &input) {
          const char *business_output = nullptr;
          business_code = execute(input.c_str(), context, &business_output);
          if (business_code != 0)
            throw BusinessFailure{business_code};
          if (!business_output)
            throw Error("business_output_invalid", 0);
          return std::string(business_output);
        },
        &abort_receipt);
    result->output_json = copy(called.output);
    result->receipt_json = copy(called.receipt);
    if (!result->output_json || !result->receipt_json) {
      te_points_call_result_free(result);
      return failed("allocation_failed", error);
    }
    result->executed = called.executed;
    result->rejected = called.rejected;
    return 0;
  } catch (const BusinessFailure &failure) {
    result->business_code = failure.code;
    result->executed = 1;
    result->receipt_json = copy(abort_receipt);
    if (!result->receipt_json)
      return failed("allocation_failed", error);
    return 0;
  } catch (const std::exception &failure) {
    return failed(failure, error);
  } catch (...) {
    return failed("client_failed", error);
  }
}

// 通过 C 接口取得并提交循环守卫结果，不接管宿主循环体。
int te_points_allow_loop(te_points_handle *handle, const char *fields, int *allowed, char **receipt,
                         const char **error) {
  if (receipt)
    *receipt = nullptr;
  if (error)
    *error = nullptr;
  if (!handle || !handle->points || !fields || !allowed)
    return failed("client_invalid", error);
  try {
    const auto loop = handle->points->allow_loop(fields);
    *allowed = loop.allowed ? 1 : 0;
    return output(loop.receipt, receipt, error);
  } catch (const std::exception &failure) {
    return failed(failure, error);
  } catch (...) {
    return failed("client_failed", error);
  }
}

// 记录显式采集缺口，避免不完整运行作为学习样本。
int te_points_capture_gap(te_points_handle *handle, const char *fields, char **receipt,
                          const char **error) {
  if (receipt)
    *receipt = nullptr;
  if (error)
    *error = nullptr;
  if (!handle || !handle->points || !fields)
    return failed("client_invalid", error);
  try {
    return output(handle->points->capture_gap(fields), receipt, error);
  } catch (const std::exception &failure) {
    return failed(failure, error);
  } catch (...) {
    return failed("client_failed", error);
  }
}

// 通过 C 接口接收隔离评测下一轮任务，复用用户原有业务入口。
int te_points_evaluation_next(te_points_handle *handle, const char *consumer, size_t index,
                              char **packet, const char **error) {
  if (packet)
    *packet = nullptr;
  if (error)
    *error = nullptr;
  if (!handle || !handle->points || !consumer)
    return failed("client_invalid", error);
  try {
    return output(handle->points->evaluation_next(consumer, index), packet, error);
  } catch (const std::exception &failure) {
    return failed(failure, error);
  } catch (...) {
    return failed("client_failed", error);
  }
}

// 向评测器报告入口失败，只传固定错误信息。
int te_points_evaluation_fail(te_points_handle *handle, const char *consumer, char **result,
                              const char **error) {
  if (result)
    *result = nullptr;
  if (error)
    *error = nullptr;
  if (!handle || !handle->points || !consumer)
    return failed("client_invalid", error);
  try {
    return output(handle->points->evaluation_fail(consumer), result, error);
  } catch (const std::exception &failure) {
    return failed(failure, error);
  } catch (...) {
    return failed("client_failed", error);
  }
}

// 释放业务包装结果中由本库分配的各缓冲区。
void te_points_call_result_free(te_points_call_result *result) {
  if (!result)
    return;
  std::free(result->output_json);
  std::free(result->receipt_json);
  std::memset(result, 0, sizeof(*result));
}

// 释放打点接口返回的字符串，避免跨分配器释放。
void te_points_free(void *value) {
  std::free(value);
}

} // 结束 C 语言链接边界。
