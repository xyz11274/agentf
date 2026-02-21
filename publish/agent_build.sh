#!/bin/bash
# Agent Build Script
# This script builds and packages AgentFlow agents

set -e

SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
PUBLISH_DIR="$PROJECT_ROOT/publish"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# Functions
print_info() {
    echo -e "${GREEN}[INFO]${NC} $1"
}

print_warn() {
    echo -e "${YELLOW}[WARN]${NC} $1"
}

print_error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

print_usage() {
    cat << EOF
Usage: agent_build.sh [COMMAND] [OPTIONS]

Commands:
    build [source] [output]     Build and package agent
    clean                       Clean build artifacts
    package                     Package agent for distribution
    publish                     Publish agent package
    help                        Show this help message

Examples:
    ./agent_build.sh build examples/agent/basic_agent build/basic_agent.zip
    ./agent_build.sh build examples/agent/calculator_agent build/calculator_agent.zip
    ./agent_build.sh clean
    ./agent_build.sh package
EOF
}

stage_source_with_builtins() {
    local source="$1"
    local stage_dir="$2"
    rm -rf "$stage_dir"
    mkdir -p "$stage_dir"
    cp -R "$source"/. "$stage_dir"/

    local builtins
    builtins=$(python - "$source/package.json" <<'PY'
import json,sys
try:
    with open(sys.argv[1], "r", encoding="utf-8") as f:
        data = json.load(f)
    arr = data.get("builtin_workflows", [])
    if isinstance(arr, list):
        print("\n".join([x for x in arr if isinstance(x, str)]))
except Exception:
    pass
PY
)

    if [ -z "$builtins" ]; then
        return 0
    fi

    mkdir -p "$stage_dir/workflows/builtin"
    while IFS= read -r builtin; do
        [ -z "$builtin" ] && continue
        local src_dir="$PUBLISH_DIR/workflow/$builtin"
        local dst_dir="$stage_dir/workflows/builtin/$builtin"
        if [ -d "$src_dir" ]; then
            cp -R "$src_dir" "$dst_dir"
            print_info "Included built-in workflow: $builtin"
        else
            print_warn "Built-in workflow not found: $builtin"
        fi
    done <<< "$builtins"
}

build_agents() {
    local source="${1:-$PUBLISH_DIR/agent}"
    local output="${2:-}"
    
    print_info "Building agents..."
    
    if [ ! -d "$source" ]; then
        print_error "Source directory not found: $source"
        return 1
    fi
    
    # Verify package.json exists in source
    if [ ! -f "$source/package.json" ]; then
        print_error "package.json not found in $source"
        return 1
    fi
    
    print_info "Building from $source"
    
    # Create output directory if specified
    if [ -n "$output" ]; then
        output_dir=$(dirname "$output")
        mkdir -p "$output_dir"
        
        # Create a staged package with opt-in built-in workflows.
        local stage_dir="$PROJECT_ROOT/build/.agent_stage_$(basename "$source")"
        stage_source_with_builtins "$source" "$stage_dir" || {
            print_error "Failed to stage source package"
            return 1
        }
        cd "$stage_dir"
        zip -r "$PROJECT_ROOT/$output" . -x "*.git*" "build/*" || {
            print_error "Failed to create package"
            return 1
        }
        rm -rf "$stage_dir"
        cd "$PROJECT_ROOT"
        print_info "Agent package created: $output"
    elif [ -f "$source/CMakeLists.txt" ]; then
        # Check if this is a CMake project
        cd "$source"
        mkdir -p build
        cd build
        cmake ..
        make
        cd "$PROJECT_ROOT"
        print_info "Agent build completed"
    else
        print_warn "No CMakeLists.txt found in $source, creating package only"
        # Create a basic package
        local stage_dir="$PROJECT_ROOT/build/.agent_stage_$(basename "$source")"
        stage_source_with_builtins "$source" "$stage_dir" || {
            print_error "Failed to stage source package"
            return 1
        }
        cd "$stage_dir"
        zip -r "$PROJECT_ROOT/build/$(basename $source).zip" . -x "*.git*" "build/*" || {
            print_error "Failed to create package"
            return 1
        }
        rm -rf "$stage_dir"
        cd "$PROJECT_ROOT"
        print_info "Agent package created: build/$(basename $source).zip"
    fi
}

clean_artifacts() {
    print_info "Cleaning build artifacts..."
    
    # Clean agent build
    if [ -d "$PUBLISH_DIR/agent/build" ]; then
        rm -rf "$PUBLISH_DIR/agent/build"
        print_info "Cleaned agent/build"
    fi
    
    # Clean workflow build
    if [ -d "$PUBLISH_DIR/workflow/build" ]; then
        rm -rf "$PUBLISH_DIR/workflow/build"
        print_info "Cleaned workflow/build"
    fi
    
    print_info "Cleanup completed"
}

package_agents() {
    print_info "Packaging agents..."
    
    # Verify package.json exists
    if [ ! -f "$PUBLISH_DIR/agent/package.json" ]; then
        print_error "package.json not found"
        return 1
    fi
    
    print_info "Agent package ready: $PUBLISH_DIR/agent"
    print_info "Run './agent_build.sh publish' to publish"
}

publish_agents() {
    print_info "Publishing agents..."
    print_warn "Publish functionality is a stub (not yet implemented)"
    print_info "To publish, prepare the agent package and upload to registry"
}

# Main
case "${1:-help}" in
    build)
        build_agents "$2" "$3"
        ;;
    clean)
        clean_artifacts
        ;;
    package)
        package_agents
        ;;
    publish)
        publish_agents
        ;;
    help)
        print_usage
        ;;
    *)
        print_error "Unknown command: $1"
        print_usage
        exit 1
        ;;
esac

exit 0
