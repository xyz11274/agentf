#ifndef AGENTFLOW_RUNTIME_LLM_STREAMING_CLIENT_H
#define AGENTFLOW_RUNTIME_LLM_STREAMING_CLIENT_H

#include <functional>
#include <string>

namespace agentflow {
namespace runtime {

class StreamingLLMClient {
public:
  struct Config {
    std::string base_url;
    std::string api_key;
    std::string model = "gpt-4.1";
    int         timeout_ms = 30000;
  };

  using ChunkCallback  = std::function<void(const std::string&)>;
  using FinishCallback = std::function<void(const std::string&)>;
  using ErrorCallback  = std::function<void(const std::string&)>;

  /// Parsed result from a single SSE chat-completion chunk payload.
  struct SseChunk {
    std::string content;         ///< delta.content (empty if absent)
    std::string finish_reason;   ///< "stop", "length", "content_filter", "tool_calls", or ""
  };

  /// Parse one SSE "data: <json>" payload line into an SseChunk.
  /// Uses RapidJSON when available (AGENTFLOW_WITH_RAPIDJSON); falls back to
  /// string-search for content only when RapidJSON is absent.
  static SseChunk parse_sse_payload(const std::string& json);

  bool stream_prompt(const std::string& prompt, const Config& config, const ChunkCallback& on_chunk,
                     const FinishCallback& on_finish = FinishCallback(),
                     const ErrorCallback&  on_error  = ErrorCallback()) const;

private:
  static std::string build_endpoint(const std::string& base_url);
  static std::string json_escape(const std::string& input);
  static std::string shell_quote(const std::string& input);
  static std::string extract_content_field(const std::string& json);
};

}  // namespace runtime
}  // namespace agentflow

#endif  // AGENTFLOW_RUNTIME_LLM_STREAMING_CLIENT_H
