# OpenJiuwen Studio Agent

`openjiuwen_studio_agent` 是一个“大颗粒集成”示例：它把 agent-studio 编排结果作为 bundled artifact 打进 AgentFlow agent 包，再通过包内 Python 适配器 node 将 AgentFlow 的请求/响应语义转换到 Studio bundle。

## 目录

```text
examples/agent/openjiuwen_studio_agent/
  agent.json
  package.json
  dispatcher_workflow.json
  coordinator_workflow.json
  studio_skill_workflow.json
  context_workflow.json
  studio_bundle/
  nodes/python/studio_adapter_node.py
  tests/test_openjiuwen_studio_agent_tui.sh
```

## 运行

```bash
./build.sh build
./build/example_openjiuwen_studio_agent_tui examples/agent/openjiuwen_studio_agent
```

## 测试

```bash
bash examples/agent/openjiuwen_studio_agent/tests/test_openjiuwen_studio_agent_tui.sh
```
