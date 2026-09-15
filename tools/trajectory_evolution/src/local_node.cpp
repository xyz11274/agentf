#include "trajectory_evolution/local_c.h"
#include <node_api.h>
#include <memory>
#include <string>
#include <vector>

namespace {
struct Handle {
  te_local_handle* value = nullptr;
  ~Handle() { te_local_close(value); }
};
struct State {
  std::vector<std::unique_ptr<Handle>> handles;
};
bool string(napi_env env, napi_value value, std::string& result, size_t limit) {
  size_t size = 0;
  if(napi_get_value_string_utf8(env, value, nullptr, 0, &size) != napi_ok || size > limit) return false;
  std::vector<char> bytes(size + 1);
  if(napi_get_value_string_utf8(env, value, bytes.data(), bytes.size(), &size) != napi_ok) return false;
  result.assign(bytes.data(), size); return true;
}
napi_value fail(napi_env env) { napi_throw_error(env, nullptr, "local_native_error"); return nullptr; }
napi_value text(napi_env env, char* raw) {
  if(!raw) return fail(env);
  napi_value value; const auto status = napi_create_string_utf8(env, raw, NAPI_AUTO_LENGTH, &value);
  te_local_free(raw); return status == napi_ok ? value : fail(env);
}
napi_value open(napi_env env, napi_callback_info info) {
  try {
    size_t count = 3; napi_value args[3]; void* context;
    if(napi_get_cb_info(env, info, &count, args, nullptr, &context) != napi_ok || count != 3) return fail(env);
    std::string config, token, options;
    if(!string(env, args[0], config, 4096) || !string(env, args[1], token, 4096) || !string(env, args[2], options, 4096)) return fail(env);
    auto handle = std::make_unique<Handle>(); char* error = nullptr;
    handle->value = te_local_open(config.c_str(), token.c_str(), options.c_str(), &error); te_local_free(error);
    if(!handle->value) return fail(env);
    napi_value result;
    if(napi_create_external(env, handle.get(), nullptr, nullptr, &result) != napi_ok) return fail(env);
    static_cast<State*>(context)->handles.push_back(std::move(handle)); return result;
  } catch(...) { return fail(env); }
}
napi_value call(napi_env env, napi_callback_info info) {
  try {
    size_t count = 2; napi_value args[2]; void* operation;
    if(napi_get_cb_info(env, info, &count, args, nullptr, &operation) != napi_ok || !count) return fail(env);
    void* pointer = nullptr; if(napi_get_value_external(env, args[0], &pointer) != napi_ok || !pointer) return fail(env);
    auto* handle = static_cast<Handle*>(pointer);
    const std::string op = static_cast<const char*>(operation);
    if(op == "close") { te_local_close(handle->value); handle->value = nullptr; }
    else if(op == "drain") te_local_drain(handle->value);
    else if(op == "enabled") {
      bool enabled; if(count != 2 || napi_get_value_bool(env, args[1], &enabled) != napi_ok) return fail(env);
      te_local_set_enabled(handle->value, enabled);
    } else if(op == "metrics") return text(env, te_local_metrics(handle->value));
    else {
      std::string request;
      if(count != 2 || !string(env, args[1], request, 1048576)) {
        char* ignored = te_local_invoke(handle->value, "", 0); te_local_free(ignored); return fail(env);
      }
      return text(env, te_local_invoke(handle->value, request.data(), request.size()));
    }
    napi_value result; napi_get_undefined(env, &result); return result;
  } catch(...) { return fail(env); }
}
napi_value init(napi_env env, napi_value exports) {
  auto* state = new State;
  napi_set_instance_data(env, state, [](napi_env, void* data, void*) { delete static_cast<State*>(data); }, nullptr);
  napi_property_descriptor methods[] = {
    {"open", nullptr, open, nullptr, nullptr, nullptr, napi_default, state},
    {"invoke", nullptr, call, nullptr, nullptr, nullptr, napi_default, const_cast<char*>("invoke")},
    {"metrics", nullptr, call, nullptr, nullptr, nullptr, napi_default, const_cast<char*>("metrics")},
    {"setEnabled", nullptr, call, nullptr, nullptr, nullptr, napi_default, const_cast<char*>("enabled")},
    {"drain", nullptr, call, nullptr, nullptr, nullptr, napi_default, const_cast<char*>("drain")},
    {"close", nullptr, call, nullptr, nullptr, nullptr, napi_default, const_cast<char*>("close")}};
  napi_define_properties(env, exports, sizeof(methods) / sizeof(methods[0]), methods); return exports;
}
}
NAPI_MODULE(NODE_GYP_MODULE_NAME, init)
