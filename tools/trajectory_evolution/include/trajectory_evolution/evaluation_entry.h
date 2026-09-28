// 多轮输入状态机：按编号投递用户消息，并用真实 run.end 的输出续接历史。
// 回调返回只表示输入已经交付；必须等待运行结束并落盘才能解锁下一轮。
// 同一会话复用历史，每轮保持独立 run 身份；异常不伪装成正常收尾。
// 此处是对外声明，具体实现位于 src；修改接口时需考虑现有调用方与持久化版本。

#pragma once
#ifdef __cplusplus
extern "C" {
#endif

/* 回调借用 JSON 输入，输入在回调执行期间有效。返回零表示接受，非零表示失败。完成由正常运行结束打点确认，不由此返回码确认；不重试回调，并应在宿主合法线程调用。非评测环境返回 0，全部轮次结束返回 1，错误返回 -1。 */
typedef int (*te_evaluation_receiver)(const char *input_json, void *context);
// 评测模式下逐条领取输入并交给既有接收回调；回调负责提交，完成由 run 结束打点确认。
// 非评测模式不驱动业务。context 借用到函数返回，返回码含义见上方入口约定。
int te_evaluate(te_evaluation_receiver receiver, void *context);

#ifdef __cplusplus
}
#endif
