#include "agentflow/trajectory/hybrid_evaluator.h"

#include <chrono>
#include <curl/curl.h>
#include <fstream>
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

const JsonValue* find_member(const JsonValue& object, const char* name) {
  if(!object.IsObject()) {
    return nullptr;
  }
  const auto member = object.FindMember(name);
  return member == object.MemberEnd() ? nullptr : &member->value;
}

std::string member_string(const JsonValue& object, const char* name) {
  const JsonValue* value = find_member(object, name);
  if(value == nullptr || !value->IsString()) {
    return {};
  }
  return std::string(value->GetString(), value->GetStringLength());
}

std::string serialize_json(const JsonValue& value) {
  rapidjson::StringBuffer buffer;
  rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
  value.Accept(writer);
  return std::string(buffer.GetString(), buffer.GetSize());
}

std::string read_bounded_file(
    const std::filesystem::path& path,
    std::size_t max_file_bytes) {
  std::error_code error;
  const auto size = std::filesystem::file_size(path, error);
  if(error) {
    throw EvaluationError("cannot stat model response file: " + error.message());
  }
  if(size > max_file_bytes) {
    throw EvaluationError("model response file exceeds configured byte limit");
  }
  std::ifstream input(path, std::ios::binary);
  if(!input) {
    throw EvaluationError("cannot open model response file");
  }
  std::string content(
      (std::istreambuf_iterator<char>(input)),
      std::istreambuf_iterator<char>());
  if(!input.eof() && input.fail()) {
    throw EvaluationError("failed while reading model response file");
  }
  return content;
}

class JsonlModelAdapter final : public ModelAdapter {
 public:
  JsonlModelAdapter(
      const std::filesystem::path& path,
      std::size_t max_file_bytes) {
    std::istringstream input(read_bounded_file(path, max_file_bytes));
    std::string line;
    std::size_t line_number = 0;
    while(std::getline(input, line)) {
      ++line_number;
      if(line.empty()) {
        continue;
      }
      rapidjson::Document document;
      document.Parse(line.data(), line.size());
      if(document.HasParseError() || !document.IsObject()) {
        throw EvaluationError(
            "invalid model response JSONL at line "
            + std::to_string(line_number));
      }
      const std::string packet_id = member_string(document, "packet_id");
      const JsonValue* judgment = find_member(document, "judgment");
      if(packet_id.empty() || judgment == nullptr || !judgment->IsObject()) {
        throw EvaluationError(
            "model response JSONL requires packet_id and judgment at line "
            + std::to_string(line_number));
      }
      if(!responses_.emplace(packet_id, serialize_json(*judgment)).second) {
        throw EvaluationError(
            "duplicate packet_id in model response JSONL at line "
            + std::to_string(line_number));
      }
    }
  }

  RawModelResponse evaluate(const ModelRequest& request) override {
    RawModelResponse response;
    response.adapter = name();
    response.model_id = model_id();
    response.attempts = 1;
    const auto item = responses_.find(request.packet_id);
    if(item == responses_.end()) {
      response.error_code = "missing_record";
      return response;
    }
    response.transport_ok = true;
    response.body = item->second;
    return response;
  }

  std::string name() const override {
    return "jsonl-replay";
  }

  std::string model_id() const override {
    return "prerecorded";
  }

 private:
  std::map<std::string, std::string> responses_;
};

void initialize_curl() {
  static std::once_flag once;
  static CURLcode result = CURLE_OK;
  std::call_once(once, []() {
    result = curl_global_init(CURL_GLOBAL_DEFAULT);
  });
  if(result != CURLE_OK) {
    throw EvaluationError("failed to initialize HTTP client");
  }
}

bool is_loopback_host(const std::string& host) {
  return host == "localhost"
      || host == "127.0.0.1"
      || host == "::1"
      || host == "[::1]";
}

void validate_endpoint(const std::string& endpoint) {
  if(endpoint.empty() || endpoint.size() > 4096) {
    throw EvaluationError("model endpoint is empty or too long");
  }
  std::unique_ptr<CURLU, decltype(&curl_url_cleanup)> url(
      curl_url(),
      &curl_url_cleanup);
  if(!url || curl_url_set(url.get(), CURLUPART_URL, endpoint.c_str(), 0) != CURLUE_OK) {
    throw EvaluationError("model endpoint is not a valid URL");
  }

  char* scheme_raw = nullptr;
  char* host_raw = nullptr;
  char* user_raw = nullptr;
  char* password_raw = nullptr;
  char* query_raw = nullptr;
  char* fragment_raw = nullptr;
  const CURLUcode scheme_result = curl_url_get(
      url.get(), CURLUPART_SCHEME, &scheme_raw, 0);
  const CURLUcode host_result = curl_url_get(
      url.get(), CURLUPART_HOST, &host_raw, 0);
  curl_url_get(url.get(), CURLUPART_USER, &user_raw, 0);
  curl_url_get(url.get(), CURLUPART_PASSWORD, &password_raw, 0);
  curl_url_get(url.get(), CURLUPART_QUERY, &query_raw, 0);
  curl_url_get(url.get(), CURLUPART_FRAGMENT, &fragment_raw, 0);

  const std::string scheme = scheme_raw == nullptr ? "" : scheme_raw;
  const std::string host = host_raw == nullptr ? "" : host_raw;
  const bool has_userinfo = (user_raw != nullptr && *user_raw != '\0')
      || (password_raw != nullptr && *password_raw != '\0');
  const bool has_query_or_fragment = (query_raw != nullptr && *query_raw != '\0')
      || (fragment_raw != nullptr && *fragment_raw != '\0');
  curl_free(scheme_raw);
  curl_free(host_raw);
  curl_free(user_raw);
  curl_free(password_raw);

  curl_free(query_raw);
  curl_free(fragment_raw);
  if(scheme_result != CURLUE_OK || host_result != CURLUE_OK || host.empty()) {
    throw EvaluationError("model endpoint must include a scheme and host");
  }
  if(scheme != "https" && !(scheme == "http" && is_loopback_host(host))) {
    throw EvaluationError(
        "model endpoint must use HTTPS; HTTP is allowed only for loopback tests");
  }
  if(has_userinfo) {
    throw EvaluationError("model endpoint must not contain credentials");
  }
  if(has_query_or_fragment) {
    throw EvaluationError("model endpoint must not contain a query or fragment");
  }
}

std::string make_openai_request_body(
    const ModelRequest& request,
    const OpenAiCompatibleOptions& options) {
  rapidjson::Document document;
  document.SetObject();
  auto& allocator = document.GetAllocator();
  document.AddMember(
      "model",
      rapidjson::Value(options.model_id.c_str(), allocator),
      allocator);
  document.AddMember("temperature", 0.0, allocator);
  document.AddMember("max_tokens", options.max_output_tokens, allocator);
  if(!options.thinking_type.empty()) {
    rapidjson::Value thinking(rapidjson::kObjectType);
    thinking.AddMember(
        "type",
        rapidjson::Value(options.thinking_type.c_str(), allocator),
        allocator);
    document.AddMember("thinking", thinking, allocator);
  }

  rapidjson::Value messages(rapidjson::kArrayType);
  rapidjson::Value system_message(rapidjson::kObjectType);
  system_message.AddMember("role", "system", allocator);
  system_message.AddMember(
      "content",
      rapidjson::Value(request.system_prompt.c_str(), allocator),
      allocator);
  messages.PushBack(system_message, allocator);

  rapidjson::Value user_message(rapidjson::kObjectType);
  user_message.AddMember("role", "user", allocator);
  user_message.AddMember(
      "content",
      rapidjson::Value(request.user_prompt.c_str(), allocator),
      allocator);
  messages.PushBack(user_message, allocator);
  document.AddMember("messages", messages, allocator);
  return serialize_json(document);
}

struct ResponseBuffer {
  std::string bytes;
  std::size_t limit = 0;
  bool exceeded = false;
};

std::size_t append_response(
    char* data,
    std::size_t size,
    std::size_t count,
    void* userdata) {
  auto* buffer = static_cast<ResponseBuffer*>(userdata);
  if(size != 0 && count > std::numeric_limits<std::size_t>::max() / size) {
    buffer->exceeded = true;
    return 0;
  }
  const std::size_t bytes = size * count;
  if(bytes > buffer->limit || buffer->bytes.size() > buffer->limit - bytes) {
    buffer->exceeded = true;
    return 0;
  }
  buffer->bytes.append(data, bytes);
  return bytes;
}

bool retryable_http_status(long status) {
  return status == 408 || status == 429 || status == 500 || status == 502
      || status == 503 || status == 504;
}

bool retryable_curl_error(CURLcode code) {
  return code == CURLE_COULDNT_RESOLVE_HOST
      || code == CURLE_COULDNT_CONNECT
      || code == CURLE_OPERATION_TIMEDOUT
      || code == CURLE_RECV_ERROR
      || code == CURLE_SEND_ERROR
      || code == CURLE_GOT_NOTHING;
}

std::string extract_model_content(
    const std::string& body,
    std::string* finish_reason) {
  finish_reason->clear();
  rapidjson::Document document;
  document.Parse(body.data(), body.size());
  if(document.HasParseError() || !document.IsObject()) {
    return {};
  }
  const JsonValue* choices = find_member(document, "choices");
  if(choices == nullptr || !choices->IsArray() || choices->Empty()) {
    return {};
  }
  const JsonValue& choice = (*choices)[0];
  const JsonValue* finish = find_member(choice, "finish_reason");
  if(finish != nullptr && finish->IsString()) {
    finish_reason->assign(finish->GetString(), finish->GetStringLength());
  }
  const JsonValue* message = find_member(choice, "message");
  const JsonValue* content = message == nullptr
      ? nullptr
      : find_member(*message, "content");
  if(content == nullptr || !content->IsString()) {
    return {};
  }
  return std::string(content->GetString(), content->GetStringLength());
}

class OpenAiCompatibleModelAdapter final : public ModelAdapter {
 public:
  explicit OpenAiCompatibleModelAdapter(OpenAiCompatibleOptions options)
      : options_(std::move(options)) {
    validate_endpoint(options_.endpoint);
    if(options_.model_id.empty() || options_.model_id.size() > 256) {
      throw EvaluationError("model ID is empty or too long");
    }
    if(!options_.thinking_type.empty()
       && options_.thinking_type != "enabled"
       && options_.thinking_type != "disabled"
       && options_.thinking_type != "auto") {
      throw EvaluationError("model thinking mode is invalid");
    }
    const auto curl_timeout_max = static_cast<unsigned long long>(
        std::numeric_limits<long>::max());
    if(options_.timeout_ms == 0
       || static_cast<unsigned long long>(options_.timeout_ms) > curl_timeout_max
       || options_.connect_timeout_ms == 0
       || static_cast<unsigned long long>(options_.connect_timeout_ms)
           > curl_timeout_max) {
      throw EvaluationError("model timeout is outside the supported range");
    }
    if(options_.max_retries > 10 || options_.max_output_tokens == 0
       || options_.max_output_tokens > 32768) {
      throw EvaluationError("model retry or output-token limit is invalid");
    }
    if(options_.max_response_bytes == 0
       || options_.max_response_bytes > 8U * 1024U * 1024U) {
      throw EvaluationError("model response byte limit is invalid");
    }
    if(options_.max_request_bytes == 0
       || options_.max_request_bytes > 8U * 1024U * 1024U) {
      throw EvaluationError("model request byte limit is invalid");
    }
    initialize_curl();
  }

  RawModelResponse evaluate(const ModelRequest& request) override {
    RawModelResponse response;
    response.adapter = name();
    response.model_id = model_id();
    const std::string request_body = make_openai_request_body(request, options_);
    if(request_body.size() > options_.max_request_bytes) {
      response.error_code = "request_too_large";
      return response;
    }

    const std::uint32_t max_attempts = options_.max_retries + 1U;
    for(std::uint32_t attempt = 1; attempt <= max_attempts; ++attempt) {
      response.attempts = attempt;
      ResponseBuffer buffer;
      buffer.limit = options_.max_response_bytes;

      std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> handle(
          curl_easy_init(),
          &curl_easy_cleanup);
      if(!handle) {
        response.error_code = "client_initialization_failed";
        return response;
      }

      curl_slist* header_list = nullptr;
      header_list = curl_slist_append(header_list, "Content-Type: application/json");
      if(!options_.api_key.empty()) {
        const std::string authorization = "Authorization: Bearer " + options_.api_key;
        header_list = curl_slist_append(header_list, authorization.c_str());
      }
      std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)> headers(
          header_list,
          &curl_slist_free_all);
      if(!headers) {
        response.error_code = "header_allocation_failed";
        return response;
      }

      curl_easy_setopt(handle.get(), CURLOPT_URL, options_.endpoint.c_str());
      curl_easy_setopt(handle.get(), CURLOPT_HTTPHEADER, headers.get());
      curl_easy_setopt(handle.get(), CURLOPT_POST, 1L);
      curl_easy_setopt(handle.get(), CURLOPT_POSTFIELDS, request_body.data());
      curl_easy_setopt(
          handle.get(), CURLOPT_POSTFIELDSIZE_LARGE,
          static_cast<curl_off_t>(request_body.size()));
      curl_easy_setopt(handle.get(), CURLOPT_WRITEFUNCTION, append_response);
      curl_easy_setopt(handle.get(), CURLOPT_WRITEDATA, &buffer);
      curl_easy_setopt(handle.get(), CURLOPT_NOSIGNAL, 1L);
      if(options_.cancelled) {
        curl_easy_setopt(handle.get(), CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(handle.get(), CURLOPT_XFERINFODATA, &options_.cancelled);
        curl_easy_setopt(handle.get(), CURLOPT_XFERINFOFUNCTION,
            +[](void* context, curl_off_t, curl_off_t, curl_off_t, curl_off_t) -> int {
              try { return (*static_cast<std::function<bool()>*>(context))() ? 1 : 0; }
              catch(...) { return 1; }
            });
      }
      curl_easy_setopt(handle.get(), CURLOPT_FOLLOWLOCATION, 0L);
      curl_easy_setopt(handle.get(), CURLOPT_MAXREDIRS, 0L);
      curl_easy_setopt(handle.get(), CURLOPT_SSL_VERIFYPEER, 1L);
      curl_easy_setopt(handle.get(), CURLOPT_SSL_VERIFYHOST, 2L);
      curl_easy_setopt(
          handle.get(), CURLOPT_TIMEOUT_MS,
          static_cast<long>(options_.timeout_ms));
      curl_easy_setopt(
          handle.get(), CURLOPT_CONNECTTIMEOUT_MS,
          static_cast<long>(options_.connect_timeout_ms));
#if LIBCURL_VERSION_NUM >= 0x075500
      curl_easy_setopt(handle.get(), CURLOPT_PROTOCOLS_STR, "http,https");
#endif
      if(options_.disable_proxy) {
        curl_easy_setopt(handle.get(), CURLOPT_PROXY, "");
        curl_easy_setopt(handle.get(), CURLOPT_NOPROXY, "*");
      }

      const CURLcode curl_result = curl_easy_perform(handle.get());
      long http_status = 0;
      curl_easy_getinfo(handle.get(), CURLINFO_RESPONSE_CODE, &http_status);

      if(curl_result == CURLE_OK && http_status >= 200 && http_status < 300) {
        response.transport_ok = true;
        std::string finish_reason;
        response.body = extract_model_content(buffer.bytes, &finish_reason);
        if(finish_reason == "length") {
          response.error_code = "output_token_limit_reached";
        } else if(response.body.empty()) {
          response.error_code = "invalid_api_response";
        }
        return response;
      }

      const bool retry = attempt < max_attempts
          && ((curl_result != CURLE_OK && retryable_curl_error(curl_result))
              || retryable_http_status(http_status));
      if(!retry) {
        if(buffer.exceeded) {
          response.error_code = "response_too_large";
        } else if(curl_result != CURLE_OK) {
          response.error_code = "transport_" + std::to_string(curl_result);
        } else {
          response.error_code = "http_" + std::to_string(http_status);
        }
        return response;
      }
      std::this_thread::sleep_for(
          std::chrono::milliseconds(100U * attempt));
    }
    response.error_code = "transport_exhausted";
    return response;
  }

  std::string name() const override {
    return "openai-compatible-http";
  }

  std::string model_id() const override {
    return options_.model_id;
  }

 private:
  OpenAiCompatibleOptions options_;
};

}  // namespace

std::shared_ptr<ModelAdapter> make_jsonl_model_adapter(
    const std::filesystem::path& path,
    std::size_t max_file_bytes) {
  return std::make_shared<JsonlModelAdapter>(path, max_file_bytes);
}

std::shared_ptr<ModelAdapter> make_openai_compatible_model_adapter(
    const OpenAiCompatibleOptions& options) {
  return std::make_shared<OpenAiCompatibleModelAdapter>(options);
}

}  // namespace agentflow::trajectory
