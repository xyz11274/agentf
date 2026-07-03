"""create_agent_package tool — generate a complete agent package from user requirements."""

import json
import os
import shutil


TOOL_NAME = "create_agent_package"
TOOL_DESCRIPTION = "Generate a complete agent package in examples/agent/ based on collected user requirements. Call this after you've gathered all info from the user through Q&A."
TOOL_PARAMS = {
    "type": "object",
    "properties": {
        "agent_name": {
            "type": "string",
            "description": "Directory name for the agent (e.g., 'my_agent'). Use lowercase with underscores."
        },
        "display_name": {
            "type": "string",
            "description": "Display name shown in agent.json (e.g., 'MyAgent'). PascalCase recommended."
        },
        "description": {
            "type": "string",
            "description": "Short description of what the agent does"
        },
        "template_type": {
            "type": "string",
            "enum": ["harness", "basic", "multi-session", "nested-workflow", "custom"],
            "description": "Agent architecture template type"
        },
        "needs_llm": {
            "type": "boolean",
            "description": "Whether the agent needs LLM integration (harness pattern)"
        },
        "needs_multi_turn": {
            "type": "boolean",
            "description": "Whether the agent needs multi-turn conversation support"
        },
        "needs_mcp": {
            "type": "boolean",
            "description": "Whether the agent needs MCP tool integration"
        },
        "needs_calculator": {
            "type": "boolean",
            "description": "Whether the agent needs a calculator skill"
        },
        "needs_echo": {
            "type": "boolean",
            "description": "Whether the agent needs an echo test skill"
        },
        "model_name": {
            "type": "string",
            "description": "LLM model name (e.g., 'gpt-4o-mini'). Only used if needs_llm is true."
        },
        "model_base_url": {
            "type": "string",
            "description": "LLM API base URL. Only used if needs_llm is true."
        },
        "api_key_env_var": {
            "type": "string",
            "description": "Environment variable name for the API key (e.g., 'API_KEY', 'OPENAI_API_KEY'). Only used if needs_llm is true."
        },
        "nodes": {
            "type": "array",
            "items": {
                "type": "object",
                "properties": {
                    "name": {"type": "string", "description": "Node name"},
                    "language": {"type": "string", "enum": ["python", "shell", "cpp"], "description": "Node implementation language"}
                }
            },
            "description": "List of custom nodes needed"
        },
        "tools_needed": {
            "type": "array",
            "items": {
                "type": "string",
                "enum": ["bash", "read_file", "write_file", "web_fetch", "echo"]
            },
            "description": "List of local tools needed (only for harness template)"
        },
        "author": {
            "type": "string",
            "description": "Author name for package.json"
        }
    },
    "required": ["agent_name", "display_name", "description", "template_type"]
}


EXAMPLES_DIR = os.path.normpath(os.path.join(
    os.path.dirname(os.path.abspath(__file__)),
    "..", "..", ".."
))


def _write_file(path: str, content: str) -> None:
    """Write content to a file, creating parent directories as needed."""
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8") as f:
        f.write(content)


def _generate_harness_package(params: dict) -> list:
    """Generate a harness-type agent package."""
    agent_dir = os.path.join(EXAMPLES_DIR, params["agent_name"])
    agent_name = params["agent_name"]
    display_name = params["display_name"]
    description = params["description"]
    model_name = params.get("model_name", "gpt-4o-mini")
    model_base_url = params.get("model_base_url", "https://api.openai.com/v1")
    api_key_var = params.get("api_key_env_var", "API_KEY")
    tools_needed = params.get("tools_needed", ["bash", "read_file", "write_file"])
    needs_mcp = params.get("needs_mcp", False)
    needs_multi_turn = params.get("needs_multi_turn", False)
    author = params.get("author", "AgentFlow User")

    generated_files = []

    # ── package.json ──
    wf_list = ["coordinator_collect.json"]
    run_wf_name = f"{agent_name}_run.json"
    wf_list.append(run_wf_name)
    pkg = {
        "name": agent_name,
        "version": "1.0.0",
        "description": description,
        "author": author,
        "workflows": wf_list,
    }
    if needs_mcp:
        pkg["dependencies"] = {"mcp": ">=1.27,<2"}
    _write_file(os.path.join(agent_dir, "package.json"),
                json.dumps(pkg, indent=2, ensure_ascii=False))
    generated_files.append("package.json")

    # ── agent.json ──
    agent_def = {
        "name": display_name,
        "version": "1.0.0",
        "description": description,
        "session_pool_size": 1,
        "max_workflows": 10,
        "max_concurrent_workflows": 5,
        "data_bus_capacity_mb": 5,
        "tasks": [
            {
                "id": 0,
                "type": "dispatcher",
                "description": f"Routes to {agent_name} skill",
                "next_skills": [f"{agent_name}_run"],
                "workflow_pool_size": 1
            },
            {
                "id": 1,
                "type": "coordinator",
                "description": "Collects skill result and returns to client",
                "workflow_file": "coordinator_collect.json",
                "workflow_pool_size": 1
            },
            {
                "id": 2,
                "type": "skill",
                "description": f"Runs the LLM think-and-act loop for {display_name}",
                "workflow_file": f"{agent_name}_run.json",
                "skill": {
                    "name": f"{agent_name}_run",
                    "config_dir": "config",
                    "instance_pool_size": 1
                },
                "workflow_pool_size": 1
            }
        ]
    }
    _write_file(os.path.join(agent_dir, "agent.json"),
                json.dumps(agent_def, indent=2, ensure_ascii=False))
    generated_files.append("agent.json")

    # ── agent.md (system prompt) ──
    skills_list = []
    if params.get("needs_echo"):
        skills_list.append("- echo")
    if params.get("needs_calculator"):
        skills_list.append("- calculator")

    skills_yaml = "\n".join(skills_list) if skills_list else "# no skills declared"

    agent_md = f"""---
name: {agent_name}
description: "{description}"
model:
  provider: openai
  name: {model_name}
  base_url: {model_base_url}
  api_key: ${{{api_key_var}}}
skills:
{skills_yaml}
---

# {display_name}

{description}

## 工作方式
1. 分析用户需求，决定下一步做什么。
2. 通过 function calling 调用可用工具完成任务。
3. 当任务完成或需要回复用户时，直接输出回答内容。

## 规则
- 优先使用工具获取信息，不要凭猜测回答。
- 每步最多调用一个工具，等待结果后再决定下一步。
- 完成后给出清晰、简洁的回答。
"""
    _write_file(os.path.join(agent_dir, "agent.md"), agent_md)
    generated_files.append("agent.md")

    # ── Run workflow JSON ──
    timeout_ms = 60000 if needs_multi_turn else 30000
    run_wf = {
        "id": f"{agent_name}-run",
        "version": "1.0",
        "name": f"{agent_name}-run",
        "description": f"LLM think-and-act loop for {display_name}",
        "nodes": [
            {
                "id": "llm_think",
                "name": "LLM Think and Act",
                "type": "task",
                "execution_mode": "python_script",
                "command": f"./nodes/{agent_name}_loop.py",
                "working_dir": f"examples/agent/{agent_name}",
                "json_output": True,
                "inputs": [
                    {"name": "input", "type": "string", "required": True},
                    {"name": "completed_skill_name", "type": "string", "required": False},
                    {"name": "session_id", "type": "string", "required": True},
                    {"name": "dialogue_id", "type": "string", "required": False}
                ],
                "outputs": [
                    {"name": "response", "type": "string"}
                ],
                "enable_hot_reload": False,
                "timeout_ms": timeout_ms
            }
        ],
        "edges": []
    }
    _write_file(os.path.join(agent_dir, run_wf_name),
                json.dumps(run_wf, indent=2, ensure_ascii=False))
    generated_files.append(run_wf_name)

    # ── coordinator_collect.json ──
    coord_wf = {
        "id": "coordinator-collect",
        "version": "1.0",
        "name": "coordinator-collect",
        "description": "Collects skill outputs from DataBus and returns to client",
        "nodes": [
            {
                "id": "collector",
                "name": "Skill Output Collector",
                "type": "task",
                "execution_mode": "python_script",
                "command": "./nodes/coordinator_collect.py",
                "working_dir": f"examples/agent/{agent_name}",
                "json_output": True,
                "timeout_ms": 1000,
                "inputs": [
                    {"name": "input", "type": "string", "required": True},
                    {"name": "session_id", "type": "string", "required": True},
                    {"name": "dialogue_id", "type": "string", "required": False}
                ],
                "outputs": [
                    {"name": "response", "type": "string"}
                ]
            }
        ],
        "edges": []
    }
    _write_file(os.path.join(agent_dir, "coordinator_collect.json"),
                json.dumps(coord_wf, indent=2, ensure_ascii=False))
    generated_files.append("coordinator_collect.json")

    # ── nodes/{agent_name}_loop.py ──
    tools_dir_ref = "tools"

    loop_py = f'''#!/usr/bin/env python3
"""
LLM Think and Act Node for {display_name}.

Single Python process with an internal while True loop:
  1. Reads agent.md for system prompt (body) and config (YAML frontmatter)
  2. Reads conversation history (messages list) from DataBus ctx namespace
  3. LLM decides: call a tool? or respond?
  4. On response: writes updated messages back to DataBus for next turn

Environment variables (injected by the worker):
  AF_INPUT               — The user's original request
  AF_DIALOGUE_ID         — Current dialogue ID
  AF_SESSION_ID          — Current session ID
  AF_DATABUS_UDS_FILE    — UDS socket for runtime DataBus access
"""

import json
import os
import re
import sys
import yaml

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from databus_client import DataBusClient


NODES_DIR = os.path.dirname(os.path.abspath(__file__))
AGENT_PACKAGE_DIR = os.path.dirname(NODES_DIR)
AGENT_MD_PATH = os.path.join(AGENT_PACKAGE_DIR, "agent.md")


# --- Load tools from the tools/ directory ---
TOOLS_DIR = os.path.join(AGENT_PACKAGE_DIR, "{tools_dir_ref}")
sys.path.insert(0, os.path.join(AGENT_PACKAGE_DIR, "{tools_dir_ref.split('/')[0]}"))
import tools as tool_registry


# ---------------------------------------------------------------------------
# agent.md parsing
# ---------------------------------------------------------------------------

RE_ENV_VAR = re.compile(r"\\${{(\\w+)}}")


def _expand_env(value):
    """Replace ${{VAR}} with os.environ value."""
    if isinstance(value, str):
        return RE_ENV_VAR.sub(lambda m: os.environ.get(m.group(1), m.group(0)), value)
    if isinstance(value, dict):
        return {{k: _expand_env(v) for k, v in value.items()}}
    if isinstance(value, list):
        return [_expand_env(v) for v in value]
    return value


def parse_agent_md(path: str) -> tuple:
    """Read agent.md, return (frontmatter_dict, body_text)."""
    if not os.path.isfile(path):
        return {{}}, ""
    with open(path, encoding="utf-8") as f:
        raw = f.read()
    parts = raw.split("---", 2)
    if len(parts) < 3:
        return {{}}, raw.strip()
    try:
        frontmatter = yaml.safe_load(parts[1]) or {{}}
        frontmatter = _expand_env(frontmatter)
    except Exception:
        frontmatter = {{}}
    body = parts[2].strip()
    return frontmatter, body


def build_tools_from_config(frontmatter: dict) -> list:
    """Build function-calling tools from the tool registry."""
    return tool_registry.get_tools()


# ---------------------------------------------------------------------------
# LLM interaction
# ---------------------------------------------------------------------------

def get_env(key: str, default: str = "") -> str:
    return os.environ.get(key, default)


def call_llm(messages: list, tools: list | None = None,
             model: str = "{model_name}",
             base_url: str = "{model_base_url}",
             api_key: str = "") -> dict:
    """Call the LLM API and return the raw response dict."""
    import requests
    url = base_url.rstrip("/")
    if not url.endswith("/chat/completions"):
        url += "/chat/completions"

    headers = {{
        "Content-Type": "application/json",
        "Authorization": f"Bearer {{api_key}}",
    }}
    body = {{
        "model": model,
        "messages": messages,
        "max_completion_tokens": 1000,
    }}
    if tools:
        body["tools"] = tools
        body["tool_choice"] = "auto"
    try:
        resp = requests.post(url, json=body, headers=headers, timeout=30)
        resp.raise_for_status()
        return resp.json()
    except Exception as e:
        return {{"error": f"LLM call failed: {{e}}", "_llm_error": True}}


# ---------------------------------------------------------------------------
# Main — internal while True loop
# ---------------------------------------------------------------------------

def execute():
    user_input = get_env("AF_INPUT", "")
    dialogue_id = get_env("AF_DIALOGUE_ID", "")
    session_id = get_env("AF_SESSION_ID", "")

    frontmatter, system_prompt = parse_agent_md(AGENT_MD_PATH)
    model_cfg = frontmatter.get("model", {{}})

    tools = build_tools_from_config(frontmatter)

    api_key = model_cfg.get("api_key", "")
    if not api_key or "${{" in api_key:
        print(json.dumps({{"error": "API key not resolved. "
                          "Set API_KEY env var or configure agent.md model.api_key."}},
                          ensure_ascii=False))
        sys.exit(1)

    databus = DataBusClient()
    history_messages = databus.read_messages() if databus.is_connected() else []

    messages = [{{"role": "system", "content": system_prompt or
                 "You are a task-oriented AI assistant."}}]
    messages.extend(history_messages)
    messages.append({{"role": "user", "content": user_input}})

    while True:
        raw = call_llm(
            messages, tools=tools,
            model=model_cfg.get("name", "{model_name}"),
            base_url=model_cfg.get("base_url", "{model_base_url}"),
            api_key=model_cfg.get("api_key", ""))

        if raw.get("_llm_error"):
            print(json.dumps({{"error": raw.get("error", "LLM call failed")}},
                              ensure_ascii=False))
            sys.exit(1)

        choice = raw.get("choices", [{{}}])[0]
        msg = choice.get("message", {{}})
        tool_calls = msg.get("tool_calls")

        if tool_calls:
            tc = tool_calls[0]
            func = tc.get("function", {{}})
            name = func.get("name", "")
            try:
                args = json.loads(func.get("arguments", "{{}}"))
            except json.JSONDecodeError:
                args = {{}}
            result = tool_registry.execute(name, args)
            messages.append({{
                "role": "tool",
                "tool_call_id": name,
                "content": result,
            }})

        else:
            response_text = msg.get("content", "")

            print(json.dumps({{"response": response_text}}, ensure_ascii=False))

            messages_to_save = messages[1:]
            if databus.is_connected():
                databus.write_messages(messages_to_save)

            break


if __name__ == "__main__":
    execute()
'''
    _write_file(os.path.join(agent_dir, "nodes", f"{agent_name}_loop.py"), loop_py)
    generated_files.append(f"nodes/{agent_name}_loop.py")

    # ── nodes/coordinator_collect.py ──
    coord_collect_py = '''#!/usr/bin/env python3
"""
Coordinator Collect Node — minimal pass-through for the coordinator.
Outputs coordinator_status to avoid overwriting the skill's "response" key.
"""

import json


def execute():
    print(json.dumps({
        "coordinator_status": "ok",
    }, ensure_ascii=False))


if __name__ == "__main__":
    execute()
'''
    _write_file(os.path.join(agent_dir, "nodes", "coordinator_collect.py"), coord_collect_py)
    generated_files.append("nodes/coordinator_collect.py")

    # ── nodes/databus_client.py ──
    databus_py = '''"""
DataBus client — wraps official UDS SDK.

Environment variables (injected by the worker):
  AF_DATABUS_UDS_FILE    — UDS socket for runtime DataBus access
  AF_SESSION_ID          — Current session ID
"""

import json
import os
import sys
import traceback

_SDK_DIR = os.path.normpath(os.path.join(
    os.path.dirname(os.path.abspath(__file__)),
    "..", "..", "..", "..", "src", "runtime", "databus", "clients", "python"
))
if _SDK_DIR not in sys.path:
    sys.path.insert(0, _SDK_DIR)

from agentflow_databus import DataBusClient as _RealDataBusClient
from agentflow_databus import DataBusError


def _log_warn(msg: str) -> None:
    print(f"[databus_client] WARN: {msg}", file=sys.stderr)


class DataBusClient:
    """Thin wrapper around the official UDS DataBusClient."""

    def __init__(self):
        self._session_id = os.environ.get("AF_SESSION_ID", "")
        self._uds_path = os.environ.get("AF_DATABUS_UDS_FILE", "")
        self._client: _RealDataBusClient | None = None

        if self._uds_path:
            try:
                self._client = _RealDataBusClient(
                    self._uds_path,
                    session_id=self._session_id,
                    timeout_seconds=30.0,
                )
            except (DataBusError, OSError) as exc:
                _log_warn(f"UDS init failed at {self._uds_path}: {exc}")
                self._client = None

    def is_connected(self) -> bool:
        return self._client is not None

    def read_messages(self) -> list:
        if not self._client:
            return []
        try:
            raw = self._client.ctx().get("session_messages", "[]")
            if raw is None or raw == "":
                return []
            return json.loads(raw) if isinstance(raw, str) else raw
        except Exception as exc:
            _log_warn(f"read_messages failed: {exc}\\n{traceback.format_exc()}")
            return []

    def write_messages(self, messages: list) -> bool:
        if not self._client:
            return False
        try:
            return self._client.ctx().put("session_messages", json.dumps(messages, ensure_ascii=False))
        except Exception as exc:
            _log_warn(f"write_messages failed: {exc}\\n{traceback.format_exc()}")
            return False

    def put(self, key: str, value: str) -> bool:
        if not self._client:
            return False
        return self._client.sys().put(key, value)

    def get(self, key: str) -> str | None:
        if not self._client:
            return None
        return self._client.sys().get(key, None)
'''
    _write_file(os.path.join(agent_dir, "nodes", "databus_client.py"), databus_py)
    generated_files.append("nodes/databus_client.py")

    # ── tools/ directory ──
    _write_file(os.path.join(agent_dir, "tools", "__init__.py"),
                '''"""
Tool Registry — auto-discovers and loads all tools from this directory.

Each tool module must export:
    TOOL_NAME, TOOL_DESCRIPTION, TOOL_PARAMS, execute()
"""

import importlib
import json
import os
import pkgutil
import sys
from types import ModuleType

_TOOLS_DIR = os.path.dirname(os.path.abspath(__file__))

_registry: dict[str, ModuleType] = {}
_llm_tools: list[dict] = []


def _discover():
    global _registry, _llm_tools
    _registry = {}
    _llm_tools = []
    for importer, mod_name, is_pkg in pkgutil.iter_modules([_TOOLS_DIR]):
        if mod_name.startswith("_"):
            continue
        try:
            mod = importlib.import_module(f"tools.{mod_name}")
        except Exception as e:
            print(f"[TOOL_LOADER] Failed to load tool '{mod_name}': {e}", file=sys.stderr)
            continue
        for attr in ("TOOL_NAME", "TOOL_DESCRIPTION", "TOOL_PARAMS", "execute"):
            if not hasattr(mod, attr):
                print(f"[TOOL_LOADER] Tool '{mod_name}' missing '{attr}', skipping", file=sys.stderr)
                break
        else:
            name = mod.TOOL_NAME
            _registry[name] = mod
            _llm_tools.append({
                "type": "function",
                "function": {
                    "name": name,
                    "description": mod.TOOL_DESCRIPTION,
                    "parameters": mod.TOOL_PARAMS,
                }
            })
            print(f"[TOOL_LOADER] Registered tool: {name}", file=sys.stderr)


def get_tools() -> list[dict]:
    if not _registry:
        _discover()
    return list(_llm_tools)


def execute(tool_name: str, tool_input: dict) -> str:
    if not _registry:
        _discover()
    mod = _registry.get(tool_name)
    if mod is None:
        return json.dumps({"error": f"Unknown tool: {tool_name}"}, ensure_ascii=False)
    try:
        return mod.execute(tool_input)
    except Exception as e:
        return json.dumps({"error": f"Tool '{tool_name}' failed: {e}"}, ensure_ascii=False)
''')
    generated_files.append("tools/__init__.py")

    # Generate requested tools
    for tool_name in tools_needed:
        tool_content = _generate_tool_code(tool_name)
        if tool_content:
            _write_file(os.path.join(agent_dir, "tools", f"{tool_name}.py"), tool_content)
            generated_files.append(f"tools/{tool_name}.py")

    # ── .env.example ──
    env_example = f"""# {display_name} Environment Configuration
# Copy to .env and fill in your values

{api_key_var}=your_api_key_here
"""
    _write_file(os.path.join(agent_dir, ".env.example"), env_example)
    generated_files.append(".env.example")

    # ── README.md ──
    readme = f"""# {display_name}

{description}

## 架构

```
用户请求
    │
    ▼
dispatcher (next_skills: ["{agent_name}_run"])
    │  框架路由 → 启动 skill task
    ▼
skill: {agent_name}_run ({run_wf_name})
    │  ┌──────────────────────────────────────────────┐
    │  │  {agent_name}_loop.py                        │
    │  │  1. 读取 agent.md → 模型配置 + 系统提示词      │
    │  │  2. 从 DataBus 读取历史消息                    │
    │  │  3. while True:  LLM 思考 → 调工具 → 回复     │
    │  └──────────────────────────────────────────────┘
    │
    ▼
coordinator (coordinator_collect.json) → 透传
    │
    ▼
  client 收到响应
```

## 目录结构

```
{agent_name}/
├── agent.json              # 任务定义
├── agent.md                # 系统提示词 + 模型配置
├── package.json            # 包元数据
├── {run_wf_name}           # skill workflow
├── coordinator_collect.json # coordinator workflow
├── .env.example            # 环境变量模板
├── README.md
├── nodes/
│   ├── {agent_name}_loop.py    # LLM 内部循环
│   ├── databus_client.py      # DataBus 客户端
│   └── coordinator_collect.py # coordinator 透传
└── tools/
    ├── __init__.py             # 工具注册表
"""
    # Add tool descriptions
    for t in tools_needed:
        readme += f"    ├── {t}.py\n"
    readme += "```\n"

    _write_file(os.path.join(agent_dir, "README.md"), readme)
    generated_files.append("README.md")

    return generated_files


def _generate_tool_code(tool_name: str) -> str | None:
    """Generate tool code for common tools."""
    tools_code = {
        "bash": '''"""bash tool — execute shell commands on the local system."""

import json
import subprocess


TOOL_NAME = "bash"
TOOL_DESCRIPTION = "Execute a shell command on the local system. Returns stdout, stderr, and exit code."
TOOL_PARAMS = {
    "type": "object",
    "properties": {
        "command": {
            "type": "string",
            "description": "The shell command to execute"
        },
        "timeout": {
            "type": "integer",
            "description": "Timeout in seconds (default 30)",
            "default": 30
        }
    },
    "required": ["command"]
}


def execute(tool_input: dict) -> str:
    command = tool_input.get("command", "")
    timeout = tool_input.get("timeout", 30)

    if not command:
        return json.dumps({"error": "command is required", "tool": "bash"},
                          ensure_ascii=False)

    try:
        result = subprocess.run(
            command, shell=True, capture_output=True, text=True, timeout=timeout
        )
        return json.dumps({
            "stdout": result.stdout,
            "stderr": result.stderr,
            "exit_code": result.returncode,
            "tool": "bash",
        }, ensure_ascii=False)
    except subprocess.TimeoutExpired:
        return json.dumps({
            "error": f"Command timed out after {timeout}s",
            "tool": "bash",
        }, ensure_ascii=False)
    except Exception as e:
        return json.dumps({
            "error": f"Command failed: {e}",
            "tool": "bash",
        }, ensure_ascii=False)
''',
        "read_file": '''"""read_file tool — read a file from the local filesystem."""

import json
import os


TOOL_NAME = "read_file"
TOOL_DESCRIPTION = "Read the contents of a file from the local filesystem."
TOOL_PARAMS = {
    "type": "object",
    "properties": {
        "file_path": {"type": "string", "description": "Path to the file to read"},
        "max_chars": {"type": "integer", "description": "Maximum characters to return (default 10000)", "default": 10000}
    },
    "required": ["file_path"]
}


def execute(tool_input: dict) -> str:
    file_path = tool_input.get("file_path", "")
    max_chars = tool_input.get("max_chars", 10000)
    if not file_path:
        return json.dumps({"error": "file_path is required", "tool": "read_file"}, ensure_ascii=False)
    resolved = os.path.abspath(file_path)
    if not os.path.isfile(resolved):
        return json.dumps({"error": f"File not found: {file_path}", "tool": "read_file"}, ensure_ascii=False)
    try:
        with open(resolved, encoding="utf-8", errors="replace") as f:
            content = f.read(max_chars)
        return json.dumps({"content": content, "char_count": len(content), "file_path": resolved, "tool": "read_file"}, ensure_ascii=False)
    except Exception as e:
        return json.dumps({"error": f"Failed to read file: {e}", "tool": "read_file"}, ensure_ascii=False)
''',
        "write_file": '''"""write_file tool — write content to a file on the local filesystem."""

import json
import os


TOOL_NAME = "write_file"
TOOL_DESCRIPTION = "Write content to a file on the local filesystem. Creates parent directories if needed."
TOOL_PARAMS = {
    "type": "object",
    "properties": {
        "file_path": {"type": "string", "description": "Path to the file to write"},
        "content": {"type": "string", "description": "Content to write"},
        "append": {"type": "boolean", "description": "Append instead of overwrite", "default": False}
    },
    "required": ["file_path", "content"]
}


def execute(tool_input: dict) -> str:
    file_path = tool_input.get("file_path", "")
    content = tool_input.get("content", "")
    append = tool_input.get("append", False)
    if not file_path:
        return json.dumps({"error": "file_path is required", "tool": "write_file"}, ensure_ascii=False)
    resolved = os.path.abspath(file_path)
    try:
        os.makedirs(os.path.dirname(resolved), exist_ok=True)
        mode = "a" if append else "w"
        with open(resolved, mode, encoding="utf-8") as f:
            f.write(content)
        return json.dumps({"success": True, "file_path": resolved, "char_count": len(content), "tool": "write_file"}, ensure_ascii=False)
    except Exception as e:
        return json.dumps({"error": f"Failed to write file: {e}", "tool": "write_file"}, ensure_ascii=False)
''',
        "web_fetch": '''"""web_fetch tool — fetch web page content."""

import json
import requests


TOOL_NAME = "web_fetch"
TOOL_DESCRIPTION = "Fetch a web page and return its text content."
TOOL_PARAMS = {
    "type": "object",
    "properties": {
        "url": {"type": "string", "description": "URL to fetch"},
        "max_chars": {"type": "integer", "description": "Maximum characters to return", "default": 10000}
    },
    "required": ["url"]
}


def execute(tool_input: dict) -> str:
    url = tool_input.get("url", "")
    max_chars = tool_input.get("max_chars", 10000)
    if not url:
        return json.dumps({"error": "url is required", "tool": "web_fetch"}, ensure_ascii=False)
    try:
        resp = requests.get(url, timeout=30)
        resp.raise_for_status()
        content = resp.text[:max_chars]
        return json.dumps({"content": content, "char_count": len(content), "url": url, "tool": "web_fetch"}, ensure_ascii=False)
    except Exception as e:
        return json.dumps({"error": f"Failed to fetch: {e}", "tool": "web_fetch"}, ensure_ascii=False)
''',
        "echo": '''"""echo tool — returns input text back for testing."""

import json


TOOL_NAME = "echo"
TOOL_DESCRIPTION = "Echo back the input text. For testing the agent loop."
TOOL_PARAMS = {
    "type": "object",
    "properties": {
        "text": {"type": "string", "description": "Text to echo back"}
    },
    "required": ["text"]
}


def execute(tool_input: dict) -> str:
    text = tool_input.get("text", "")
    return json.dumps({"result": f"Echo: {text}", "tool": "echo"}, ensure_ascii=False)
''',
    }
    return tools_code.get(tool_name)


def _generate_basic_package(params: dict) -> list:
    """Generate a basic-type agent package (DAG workflow mode)."""
    agent_dir = os.path.join(EXAMPLES_DIR, params["agent_name"])
    agent_name = params["agent_name"]
    display_name = params["display_name"]
    description = params["description"]
    author = params.get("author", "AgentFlow User")
    nodes = params.get("nodes", [])

    generated_files = []

    # ── package.json ──
    wf_list = ["workflow.json", "coordinator_workflow.json"]
    pkg = {
        "name": agent_name,
        "version": "1.0.0",
        "description": description,
        "author": author,
        "workflows": wf_list,
    }
    _write_file(os.path.join(agent_dir, "package.json"),
                json.dumps(pkg, indent=2, ensure_ascii=False))
    generated_files.append("package.json")

    # ── agent.json ──
    agent_def = {
        "name": display_name,
        "version": "1.0.0",
        "description": description,
        "session_pool_size": 1,
        "max_workflows": 10,
        "max_concurrent_workflows": 5,
        "tasks": [
            {
                "id": 0,
                "type": "dispatcher",
                "description": f"Dispatch user requests for {display_name}",
                "workflow_file": "workflow.json",
                "workflow_pool_size": 1
            },
            {
                "id": 1,
                "type": "coordinator",
                "description": "Coordinate task outputs",
                "workflow_file": "coordinator_workflow.json",
                "workflow_pool_size": 1
            },
            {
                "id": 2,
                "type": "skill",
                "description": f"Run {display_name} skill",
                "workflow_file": "workflow.json",
                "skill": {
                    "name": f"{agent_name}-skill",
                    "config_dir": f"skills/{agent_name}",
                    "instance_pool_size": 1
                },
                "workflow_pool_size": 1
            }
        ]
    }
    _write_file(os.path.join(agent_dir, "agent.json"),
                json.dumps(agent_def, indent=2, ensure_ascii=False))
    generated_files.append("agent.json")

    # ── Main workflow.json ──
    wf_nodes = [
        {
            "id": "process",
            "name": "Process Node",
            "type": "task",
            "execution_mode": "shell" if not nodes else (
                "python_script" if nodes[0].get("language") == "python" else "shell"
            ),
            "command": f"./nodes/process.sh" if not nodes else (
                f"./nodes/{nodes[0]['name']}.py" if nodes[0].get("language") == "python"
                else f"./nodes/{nodes[0]['name']}.sh"
            ),
            "working_dir": f"examples/agent/{agent_name}",
            "json_output": True,
            "inputs": [
                {"name": "input", "type": "string", "required": True}
            ],
            "outputs": [
                {"name": "result", "type": "string"}
            ],
            "timeout_ms": 30000
        }
    ]
    workflow_json = {
        "id": f"{agent_name}-workflow",
        "version": "1.0",
        "name": f"{agent_name}-workflow",
        "description": f"Main workflow for {display_name}",
        "nodes": wf_nodes,
        "edges": []
    }
    _write_file(os.path.join(agent_dir, "workflow.json"),
                json.dumps(workflow_json, indent=2, ensure_ascii=False))
    generated_files.append("workflow.json")

    # ── coordinator_workflow.json ──
    coord_wf = {
        "id": f"{agent_name}-coordinator",
        "version": "1.0",
        "name": f"{agent_name}-coordinator",
        "description": "Coordinator workflow",
        "nodes": [
            {
                "id": "collect",
                "name": "Collect Output",
                "type": "task",
                "execution_mode": "shell",
                "command": "echo '{\"response\": \"'\"$AF_INPUT\"'\"}'",
                "working_dir": f"examples/agent/{agent_name}",
                "json_output": True,
                "timeout_ms": 1000,
                "inputs": [
                    {"name": "input", "type": "string", "required": True}
                ],
                "outputs": [
                    {"name": "response", "type": "string"}
                ]
            }
        ],
        "edges": []
    }
    _write_file(os.path.join(agent_dir, "coordinator_workflow.json"),
                json.dumps(coord_wf, indent=2, ensure_ascii=False))
    generated_files.append("coordinator_workflow.json")

    # ── nodes directory ──
    if nodes:
        for node in nodes:
            node_name = node["name"]
            node_lang = node.get("language", "shell")
            if node_lang == "python":
                node_content = f'''#!/usr/bin/env python3
"""
{node_name} — process node for {display_name}.
"""

import json
import os


def execute():
    user_input = os.environ.get("AF_INPUT", "")
    # TODO: implement your logic here
    result = {{"result": f"Processed: {{user_input}}", "node": "{node_name}"}}
    print(json.dumps(result, ensure_ascii=False))


if __name__ == "__main__":
    execute()
'''
                ext = "py"
            else:
                node_content = f'''#!/bin/bash
# {node_name} — process node for {display_name}
# TODO: implement your logic here
echo '{{"result": "Processed: $AF_INPUT", "node": "{node_name}"}}'
'''
                ext = "sh"
            _write_file(os.path.join(agent_dir, "nodes", f"{node_name}.{ext}"), node_content)
            generated_files.append(f"nodes/{node_name}.{ext}")
    else:
        _write_file(os.path.join(agent_dir, "nodes", "process.sh"),
                    '#!/bin/bash\necho \'{"result": "Hello from basic agent! Input: \'"$AF_INPUT"\'"}\'\n')
        generated_files.append("nodes/process.sh")

    # ── README.md ──
    readme = f"""# {display_name}

{description}

## 目录结构

```
{agent_name}/
├── agent.json                    # 任务定义
├── package.json                  # 包元数据
├── workflow.json                 # 主 workflow
├── coordinator_workflow.json     # coordinator workflow
├── README.md
└── nodes/
    └── process.sh                # 处理节点
```

## 运行

```bash
./build/agent_runner examples/agent/{agent_name} --interactive
```
"""
    _write_file(os.path.join(agent_dir, "README.md"), readme)
    generated_files.append("README.md")

    return generated_files


def execute(tool_input: dict) -> str:
    agent_name = tool_input.get("agent_name", "")
    template_type = tool_input.get("template_type", "")

    if not agent_name:
        return json.dumps({"error": "agent_name is required", "tool": "create_agent_package"},
                          ensure_ascii=False)
    if not template_type:
        return json.dumps({"error": "template_type is required", "tool": "create_agent_package"},
                          ensure_ascii=False)

    agent_dir = os.path.join(EXAMPLES_DIR, agent_name)

    # Check if agent already exists
    if os.path.exists(agent_dir):
        return json.dumps({
            "error": f"Agent '{agent_name}' already exists at {agent_dir}. "
                     f"Use a different name or remove the existing directory first.",
            "tool": "create_agent_package",
        }, ensure_ascii=False)

    try:
        if template_type == "harness":
            files = _generate_harness_package(tool_input)
        elif template_type == "basic":
            files = _generate_basic_package(tool_input)
        elif template_type in ("multi-session", "nested-workflow", "custom"):
            files = _generate_harness_package(tool_input)  # fallback to harness for now
        else:
            return json.dumps({
                "error": f"Unknown template type: {template_type}",
                "tool": "create_agent_package",
            }, ensure_ascii=False)

        return json.dumps({
            "success": True,
            "agent_name": agent_name,
            "path": agent_dir,
            "files_created": files,
            "total_files": len(files),
            "tool": "create_agent_package",
        }, ensure_ascii=False)

    except Exception as e:
        return json.dumps({
            "error": f"Failed to create agent package: {e}",
            "tool": "create_agent_package",
        }, ensure_ascii=False)
