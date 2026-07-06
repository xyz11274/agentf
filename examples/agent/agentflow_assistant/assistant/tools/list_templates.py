"""list_templates tool — list available agent templates for user reference."""

import json
import os

TOOL_NAME = "list_templates"
TOOL_DESCRIPTION = "List all available agent template types for the user to choose from"
TOOL_PARAMS = {
    "type": "object",
    "properties": {},
    "required": []
}


TEMPLATES = [
    {
        "id": "harness",
        "name": "Harness Agent (内部循环模式)",
        "description": "LLM 驱动的内部循环 Agent。单 coordinator workflow 内运行 while-True 循环，LLM 自主决定调用工具还是回复用户。适合单一 LLM 驱动、工具调用简单的场景。",
        "features": [
            "Python while-True 内部循环",
            "进程内工具调用（TOOL_REGISTRY）",
            "通过 agent.md 配置模型和提示词",
            "支持 MCP 工具集成",
            "支持跨轮对话上下文"
        ],
        "example": "harness_agent"
    },
    {
        "id": "basic",
        "name": "Basic Agent (基础 DAG 模式)",
        "description": "基础 DAG 工作流模式。通过 workflow.json 定义 node + edges 形成有向无环图。适合数据处理、条件分支等确定性流程。",
        "features": [
            "Workflow DAG 驱动",
            "支持多节点顺序/并行执行",
            "支持 shell 和 Python 节点",
            "轻量级，无 LLM 依赖"
        ],
        "example": "basic_agent"
    },
    {
        "id": "multi-session",
        "name": "Multi-Session Chat (多会话聊天模式)",
        "description": "多会话聊天 Agent。支持上下文管理、多轮对话、多 skill 调度。适合复杂的对话场景，需要框架管理 skill 生命周期。",
        "features": [
            "会话上下文管理",
            "多 skill 并发调度",
            "dispatcher + coordinator + skill 三级路由",
            "跨轮对话上下文持久化",
            "支持多种编程语言节点"
        ],
        "example": "multi_session_chat"
    },
    {
        "id": "nested-workflow",
        "name": "Nested Workflow (嵌套工作流模式)",
        "description": "嵌套工作流模式。主 workflow 可以调用子 workflow，形成层级结构。适合复杂任务拆解为多个子流程的场景。",
        "features": [
            "Workflow 嵌套调用",
            "子流程独立部署和测试",
            "数据在层级间传递",
            "适合复杂业务编排"
        ],
        "example": "nested_workflow_agent"
    },
    {
        "id": "custom",
        "name": "自定义 Agent",
        "description": "从零开始自定义创建 Agent。由助手根据你的需求逐步引导，生成最合适的结构。",
        "features": [
            "完全定制化",
            "按需选择组件",
            "灵活组合各种模式"
        ],
        "example": None
    }
]


def execute(tool_input: dict) -> str:
    return json.dumps({
        "templates": TEMPLATES,
        "total": len(TEMPLATES),
        "tool": "list_templates",
    }, ensure_ascii=False)
