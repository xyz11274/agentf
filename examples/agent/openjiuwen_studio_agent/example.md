# OpenJiuwen Studio Agent 示例

`examples/agent/openjiuwen_studio_agent` 展示了一条**大颗粒集成**路径：先不把 agent-studio 的画布语义细拆为 AgentFlow 原生 workflow，而是把 Studio 编排结果作为 bundled artifact 打入 AgentFlow agent 包，再通过包内适配器 node 完成两边语义转换。

## 目标

- 先让一个 Studio 编排后的 agent 能在 AgentFlow 中跑起来。
- `dispatcher/coordinator/skill/contextupdater` 由 AgentFlow 包提供默认壳。
- 语义转换尽量放在示例包和 vendored openJiuwen 兼容层中完成。
- 避免为首个版本大改 AgentFlow 核心执行模型。

## 目录结构

```text
examples/agent/openjiuwen_studio_agent/
  agent.json
  package.json
  dispatcher_workflow.json
  coordinator_workflow.json
  studio_skill_workflow.json
  context_workflow.json
  studio_bundle/
    agent_manifest.json
    workflows/studio-chat-workflow.json
    prompts/system_prompt.txt
  nodes/python/studio_adapter_node.py
  openjiuwen_studio_agent_tui.cpp
```

## 运行路径

1. TUI 把用户输入交给 `openjiuwen-studio-dispatcher-workflow`。
2. dispatcher 调用 `studio_adapter_node.py`。
3. 适配器从 `studio_bundle/` 读取 Studio 编排产物。
4. 适配器借助 vendored `openjiuwen.core.studio_runtime` 执行大颗粒转换：
   - AgentFlow 请求上下文 -> Studio bundle 输入
   - Studio bundle 输出 -> AgentFlow `response` / `context_summary` / trace
5. 响应通过 AgentFlow 现有消息队列返回到 TUI。

## 为什么是大颗粒方案

当前看到的 `agent-studio` 后端模型更偏向 DB 驱动的 agent/workflow 发布对象，而 AgentFlow 运行时更偏向文件化 package + workflow JSON。首个版本如果直接追求逐节点、逐控制流的完整 native 映射，改动面会很大。

这个示例先验证一件更关键的事情：**Studio 编排结果可以作为 bundle 随 AgentFlow 包交付，并通过适配器 node 被 AgentFlow 正常调度和执行。**

## 当前 bundle 内容

示例内的 `studio_bundle/` 保存一个最小 Studio agent 快照，包含：

- `agent_manifest.json`：agent 元数据、插件、memory、knowledge、模型配置
- `workflows/studio-chat-workflow.json`：一个简化的 Studio workflow schema
- `prompts/system_prompt.txt`：示例 system prompt

后续如果拿到稳定的 Studio 导出格式，可以把这里的 bundle 替换成真实导出产物，而不必先重写 AgentFlow 核心调度模型。

## 构建与测试

```bash
./build.sh build
./build/example_openjiuwen_studio_agent_tui examples/agent/openjiuwen_studio_agent
bash examples/agent/openjiuwen_studio_agent/tests/test_openjiuwen_studio_agent_tui.sh
./build.sh test
```

## 后续演进

当这个大颗粒样例稳定后，可以继续往两个方向演进：

- 提炼通用 importer，把 `studio_bundle/` 的打包和壳文件生成自动化。
- 逐步把更多 Studio node/control-flow 从 adapter 模式下沉到 AgentFlow 原生 workflow DSL。
