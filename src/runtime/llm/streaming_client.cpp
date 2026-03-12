#include "runtime/llm/streaming_client.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <system_error>
#include <sstream>

namespace agentflow {
namespace runtime {

namespace {

class TempFileGuard {
 public:
  explicit TempFileGuard(std::filesystem::path path) : path_(std::move(path)) {}

  ~TempFileGuard() {
    if(path_.empty()) {
      return;
    }
    std::error_code ec;
    std::filesystem::remove(path_, ec);
  }

 private:
  std::filesystem::path path_;
};

class PipeGuard {
 public:
  explicit PipeGuard(FILE* pipe) : pipe_(pipe) {}

  ~PipeGuard() {
    if(pipe_ != nullptr) {
      pclose(pipe_);
    }
  }

  FILE* get() const { return pipe_; }

  explicit operator bool() const { return pipe_ != nullptr; }

  int close() {
    FILE* raw = pipe_;
    pipe_     = nullptr;
    return raw != nullptr ? pclose(raw) : 0;
  }

 private:
  FILE* pipe_ = nullptr;
};

}  // namespace

bool StreamingLLMClient::stream_prompt(const std::string& prompt, const Config& config,
                                       const ChunkCallback& on_chunk,
                                       const FinishCallback& on_finish,
                                       const ErrorCallback& on_error) const {
  if(config.base_url.empty()) {
    if(on_error) {
      on_error("LLM base_url is empty");
    }
    return false;
  }

  if(config.base_url.rfind("mock://", 0) == 0) {
    std::istringstream iss(prompt);
    std::string        chunk;
    std::string        final_text;
    bool               emitted = false;
    while(iss >> chunk) {
      emitted = true;
      if(on_chunk) {
        on_chunk(chunk);
      }
      if(!final_text.empty()) {
        final_text.push_back(' ');
      }
      final_text += chunk;
    }
    if(!emitted && on_chunk) {
      on_chunk(prompt);
      final_text = prompt;
    }
    if(on_finish) {
      on_finish(final_text);
    }
    return true;
  }

  const auto tmp_dir = std::filesystem::temp_directory_path();
  const auto body_path =
      tmp_dir / ("agentflow_llm_body_" + std::to_string(std::hash<std::string>{}(prompt)) + ".json");
  TempFileGuard body_file_guard(body_path);
  {
    std::ofstream body(body_path, std::ios::trunc);
    if(!body.is_open()) {
      if(on_error) {
        on_error("Failed to create streaming request body");
      }
      return false;
    }
    body << "{"
         << "\"model\":\"" << json_escape(config.model) << "\","
         << "\"stream\":true,"
         << "\"messages\":[{\"role\":\"user\",\"content\":\"" << json_escape(prompt) << "\"}]"
         << "}";
    body.close();
    if(!body) {
      if(on_error) {
        on_error("Failed to write streaming request body");
      }
      return false;
    }
  }

  std::ostringstream cmd;
  cmd << "curl -sS -N"
      << " --max-time " << (config.timeout_ms > 0 ? (config.timeout_ms / 1000) : 30)
      << " -H " << shell_quote("Content-Type: application/json");
  if(!config.api_key.empty()) {
    cmd << " -H " << shell_quote("Authorization: Bearer " + config.api_key);
  }
  cmd << " --data @" << shell_quote(body_path.string()) << " "
       << shell_quote(build_endpoint(config.base_url));

  PipeGuard pipe(popen(cmd.str().c_str(), "r"));
  if(!pipe) {
    if(on_error) {
      on_error("Failed to execute curl for streaming request");
    }
    return false;
  }

  bool        saw_stream_data = false;
  std::string final_text;
  char        buffer[4096];
  while(fgets(buffer, sizeof(buffer), pipe.get()) != nullptr) {
    std::string line(buffer);
    if(line.rfind("data:", 0) == 0) {
      saw_stream_data = true;
      std::string payload = line.substr(5);
      while(!payload.empty() && (payload.front() == ' ' || payload.front() == '\t')) {
        payload.erase(payload.begin());
      }
      while(!payload.empty() && (payload.back() == '\n' || payload.back() == '\r')) {
        payload.pop_back();
      }
      if(payload == "[DONE]") {
        continue;
      }
      const std::string chunk = extract_content_field(payload);
      if(!chunk.empty()) {
        if(on_chunk) {
          on_chunk(chunk);
        }
        final_text += chunk;
      }
      continue;
    }
    final_text += line;
  }
  const int rc = pipe.close();
  if(rc != 0) {
    if(on_error) {
      on_error("curl returned non-zero exit status");
    }
    return false;
  }
  if(!saw_stream_data && !final_text.empty()) {
    const std::string extracted = extract_content_field(final_text);
    const std::string content   = extracted.empty() ? final_text : extracted;
    if(on_chunk) {
      on_chunk(content);
    }
    final_text = content;
  }
  if(on_finish) {
    on_finish(final_text);
  }
  return true;
}

std::string StreamingLLMClient::build_endpoint(const std::string& base_url) {
  if(base_url.find("/chat/completions") != std::string::npos) {
    return base_url;
  }
  if(!base_url.empty() && base_url.back() == '/') {
    return base_url + "chat/completions";
  }
  return base_url + "/chat/completions";
}

std::string StreamingLLMClient::json_escape(const std::string& input) {
  std::string output;
  output.reserve(input.size());
  for(char ch : input) {
    switch(ch) {
      case '\\':
        output += "\\\\";
        break;
      case '"':
        output += "\\\"";
        break;
      case '\n':
        output += "\\n";
        break;
      case '\r':
        output += "\\r";
        break;
      case '\t':
        output += "\\t";
        break;
      default:
        output.push_back(ch);
        break;
    }
  }
  return output;
}

std::string StreamingLLMClient::shell_quote(const std::string& input) {
  std::string output = "'";
  for(char ch : input) {
    if(ch == '\'') {
      output += "'\\''";
    } else {
      output.push_back(ch);
    }
  }
  output.push_back('\'');
  return output;
}

std::string StreamingLLMClient::extract_content_field(const std::string& json) {
  const std::string needle = "\"content\":\"";
  size_t            pos    = json.find(needle);
  if(pos == std::string::npos) {
    return "";
  }
  pos += needle.size();
  std::string output;
  while(pos < json.size()) {
    const char ch = json[pos++];
    if(ch == '\\' && pos < json.size()) {
      const char next = json[pos++];
      switch(next) {
        case 'n':
          output.push_back('\n');
          break;
        case 'r':
          output.push_back('\r');
          break;
        case 't':
          output.push_back('\t');
          break;
        case '\\':
          output.push_back('\\');
          break;
        case '"':
          output.push_back('"');
          break;
        default:
          output.push_back(next);
          break;
      }
      continue;
    }
    if(ch == '"') {
      break;
    }
    output.push_back(ch);
  }
  return output;
}

}  // namespace runtime
}  // namespace agentflow
