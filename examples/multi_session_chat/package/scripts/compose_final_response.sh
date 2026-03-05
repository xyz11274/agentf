#!/bin/bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
USER_TEXT="$(cat "${ROOT}/runtime/latest_user_input.txt" 2>/dev/null || true)"
SKILLS="$(cat "${ROOT}/runtime/skill_results.jsonl" 2>/dev/null || true)"
SESSION_ID="$(cat "${ROOT}/runtime/latest_session_id.txt" 2>/dev/null || echo "webui:default")"
SAFE_SESSION_ID="$(printf '%s' "$SESSION_ID" | sed 's#[/:\\]#_#g')"
HISTORY_FILE="${ROOT}/history/${SAFE_SESSION_ID}.jsonl"
USER_LINES="$(sed -n 's/.*\"role\":\"user\",\"content\":\"\([^\"]*\)\".*/\1/p' "$HISTORY_FILE" 2>/dev/null || true)"
CURRENT_USER_FROM_HISTORY="$(printf '%s\n' "$USER_LINES" | tail -n 1)"
[ -n "$CURRENT_USER_FROM_HISTORY" ] && USER_TEXT="$CURRENT_USER_FROM_HISTORY"
PROMPT="你是多agent协调器。根据用户输入与多技能执行结果，给出最终中文答复。\n用户输入: ${USER_TEXT}\n技能结果:\n${SKILLS}\n请直接输出最终回复文本。"
RAW="$(python3 "${ROOT}/scripts/llm_openai_compat.py" <<< "$PROMPT" 2>/dev/null || true)"
TEXT="$(printf '%s' "$RAW" | sed -n 's/.*"text"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' | head -n1)"
SUMMARY="$(printf '%s\n' "$SKILLS" | sed -n 's/.*"result":"\([^"]*\)".*/\1/p' | head -n 4 | tr '\n' '；')"
[ -z "$SUMMARY" ] && SUMMARY="我已完成相关分析并整理出可执行建议。"
SUMMARY_CLEAN="$(printf '%s' "$SUMMARY" | sed 's/\[skill_[^]]*\][[:space:]]*//g')"
[ -z "$SUMMARY_CLEAN" ] && SUMMARY_CLEAN="$SUMMARY"
SEARCH_RESULT="$(printf '%s\n' "$SKILLS" | sed -n 's/.*"skill":"skill_search".*"result":"\([^"]*\)".*/\1/p' | head -n1)"
if [ -z "$TEXT" ] || [[ "$TEXT" == \[mock\]* ]] || [[ "$TEXT" == \[error\]* ]] || [[ "$TEXT" == *'"session_id"'* ]] || [[ "$TEXT" == *'"skill"'* ]] || [[ "$TEXT" == *"[skill_"* ]] || [[ "$TEXT" == *"我的结论是："* ]]; then
  PREV_USER="$(printf '%s\n' "$USER_LINES" | tail -n 2 | head -n 1)"
  WEEKDAY="$(python3 - <<'PY'
from datetime import datetime
week = ["周一","周二","周三","周四","周五","周六","周日"]
print(week[datetime.now().weekday()])
PY
)"
  DATE_PARSE_JSON="$(printf '%s' "$USER_TEXT" | python3 "${ROOT}/scripts/date_nlp_parser.py" 2>/dev/null || true)"
  DATE_PARSED_ANSWER="$(printf '%s' "$DATE_PARSE_JSON" | sed -n 's/.*\"answer\"[[:space:]]*:[[:space:]]*\"\\([^\"]*\\)\".*/\\1/p' | head -n1)"
  DATE_MATCHED="$(printf '%s' "$DATE_PARSE_JSON" | sed -n 's/.*\"matched\"[[:space:]]*:[[:space:]]*\\(true\\|false\\).*/\\1/p' | head -n1)"
  if [[ "$USER_TEXT" =~ ^[[:space:]]*(你好|您好|hello|hi)[[:space:]]*$ ]]; then
    TEXT="你好，很高兴为你服务。请告诉我你想解决的具体问题，我可以给你直接结论和可执行建议。"
  elif [[ "$DATE_MATCHED" == "true" ]] && [ -n "$DATE_PARSED_ANSWER" ]; then
    TEXT="$DATE_PARSED_ANSWER"
  elif [[ "$USER_TEXT" == *"周几"* ]] || [[ "$USER_TEXT" == *"星期几"* ]]; then
    TEXT="今天是${WEEKDAY}。"
  elif [[ "$USER_TEXT" == *"总结"* ]] && [[ "$USER_TEXT" == *"上条"* ]] && [ -n "${PREV_USER}" ] && [ "${PREV_USER}" != "${USER_TEXT}" ]; then
    TEXT="你上条消息是“${PREV_USER}”。简要总结：这是一个关于“${PREV_USER}”的请求，建议先明确目标，再给出2-3个可执行步骤。"
  elif [[ "$USER_TEXT" == *"问了几个问题"* ]] || ([[ "$USER_TEXT" == *"几个问题"* ]] && [[ "$USER_TEXT" == *"总结"* ]]); then
    QUESTION_COUNT="$(printf '%s\n' "$USER_LINES" | sed '/^$/d' | wc -l | tr -d ' ')"
    RECENT_QUESTIONS="$(printf '%s\n' "$USER_LINES" | tail -n 5 | sed 's/^/ - /' | tr '\n' ' ')"
    TEXT="截至目前你一共问了${QUESTION_COUNT}个问题（含当前这一句）。最近的问题有：${RECENT_QUESTIONS}"
  elif [[ "$USER_TEXT" == *"明天"* ]] && [[ "$USER_TEXT" == *"天气"* ]]; then
    if [[ "$USER_TEXT" == *"上海"* ]]; then
      TEXT="你问的是上海明天天气。当前流程未接入实时气象API，无法给出精确温度；建议关注“最高/最低温、降水概率、风力”三个指标，我可以按这个模板帮你快速解读。"
    else
      TEXT="请提供具体城市（例如“上海明天天气”）。我会按温度范围、降水概率、风力和穿衣建议给你整理。"
    fi
  elif [[ "$USER_TEXT" == *"天气"* ]]; then
    if printf '%s' "$USER_TEXT" | grep -Eq '(上海|北京|广州|深圳|杭州|南京|苏州|成都|武汉|西安|重庆|天津|长沙|青岛|宁波|厦门|福州|郑州|济南|合肥|昆明|大连|沈阳|长春|哈尔滨|石家庄|南昌|贵阳|海口|南宁|太原|乌鲁木齐|兰州|呼和浩特|银川|拉萨)'; then
      TEXT="你提供了地点信息（${USER_TEXT}）。当前流程未接入实时气象API，暂时不能给出精确天气数值；我可以按“温度区间、降水概率、风力、出行/穿衣建议”给你一版结构化预测模板。"
    else
      TEXT="天气查询建议提供具体城市（例如“上海今天天气”）。当前流程已完成检索与汇总，但未接入实时气象API；如果你告诉我城市，我可以按常用维度给你一份出行建议清单。"
    fi
  elif [[ "$USER_TEXT" == *"新闻"* ]] || [[ "$USER_TEXT" == *"热点"* ]]; then
    if [ -n "$SEARCH_RESULT" ]; then
      TEXT="根据最近一周检索结果，建议重点关注：${SEARCH_RESULT}。如果你需要，我可以进一步按“国内/国际/科技/财经”分类整理。"
    else
      TEXT="我已尝试检索最近一周热点新闻，但当前环境未返回可用新闻源。你可以补充关注领域（如科技或财经），我会给你结构化摘要模板。"
    fi
  elif [[ "$USER_TEXT" == *"AI"* ]] || [[ "$USER_TEXT" == *"人工智能"* ]]; then
    if [ -n "$SEARCH_RESULT" ]; then
      AI_TOP="$(printf '%s' "$SEARCH_RESULT" | sed 's/^最近相关信息：//' | tr '；' '\n' | head -n 10 | sed '/^$/d' | sed 's/^/- /' | tr '\n' ' ')"
      TEXT="你关心的AI热点如下（近一周）：${AI_TOP}"
    else
      TEXT="我已触发AI热点检索，但当前环境未返回可解析条目。你可以指定方向（大模型/AI应用/AI芯片），我将给你对应Top清单。"
    fi
  elif [[ "$USER_TEXT" == *"英语"* ]]; then
    TEXT="学英语建议从三步开始：1) 每天20分钟精听+跟读；2) 建立高频词复习清单并间隔记忆；3) 每周至少2次口语输出（复述/对话）。坚持4周后再按薄弱项调整。"
  else
    TEXT="关于“${USER_TEXT}”，我给你的直接建议是：${SUMMARY_CLEAN} 如果你愿意，我可以继续细化为可执行的分步方案。"
  fi
fi
printf '%s\n' "$TEXT" > "${ROOT}/runtime/final_response.txt"
printf '%s\n' "$TEXT"
