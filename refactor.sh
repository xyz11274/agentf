#!/bin/bash
# AgentFlow Refactoring Script
# This script reorganizes the project structure

set -e

echo "=== AgentFlow Refactoring Script ==="

# ============================================================================
# Step 1: Create directory structure
# ============================================================================
echo "[Step 1] Creating directory structure..."

# Source directories
mkdir -p src/agent
mkdir -p src/workflow/node
mkdir -p src/workflow/edge
mkdir -p src/workflow/dsl
mkdir -p src/runtime/actor
mkdir -p src/runtime/storage
mkdir -p src/runtime/scheduler
mkdir -p src/runtime/communication
mkdir -p src/runtime/network
mkdir -p src/utils/log

# Header directories
mkdir -p include/external/agent
mkdir -p include/external/workflow/node
mkdir -p include/external/workflow/edge
mkdir -p include/external/workflow/dsl
mkdir -p include/external/runtime/actor
mkdir -p include/external/runtime/storage
mkdir -p include/external/runtime/scheduler
mkdir -p include/external/runtime/communication
mkdir -p include/external/runtime/network
mkdir -p include/internal/adapter/actor
mkdir -p include/internal/adapter/dsl
mkdir -p include/internal/adapter/event
mkdir -p include/internal/adapter/network
mkdir -p include/internal/adapter/serialization

# Vendor directories
mkdir -p vendor/adapter/event
mkdir -p vendor/adapter/network
mkdir -p vendor/adapter/serialization

echo "  Done."

# ============================================================================
# Step 2: Move source files
# ============================================================================
echo "[Step 2] Moving source files..."

# Agent
cp src/core/agent.cpp src/agent/agent.cpp

# Workflow
cp src/core/workflow.cpp src/workflow/workflow.cpp
cp src/core/node.cpp src/workflow/node/node.cpp
cp src/core/edge.cpp src/workflow/edge/edge.cpp
cp src/dsl/workflow_parser.cpp src/workflow/dsl/workflow_parser.cpp
cp src/dsl/json_adapter_factory.cpp src/workflow/dsl/json_adapter_factory.cpp
cp src/actor/node_actor.cpp src/workflow/node/node_actor.cpp
cp src/actor/workflow_actor.cpp src/workflow/workflow_actor.cpp
cp src/runtime/code_loader.cpp src/workflow/node/code_loader.cpp
cp src/storage/workflow_storage.cpp src/workflow/workflow_storage.cpp

# Runtime - actor
cp src/actor/actor_system.cpp src/runtime/actor/actor_system.cpp
cp src/actor/adapter_factory.cpp src/runtime/actor/adapter_factory.cpp
cp src/actor/ask_pattern_manager.cpp src/runtime/actor/ask_pattern_manager.cpp
cp src/actor/message_serializer.cpp src/runtime/actor/message_serializer.cpp

# Runtime - scheduler
cp src/runtime/agent_runtime.cpp src/runtime/scheduler/agent_runtime.cpp
cp src/runtime/control_flow.cpp src/runtime/scheduler/control_flow.cpp
cp src/runtime/performance_metrics.cpp src/runtime/scheduler/performance_metrics.cpp

# Runtime - communication
cp src/runtime/databus.cpp src/runtime/communication/databus.cpp
cp src/runtime/zero_copy_databus.cpp src/runtime/communication/zero_copy_databus.cpp
cp src/runtime/local_channel.cpp src/runtime/communication/local_channel.cpp
cp src/runtime/hot_reload.cpp src/runtime/communication/hot_reload.cpp

# Runtime - storage
cp src/storage/storage.cpp src/runtime/storage/storage.cpp
cp src/storage/storage_manager.cpp src/runtime/storage/storage_manager.cpp
cp src/storage/agent_storage.cpp src/runtime/storage/agent_storage.cpp

# Runtime - network
cp src/network/private_protocol.cpp src/runtime/network/private_protocol.cpp
cp src/network/request.cpp src/runtime/network/request.cpp
cp src/network/response.cpp src/runtime/network/response.cpp

# Runtime - event & metrics
cp src/event/event.cpp src/runtime/event.cpp
# metrics.cpp stays in runtime/

# Utils
cp src/utils/logger.cpp src/utils/log/logger.cpp
cp src/utils/thread_pool.cpp src/utils/log/thread_pool.cpp

echo "  Done."

# ============================================================================
# Step 3: Move header files
# ============================================================================
echo "[Step 3] Moving header files..."

# External - agent
cp include/external/core/agent.h include/external/agent/agent.h

# External - workflow
cp include/external/core/workflow.h include/external/workflow/workflow.h
cp include/external/core/node.h include/external/workflow/node.h
cp include/external/core/edge.h include/external/workflow/edge.h
cp include/external/dsl/workflow_parser.h include/external/workflow/workflow_parser.h
cp include/external/dsl/json_adapter.h include/external/workflow/json_adapter.h

# External - runtime/actor
cp include/external/actor/actor_system.h include/external/runtime/actor/actor_system.h
cp include/external/actor/message_serializer.h include/external/runtime/actor/message_serializer.h

# External - workflow actors (keep original location too for compatibility)
cp include/external/actor/node_actor.h include/external/workflow/node_actor.h
cp include/external/actor/workflow_actor.h include/external/workflow/workflow_actor.h

# External - runtime/scheduler
cp include/external/runtime/agent_runtime.h include/external/runtime/scheduler/agent_runtime.h
cp include/external/runtime/control_flow.h include/external/runtime/scheduler/control_flow.h

# External - runtime/communication
cp include/external/runtime/databus.h include/external/runtime/communication/databus.h
cp include/external/runtime/zero_copy_databus.h include/external/runtime/communication/zero_copy_databus.h
cp include/external/runtime/local_channel.h include/external/runtime/communication/local_channel.h
cp include/external/runtime/hot_reload.h include/external/runtime/communication/hot_reload.h

# External - runtime/storage
cp include/external/storage/storage.h include/external/runtime/storage/storage.h
cp include/external/storage/storage_manager.h include/external/runtime/storage/storage_manager.h
cp include/external/storage/agent_storage.h include/external/runtime/storage/agent_storage.h

# External - workflow (code_loader)
cp include/external/runtime/code_loader.h include/external/workflow/node/code_loader.h

# External - workflow_storage
cp include/external/storage/workflow_storage.h include/external/workflow/workflow_storage.h

# Internal - adapter
cp include/internal/actor/adapter/protoactor_impl.h include/internal/adapter/actor/protoactor_impl.h
cp include/internal/actor/adapter/stub_impl.h include/internal/adapter/actor/stub_impl.h
cp include/internal/dsl/json_adapter/rapidjson_impl.h include/internal/adapter/dsl/rapidjson_impl.h
cp include/internal/dsl/json_adapter/stub_impl.h include/internal/adapter/dsl/stub_impl.h
cp include/internal/event/eventloop_adapter/libevent_impl.h include/internal/adapter/event/libevent_impl.h
cp include/internal/network/websocket_adapter/libwebsockets_impl.h include/internal/adapter/network/libwebsockets_impl.h
cp include/internal/network/ssl_adapter/openssl_impl.h include/internal/adapter/network/openssl_impl.h
cp include/internal/serialization/proto_adapter/protobuf_impl.h include/internal/adapter/serialization/protobuf_impl.h

echo "  Done."

# ============================================================================
# Step 4: Move vendor adapter files
# ============================================================================
echo "[Step 4] Moving vendor adapter files..."

# Event
cp vendor/adapter/eventloop/libevent_impl.cpp vendor/adapter/event/libevent_impl.cpp
cp vendor/adapter/eventloop/libevent_impl.h vendor/adapter/event/libevent_impl.h
cp src/event/adapter_factory.cpp vendor/adapter/event/adapter_factory.cpp

# Network
cp src/network/adapter_factory.cpp vendor/adapter/network/adapter_factory.cpp

# Serialization
cp src/serialization/adapter_factory.cpp vendor/adapter/serialization/adapter_factory.cpp

echo "  Done."

# ============================================================================
# Step 5: Update include paths in copied files
# ============================================================================
echo "[Step 5] Updating include paths..."

# Function to update include paths
update_include() {
    local file="$1"
    if [ -f "$file" ]; then
        # Core -> agent/workflow
        sed -i 's|#include "core/agent.h"|#include "agent/agent.h"|g' "$file"
        sed -i 's|#include "core/workflow.h"|#include "workflow/workflow.h"|g' "$file"
        sed -i 's|#include "core/node.h"|#include "workflow/node.h"|g' "$file"
        sed -i 's|#include "core/edge.h"|#include "workflow/edge.h"|g' "$file"

        # DSL -> workflow
        sed -i 's|#include "dsl/workflow_parser.h"|#include "workflow/workflow_parser.h"|g' "$file"
        sed -i 's|#include "dsl/json_adapter.h"|#include "workflow/json_adapter.h"|g' "$file"

        # Runtime -> runtime/communication
        sed -i 's|#include "runtime/databus.h"|#include "runtime/communication/databus.h"|g' "$file"
        sed -i 's|#include "runtime/zero_copy_databus.h"|#include "runtime/communication/zero_copy_databus.h"|g' "$file"
        sed -i 's|#include "runtime/local_channel.h"|#include "runtime/communication/local_channel.h"|g' "$file"
        sed -i 's|#include "runtime/hot_reload.h"|#include "runtime/communication/hot_reload.h"|g' "$file"

        # Runtime -> runtime/scheduler
        sed -i 's|#include "runtime/agent_runtime.h"|#include "runtime/scheduler/agent_runtime.h"|g' "$file"
        sed -i 's|#include "runtime/control_flow.h"|#include "runtime/scheduler/control_flow.h"|g' "$file"

        # Runtime -> workflow/node
        sed -i 's|#include "runtime/code_loader.h"|#include "workflow/node/code_loader.h"|g' "$file"

        # Storage -> runtime/storage
        sed -i 's|#include "storage/storage.h"|#include "runtime/storage/storage.h"|g' "$file"
        sed -i 's|#include "storage/storage_manager.h"|#include "runtime/storage/storage_manager.h"|g' "$file"
        sed -i 's|#include "storage/agent_storage.h"|#include "runtime/storage/agent_storage.h"|g' "$file"
        sed -i 's|#include "storage/workflow_storage.h"|#include "workflow/workflow_storage.h"|g' "$file"

        # Actor -> runtime/actor
        sed -i 's|#include "actor/actor_system.h"|#include "runtime/actor/actor_system.h"|g' "$file"
        sed -i 's|#include "actor/message_serializer.h"|#include "runtime/actor/message_serializer.h"|g' "$file"
        sed -i 's|#include "actor/adapter_factory.h"|#include "runtime/actor/adapter_factory.h"|g' "$file"
        sed -i 's|#include "actor/ask_pattern_manager.h"|#include "runtime/actor/ask_pattern_manager.h"|g' "$file"

        # Actor -> workflow
        sed -i 's|#include "actor/node_actor.h"|#include "workflow/node_actor.h"|g' "$file"
        sed -i 's|#include "actor/workflow_actor.h"|#include "workflow/workflow_actor.h"|g' "$file"

        # Internal adapter paths
        sed -i 's|#include "actor/adapter/protoactor_impl.h"|#include "adapter/actor/protoactor_impl.h"|g' "$file"
        sed -i 's|#include "actor/adapter/stub_impl.h"|#include "adapter/actor/stub_impl.h"|g' "$file"
        sed -i 's|#include "dsl/json_adapter/rapidjson_impl.h"|#include "adapter/dsl/rapidjson_impl.h"|g' "$file"
        sed -i 's|#include "dsl/json_adapter/stub_impl.h"|#include "adapter/dsl/stub_impl.h"|g' "$file"
        sed -i 's|#include "event/eventloop_adapter/libevent_impl.h"|#include "adapter/event/libevent_impl.h"|g' "$file"
        sed -i 's|#include "network/websocket_adapter/libwebsockets_impl.h"|#include "adapter/network/libwebsockets_impl.h"|g' "$file"
        sed -i 's|#include "network/ssl_adapter/openssl_impl.h"|#include "adapter/network/openssl_impl.h"|g' "$file"
        sed -i 's|#include "serialization/proto_adapter/protobuf_impl.h"|#include "adapter/serialization/protobuf_impl.h"|g' "$file"
    fi
}

# Update all copied source files
for file in $(find src/agent src/workflow src/runtime src/utils/log -name "*.cpp" 2>/dev/null); do
    update_include "$file"
done

# Update all copied header files
for file in $(find include/external/agent include/external/workflow include/external/runtime include/internal/adapter -name "*.h" 2>/dev/null); do
    update_include "$file"
done

# Update vendor adapter files
for file in $(find vendor/adapter/event vendor/adapter/network vendor/adapter/serialization -name "*.cpp" -o -name "*.h" 2>/dev/null); do
    update_include "$file"
done

echo "  Done."

echo ""
echo "=== Refactoring Complete ==="
echo "New source files are in:"
echo "  src/agent/"
echo "  src/workflow/"
echo "  src/runtime/"
echo "  src/utils/log/"
echo ""
echo "New header files are in:"
echo "  include/external/agent/"
echo "  include/external/workflow/"
echo "  include/external/runtime/"
echo "  include/internal/adapter/"
echo ""
echo "Vendor adapters are in:"
echo "  vendor/adapter/event/"
echo "  vendor/adapter/network/"
echo "  vendor/adapter/serialization/"
echo ""
echo "Next steps:"
echo "1. Update CMakeLists.txt with new source paths"
echo "2. Update test files"
echo "3. Update example files"
echo "4. Delete old directories"
echo "5. Build and test"
