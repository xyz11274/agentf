#ifndef COREMIND_BRIDGE_UDS_MODEL_PROXY_H
#define COREMIND_BRIDGE_UDS_MODEL_PROXY_H

#include <atomic>
#include <string>
#include <thread>

/// UDS 模型代理 — 接收 Node 的 LLM 推理请求并转发。
///
/// 两种转发路径：
///   - 生产（coremind_mode=true）：通过 MicroserviceManager 解析 IMInferScheduler
///     地址，转发到 http://{ip}:27321/infermind/llm/v1
///   - 本地开发（coremind_mode=false）：转发到 --llm-api-url 指定的 OpenAI 兼容 API
class UdsModelProxy {
public:
    UdsModelProxy();
    ~UdsModelProxy();

    /// 启动 UDS 监听线程。不阻塞。
    /// @param socket_path  UDS socket 路径，如 /tmp/aisf_plugin.sock
    /// @param llm_api_url OpenAI 兼容 API 的 base URL（dev 模式使用）
    /// @param coremind_mode 是否启用 CoreMind 生产路径（解析 IMInferScheduler）
    bool start(const std::string& socket_path, const std::string& llm_api_url,
               bool coremind_mode = false);

    /// 停止 UDS 监听线程，清理 socket 文件。
    void stop();

    /// 是否正在运行。
    bool running() const { return running_; }

private:
    void run_loop();
    void handle_client(int client_fd);
    void handle_llm_chat(int client_fd, const std::string& request_id,
                         const std::string& body, bool stream);
    void send_frame(int client_fd, const std::string& json_data);
    bool recv_frame(int client_fd, std::string& out_json);

    std::atomic<bool> running_{false};
    std::string socket_path_;
    std::string llm_api_url_;
    bool coremind_mode_{false};
    int server_fd_{-1};
    std::thread listener_thread_;
};

#endif // COREMIND_BRIDGE_UDS_MODEL_PROXY_H