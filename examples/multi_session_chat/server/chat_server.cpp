// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <csignal>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "agent/agent.h"
#include "runtime/agent_runtime.h"
#include "runtime/databus/databus.h"

namespace {

using namespace agentflow;

std::string json_escape(const std::string& input) {
  std::string out;
  out.reserve(input.size() + 8);
  for (char c : input) {
    switch (c) {
      case '\\':
        out += "\\\\";
        break;
      case '"':
        out += "\\\"";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        out += c;
        break;
    }
  }
  return out;
}

std::string utc_iso8601_now() {
  auto now = std::chrono::system_clock::now();
  auto t = std::chrono::system_clock::to_time_t(now);
  std::tm tm{};
  gmtime_r(&t, &tm);
  std::ostringstream oss;
  oss << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
  return oss.str();
}

std::string sanitize_session_id(const std::string& session_id) {
  std::string safe = session_id;
  for (char& c : safe) {
    if (c == '/' || c == '\\' || c == ':') {
      c = '_';
    }
  }
  return safe;
}

class ConversationStore {
 public:
  explicit ConversationStore(const std::string& root) : root_(root) {
    std::filesystem::create_directories(root_);
  }

  void append(const std::string& session_id, const std::string& role, const std::string& content) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::ofstream out(file_for(session_id), std::ios::app);
    out << "{\"ts\":\"" << utc_iso8601_now() << "\",\"session_id\":\"" << json_escape(session_id)
        << "\",\"role\":\"" << json_escape(role) << "\",\"content\":\"" << json_escape(content)
        << "\"}\n";
  }

  std::string recent_as_json(const std::string& session_id, size_t limit) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::ifstream in(file_for(session_id));
    if (!in.is_open()) {
      return "[]";
    }
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(in, line)) {
      if (!line.empty()) {
        lines.push_back(line);
      }
    }
    if (lines.empty()) {
      return "[]";
    }
    size_t start = lines.size() > limit ? (lines.size() - limit) : 0;
    std::ostringstream oss;
    oss << "[";
    for (size_t i = start; i < lines.size(); ++i) {
      if (i > start) {
        oss << ",";
      }
      oss << lines[i];
    }
    oss << "]";
    return oss.str();
  }

  bool clear_session(const std::string& session_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::error_code ec;
    return std::filesystem::remove(file_for(session_id), ec) || !std::filesystem::exists(file_for(session_id));
  }

 private:
  std::filesystem::path file_for(const std::string& session_id) const {
    return root_ / (sanitize_session_id(session_id) + ".jsonl");
  }

  std::filesystem::path root_;
  std::mutex mutex_;
};

class ChatAgentBridge {
 public:
  explicit ChatAgentBridge(const std::string& package_path) : package_path_(package_path) {}

  ~ChatAgentBridge() {
    if (runtime_) {
      runtime_->stop();
    }
    if (agent_) {
      agent_->stop();
    }
  }

  bool initialize() {
    agent_ = framework::MMAgent::create_from_package(package_path_);
    if (!agent_ || !agent_->initialize()) {
      return false;
    }
    runtime_ = std::make_unique<runtime::MMAgentRuntime>();
    if (!runtime_->register_agent(agent_) || !runtime_->start() || !agent_->start()) {
      return false;
    }
    return true;
  }

  const std::string& package_path() const { return package_path_; }

  std::string execute(const std::string& workflow_id, const std::string& payload) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto& bus = runtime_->get_databus(agent_->get_id());
    for (const std::string& key : {"final_response", "response", "output", "result", "stdout",
                                   "default-dispatcher-workflow.reply.stdout",
                                   "default-dispatcher-workflow.pipeline.stdout",
                                   "default-coordinator-workflow.reply.stdout"}) {
      bus.remove(key);
    }
    std::filesystem::remove(std::filesystem::path(package_path_) / "runtime" / "final_response.txt");
    DataMap inputs;
    inputs["input"] = payload;
    if (!runtime_->execute_workflow(agent_->get_id(), workflow_id, inputs)) {
      return "[error] failed to execute workflow";
    }
    int max_wait_sec = 45;
    if (const char* timeout_env = std::getenv("CHAT_WORKFLOW_TIMEOUT_SEC"); timeout_env && *timeout_env) {
      try {
        max_wait_sec = std::max(5, std::stoi(timeout_env));
      } catch (...) {
      }
    }
    auto start = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - start < std::chrono::seconds(max_wait_sec)) {
      for (const std::string& key : {"final_response", "response", "output", "result", "stdout",
                                     "default-dispatcher-workflow.reply.stdout",
                                     "default-dispatcher-workflow.pipeline.stdout",
                                     "default-coordinator-workflow.reply.stdout"}) {
        DataVariant value;
        if (bus.get(key, value) && std::holds_alternative<std::string>(value)) {
          std::string text = std::get<std::string>(value);
          if (!text.empty()) {
            return text;
          }
        }
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    {
      std::ifstream in(std::filesystem::path(package_path_) / "runtime" / "final_response.txt");
      std::string from_file;
      if (in.is_open() && std::getline(in, from_file) && !from_file.empty()) {
        return from_file;
      }
    }
    return "[error] workflow timeout or no response produced";
  }

 private:
  std::string package_path_;
  std::shared_ptr<framework::MMAgent> agent_;
  std::unique_ptr<runtime::MMAgentRuntime> runtime_;
  std::mutex mutex_;
};

uint32_t rol(uint32_t value, uint32_t bits) {
  return (value << bits) | (value >> (32 - bits));
}

std::string sha1(const std::string& input) {
  std::vector<uint8_t> data(input.begin(), input.end());
  uint64_t bit_len = static_cast<uint64_t>(data.size()) * 8;
  data.push_back(0x80);
  while ((data.size() % 64) != 56) {
    data.push_back(0);
  }
  for (int i = 7; i >= 0; --i) {
    data.push_back(static_cast<uint8_t>((bit_len >> (i * 8)) & 0xFF));
  }

  uint32_t h0 = 0x67452301;
  uint32_t h1 = 0xEFCDAB89;
  uint32_t h2 = 0x98BADCFE;
  uint32_t h3 = 0x10325476;
  uint32_t h4 = 0xC3D2E1F0;

  for (size_t chunk = 0; chunk < data.size(); chunk += 64) {
    uint32_t w[80] = {0};
    for (int i = 0; i < 16; ++i) {
      w[i] = (static_cast<uint32_t>(data[chunk + i * 4]) << 24) |
             (static_cast<uint32_t>(data[chunk + i * 4 + 1]) << 16) |
             (static_cast<uint32_t>(data[chunk + i * 4 + 2]) << 8) |
             static_cast<uint32_t>(data[chunk + i * 4 + 3]);
    }
    for (int i = 16; i < 80; ++i) {
      w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    }

    uint32_t a = h0, b = h1, c = h2, d = h3, e = h4;
    for (int i = 0; i < 80; ++i) {
      uint32_t f = 0;
      uint32_t k = 0;
      if (i < 20) {
        f = (b & c) | ((~b) & d);
        k = 0x5A827999;
      } else if (i < 40) {
        f = b ^ c ^ d;
        k = 0x6ED9EBA1;
      } else if (i < 60) {
        f = (b & c) | (b & d) | (c & d);
        k = 0x8F1BBCDC;
      } else {
        f = b ^ c ^ d;
        k = 0xCA62C1D6;
      }
      uint32_t temp = rol(a, 5) + f + e + k + w[i];
      e = d;
      d = c;
      c = rol(b, 30);
      b = a;
      a = temp;
    }
    h0 += a;
    h1 += b;
    h2 += c;
    h3 += d;
    h4 += e;
  }

  std::string out(20, '\0');
  uint32_t hs[5] = {h0, h1, h2, h3, h4};
  for (int i = 0; i < 5; ++i) {
    out[i * 4] = static_cast<char>((hs[i] >> 24) & 0xFF);
    out[i * 4 + 1] = static_cast<char>((hs[i] >> 16) & 0xFF);
    out[i * 4 + 2] = static_cast<char>((hs[i] >> 8) & 0xFF);
    out[i * 4 + 3] = static_cast<char>(hs[i] & 0xFF);
  }
  return out;
}

std::string base64_encode(const std::string& input) {
  static const char* table =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  size_t i = 0;
  while (i + 2 < input.size()) {
    uint32_t triple = (static_cast<uint8_t>(input[i]) << 16) |
                      (static_cast<uint8_t>(input[i + 1]) << 8) |
                      static_cast<uint8_t>(input[i + 2]);
    out += table[(triple >> 18) & 0x3F];
    out += table[(triple >> 12) & 0x3F];
    out += table[(triple >> 6) & 0x3F];
    out += table[triple & 0x3F];
    i += 3;
  }
  if (i < input.size()) {
    uint32_t triple = static_cast<uint8_t>(input[i]) << 16;
    out += table[(triple >> 18) & 0x3F];
    if (i + 1 < input.size()) {
      triple |= static_cast<uint8_t>(input[i + 1]) << 8;
      out += table[(triple >> 12) & 0x3F];
      out += table[(triple >> 6) & 0x3F];
      out += '=';
    } else {
      out += table[(triple >> 12) & 0x3F];
      out += '=';
      out += '=';
    }
  }
  return out;
}

bool recv_exact(int fd, void* buf, size_t len) {
  uint8_t* p = static_cast<uint8_t*>(buf);
  size_t read_total = 0;
  while (read_total < len) {
    ssize_t n = recv(fd, p + read_total, len - read_total, 0);
    if (n <= 0) {
      return false;
    }
    read_total += static_cast<size_t>(n);
  }
  return true;
}

bool send_all(int fd, const void* buf, size_t len) {
  const uint8_t* p = static_cast<const uint8_t*>(buf);
  size_t written = 0;
  while (written < len) {
    ssize_t n = send(fd, p + written, len - written, 0);
    if (n <= 0) {
      return false;
    }
    written += static_cast<size_t>(n);
  }
  return true;
}

std::string get_header_value(const std::string& headers, const std::string& name) {
  std::istringstream iss(headers);
  std::string line;
  std::string prefix = name + ":";
  while (std::getline(iss, line)) {
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    if (line.rfind(prefix, 0) == 0) {
      size_t pos = line.find(':');
      std::string value = line.substr(pos + 1);
      value.erase(value.begin(), std::find_if(value.begin(), value.end(), [](unsigned char ch) {
                    return !std::isspace(ch);
                  }));
      return value;
    }
  }
  return "";
}

std::string extract_json_string(const std::string& json, const std::string& key) {
  std::string k = "\"" + key + "\"";
  size_t key_pos = json.find(k);
  if (key_pos == std::string::npos) {
    return "";
  }
  size_t colon = json.find(':', key_pos + k.size());
  if (colon == std::string::npos) {
    return "";
  }
  size_t start = json.find('"', colon + 1);
  if (start == std::string::npos) {
    return "";
  }
  std::string out;
  bool escaped = false;
  for (size_t i = start + 1; i < json.size(); ++i) {
    char c = json[i];
    if (escaped) {
      out += c;
      escaped = false;
      continue;
    }
    if (c == '\\') {
      escaped = true;
      continue;
    }
    if (c == '"') {
      return out;
    }
    out += c;
  }
  return "";
}

std::string mime_type_for(const std::string& path) {
  if (path.size() >= 5 && path.substr(path.size() - 5) == ".html") return "text/html; charset=utf-8";
  if (path.size() >= 3 && path.substr(path.size() - 3) == ".js") return "application/javascript";
  if (path.size() >= 4 && path.substr(path.size() - 4) == ".css") return "text/css";
  return "text/plain; charset=utf-8";
}

std::string url_decode(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '%' && i + 2 < s.size()) {
      int hi = std::isdigit(s[i + 1]) ? s[i + 1] - '0' : (std::tolower(s[i + 1]) - 'a' + 10);
      int lo = std::isdigit(s[i + 2]) ? s[i + 2] - '0' : (std::tolower(s[i + 2]) - 'a' + 10);
      if (hi >= 0 && hi < 16 && lo >= 0 && lo < 16) {
        out.push_back(static_cast<char>(hi * 16 + lo));
        i += 2;
        continue;
      }
    }
    out.push_back(s[i] == '+' ? ' ' : s[i]);
  }
  return out;
}

int create_listen_socket(const std::string& host, int port) {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    return -1;
  }
  int opt = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(static_cast<uint16_t>(port));
  if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) <= 0) {
    close(fd);
    return -1;
  }
  if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
    close(fd);
    return -1;
  }
  if (listen(fd, 16) < 0) {
    close(fd);
    return -1;
  }
  return fd;
}

void run_http_server(const std::string& host, int port, const std::filesystem::path& web_root,
                     ChatAgentBridge& bridge, ConversationStore& store) {
  int server_fd = create_listen_socket(host, port);
  if (server_fd < 0) {
    std::cerr << "Failed to start HTTP server on " << host << ":" << port << std::endl;
    return;
  }
  while (true) {
    int client = accept(server_fd, nullptr, nullptr);
    if (client < 0) {
      continue;
    }
    char buffer[4096];
    ssize_t n = recv(client, buffer, sizeof(buffer) - 1, 0);
    if (n <= 0) {
      close(client);
      continue;
    }
    buffer[n] = '\0';
    std::string headers(buffer, static_cast<size_t>(n));
    size_t header_end = headers.find("\r\n\r\n");
    if (header_end == std::string::npos) {
      close(client);
      continue;
    }
    std::string header_only = headers.substr(0, header_end + 4);
    size_t content_len = 0;
    std::string lower_headers = header_only;
    std::transform(lower_headers.begin(), lower_headers.end(), lower_headers.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    size_t cl_pos = lower_headers.find("content-length:");
    if (cl_pos != std::string::npos) {
      size_t cl_end = lower_headers.find("\r\n", cl_pos);
      std::string len_str = lower_headers.substr(cl_pos + 15, cl_end - (cl_pos + 15));
      len_str.erase(std::remove_if(len_str.begin(), len_str.end(),
                                   [](unsigned char ch) { return std::isspace(ch); }),
                    len_str.end());
      if (!len_str.empty()) {
        content_len = static_cast<size_t>(std::stoul(len_str));
      }
    }
    std::string body_bytes = headers.substr(header_end + 4);
    while (body_bytes.size() < content_len) {
      ssize_t m = recv(client, buffer, sizeof(buffer), 0);
      if (m <= 0) {
        break;
      }
      body_bytes.append(buffer, static_cast<size_t>(m));
    }
    headers = header_only + body_bytes;
    std::istringstream req(header_only);
    std::string method, uri, version;
    req >> method >> uri >> version;
    std::string body = body_bytes;
    if (method == "POST" && uri == "/api/chat") {
      auto json_field = [&](const std::string& key) {
        std::string k = "\"" + key + "\"";
        size_t p = body.find(k);
        if (p == std::string::npos) return std::string();
        size_t c = body.find(':', p + k.size());
        if (c == std::string::npos) return std::string();
        size_t s = body.find('"', c + 1);
        if (s == std::string::npos) return std::string();
        std::string out;
        bool escaped = false;
        for (size_t i = s + 1; i < body.size(); ++i) {
          char ch = body[i];
          if (escaped) {
            out += ch;
            escaped = false;
            continue;
          }
          if (ch == '\\') {
            escaped = true;
            continue;
          }
          if (ch == '"') return out;
          out += ch;
        }
        return std::string();
      };
      std::string session_id = json_field("session_id");
      if (session_id.empty()) session_id = "webui:default";
      std::string text = json_field("text");
      if (text.empty()) {
        const std::string resp =
            "HTTP/1.1 400 Bad Request\r\nContent-Type: application/json\r\nContent-Length: 18\r\n\r\n{\"error\":\"text\"}";
        send_all(client, resp.data(), resp.size());
        close(client);
        continue;
      }
      std::string workflow_id = json_field("workflow_id");
      if (workflow_id.empty()) workflow_id = "default-dispatcher-workflow";
      store.append(session_id, "user", text);
      {
        std::filesystem::create_directories(std::filesystem::path(bridge.package_path()) / "runtime");
        std::ofstream latest(std::filesystem::path(bridge.package_path()) / "runtime" /
                                 "latest_user_input.txt",
                             std::ios::trunc);
        latest << text;
        std::ofstream latest_sid(std::filesystem::path(bridge.package_path()) / "runtime" /
                                     "latest_session_id.txt",
                                 std::ios::trunc);
        latest_sid << session_id;
      }
      std::string history = store.recent_as_json(session_id, 12);
      std::string payload = "session=" + session_id + "\nuser=" + text + "\nhistory=" + history;
      std::string result = bridge.execute(workflow_id, payload);
      store.append(session_id, "assistant", result);
      std::ostringstream payload_json;
      payload_json << "{\"type\":\"message\",\"role\":\"assistant\",\"session_id\":\""
                   << json_escape(session_id) << "\",\"text\":\"" << json_escape(result) << "\"}";
      auto json = payload_json.str();
      std::ostringstream resp;
      resp << "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " << json.size()
           << "\r\n\r\n" << json;
      auto s = resp.str();
      send_all(client, s.data(), s.size());
      close(client);
      continue;
    }
    if ((method == "GET" || method == "DELETE") && uri.rfind("/api/history", 0) == 0) {
      std::string session_id = "webui:default";
      size_t limit = 50;
      size_t q = uri.find('?');
      if (q != std::string::npos) {
        std::istringstream qs(uri.substr(q + 1));
        std::string pair;
        while (std::getline(qs, pair, '&')) {
          size_t eq = pair.find('=');
          if (eq == std::string::npos) continue;
          std::string k = pair.substr(0, eq);
          std::string v = url_decode(pair.substr(eq + 1));
          if (k == "session_id" && !v.empty()) {
            session_id = v;
          } else if (k == "limit" && !v.empty()) {
            limit = static_cast<size_t>(std::stoul(v));
          }
        }
      }
      std::string json;
      if (method == "DELETE") {
        bool ok = store.clear_session(session_id);
        json = ok ? "{\"ok\":true}" : "{\"ok\":false}";
      } else {
        json = store.recent_as_json(session_id, limit);
      }
      std::ostringstream resp;
      resp << "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " << json.size()
           << "\r\n\r\n" << json;
      auto s = resp.str();
      send_all(client, s.data(), s.size());
      close(client);
      continue;
    }
    if (method != "GET") {
      const std::string resp = "HTTP/1.1 405 Method Not Allowed\r\nContent-Length: 0\r\n\r\n";
      send_all(client, resp.data(), resp.size());
      close(client);
      continue;
    }
    if (uri == "/") {
      uri = "/index.html";
    }
    if (uri.find("..") != std::string::npos) {
      const std::string resp = "HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\n\r\n";
      send_all(client, resp.data(), resp.size());
      close(client);
      continue;
    }
    auto file_path = web_root / uri.substr(1);
    if (!std::filesystem::exists(file_path) || !std::filesystem::is_regular_file(file_path)) {
      const std::string body = "Not Found\n";
      std::ostringstream resp;
      resp << "HTTP/1.1 404 Not Found\r\nContent-Type: text/plain\r\nContent-Length: " << body.size()
           << "\r\n\r\n" << body;
      auto s = resp.str();
      send_all(client, s.data(), s.size());
      close(client);
      continue;
    }
    std::ifstream in(file_path, std::ios::binary);
    std::string file_body((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::ostringstream resp;
    resp << "HTTP/1.1 200 OK\r\nContent-Type: " << mime_type_for(file_path.string())
         << "\r\nContent-Length: " << file_body.size() << "\r\n\r\n";
    auto head = resp.str();
    send_all(client, head.data(), head.size());
    send_all(client, file_body.data(), file_body.size());
    close(client);
  }
}

bool read_ws_text_frame(int fd, std::string& payload) {
  uint8_t header[2];
  if (!recv_exact(fd, header, 2)) return false;
  uint8_t opcode = header[0] & 0x0F;
  bool masked = (header[1] & 0x80) != 0;
  uint64_t len = header[1] & 0x7F;
  if (len == 126) {
    uint8_t ext[2];
    if (!recv_exact(fd, ext, 2)) return false;
    len = (static_cast<uint64_t>(ext[0]) << 8) | ext[1];
  } else if (len == 127) {
    uint8_t ext[8];
    if (!recv_exact(fd, ext, 8)) return false;
    len = 0;
    for (int i = 0; i < 8; ++i) {
      len = (len << 8) | ext[i];
    }
  }
  uint8_t mask[4] = {0, 0, 0, 0};
  if (masked && !recv_exact(fd, mask, 4)) return false;
  std::string data(len, '\0');
  if (len > 0 && !recv_exact(fd, data.data(), len)) return false;
  if (masked) {
    for (uint64_t i = 0; i < len; ++i) {
      data[static_cast<size_t>(i)] ^= mask[i % 4];
    }
  }
  if (opcode == 0x8) {
    return false;
  }
  if (opcode != 0x1) {
    payload.clear();
    return true;
  }
  payload = data;
  return true;
}

bool send_ws_text_frame(int fd, const std::string& payload) {
  std::vector<uint8_t> frame;
  frame.push_back(0x81);
  if (payload.size() < 126) {
    frame.push_back(static_cast<uint8_t>(payload.size()));
  } else if (payload.size() <= 0xFFFF) {
    frame.push_back(126);
    frame.push_back(static_cast<uint8_t>((payload.size() >> 8) & 0xFF));
    frame.push_back(static_cast<uint8_t>(payload.size() & 0xFF));
  } else {
    frame.push_back(127);
    for (int i = 7; i >= 0; --i) {
      frame.push_back(static_cast<uint8_t>((payload.size() >> (i * 8)) & 0xFF));
    }
  }
  frame.insert(frame.end(), payload.begin(), payload.end());
  return send_all(fd, frame.data(), frame.size());
}

void handle_ws_client(int client_fd, ChatAgentBridge& bridge, ConversationStore& store) {
  std::string headers;
  char buf[4096];
  while (headers.find("\r\n\r\n") == std::string::npos) {
    ssize_t n = recv(client_fd, buf, sizeof(buf), 0);
    if (n <= 0) {
      close(client_fd);
      return;
    }
    headers.append(buf, buf + n);
    if (headers.size() > 16384) {
      close(client_fd);
      return;
    }
  }
  auto key = get_header_value(headers, "Sec-WebSocket-Key");
  if (key.empty()) {
    close(client_fd);
    return;
  }
  std::string accept =
      base64_encode(sha1(key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"));
  std::ostringstream hs;
  hs << "HTTP/1.1 101 Switching Protocols\r\n"
     << "Upgrade: websocket\r\n"
     << "Connection: Upgrade\r\n"
     << "Sec-WebSocket-Accept: " << accept << "\r\n\r\n";
  auto hs_str = hs.str();
  if (!send_all(client_fd, hs_str.data(), hs_str.size())) {
    close(client_fd);
    return;
  }

  send_ws_text_frame(client_fd, "{\"type\":\"ready\",\"message\":\"connected\"}");
  while (true) {
    std::string request;
    if (!read_ws_text_frame(client_fd, request)) {
      break;
    }
    if (request.empty()) {
      continue;
    }
    std::string session_id = extract_json_string(request, "session_id");
    if (session_id.empty()) session_id = "webui:default";
    std::string text = extract_json_string(request, "text");
    if (text.empty()) {
      continue;
    }
    std::string workflow_id = extract_json_string(request, "workflow_id");
    if (workflow_id.empty()) workflow_id = "default-dispatcher-workflow";

    store.append(session_id, "user", text);
    {
      std::filesystem::create_directories(std::filesystem::path(bridge.package_path()) / "runtime");
      std::ofstream latest(std::filesystem::path(bridge.package_path()) / "runtime" /
                               "latest_user_input.txt",
                           std::ios::trunc);
      latest << text;
      std::ofstream latest_sid(std::filesystem::path(bridge.package_path()) / "runtime" /
                                   "latest_session_id.txt",
                               std::ios::trunc);
      latest_sid << session_id;
    }
    {
      std::ostringstream user_evt;
      user_evt << "{\"type\":\"message\",\"role\":\"user\",\"session_id\":\"" << json_escape(session_id)
               << "\",\"text\":\"" << json_escape(text) << "\"}";
      if (!send_ws_text_frame(client_fd, user_evt.str())) {
        break;
      }
    }
    std::string history = store.recent_as_json(session_id, 12);
    std::string payload = "session=" + session_id + "\nuser=" + text + "\nhistory=" + history;
    std::string result = bridge.execute(workflow_id, payload);
    store.append(session_id, "assistant", result);

    std::ostringstream response;
    response << "{\"type\":\"message\",\"role\":\"assistant\",\"session_id\":\"" << json_escape(session_id)
             << "\",\"text\":\"" << json_escape(result) << "\"}";
    if (!send_ws_text_frame(client_fd, response.str())) {
      break;
    }
  }
  close(client_fd);
}

void run_ws_server(const std::string& host, int port, ChatAgentBridge& bridge, ConversationStore& store) {
  int server_fd = create_listen_socket(host, port);
  if (server_fd < 0) {
    std::cerr << "Failed to start WebSocket server on " << host << ":" << port << std::endl;
    return;
  }
  while (true) {
    int client = accept(server_fd, nullptr, nullptr);
    if (client < 0) {
      continue;
    }
    std::thread(handle_ws_client, client, std::ref(bridge), std::ref(store)).detach();
  }
}

struct Config {
  std::string package_path = "examples/multi_session_chat/package";
  std::string host = "127.0.0.1";
  int http_port = 8088;
  int ws_port = 8765;
  std::string history_dir = "examples/multi_session_chat/package/history";
  std::string web_root = "examples/multi_session_chat/server/webui";
};

Config parse_args(int argc, char* argv[]) {
  Config cfg;
  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    auto consume = [&](std::string& target) {
      if (i + 1 < argc) target = argv[++i];
    };
    if (arg == "--package") {
      consume(cfg.package_path);
    } else if (arg == "--host") {
      consume(cfg.host);
    } else if (arg == "--history-dir") {
      consume(cfg.history_dir);
    } else if (arg == "--web-root") {
      consume(cfg.web_root);
    } else if (arg == "--http-port" && i + 1 < argc) {
      cfg.http_port = std::stoi(argv[++i]);
    } else if (arg == "--ws-port" && i + 1 < argc) {
      cfg.ws_port = std::stoi(argv[++i]);
    }
  }
  return cfg;
}

}  // namespace

int main(int argc, char* argv[]) {
  std::signal(SIGPIPE, SIG_IGN);
  auto config = parse_args(argc, argv);

  ChatAgentBridge bridge(config.package_path);
  if (!bridge.initialize()) {
    std::cerr << "Failed to initialize agent/runtime from package: " << config.package_path << std::endl;
    return 1;
  }

  ConversationStore store(config.history_dir);
  std::thread http_thread(run_http_server, config.host, config.http_port,
                          std::filesystem::path(config.web_root), std::ref(bridge), std::ref(store));
  run_ws_server(config.host, config.ws_port, bridge, store);
  http_thread.join();
  return 0;
}
