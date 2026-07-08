"""
Coordinator prompts — system prompt, tool definition, and message templates
for the coordinator node (coordinator_decide.py).
"""

# ---------------------------------------------------------------------------
# Coordinator system prompt template — defines role, run_skills usage, output format
# ---------------------------------------------------------------------------
# {available_skills} is replaced at runtime by the coordinator node.

PROTOCOL_TEMPLATE = """## 你的工作方式

你是一个任务编排 AI，通过 run_skills 工具委派 skill 执行子任务。
你不能直接读取文件、执行命令或搜索——所有具体工作必须委派给 skill。

## run_skills 使用方式

调用 run_skills 时，为每个 skill 指定：
- name (必填): skill 名称，可用选项：{available_skills}
- prompt (必填): 给这个 skill 的任务指令，告诉它应该做什么
- load_skills (可选): skill 应加载的技能文档名称列表
- exec_skill_key (可选): 结构化输入数据的 DataBus key
- force (可选, 默认 false): 强制重新执行已完成的 skill

## 决策规则

- 需要执行子任务 → 调用 run_skills 委派
- 可以在一次 run_skills 中委派多个独立子任务，它们会**并行执行**
- 每个 skill 完成后你会立即收到结果，可以根据部分结果决定是否再委派更多 skill
- 但**最终回复用户前，必须等所有已派发的 skill 全部完成**
- **所有子任务完成且结果满意 → 直接回复用户（不调用工具）**
- 结果不完整或有新需求 → 再次调用 run_skills 补充
- 追加新 skill 时注意不要与已完成 skill 的工作冲突或重复

## 失败模式
1.如果用户没有明确要求视频生成，不要进行额外的视频生成。
2.如果生成的网络资源因为网络访问无法下载,不用进行额外尝试，直接返回错误告知用户
3.生成的产物下载以后会被自动收集以展示给用户，不需要你自己提供http服务器

## 输出格式

当你决定直接回复用户时（finish=stop），必须使用以下格式：

<think>
你的推理过程（评估 skill 结果、判断依据、决策原因等）
</think>
给用户的最终回复内容（markdown 格式）

注意：
- <think> 标签内的内容不会展示给用户，仅用于 trace 记录
- <think> 标签外的内容才是用户看到的最终回复
- 不要在最终回复中重复推理过程的内容
"""

# Fallback when agent.md body is missing
COORDINATOR_FALLBACK_SYSTEM = "你是一个任务编排 AI。分析用户需求，委派 skill 执行子任务。"

# ---------------------------------------------------------------------------
# run_skills tool definition
# ---------------------------------------------------------------------------

RUN_SKILLS_TOOL_DEFINITION = {
    "type": "function",
    "function": {
        "name": "run_skills",
        "description": (
            "委派一个或多个 skill 去执行子任务。多个 skill 会同时启动并行执行。"
            "每个 skill 完成后你会立即收到它的结果，你可以根据已完成 skill 的结果"
            "决定是否需要再委派更多 skill（动态追加）。"
            "但最终回复用户前，必须等所有已派发的 skill 全部完成。"
        ),
        "parameters": {
            "type": "object",
            "properties": {
                "skills": {
                    "type": "array",
                    "items": {
                        "type": "object",
                        "properties": {
                            "name": {
                                "type": "string",
                                "enum": [],
                                "description": "Skill 名称，要从可用列表中选择",
                            },
                            "prompt": {
                                "type": "string",
                                "description": "给这个 skill 的任务指令。"
                                             "告诉 skill executor 应该完成什么具体工作。",
                            },
                            "load_skills": {
                                "type": "array",
                                "items": {"type": "string"},
                                "description": "该 skill 应加载的额外技能文档名称列表。",
                            },
                            "exec_skill_key": {
                                "type": "string",
                                "description": "DataBus key，指向该 skill 的结构化输入数据。",
                            },
                            "force": {
                                "type": "boolean",
                                "description": "强制重新执行，即使该 skill 已启动过。默认 false。",
                            },
                        },
                        "required": ["name", "prompt"],
                    },
                    "description": "要执行的 skill 列表，每个 skill 带有任务指派信息",
                },
                "reason": {
                    "type": "string",
                    "description": "为什么需要这些 skill（用于审计和可追溯性）",
                },
            },
            "required": ["skills"],
        },
    },
}

# ---------------------------------------------------------------------------
# Skill completion message templates
# ---------------------------------------------------------------------------

SKILL_COMPLETION_TEMPLATE = (
    "Skill '{completed_skill}' 已完成 (success={skill_success})。\n"
    "结果: {skill_results}\n"
    "{decision}"
)

DECISION_FAILURE_PROMPT = (
    "请根据失败原因判断：直接回复用户并说明失败，或在尝试次数允许时重新委派 skill。"
)

DECISION_SUCCESS_PROMPT = (
    "请评估结果，决定是否需要再委派 skill 或直接回复用户。"
)
