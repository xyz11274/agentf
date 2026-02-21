# Workflow/Node Development Guide

## Overview
This guide covers developing and deploying Workflows and Nodes using AgentFlow framework.

## Node Development

### What is a Node?
A Node is a basic execution unit in a Workflow DAG (Directed Acyclic Graph). Each node represents a task that can be executed independently.

### Node Execution Modes
AgentFlow supports multiple execution modes:

1. **CPP_FUNCTION** - Execute C++ functions directly
2. **SHELL_SCRIPT** - Execute shell scripts
3. **EXECUTABLE** - Execute external programs
4. **PYTHON_SCRIPT** - Execute Python scripts
5. **SO_LIBRARY** - Execute shared object libraries

### Creating a Node

#### Via Code
```cpp
#include "workflow/node.h"

using namespace agentflow::framework;

Node::Config config;
config.id = "my_node";
config.name = "My First Node";
config.execution_mode = Node::ExecutionMode::CPP_FUNCTION;
config.cpp_function = [](const DataMap& inputs, DataMap& outputs) -> bool {
  // Your logic here
  outputs["result"] = inputs["input"] + 100;
  return true;
};

Node node(config);
node.initialize();
node.execute();
```

#### Via JSON Configuration
```json
{
  "id": "my_node",
  "name": "My First Node",
  "type": "computation",
  "execution_mode": "CPP_FUNCTION",
  "input_params": ["input"],
  "output_params": ["result"],
  "max_retries": 3,
  "timeout_ms": 5000
}
```

### Node State Machine
Nodes follow this state progression:
- **WAITING** - Initial state
- **READY** - After initialization
- **EXECUTING** - During execution
- **SUCCESS** - After successful completion
- **FAILED** - After failure
- **STOPPED** - Stopped by user
- **TERMINATED** - Forcefully terminated
- **RETRYING** - Retrying after failure

### Input/Output Handling

Nodes accept inputs and produce outputs using a key-value map:

```cpp
DataMap inputs;
inputs["temperature"] = 25.5;
inputs["humidity"] = 60;

node.set_inputs(inputs);
node.execute();

DataMap outputs = node.get_outputs();
double result = std::get<double>(outputs["result"]);
```

### Error Handling and Retries

Configure retry behavior:
```cpp
config.max_retries = 3;           // Maximum retry attempts
config.retry_delay_ms = 1000;     // Delay between retries
config.timeout_ms = 5000;         // Execution timeout
```

### Hot Reload

Enable code reloading without restarting:
```cpp
config.enable_hot_reload = true;
config.hot_reload_config.enabled = true;
config.hot_reload_config.check_interval_ms = 1000;
config.hot_reload_config.auto_reload = true;
```

## Workflow Development

### What is a Workflow?
A Workflow is a collection of Nodes connected by Edges, representing a DAG of tasks to execute in a coordinated manner.

### Workflow Configuration

#### Via JSON DSL
```json
{
  "id": "sample_workflow",
  "name": "Sample Processing Workflow",
  "nodes": [
    {
      "id": "node1",
      "name": "Extract Data",
      "execution_mode": "PYTHON_SCRIPT",
      "command": "extract.py"
    },
    {
      "id": "node2",
      "name": "Process Data",
      "execution_mode": "CPP_FUNCTION"
    }
  ],
  "edges": [
    {
      "from_node_id": "node1",
      "to_node_id": "node2"
    }
  ]
}
```

#### Via Code
```cpp
#include "workflow/workflow.h"

using namespace agentflow::framework;

Workflow::Config wf_config;
wf_config.id = "my_workflow";
wf_config.name = "My Workflow";

Workflow workflow(wf_config);
workflow.add_node(node1);
workflow.add_node(node2);
workflow.add_edge(node1.get_id(), node2.get_id());
workflow.execute();
```

## Node Adapter

The Node Adapter is a standalone executable for executing individual nodes from the command line.

### Usage
```bash
node_adapter_bin --config <config.json> --inputs <inputs.json> --output <output.json>
```

### Example Configuration
```json
{
  "id": "standalone_node",
  "name": "Standalone Execution",
  "execution_mode": "PYTHON_SCRIPT",
  "command": "my_script.py",
  "input_params": ["input_file", "mode"],
  "output_params": ["result"],
  "timeout_ms": 30000
}
```

### Example Inputs
```json
{
  "input_file": "/path/to/input.txt",
  "mode": "process"
}
```

### Results
After execution, results are written to the output file:
```json
{
  "result": "processed_output",
  "status": "SUCCESS"
}
```

## Best Practices

### Performance
1. Use CPP_FUNCTION for CPU-bound work
2. Use PYTHON_SCRIPT for data science tasks
3. Use SHELL_SCRIPT for integration with external tools
4. Set appropriate timeouts to prevent hanging

### Reliability
1. Always implement error handling
2. Configure max_retries appropriately
3. Use meaningful node IDs and names
4. Test nodes independently before adding to workflows

### Scalability
1. Break large tasks into smaller nodes
2. Enable parallelism where possible
3. Monitor resource usage
4. Use hot reload during development

### Debugging
1. Enable debug logging
2. Check log files for execution details
3. Use the visualizer to understand workflow structure
4. Test node inputs/outputs explicitly

## Advanced Topics

### Custom Node Types
Create custom execution modes by extending the Node class.

### Resource Management
Set memory and CPU limits to prevent resource exhaustion:
```cpp
config.memory_limit_mb = 512;
config.cpu_limit = 2;
```

### Event Handling
Nodes can emit events during execution for monitoring:
```cpp
// Events are automatically emitted by the framework
// Subscribe to events via the event bus
```

## Troubleshooting

### Node Won't Execute
- Check execution_mode is valid
- Verify command/script path exists
- Check file permissions
- Review logs for detailed errors

### Timeout Issues
- Increase timeout_ms if task takes longer
- Profile your code to identify bottlenecks
- Consider breaking into smaller nodes

### Memory Issues
- Set memory_limit_mb to enforce limits
- Check for memory leaks in C++ code
- Monitor output data size

## See Also
- Full API Reference: `/publish/README.md`
- Examples: `/examples/workflow/`
- Agent Development: `/publish/agent/agentCard.md`
