# Vendor Adapters

This directory contains adapter implementations for third-party libraries that are used in AgentFlow.

## Directory Structure

```
vendor/adapter/
├── dsl/              # DSL parser adapter implementations (rapidjson, etc.)
└── actor/            # Actor framework adapter implementations (protoactor, etc.)
```

## Purpose

Third-party library adapters provide a uniform interface for AgentFlow to interact with different implementations of common functionality. This allows:

- **Pluggable implementations**: Switch between different libraries without changing core code
- **Isolation**: Keep third-party dependencies separate from core implementation
- **Testing**: Easier to mock and test core functionality
- **Portability**: Support multiple platforms with different available libraries

## Adding a New Adapter

1. Create a new directory under the appropriate category (e.g., `network/my_adapter`)
2. Implement the adapter interface defined in `include/external/agentflow/<category>/adapter.h`
3. Add any required third-party library to `vendor.conf` for download
4. Update `CMakeLists.txt` to include the new adapter

## Example: Adding a Network Adapter

```cpp
// vendor/adapter/network/my_adapter/my_adapter_impl.h

namespace agentflow {
namespace network {
namespace my_adapter {

public:
    bool connect(const std::string& url) override;
    bool send(const Message& msg) override;
    void disconnect() override;
};

} // namespace my_adapter
} // namespace network
} // namespace agentflow
```

## License

Adapter implementations follow the same license as the parent project. Refer to individual third-party library licenses for their specific terms.
