# Agent Card

## Overview
A sample agent template for AgentFlow framework.

## Features
- Workflow execution
- Data bus communication
- Event handling

## Configuration
Agents can be configured via `package.json` with the following options:

```json
{
  "agentFlow": {
    "type": "agent",
    "capabilities": {
      "workflows": true,
      "databus": true,
      "events": true
    }
  }
}
```

## Building
```bash
cmake . && make
```

## Usage
```cpp
#include "agent/agent.h"

using namespace agentflow;
framework::Agent agent;
agent.initialize();
// Use agent...
```

## API Reference
See `/publish/README.md` for complete API documentation.

## Requirements
- C++17 or later
- CMake 3.10+
- AgentFlow core library

## License
MIT License
