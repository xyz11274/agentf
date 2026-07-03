#!/bin/bash
# Multi-Language DataBus Test Script
# Tests the chain of C++ -> Python -> Go -> Shell nodes

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
BUILD_DIR="$PROJECT_ROOT/build"

echo "============================================"
echo "Multi-Language DataBus Chain Test"
echo "============================================"
echo ""

# Build the project if needed
if [ ! -d "$BUILD_DIR" ]; then
    echo "Building project first..."
    cd "$PROJECT_ROOT"
    ./build.sh build
fi

# Build the multi-language test agent libraries
echo "Building multi-language test agent libraries..."
cd "$BUILD_DIR"
cmake --build . --target chain_cpp_node chain_coordinator_node 2>/dev/null || {
    echo "Libraries may already be built or need to be added to main CMakeLists.txt"
}

# Check if libraries exist
LIB_DIR="$BUILD_DIR/lib"
if [ ! -f "$LIB_DIR/libchain_cpp_node.so" ]; then
    echo "Building chain_cpp_node.so..."
    cd "$SCRIPT_DIR"
    g++ -shared -fPIC -o "$LIB_DIR/libchain_cpp_node.so" \
        -I"$PROJECT_ROOT/include" \
        -I"$PROJECT_ROOT/include/external" \
        -I"$PROJECT_ROOT" \
        nodes/chain_cpp_node.cpp \
        -L"$LIB_DIR" -lagentflow -Wl,-rpath,"$LIB_DIR"
fi

if [ ! -f "$LIB_DIR/libchain_coordinator_node.so" ]; then
    echo "Building chain_coordinator_node.so..."
    cd "$SCRIPT_DIR"
    g++ -shared -fPIC -o "$LIB_DIR/libchain_coordinator_node.so" \
        -I"$PROJECT_ROOT/include" \
        -I"$PROJECT_ROOT/include/external" \
        -I"$PROJECT_ROOT" \
        nodes/chain_coordinator_node.cpp \
        -L"$LIB_DIR" -lagentflow -Wl,-rpath,"$LIB_DIR"
fi

# Make scripts executable
chmod +x "$SCRIPT_DIR/nodes/python_transformer.py"
chmod +x "$SCRIPT_DIR/nodes/go_processor.go"
chmod +x "$SCRIPT_DIR/nodes/shell_verifier.sh"

echo ""
echo "Libraries built:"
ls -la "$LIB_DIR"/libchain*.so 2>/dev/null || echo "No chain libraries found"
echo ""
echo "Scripts ready:"
ls -la "$SCRIPT_DIR/nodes/"*.py "$SCRIPT_DIR/nodes/"*.sh 2>/dev/null || true
echo ""
echo "============================================"
echo "Test Setup Complete"
echo "============================================"
echo ""
echo "To run the multi-language test agent:"
echo "  cd $PROJECT_ROOT"
echo "  LLM_API_KEY=sk-xxx ./build/bin/run_agent_example multi_language_databus"
echo ""