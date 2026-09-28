// 模型调用适配器：封装请求、认证、响应解析、超时与传输重试。
// 凭据只用于请求认证，不写入日志、诊断或模型上下文。
// 传输失败与语义协议失败分开处理；调用预算与取消信号必须贯穿重试。

// 模型适配器负责传输、限长与结构化响应；业务证据的含义由评测器决定。
#include "agentflow/trajectory/hybrid_evaluator.h"
#include "model_json.h"

#include <algorithm>
#include <chrono>
#include <curl/curl.h>
#include <fstream>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <sstream>
#include <thread>
#include <utility>

namespace agentflow::trajectory {
namespace {

using JsonValue = rapidjson::Value;

// 查找 JSON 对象成员；缺失时返回空指针，由调用方决定是否允许缺省。
const JsonValue *find_member(const JsonValue &object, const char *name) {
  if (!object.IsObject()) {
    return nullptr;
  }
  const auto member = object.FindMember(name);
  return member == object.MemberEnd() ? nullptr : &member->value;
}

// 读取字符串成员并应用本函数的缺省约定；不把其他 JSON 类型隐式转成字符串。
std::string member_string(const JsonValue &object, const char *name) {
  const JsonValue *value = find_member(object, name);
  if (value == nullptr || !value->IsString()) {
    return {};
  }
  return std::string(value->GetString(), value->GetStringLength());
}

// 将 JSON 值序列化为紧凑文本，不改变应用字段的实际内容。
std::string serialize_json(const JsonValue &value) {
  rapidjson::StringBuffer buffer;
  rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
  value.Accept(writer);
  return std::string(buffer.GetString(), buffer.GetSize());
}

// 先限制可读取的文件大小，再返回内容；异常由调用方转成固定诊断。
std::string read_bounded_file(const std::filesystem::path &path, std::size_t max_file_bytes) {
  std::error_code error;
  const auto size = std::filesystem::file_size(path, error);
  if (error) {
    throw EvaluationError("cannot stat model response file: " + error.message());
  }
  if (size > max_file_bytes) {
    throw EvaluationError("model response file exceeds configured byte limit");
  }
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    throw EvaluationError("cannot open model response file");
  }
  std::string content((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
  if (!input.eof() && input.fail()) {
    throw EvaluationError("failed while reading model response file");
  }
  return content;
}

class JsonlModelAdapter final : public ModelAdapter {
public:
  // 一次性读取有界 JSONL 回放文件，按 packet_id 建索引。
  // 缺少判断对象、无效 JSON 或重复身份都在构造时拒绝，不等到评测中途才发现。
  JsonlModelAdapter(const std::filesystem::path &path, std::size_t max_file_bytes) {
    std::istringstream input(read_bounded_file(path, max_file_bytes));
    std::string line;
    std::size_t line_number = 0;
    while (std::getline(input, line)) {
      ++line_number;
      if (line.empty()) {
        continue;
      }
      rapidjson::Document document;
      document.Parse(line.data(), line.size());
      if (document.HasParseError() || !document.IsObject()) {
        throw EvaluationError("invalid model response JSONL at line " +
                              std::to_string(line_number));
      }
      const std::string packet_id = member_string(document, "packet_id");
      const JsonValue *judgment = find_member(document, "judgment");
      if (packet_id.empty() || judgment == nullptr || !judgment->IsObject()) {
        throw EvaluationError("model response JSONL requires packet_id and judgment at line " +
                              std::to_string(line_number));
      }
      if (!responses_.emplace(packet_id, serialize_json(*judgment)).second) {
        throw EvaluationError("duplicate packet_id in model response JSONL at line " +
                              std::to_string(line_number));
      }
    }
  }

  // 按请求身份取出预先记录的裁判响应，不发起网络调用；未命中返回 missing_record。
  RawModelResponse evaluate(const ModelRequest &request) override {
    RawModelResponse response;
    response.adapter = name();
    response.model_id = model_id();
    response.attempts = 1;
    const auto item = responses_.find(request.packet_id);
    if (item == responses_.end()) {
      response.error_code = "missing_record";
      return response;
    }
    response.transport_ok = true;
    response.body = item->second;
    return response;
  }

  // 返回当前对象的稳定名称，供协议选择和诊断引用。
  std::string name() const override {
    return "jsonl-replay";
  }

  // 返回当前适配器配置的模型标识，不包含认证凭据。
  std::string model_id() const override {
    return "prerecorded";
  }

private:
  std::map<std::string, std::string> responses_;
};

// 初始化进程级网络资源，供后续连接复用；初始化不携带业务凭据。
void initialize_curl() {
  static std::once_flag once;
  static CURLcode result = CURLE_OK;
  std::call_once(once, []() { result = curl_global_init(CURL_GLOBAL_DEFAULT); });
  if (result != CURLE_OK) {
    throw EvaluationError("failed to initialize HTTP client");
  }
}

// 判断目标是否为回环地址，用于限定允许的本地非加密连接。
bool is_loopback_host(const std::string &host) {
  return host == "localhost" || host == "127.0.0.1" || host == "::1" || host == "[::1]";
}

// 在发送前校验目标地址；端点配置错误不应导致凭据被发送到任意位置。
void validate_endpoint(const std::string &endpoint) {
  if (endpoint.empty() || endpoint.size() > 4096) {
    throw EvaluationError("model endpoint is empty or too long");
  }
  std::unique_ptr<CURLU, decltype(&curl_url_cleanup)> url(curl_url(), &curl_url_cleanup);
  if (!url || curl_url_set(url.get(), CURLUPART_URL, endpoint.c_str(), 0) != CURLUE_OK) {
    throw EvaluationError("model endpoint is not a valid URL");
  }

  char *scheme_raw = nullptr;
  char *host_raw = nullptr;
  char *user_raw = nullptr;
  char *password_raw = nullptr;
  char *query_raw = nullptr;
  char *fragment_raw = nullptr;
  const CURLUcode scheme_result = curl_url_get(url.get(), CURLUPART_SCHEME, &scheme_raw, 0);
  const CURLUcode host_result = curl_url_get(url.get(), CURLUPART_HOST, &host_raw, 0);
  curl_url_get(url.get(), CURLUPART_USER, &user_raw, 0);
  curl_url_get(url.get(), CURLUPART_PASSWORD, &password_raw, 0);
  curl_url_get(url.get(), CURLUPART_QUERY, &query_raw, 0);
  curl_url_get(url.get(), CURLUPART_FRAGMENT, &fragment_raw, 0);

  const std::string scheme = scheme_raw == nullptr ? "" : scheme_raw;
  const std::string host = host_raw == nullptr ? "" : host_raw;
  const bool has_userinfo = (user_raw != nullptr && *user_raw != '\0') ||
                            (password_raw != nullptr && *password_raw != '\0');
  const bool has_query_or_fragment = (query_raw != nullptr && *query_raw != '\0') ||
                                     (fragment_raw != nullptr && *fragment_raw != '\0');
  curl_free(scheme_raw);
  curl_free(host_raw);
  curl_free(user_raw);
  curl_free(password_raw);

  curl_free(query_raw);
  curl_free(fragment_raw);
  if (scheme_result != CURLUE_OK || host_result != CURLUE_OK || host.empty()) {
    throw EvaluationError("model endpoint must include a scheme and host");
  }
  if (scheme != "https" && !(scheme == "http" && is_loopback_host(host))) {
    throw EvaluationError("model endpoint must use HTTPS; HTTP is allowed only for loopback tests");
  }
  if (has_userinfo) {
    throw EvaluationError("model endpoint must not contain credentials");
  }
  if (has_query_or_fragment) {
    throw EvaluationError("model endpoint must not contain a query or fragment");
  }
}

// 只构造本次请求所需字段；thinking、输出预算和流式选项由配置决定。
std::string make_openai_request_body(const ModelRequest &request,
                                     const OpenAiCompatibleOptions &options) {
  rapidjson::Document document;
  document.SetObject();
  auto &allocator = document.GetAllocator();
  document.AddMember("model", rapidjson::Value(options.model_id.c_str(), allocator), allocator);
  document.AddMember("temperature", 0.0, allocator);
  document.AddMember("max_tokens", options.max_output_tokens, allocator);
  if (options.stream)
    document.AddMember("stream", true, allocator);
  if (options.json_object_output) {
    rapidjson::Value format(rapidjson::kObjectType);
    format.AddMember("type", "json_object", allocator);
    document.AddMember("response_format", format, allocator);
  }
  if (!options.thinking_type.empty()) {
    rapidjson::Value thinking(rapidjson::kObjectType);
    thinking.AddMember("type", rapidjson::Value(options.thinking_type.c_str(), allocator),
                       allocator);
    document.AddMember("thinking", thinking, allocator);
  }

  rapidjson::Value messages(rapidjson::kArrayType);
  rapidjson::Value system_message(rapidjson::kObjectType);
  system_message.AddMember("role", "system", allocator);
  system_message.AddMember(
      "content",
      rapidjson::Value(request.system_prompt.data(),
                       static_cast<rapidjson::SizeType>(request.system_prompt.size()), allocator),
      allocator);
  messages.PushBack(system_message, allocator);

  rapidjson::Value user_message(rapidjson::kObjectType);
  user_message.AddMember("role", "user", allocator);
  user_message.AddMember(
      "content",
      rapidjson::Value(request.user_prompt.data(),
                       static_cast<rapidjson::SizeType>(request.user_prompt.size()), allocator),
      allocator);
  messages.PushBack(user_message, allocator);
  document.AddMember("messages", messages, allocator);
  return serialize_json(document);
}

struct ResponseBuffer {
  std::string bytes;
  std::size_t limit = 0;
  bool exceeded = false;
  std::chrono::steady_clock::time_point activity = std::chrono::steady_clock::now();
};

// 接收响应时持续检查大小，避免无界增长后才发现超出输出预算。
std::size_t append_response(char *data, std::size_t size, std::size_t count, void *userdata) {
  auto *buffer = static_cast<ResponseBuffer *>(userdata);
  if (size != 0 && count > std::numeric_limits<std::size_t>::max() / size) {
    buffer->exceeded = true;
    return 0;
  }
  const std::size_t bytes = size * count;
  if (bytes > buffer->limit || buffer->bytes.size() > buffer->limit - bytes) {
    buffer->exceeded = true;
    return 0;
  }
  try {
    buffer->bytes.append(data, bytes);
  } catch (...) {
    return 0;
  } // 分配失败时不能让异常穿越 C 回调边界。
  if (bytes)
    buffer->activity = std::chrono::steady_clock::now();
  return bytes;
}

// 仅将明确可重试的 HTTP 状态纳入重试，永久参数错误不能无限重发。
bool retryable_http_status(long status) {
  return status == 408 || status == 429 || status == 500 || status == 502 || status == 503 ||
         status == 504;
}

// 区分暂时传输故障与不可重试错误，所有尝试共享同一请求时间预算。
bool retryable_curl_error(CURLcode code) {
  return code == CURLE_COULDNT_RESOLVE_HOST || code == CURLE_COULDNT_CONNECT ||
         code == CURLE_OPERATION_TIMEDOUT || code == CURLE_RECV_ERROR || code == CURLE_SEND_ERROR ||
         code == CURLE_GOT_NOTHING;
}

// 区分接口封装与实际模型文本，finish_reason 异常不能被当作完整结果。
std::string extract_model_content(const std::string &body, std::string *finish_reason) {
  finish_reason->clear();
  rapidjson::Document document;
  document.Parse<rapidjson::kParseValidateEncodingFlag | rapidjson::kParseIterativeFlag>(
      body.data(), body.size());
  if (document.HasParseError() || !unique_object_fields(document) ||
      (document.HasMember("error") && !document["error"].IsNull())) {
    return {};
  }
  const JsonValue *choices = find_member(document, "choices");
  if (choices == nullptr || !choices->IsArray() || choices->Size() != 1) {
    return {};
  }
  const JsonValue &choice = (*choices)[0];
  if (!unique_object_fields(choice))
    return {};
  const auto *index = find_member(choice, "index");
  if (index && (!index->IsInt() || index->GetInt() != 0))
    return {};
  const JsonValue *finish = find_member(choice, "finish_reason");
  if (finish != nullptr && !finish->IsNull()) {
    if (!finish->IsString())
      return {};
    finish_reason->assign(finish->GetString(), finish->GetStringLength());
    if (*finish_reason != "stop" && *finish_reason != "length")
      return {};
  }
  const JsonValue *message = find_member(choice, "message");
  if (!message || !unique_object_fields(*message))
    return {};
  const auto *refusal = message ? find_member(*message, "refusal") : nullptr;
  if (refusal && !refusal->IsNull() && (!refusal->IsString() || refusal->GetStringLength()))
    return {};
  const JsonValue *content = message == nullptr ? nullptr : find_member(*message, "content");
  if (content == nullptr || !content->IsString()) {
    return {};
  }
  return std::string(content->GetString(), content->GetStringLength());
}

// 逐段组装流式结果并检查结束状态；中途断开不构成有效的完整回答。
std::string extract_stream_content(const std::string &body, std::size_t limit,
                                   std::string *finish_reason) {
  // 部分兼容提供方忽略流式参数，直接返回普通 JSON 信封。
  const auto first = body.find_first_not_of(" \r\n\t");
  if (first != std::string::npos && body[first] == '{')
    return body.size() <= limit ? extract_model_content(body, finish_reason) : std::string();
  std::string result, line, data;
  bool done = false;
  auto frame = [&]() -> bool {
    if (data.empty())
      return true;
    if (done)
      return false;
    if (data == "[DONE]") {
      done = true;
      data.clear();
      return true;
    }
    rapidjson::Document value;
    value.Parse<rapidjson::kParseValidateEncodingFlag | rapidjson::kParseIterativeFlag>(
        data.data(), data.size());
    if (value.HasParseError() || !unique_object_fields(value) ||
        (value.HasMember("error") && !value["error"].IsNull()))
      return false;
    const auto *choices = find_member(value, "choices");
    if (!choices || !choices->IsArray() || choices->Size() > 1)
      return false;
    for (const auto &choice : choices->GetArray()) {
      const auto *index = find_member(choice, "index");
      if (!unique_object_fields(choice) || (index && (!index->IsInt() || index->GetInt() != 0)))
        return false;
      const auto *delta = find_member(choice, "delta");
      if (!delta || !unique_object_fields(*delta))
        return false;
      const auto *refusal = find_member(*delta, "refusal");
      if (refusal && !refusal->IsNull() && (!refusal->IsString() || refusal->GetStringLength()))
        return false;
      const auto *content = find_member(*delta, "content");
      if (content && !content->IsNull()) {
        if (!content->IsString() || result.size() + content->GetStringLength() > limit ||
            (!finish_reason->empty() && content->GetStringLength()))
          return false;
        result.append(content->GetString(), content->GetStringLength());
      }
      const auto *finish = find_member(choice, "finish_reason");
      if (finish && !finish->IsNull()) {
        if (!finish->IsString() || !finish_reason->empty())
          return false;
        finish_reason->assign(finish->GetString(), finish->GetStringLength());
      }
    }
    data.clear();
    return true;
  };
  std::istringstream lines(body);
  bool first_line = true;
  while (std::getline(lines, line)) {
    if (first_line && line.compare(0, 3, "\xef\xbb\xbf") == 0)
      line.erase(0, 3);
    first_line = false;
    if (!line.empty() && line.back() == '\r')
      line.pop_back();
    if (line.empty()) {
      if (!frame())
        return {};
    } else if (line.compare(0, 5, "data:") == 0) {
      const auto offset = line.size() > 5 && line[5] == ' ' ? 6 : 5;
      if (!data.empty())
        data += '\n';
      data += line.substr(offset);
    }
  }
  if (!frame() || !done || (*finish_reason != "stop" && *finish_reason != "length"))
    return {};
  return result;
}

class OpenAiCompatibleModelAdapter final : public ModelAdapter {
public:
  // 初始化 OpenAiCompatibleModelAdapter 的状态和资源；输入参数的校验与所有权在构造阶段确定。
  // 对象创建后复用既有配置，实际执行与收尾分别由后续接口负责。
  explicit OpenAiCompatibleModelAdapter(OpenAiCompatibleOptions options)
      : options_(std::move(options)) {
    validate_endpoint(options_.endpoint);
    if (options_.model_id.empty() || options_.model_id.size() > 256) {
      throw EvaluationError("model ID is empty or too long");
    }
    if (!options_.thinking_type.empty() && options_.thinking_type != "enabled" &&
        options_.thinking_type != "disabled" && options_.thinking_type != "auto") {
      throw EvaluationError("model thinking mode is invalid");
    }
    const auto curl_timeout_max = static_cast<unsigned long long>(std::numeric_limits<long>::max());
    if (options_.timeout_ms == 0 ||
        static_cast<unsigned long long>(options_.timeout_ms) > curl_timeout_max ||
        options_.connect_timeout_ms == 0 ||
        static_cast<unsigned long long>(options_.connect_timeout_ms) > curl_timeout_max) {
      throw EvaluationError("model timeout is outside the supported range");
    }
    if (options_.max_retries > 10 || options_.max_output_tokens == 0 ||
        options_.max_output_tokens > 32768) {
      throw EvaluationError("model retry or output-token limit is invalid");
    }
    if (options_.idle_timeout_ms && !options_.stream)
      throw EvaluationError("model idle timeout requires streaming");
    if (options_.api_key.size() > 16384 ||
        options_.api_key.find_first_of("\r\n") != std::string::npos ||
        options_.api_key.find('\0') != std::string::npos)
      throw EvaluationError("model credential contains invalid header bytes");
    if (options_.max_response_bytes == 0 || options_.max_response_bytes > 8U * 1024U * 1024U) {
      throw EvaluationError("model response byte limit is invalid");
    }
    if (options_.max_request_bytes == 0 || options_.max_request_bytes > 8U * 1024U * 1024U) {
      throw EvaluationError("model request byte limit is invalid");
    }
    initialize_curl();
  }

  // 销毁对象时释放本类持有的资源；收尾不能抛出业务异常，也不能继续接收新请求。
  ~OpenAiCompatibleModelAdapter() override {
    for (auto *handle : idle_handles_)
      curl_easy_cleanup(handle);
  }

  // 发送一个 OpenAI 兼容请求，复用连接并让所有传输重试共享同一总期限。
  // 此层只处理传输、响应信封和流式收集；裁判内容的业务校验由上层执行。
  RawModelResponse evaluate(const ModelRequest &request) override {
    RawModelResponse response;
    response.adapter = name();
    response.model_id = model_id();
    const std::string request_body = make_openai_request_body(request, options_);
    if (request_body.size() > options_.max_request_bytes) {
      response.error_code = "request_too_large";
      return response;
    }

    // 所有重试共享一次网络预算，卡住的尝试不能续期总期限。
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(options_.timeout_ms);
    const std::uint32_t max_attempts = options_.max_retries + 1U;
    for (std::uint32_t attempt = 1; attempt <= max_attempts; ++attempt) {
      if (options_.cancelled && options_.cancelled()) {
        response.error_code = "transport_" + std::to_string(CURLE_ABORTED_BY_CALLBACK);
        return response;
      }
      const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
          deadline - std::chrono::steady_clock::now());
      if (remaining.count() <= 0) {
        response.error_code = "transport_" + std::to_string(CURLE_OPERATION_TIMEDOUT);
        return response;
      }
      // 为后续尝试保留时间，避免一次停滞耗尽全部预算。
      const auto attempt_timeout =
          std::max<std::int64_t>(1, remaining.count() / (max_attempts - attempt + 1U));
      response.attempts = attempt;
      ResponseBuffer buffer;
      // SSE 帧大小限制与解码后内容预算分开管理。
      buffer.limit = options_.stream ? std::min<std::size_t>(8U * 1024U * 1024U,
                                                             options_.max_response_bytes * 32U)
                                     : options_.max_response_bytes;

      const auto release = [this](CURL *handle) noexcept {
        curl_easy_reset(handle); // 清除凭据和回调指针，但保留可复用连接。
        try {
          std::lock_guard<std::mutex> guard(handles_mutex_);
          if (idle_handles_.size() < 8) {
            idle_handles_.push_back(handle);
            return;
          }
        } catch (...) {
        }
        curl_easy_cleanup(handle);
      };
      CURL *available = nullptr;
      {
        std::lock_guard<std::mutex> guard(handles_mutex_);
        if (!idle_handles_.empty()) {
          available = idle_handles_.back();
          idle_handles_.pop_back();
        }
      }
      std::unique_ptr<CURL, decltype(release)> handle(available ? available : curl_easy_init(),
                                                      release);
      if (!handle) {
        response.error_code = "client_initialization_failed";
        return response;
      }

      std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)> headers(nullptr,
                                                                          &curl_slist_free_all);
      const auto header = [&](const std::string &value) {
        auto *list = curl_slist_append(headers.get(), value.c_str());
        if (!list)
          return false;
        headers.release();
        headers.reset(list);
        return true;
      };
      if (!header("Content-Type: application/json") ||
          (!options_.api_key.empty() && !header("Authorization: Bearer " + options_.api_key))) {
        response.error_code = "header_allocation_failed";
        return response;
      }

      curl_easy_setopt(handle.get(), CURLOPT_URL, options_.endpoint.c_str());
      curl_easy_setopt(handle.get(), CURLOPT_HTTPHEADER, headers.get());
      curl_easy_setopt(handle.get(), CURLOPT_POST, 1L);
      curl_easy_setopt(handle.get(), CURLOPT_POSTFIELDS, request_body.data());
      curl_easy_setopt(handle.get(), CURLOPT_POSTFIELDSIZE_LARGE,
                       static_cast<curl_off_t>(request_body.size()));
      curl_easy_setopt(handle.get(), CURLOPT_WRITEFUNCTION, append_response);
      curl_easy_setopt(handle.get(), CURLOPT_WRITEDATA, &buffer);
      curl_easy_setopt(handle.get(), CURLOPT_NOSIGNAL, 1L);

      struct Progress {
        const OpenAiCompatibleOptions *options;
        const ResponseBuffer *buffer;
        bool idle = false;
      } progress{&options_, &buffer};

      if (options_.cancelled || options_.idle_timeout_ms) {
        curl_easy_setopt(handle.get(), CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(handle.get(), CURLOPT_XFERINFODATA, &progress);
        curl_easy_setopt(
            handle.get(), CURLOPT_XFERINFOFUNCTION,
            +[](void *context, curl_off_t, curl_off_t, curl_off_t, curl_off_t) -> int {
              auto &state = *static_cast<Progress *>(context);
              try {
                if (state.options->cancelled && state.options->cancelled())
                  return 1;
                state.idle = state.options->idle_timeout_ms &&
                             std::chrono::steady_clock::now() - state.buffer->activity >=
                                 std::chrono::milliseconds(state.options->idle_timeout_ms);
                return state.idle ? 1 : 0;
              } catch (...) {
                return 1;
              }
            });
      }
      curl_easy_setopt(handle.get(), CURLOPT_FOLLOWLOCATION, 0L);
      curl_easy_setopt(handle.get(), CURLOPT_MAXREDIRS, 0L);
      curl_easy_setopt(handle.get(), CURLOPT_SSL_VERIFYPEER, 1L);
      curl_easy_setopt(handle.get(), CURLOPT_SSL_VERIFYHOST, 2L);
      curl_easy_setopt(handle.get(), CURLOPT_TIMEOUT_MS, static_cast<long>(attempt_timeout));
      curl_easy_setopt(handle.get(), CURLOPT_CONNECTTIMEOUT_MS,
                       static_cast<long>(options_.connect_timeout_ms));
#if LIBCURL_VERSION_NUM >= 0x075500
      curl_easy_setopt(handle.get(), CURLOPT_PROTOCOLS_STR, "http,https");
#endif
      if (options_.disable_proxy) {
        curl_easy_setopt(handle.get(), CURLOPT_PROXY, "");
        curl_easy_setopt(handle.get(), CURLOPT_NOPROXY, "*");
      }

      CURLcode curl_result = curl_easy_perform(handle.get());
      if (progress.idle)
        curl_result = CURLE_OPERATION_TIMEDOUT;
      long http_status = 0;
      curl_easy_getinfo(handle.get(), CURLINFO_RESPONSE_CODE, &http_status);

      if (curl_result == CURLE_OK && http_status >= 200 && http_status < 300) {
        response.transport_ok = true;
        std::string finish_reason;
        response.body =
            options_.stream
                ? extract_stream_content(buffer.bytes, options_.max_response_bytes, &finish_reason)
                : extract_model_content(buffer.bytes, &finish_reason);
        if (finish_reason == "length") {
          response.error_code = "output_token_limit_reached";
        } else if (response.body.empty()) {
          response.error_code = "invalid_api_response";
        }
        return response;
      }

      const bool retry = attempt < max_attempts &&
                         ((curl_result != CURLE_OK && retryable_curl_error(curl_result)) ||
                          retryable_http_status(http_status));
      if (!retry) {
        if (buffer.exceeded) {
          response.error_code = "response_too_large";
        } else if (curl_result != CURLE_OK) {
          response.error_code = "transport_" + std::to_string(curl_result);
        } else {
          response.error_code = "http_" + std::to_string(http_status);
        }
        return response;
      }
      // 429/5xx 通常按秒级窗口恢复。旧的 100ms 线性等待会让并发请求同步撞回
      // 同一个限流窗口；改为有上限的指数退避，并按请求身份加入稳定抖动。
      const auto exponent = std::min<std::uint32_t>(attempt - 1U, 4U);
      const auto base_pause = std::chrono::milliseconds(500U * (1U << exponent));
      const auto jitter = std::chrono::milliseconds(
          std::hash<std::string>{}(request.packet_id) % 251U);
      const auto pause = std::min(base_pause + jitter,
                                  std::chrono::duration_cast<std::chrono::milliseconds>(
                                      deadline - std::chrono::steady_clock::now()));
      if (pause.count() > 0)
        std::this_thread::sleep_for(pause);
    }
    response.error_code = "transport_exhausted";
    return response;
  }

  // 返回当前对象的稳定名称，供协议选择和诊断引用。
  std::string name() const override {
    return "openai-compatible-http";
  }

  // 返回当前适配器配置的模型标识，不包含认证凭据。
  std::string model_id() const override {
    return options_.model_id;
  }

private:
  OpenAiCompatibleOptions options_;
  std::mutex handles_mutex_;
  std::vector<CURL *> idle_handles_;
};

} // 结束当前匿名命名空间。

// 创建离线响应适配器，便于可重复回归；它不会替代真实实验的模型能力验证。
std::shared_ptr<ModelAdapter> make_jsonl_model_adapter(const std::filesystem::path &path,
                                                       std::size_t max_file_bytes) {
  return std::make_shared<JsonlModelAdapter>(path, max_file_bytes);
}

// 创建兼容聊天接口的模型适配器，集中处理超时、取消和凭据，业务层不直接拼接网络请求。
std::shared_ptr<ModelAdapter>
make_openai_compatible_model_adapter(const OpenAiCompatibleOptions &options) {
  return std::make_shared<OpenAiCompatibleModelAdapter>(options);
}

} // 命名空间结束：agentflow::trajectory
