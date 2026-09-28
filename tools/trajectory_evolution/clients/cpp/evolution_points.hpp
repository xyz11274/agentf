// cpp 客户端：通过统一协议收集执行事件并接收策略建议。
// 传输与业务执行分离；接收回执不代表 Agent 已完成整个任务。
// 打点的输入是建议修改前的请求，执行后必须回传实际输入、输出与状态。
// 取消、超时和采集缺口需要显式处理，不能把缺失证据当成正常完成。

#pragma once

#include "evolution.hpp"
#include <trajectory_evolution/local_c.h>
#include <trajectory_evolution/local_runtime.h>
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <iomanip>

#include <functional>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

namespace trajectory_evolution_http {

class Points {
public:
  using Transport = std::function<std::string(const std::string &)>;

  struct CallResult {
    std::string output;
    std::string receipt;
    bool executed = false;
    bool rejected = false;
  };

  struct LoopResult {
    bool allowed = true;
    std::string receipt;
  };

  // 初始化 Points 的状态和资源；输入参数的校验与所有权在构造阶段确定。
  // 对象创建后复用既有配置，实际执行与收尾分别由后续接口负责。
  Points(Points &&other) noexcept : transport_(std::move(other.transport_)) {}

  Points(const Points &) = delete;
  Points &operator=(const Points &) = delete;

  // 初始化 Points 的状态和资源；输入参数的校验与所有权在构造阶段确定。
  // 对象创建后复用既有配置，实际执行与收尾分别由后续接口负责。
  explicit Points(Transport transport) : transport_(std::move(transport)) {
    if (!transport_)
      throw Error("point_transport_missing", 0);
  }

  // 创建使用 HTTP 传输的打点入口；调用方复用同一个客户端配置。
  static Points http(const te_client &source) {
    if (!source.url || !source.token)
      throw Error("client_invalid", 0);
    const std::string url = source.url, token = source.token;
    const auto timeout = source.timeout_ms;
    const auto limit = source.max_response_bytes;
    const auto retries = source.retries;
    return Points([url, token, timeout, limit, retries](const std::string &request) {
      const auto packet = parse_object(request);
      const bool boundary = string_value(packet, "operation") == "point";
      te_client client{url.c_str(), token.c_str(), boundary ? std::min<long>(timeout, 20) : timeout,
                       limit, boundary ? 0U : retries};
      char *raw = nullptr;
      long status = 0;
      const char *code = nullptr;
      if (te_invoke(&client, request.c_str(), &raw, &status, &code) != 0)
        throw Error(code ? code : "observer_unavailable", status);
      std::unique_ptr<char, decltype(&te_free)> response(raw, &te_free);
      if (!response)
        throw Error("response_missing", status);
      return std::string(response.get());
    });
  }

  // 创建进程内打点入口，复用稳定本地接口，避免业务边界访问网络。
  static Points local(te_local_handle *handle) {
    if (!handle)
      throw Error("local_unavailable", 0);
    return Points([handle](const std::string &request) {
      std::unique_ptr<char, decltype(&te_local_free)> response(
          te_local_invoke(handle, request.data(), request.size()), &te_local_free);
      if (!response)
        throw Error("local_unavailable", 0);
      return std::string(response.get());
    });
  }

  // 创建进程内打点入口，复用稳定本地接口，避免业务边界访问网络。
  static Points local(trajectory::evolution::LocalRuntime &runtime) {
    return Points([&runtime](const std::string &request) { return runtime.invoke(request).body; });
  }

  // 提交框架无关打点，必要时在同一边界先取得策略决定，再记录实际执行结果。
  std::string point(const std::string &fields_json) const {
    auto fields = parse_object(fields_json);
    const auto run = run_key(fields);
    {
      std::lock_guard<std::mutex> lock(gaps_mutex_);
      if (!run.empty() && (gaps_full_ || gaps_.count(run)))
        set_bool(fields, "capture_gap", true);
    }
    std::string observed;
    try {
      observed = result(invoke_document(fields, "point"));
    } catch (...) {
      observed = fallback(fields);
    }
    const auto value = parse_object(observed);
    if (!run.empty()) {
      std::lock_guard<std::mutex> lock(gaps_mutex_);
      if (value.HasMember("degraded") && value["degraded"].IsBool() &&
          value["degraded"].GetBool()) {
        if (gaps_.size() < 1024)
          gaps_.insert(run);
        else
          gaps_full_ = true;
      } else if ((value.HasMember("closed") && value["closed"].IsBool() &&
                  value["closed"].GetBool()) ||
                 (value.HasMember("run_closed") && value["run_closed"].IsBool() &&
                  value["run_closed"].GetBool()))
        gaps_.erase(run);
    }
    return observed;
  }

  // 包装宿主业务调用：先查询策略，再最多执行一次原业务函数，最后记录结果。
  template <class Execute>
  CallResult call(const std::string &fields_json, Execute &&execute,
                  std::string *abort_receipt = nullptr) const {
    auto fields = parse_object(fields_json);
    auto before = clone(fields);
    set_string(before, "phase", "before");
    const auto prepared = parse_object(point(dump(before)));
    const auto rejected = string_value(prepared, "decision") == "reject";
    const auto actual = prepared.HasMember("input") ? dump(prepared["input"])
                        : fields.HasMember("input") ? dump(fields["input"])
                                                    : "null";
    std::string output;
    if (rejected) {
      const auto reason = string_value(prepared, "reason", "policy_rejected");
      rapidjson::Document value(rapidjson::kObjectType);
      set_string(value, "status", "rejected");
      set_string(value, "reason", reason);
      set_string(value, "error", reason);
      output = dump(value);
    } else {
      try {
        output = std::forward<Execute>(execute)(actual);
        parse_value(output);
      } catch (...) {
        try {
          auto abort = identity(fields, prepared);
          set_string(abort, "phase", "abort");
          set_string(abort, "status", "unknown");
          add_null(abort, "output");
          const auto receipt = point(dump(abort));
          if (abort_receipt)
            *abort_receipt = receipt;
        } catch (...) {
        }
        throw;
      }
    }
    auto after = identity(fields, prepared);
    set_string(after, "phase", "after");
    set_bool(after, "executed", !rejected);
    add_json(after, "actual_input", actual);
    add_json(after, "output", output);
    return {output, point(dump(after)), !rejected, rejected};
  }

  // 在原循环条件处取得并提交实际控制决定；返回是否允许继续，而不是另起一套循环。
  LoopResult allow_loop(const std::string &fields_json) const {
    auto fields = parse_object(fields_json);
    set_string(fields, "kind", "loop");
    if (fields.HasMember("ref")) {
      set_string(fields, "phase", "guard");
      const auto receipt = point(dump(fields));
      const auto value = parse_object(receipt);
      return {string_value(value, "decision", "continue") != "stop", receipt};
    }
    auto before = clone(fields);
    set_string(before, "phase", "before");
    const auto prepared = parse_object(point(dump(before)));
    const auto decision = string_value(prepared, "decision", "continue");
    auto after = identity(fields, prepared);
    set_string(after, "kind", "loop");
    set_string(after, "phase", "after");
    set_string(after, "decision", decision);
    const auto receipt = point(dump(after));
    return {decision != "stop", receipt};
  }

  // 显式报告采集缺口，防止不完整轨迹进入后续学习与评测。
  std::string capture_gap(const std::string &fields_json) const {
    auto fields = parse_object(fields_json);
    if (fields.HasMember("ref")) {
      set_string(fields, "kind", "run");
      set_string(fields, "phase", "gap");
      return point(dump(fields));
    }
    if (!fields.HasMember("reason"))
      set_string(fields, "reason", "observer_unavailable");
    return result(invoke_document(fields, "capture.gap"));
  }

  // 通过现有接收入口取得隔离评测下一轮输入；上一轮尚未结束时不提前交付。
  std::string evaluation_next(const std::string &consumer, std::size_t index) const {
    rapidjson::Document fields(rapidjson::kObjectType);
    set_string(fields, "consumer", consumer);
    rapidjson::Value number;
    number.SetUint64(index);
    fields.AddMember("index", number, fields.GetAllocator());
    return result(invoke_document(fields, "evaluation.next"));
  }

  // 报告评测入口执行异常，保留固定错误类型，不发送业务异常正文。
  std::string evaluation_fail(const std::string &consumer) const {
    rapidjson::Document fields(rapidjson::kObjectType);
    set_string(fields, "consumer", consumer);
    return result(invoke_document(fields, "evaluation.fail"));
  }

private:
  // 解析单个 JSON 值，类型约束由调用边界继续验证。
  static rapidjson::Document parse_value(const std::string &json) {
    rapidjson::Document value;
    value.Parse<rapidjson::kParseValidateEncodingFlag | rapidjson::kParseIterativeFlag>(
        json.data(), json.size());
    if (value.HasParseError())
      throw Error("request_invalid", 0);
    return value;
  }

  // 解析必须为对象的 JSON 输入，拒绝数组或标量绕过字段契约。
  static rapidjson::Document parse_object(const std::string &json) {
    auto value = parse_value(json);
    if (!value.IsObject())
      throw Error("object_required", 0);
    return value;
  }

  // 复制 JSON 内容，使返回值不依赖即将销毁的临时分配器。
  static rapidjson::Document clone(const rapidjson::Value &value) {
    rapidjson::Document result;
    result.CopyFrom(value, result.GetAllocator());
    return result;
  }

  // 序列化当前对象为紧凑 JSON，供协议传输或持久化使用。
  static std::string dump(const rapidjson::Value &value) {
    rapidjson::StringBuffer buffer;
    rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
    if (!value.Accept(writer))
      throw Error("request_invalid", 0);
    return {buffer.GetString(), buffer.GetSize()};
  }

  // 移除指定成员，为重写协议字段准备一致对象。
  static void erase(rapidjson::Document &value, const char *key) {
    if (value.HasMember(key))
      value.RemoveMember(key);
  }

  // 按当前分配器写入字符串字段，避免保存悬空字符指针。
  static void set_string(rapidjson::Document &value, const char *key, const std::string &text) {
    erase(value, key);
    rapidjson::Value name(key, value.GetAllocator());
    rapidjson::Value item(text.data(), text.size(), value.GetAllocator());
    value.AddMember(name, item, value.GetAllocator());
  }

  // 写入明确布尔字段，不以字符串真假替代协议布尔类型。
  static void set_bool(rapidjson::Document &value, const char *key, bool flag) {
    erase(value, key);
    rapidjson::Value name(key, value.GetAllocator());
    value.AddMember(name, flag, value.GetAllocator());
  }

  // 写入显式空值，用于与缺失字段区分。
  static void add_null(rapidjson::Document &value, const char *key) {
    erase(value, key);
    rapidjson::Value name(key, value.GetAllocator()), item;
    item.SetNull();
    value.AddMember(name, item, value.GetAllocator());
  }

  // 复制结构化值到目标对象，保持目标分配器所有权。
  static void add_json(rapidjson::Document &value, const char *key, const std::string &json) {
    const auto parsed = parse_value(json);
    erase(value, key);
    rapidjson::Value name(key, value.GetAllocator()), item;
    item.CopyFrom(parsed, value.GetAllocator());
    value.AddMember(name, item, value.GetAllocator());
  }

  // 读取并校验字符串字段，避免对非法类型直接取值。
  static std::string string_value(const rapidjson::Value &value, const char *key,
                                  const std::string &fallback = {}) {
    return value.IsObject() && value.HasMember(key) && value[key].IsString()
               ? std::string(value[key].GetString(), value[key].GetStringLength())
               : fallback;
  }

  // 构造或复用本次协议请求身份，保证重试仍指向同一请求。
  static std::string request_id() {
    static std::atomic<std::uint64_t> sequence{0};
    const auto tick = std::chrono::system_clock::now().time_since_epoch().count();
#ifdef _WIN32
    const auto process = _getpid();
#else
    const auto process = getpid();
#endif
    std::ostringstream value;
    value << "cpp-" << std::hex << tick << "-" << process << "-"
          << sequence.fetch_add(1, std::memory_order_relaxed);
    return value.str();
  }

  // 发送已经构造的协议对象并校验响应信封。
  rapidjson::Document invoke_document(const rapidjson::Value &fields,
                                      const std::string &operation) const {
    auto request = clone(fields);
    set_string(request, "operation", operation);
    if (!request.HasMember("request_id"))
      set_string(request, "request_id", request_id());
    const auto envelope = parse_object(transport_(dump(request)));
    if (!envelope.HasMember("ok") || !envelope["ok"].IsBool() || !envelope["ok"].GetBool()) {
      throw Error(string_value(envelope, "error", "rpc_failed").c_str(), 0);
    }
    if (!envelope.HasMember("result") || !envelope["result"].IsObject())
      throw Error("response_invalid", 0);
    return clone(envelope["result"]);
  }

  // 提取标准响应中的结果字段，失败信封不能直接当作业务结果。
  static std::string result(rapidjson::Document value) {
    return dump(value);
  }

  // 读取当前打点所属运行键，供缺口跟踪与结束清理使用。
  static std::string run_key(const rapidjson::Value &fields) {
    if (!fields.IsObject() || !fields.HasMember("ref") || !fields["ref"].IsObject() ||
        !fields["ref"].HasMember("run") || !fields["ref"]["run"].IsString() ||
        fields["ref"]["run"].GetStringLength() > 256)
      return {};
    return {fields["ref"]["run"].GetString(), fields["ref"]["run"].GetStringLength()};
  }

  // 提取协议身份字段，保留调用与运行之间的关联。
  static rapidjson::Document identity(const rapidjson::Value &fields,
                                      const rapidjson::Value &prepared) {
    rapidjson::Document value(rapidjson::kObjectType);
    if (fields.HasMember("ref")) {
      rapidjson::Value ref;
      ref.CopyFrom(fields["ref"], value.GetAllocator());
      value.AddMember("ref", ref, value.GetAllocator());
    } else if (prepared.HasMember("context")) {
      rapidjson::Value context;
      context.CopyFrom(prepared["context"], value.GetAllocator());
      value.AddMember("context", context, value.GetAllocator());
    }
    if (fields.HasMember("kind")) {
      rapidjson::Value kind;
      kind.CopyFrom(fields["kind"], value.GetAllocator());
      value.AddMember("kind", kind, value.GetAllocator());
    }
    return value;
  }

  // 构造保持原业务继续执行的降级响应，并记录采集不完整。
  static std::string fallback(const rapidjson::Value &fields) {
    rapidjson::Document value(rapidjson::kObjectType);
    rapidjson::Value context(rapidjson::kObjectType);
    if (fields.HasMember("context") && fields["context"].IsObject())
      context.CopyFrom(fields["context"], value.GetAllocator());
    const auto kind = string_value(fields, "kind", string_value(context, "kind", "custom"));
    rapidjson::Value context_name("context", value.GetAllocator());
    rapidjson::Value kind_name("kind", value.GetAllocator());
    rapidjson::Value kind_value(kind.data(), kind.size(), value.GetAllocator());
    if (context.HasMember("kind"))
      context.RemoveMember("kind");
    context.AddMember(kind_name, kind_value, value.GetAllocator());
    rapidjson::Value degraded_name("degraded", value.GetAllocator());
    context.AddMember(degraded_name, true, value.GetAllocator());
    value.AddMember(context_name, context, value.GetAllocator());
    if (fields.HasMember("input")) {
      rapidjson::Value input;
      input.CopyFrom(fields["input"], value.GetAllocator());
      value.AddMember("input", input, value.GetAllocator());
    } else
      add_null(value, "input");
    set_bool(value, "degraded", true);
    set_bool(value, "capture_complete", false);
    set_bool(value, "policy_applied", false);
    set_bool(value, "closed", false);
    set_bool(value, "ended", false);
    set_string(value, "reason", "observer_unavailable");
    set_string(value, "decision", kind == "loop" ? "continue" : "allow");
    return dump(value);
  }

  Transport transport_;
  mutable std::mutex gaps_mutex_;
  mutable std::set<std::string> gaps_;
  mutable bool gaps_full_ = false;
};

} // 命名空间结束：trajectory_evolution_http
