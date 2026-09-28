// 轨迹内部辅助声明：共享转换、验证与分析所需的实现接口。
// 只在包内引用，避免将内部 JSON 表达或实现依赖扩散到公开 API。

#pragma once

#include "trajectory_evolution/trace.h"
#include "agentflow/trajectory/loop_episode_analyzer.h"

namespace trajectory::evolution {

// 两种输入格式最终产生同一分析表示；
// 旧 SessionLog 继续使用已有解析器及其框架相关规则。
agentflow::trajectory::AnalysisReport analyze_trace(const ValidatedTrace &trace);
// 把标准轨迹内容映射为分析可读事件流，真实身份和因果关系保持不变。
std::string trace_content_jsonl(const v1::TraceRun &trace);

} // 命名空间结束：trajectory::evolution
