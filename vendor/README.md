# Vendor Dependencies and Adapters

This directory contains third-party dependencies and their adapter implementations.

## Directory Structure

```
vendor/
├── vendor.conf        # Third-party dependencies configuration
├── vendor.sh          # Dependencies download script
├── README.md          # This file
├── adapter/           # Adapter implementations for third-party libraries
│   ├── actor/         # Actor framework adapters (protoactor, stub)
│   ├── dsl/           # DSL/JSON parser adapters (rapidjson, stub)
│   ├── eventloop/     # Event loop adapters (libevent)
│   ├── network/       # Network adapters (reserved)
│   ├── serialization/ # Serialization adapters (reserved)
│   └── README.md
└── <downloaded>/      # Downloaded third-party libraries (gitignored)
    ├── json/          # e.g., nlohmann_json
    ├── fmt/           # e.g., fmtlib
    └── ...
```

## Quick Start

### 1. Configure Dependencies

Edit `vendor.conf` in this directory to add your dependencies:

```bash
# Format: name|url|branch|target_dir
nlohmann_json|https://github.com/nlohmann/json.git|develop|json
fmtlib|https://github.com/fmtlib/fmt.git|master|fmt
```

### 2. Download Dependencies

```bash
./vendor/vendor.sh
```

This will download all configured dependencies to this `vendor/` directory.

## Usage

```bash
# Download all configured dependencies
./vendor/vendor.sh

# List configured dependencies and their status
./vendor/vendor.sh --list

# Update all downloaded dependencies
./vendor/vendor.sh --update

# Clean vendor directory (removes all downloaded code)
./vendor/vendor.sh --clean

# Show help
./vendor/vendor.sh --help
```

## Configuration Format

Each line in `vendor.conf` should follow this format:

```
name|url|branch|target_dir
```

- **name**: A descriptive name for the dependency
- **url**: Git repository URL
- **branch**: Branch/tag to clone
- **target_dir**: Directory name under vendor/

Example:
```
nlohmann_json|https://github.com/nlohmann/json.git|develop|json
```

This will clone the repository to `vendor/json/`.

## Adapters

The `adapter/` subdirectory contains adapter implementations that wrap third-party libraries:

- **actor/**: Actor framework adapters (protoactor_impl.cpp, stub_impl.cpp)
- **dsl/**: JSON parser adapters (json_adapter_rapidjson.cpp, json_adapter_stub.cpp)
- **eventloop/**: Event loop adapters (libevent_impl.cpp)

These adapters provide a uniform interface for AgentFlow to interact with different implementations.

## Notes

- Downloaded third-party libraries are excluded from Git (see `.gitignore`)
- Adapter source code IS tracked by Git
- Each developer needs to run `./vendor/vendor.sh` after cloning the project
- Use `--update` to pull latest changes from upstream repositories
- Use `--clean` to remove all vendor code and re-download

## Integration with CMake

The CMakeLists.txt includes vendor adapter sources:

```cmake
set(AGENTFLOW_VENDOR_DIR ${CMAKE_CURRENT_SOURCE_DIR}/vendor)

# Adapter sources
set(AGENTFLOW_ADAPTER_SOURCES
    ${AGENTFLOW_VENDOR_DIR}/adapter/actor/stub_impl.cpp
    ${AGENTFLOW_VENDOR_DIR}/adapter/dsl/json_adapter_stub.cpp
    ${AGENTFLOW_VENDOR_DIR}/adapter/eventloop/libevent_impl.cpp
    ...
)
```

To use downloaded vendor libraries:

```cmake
# Example: Add vendor library include path
include_directories(${CMAKE_SOURCE_DIR}/vendor/json/include)

# Example: Add vendor library as subdirectory
add_subdirectory(${CMAKE_SOURCE_DIR}/vendor/fmt)
```
