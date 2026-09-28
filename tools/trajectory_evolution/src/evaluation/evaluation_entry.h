// 多轮输入状态机：按编号投递用户消息，并用真实 run.end 的输出续接历史。
// 回调返回只表示输入已经交付；必须等待运行结束并落盘才能解锁下一轮。
// 同一会话复用历史，每轮保持独立 run 身份；异常不伪装成正常收尾。

#pragma once
#include "integration_json.h"
#include <rapidjson/pointer.h>

namespace trajectory::evolution::integration {

// 试验内交付：派发确认不关闭运行。
// 该状态由服务互斥锁保护，此处不调用框架代码。
struct EvaluationEntry {
  Json specification, packet, messages = Json::Array{};
  std::string consumer, run_id, failure;
  std::size_t index = 0;
  bool delivered = false, done = false;

  // 展开模板或输入绑定，明确区分字面量与评测占位符。
  static Json render(const Json &pattern, const Json &values, std::size_t depth = 0) {
    require(depth <= 32, "evaluation_entry_template_too_deep");
    if (pattern.object()) {
      if (pattern.has("$eval")) {
        require(pattern.fields().size() == 1 && values.has(pattern.str("$eval")),
                "evaluation_entry_binding_invalid");
        return values[pattern.str("$eval")];
      }
      Json out = Json::Object{};
      for (const auto &p : pattern.fields())
        out[p.first] = render(p.second, values, depth + 1);
      return out;
    }
    if (pattern.array()) {
      Json out = Json::Array{};
      for (const auto &item : pattern.items())
        out.push(render(item, values, depth + 1));
      return out;
    }
    return pattern;
  }

  // 验证或读取当前对象的字段集合，拒绝与协议不符的结构。
  static Json fields() {
    return Json{{"input", nullptr},           {"messages", nullptr},    {"user_id", nullptr},
                {"conversation_id", nullptr}, {"run_key", nullptr},     {"turn", nullptr},
                {"initial_state", nullptr},   {"constraints", nullptr}, {"budget", nullptr}};
  }

  // 在进入执行前检查契约和参数，失败时返回明确错误而非隐式猜测。
  static void validate(const Json &config) {
    keys(config, {"input", "output_pointer"});
    require(config.has("input"), "evaluation_entry_input_required");
    render(config["input"], fields());
    require(uses(config["input"], "input") || uses(config["input"], "messages"),
            "evaluation_entry_task_unbound");
    const auto path = config.str("output_pointer");
    require(path.size() <= 2048 && path.find('\0') == std::string::npos &&
                rapidjson::Pointer(path.c_str()).IsValid(),
            "evaluation_entry_output_pointer_invalid");
  }

  // 判断输入模板是否使用指定占位字段，供入口兼容检查使用。
  static bool uses(const Json &pattern, const std::string &name) {
    if (pattern.object()) {
      if (pattern.has("$eval"))
        return pattern.str("$eval") == name;
      for (const auto &p : pattern.fields())
        if (uses(p.second, name))
          return true;
    } else if (pattern.array())
      for (const auto &p : pattern.items())
        if (uses(p, name))
          return true;
    return false;
  }

  // 检查当前入口是否被配置启用，不因存在配置文件就自动接管宿主。
  bool enabled() const {
    return !specification.null();
  }

  // 统计当前契约中的有效项，避免重复声明扩大能力数量。
  std::size_t count() const {
    return specification["task"]["followup_inputs"].items().size() + 1;
  }

  // 在真实运行前建立评测注入上下文，固定本次策略和数据身份。
  void before_run() {
    if (enabled() && !(delivered && !done && failure.empty() && run_id.empty())) {
      failure = "evaluation_entry_unexpected_run";
      throw Error("evaluation_entry_unexpected_run");
    }
  }

  // 推进多轮评测状态；前一轮终态确认后才交付下一轮输入。
  void advance(const Json &run) {
    if (run.flag("incomplete", false) || run["events"].items().empty() ||
        run["events"].items().back().str("kind") != "run.end") {
      failure = "evaluation_entry_trace_incomplete";
      return;
    }
    const auto &output = run["events"].items().back()["output"];
    rapidjson::Document document;
    const auto bytes = output.dump();
    document.Parse(bytes.c_str());
    const auto *value =
        rapidjson::Pointer(specification["binding"].str("output_pointer").c_str()).Get(document);
    if (!value) {
      failure = "evaluation_entry_output_missing";
      return;
    }
    auto content = Json::convert(*value);
    messages.push(
        Json{{"role", "assistant"},
             {"content", content.string() ? content : Json(content.dump_for_transport())}});
    if (messages.dump().size() > 1048576) {
      failure = "evaluation_entry_history_limit";
      return;
    }
    ++index;
    delivered = false;
    run_id.clear();
    packet = nullptr;
    done = index == count();
  }

  // 获取下一条允许交付的评测输入，空返回代表当前无可执行任务。
  Json next(const Json &request, const Json &run, bool persisted) {
    keys(request, {"operation", "request_id", "consumer", "index"});
    require(enabled(), "evaluation_entry_not_enabled", 409);
    const auto who = request.str("consumer");
    require(!who.empty() && who.size() <= 128 &&
                who.find_first_not_of(
                    "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") ==
                    std::string::npos,
            "evaluation_entry_consumer_invalid");
    require(consumer.empty() || consumer == who, "evaluation_entry_already_bound", 409);
    const auto requested = request["index"].integer();
    require(requested == index || (delivered && requested == index + 1),
            "evaluation_entry_sequence_invalid", 409);
    consumer = who;
    if (!failure.empty())
      return Json{{"state", "failed"}};
    if (requested == index + 1) {
      // 已确认采集缺口时不再等待 run.end；接收回执本身仍不能结束异步 run。
      if (run.flag("incomplete", false)) {
        failure = "evaluation_entry_trace_incomplete";
        return Json{{"state", "failed"}};
      }
      if (run.null() || !run.flag("closed", false) || !persisted)
        return Json{{"state", "waiting"}, {"retry_after_ms", 100}};
      advance(run);
    }
    if (!failure.empty())
      return Json{{"state", "failed"}};
    if (done)
      return Json{{"state", "done"}};
    if (!delivered) {
      const auto &task = specification["task"];
      auto values = fields();
      values["input"] = index ? task["followup_inputs"].items().at(index - 1) : task["task_input"];
      messages.push(Json{{"role", "user"}, {"content", values["input"]}});
      values["messages"] = messages;
      values["user_id"] = specification["user_id"];
      values["conversation_id"] = specification["trial_id"];
      values["run_key"] = specification.str("trial_id") + "-" + std::to_string(index);
      values["turn"] = index;
      for (const auto *name : {"initial_state", "constraints", "budget"})
        values[name] = task[name];
      packet = Json{{"state", "input"},
                    {"index", index},
                    {"input", render(specification["binding"]["input"], values)}};
      require(packet.dump().size() <= 1048576, "evaluation_entry_input_limit");
      delivered = true;
    }
    return packet;
  }
};

} // 命名空间结束：trajectory::evolution::integration
