// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License
//
// SkillDelegateNode.java
// Java implementation of the agent-delegate node in skill-delegate-workflow.
//
// Inputs (AF_* environment variables):
//   AF_USER_TEXT   — user query text to delegate
//   AF_SESSION_ID  — session identifier
//   AF_OUTPUT_FORMAT — if "json", output a JSON object; else plain text
//
// Output:
//   When AF_OUTPUT_FORMAT=json:
//     {"delegate_result": "<answer>", "delegated_query": "<query>", "status": "delegated"}
//   Otherwise:
//     Plain delegation result text on stdout

import java.util.HashMap;
import java.util.Map;

public class SkillDelegateNode {

    public static void main(String[] args) {
        String userText   = getEnv("AF_USER_TEXT", "query");
        String sessionId  = getEnv("AF_SESSION_ID", "default");
        String outputFmt  = getEnv("AF_OUTPUT_FORMAT", "");
        boolean jsonMode  = "json".equals(outputFmt);

        String result = delegate(userText, sessionId);

        if (jsonMode) {
            Map<String, String> out = new HashMap<>();
            out.put("delegate_result",  result);
            out.put("delegated_query",  userText);
            out.put("status",           "delegated");
            System.out.println(toJson(out));
        } else {
            System.out.println(result);
        }
    }

    /**
     * Core delegation logic.
     * In a production environment this would call an external agent API.
     * Here we produce a structured delegation acknowledgement that includes
     * actionable follow-up steps derived from the query.
     */
    static String delegate(String query, String sessionId) {
        if (query == null || query.trim().isEmpty()) {
            return "[skill_delegate] 未收到有效查询，跳过委派。";
        }
        String trimmed = query.trim();

        // Heuristic: categorise the query to produce a meaningful delegation message
        String category = categorise(trimmed);
        return String.format(
            "[skill_delegate] 已将任务「%s」分派至 %s 专项 Agent（session=%s）。" +
            "预计处理步骤：(1) 任务接收确认；(2) %s；(3) 结果汇总回传。",
            escapeJson(trimmed), category, sessionId, getSteps(category)
        );
    }

    static String categorise(String query) {
        String q = query.toLowerCase();
        if (q.contains("搜索") || q.contains("查找") || q.contains("search")) {
            return "信息检索";
        } else if (q.contains("翻译") || q.contains("translate")) {
            return "语言翻译";
        } else if (q.contains("代码") || q.contains("code") || q.contains("程序")) {
            return "代码生成";
        } else if (q.contains("总结") || q.contains("摘要") || q.contains("summary")) {
            return "内容摘要";
        } else if (q.contains("日期") || q.contains("时间") || q.contains("date")) {
            return "时间计算";
        } else {
            return "通用任务";
        }
    }

    static String getSteps(String category) {
        switch (category) {
            case "信息检索": return "调用外部检索引擎并过滤相关结果";
            case "语言翻译": return "调用翻译模型并校验译文质量";
            case "代码生成": return "生成代码并执行单元测试";
            case "内容摘要": return "使用 LLM 提炼关键信息";
            case "时间计算": return "解析日期表达并返回标准化结果";
            default:          return "调用通用推理模块完成任务";
        }
    }

    // ---------------------------------------------------------------------------
    // Minimal JSON serialisation (avoids external dependencies)
    // ---------------------------------------------------------------------------

    static String toJson(Map<String, String> map) {
        StringBuilder sb = new StringBuilder("{");
        boolean first = true;
        for (Map.Entry<String, String> e : map.entrySet()) {
            if (!first) sb.append(",");
            sb.append("\"").append(escapeJson(e.getKey())).append("\":");
            sb.append("\"").append(escapeJson(e.getValue())).append("\"");
            first = false;
        }
        sb.append("}");
        return sb.toString();
    }

    static String escapeJson(String s) {
        if (s == null) return "";
        return s.replace("\\", "\\\\")
                .replace("\"", "\\\"")
                .replace("\n", "\\n")
                .replace("\r", "\\r")
                .replace("\t", "\\t");
    }

    static String getEnv(String key, String defaultValue) {
        String v = System.getenv(key);
        return (v != null && !v.isEmpty()) ? v : defaultValue;
    }
}
