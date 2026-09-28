// Node 原生桥接：把 JavaScript 请求与底层 C ABI 互相转换。
// 注意宿主运行时和原生句柄的生命周期，错误通过语言边界安全返回。
// 此文件只负责绑定，不重复实现策略算法或记忆管理。

#include "trajectory_evolution/local_c.h"
#include <node_api.h>
#include <memory>
#include <string>
#include <vector>

namespace {
struct Handle {
  te_local_handle *value = nullptr;

  // 销毁对象时释放本类持有的资源；收尾不能抛出业务异常，也不能继续接收新请求。
  ~Handle() {
    te_local_close(value);
  }
};

struct State {
  std::vector<std::unique_ptr<Handle>> handles;
};

// 读取或检查字符串表示，保持接口要求的类型和编码约束。
bool string(napi_env env, napi_value value, std::string &result, size_t limit) {
  size_t size = 0;
  if (napi_get_value_string_utf8(env, value, nullptr, 0, &size) != napi_ok || size > limit)
    return false;
  std::vector<char> bytes(size + 1);
  if (napi_get_value_string_utf8(env, value, bytes.data(), bytes.size(), &size) != napi_ok)
    return false;
  result.assign(bytes.data(), size);
  return true;
}

// 返回或提交固定失败诊断，不把业务正文或凭据放入错误消息。
napi_value fail(napi_env env) {
  napi_throw_error(env, nullptr, "local_native_error");
  return nullptr;
}

// 读取经过类型检查的字符串值，拒绝其他类型隐式转换。
napi_value text(napi_env env, char *raw) {
  if (!raw)
    return fail(env);
  napi_value value;
  const auto status = napi_create_string_utf8(env, raw, NAPI_AUTO_LENGTH, &value);
  te_local_free(raw);
  return status == napi_ok ? value : fail(env);
}

// 创建当前接口需要的资源，失败路径释放已成功创建的部分。
napi_value open(napi_env env, napi_callback_info info) {
  try {
    size_t count = 3;
    napi_value args[3];
    void *context;
    if (napi_get_cb_info(env, info, &count, args, nullptr, &context) != napi_ok || count != 3)
      return fail(env);
    std::string config, token, options;
    if (!string(env, args[0], config, 4096) || !string(env, args[1], token, 4096) ||
        !string(env, args[2], options, 4096))
      return fail(env);
    auto handle = std::make_unique<Handle>();
    char *error = nullptr;
    handle->value = te_local_open(config.c_str(), token.c_str(), options.c_str(), &error);
    te_local_free(error);
    if (!handle->value)
      return fail(env);
    napi_value result;
    if (napi_create_external(env, handle.get(), nullptr, nullptr, &result) != napi_ok)
      return fail(env);
    static_cast<State *>(context)->handles.push_back(std::move(handle));
    return result;
  } catch (...) {
    return fail(env);
  }
}

// 包装宿主业务调用：先查询策略，再最多执行一次原业务函数，最后记录结果。
napi_value call(napi_env env, napi_callback_info info) {
  try {
    size_t count = 2;
    napi_value args[2];
    void *operation;
    if (napi_get_cb_info(env, info, &count, args, nullptr, &operation) != napi_ok || !count)
      return fail(env);
    void *pointer = nullptr;
    if (napi_get_value_external(env, args[0], &pointer) != napi_ok || !pointer)
      return fail(env);
    auto *handle = static_cast<Handle *>(pointer);
    const std::string op = static_cast<const char *>(operation);
    if (op == "close") {
      te_local_close(handle->value);
      handle->value = nullptr;
    } else if (op == "drain")
      te_local_drain(handle->value);
    else if (op == "enabled") {
      bool enabled;
      if (count != 2 || napi_get_value_bool(env, args[1], &enabled) != napi_ok)
        return fail(env);
      te_local_set_enabled(handle->value, enabled);
    } else if (op == "metrics")
      return text(env, te_local_metrics(handle->value));
    else {
      std::string request;
      if (count != 2 || !string(env, args[1], request, 1048576)) {
        char *ignored = te_local_invoke(handle->value, "", 0);
        te_local_free(ignored);
        return fail(env);
      }
      return text(env, te_local_invoke(handle->value, request.data(), request.size()));
    }
    napi_value result;
    napi_get_undefined(env, &result);
    return result;
  } catch (...) {
    return fail(env);
  }
}

// 注册本地模块导出函数，保持原生接口名称与类型一致。
napi_value init(napi_env env, napi_value exports) {
  auto *state = new State;
  napi_set_instance_data(
      env, state, [](napi_env, void *data, void *) { delete static_cast<State *>(data); }, nullptr);
  napi_property_descriptor methods[] = {
      {"open", nullptr, open, nullptr, nullptr, nullptr, napi_default, state},
      {"invoke", nullptr, call, nullptr, nullptr, nullptr, napi_default,
       const_cast<char *>("invoke")},
      {"metrics", nullptr, call, nullptr, nullptr, nullptr, napi_default,
       const_cast<char *>("metrics")},
      {"setEnabled", nullptr, call, nullptr, nullptr, nullptr, napi_default,
       const_cast<char *>("enabled")},
      {"drain", nullptr, call, nullptr, nullptr, nullptr, napi_default,
       const_cast<char *>("drain")},
      {"close", nullptr, call, nullptr, nullptr, nullptr, napi_default,
       const_cast<char *>("close")}};
  napi_define_properties(env, exports, sizeof(methods) / sizeof(methods[0]), methods);
  return exports;
}
}
NAPI_MODULE(NODE_GYP_MODULE_NAME, init)
