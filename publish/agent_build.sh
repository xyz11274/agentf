#!/bin/bash
# Agent Build Script
set -e

SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
PUBLISH_DIR="$PROJECT_ROOT/publish"

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

print_info() { echo -e "${GREEN}[INFO]${NC} $1"; }
print_warn() { echo -e "${YELLOW}[WARN]${NC} $1"; }
print_error() { echo -e "${RED}[ERROR]${NC} $1"; }

print_usage() {
    cat << EOF2
Usage: agent_build.sh [COMMAND] [OPTIONS]

Commands:
    build [source] [output_dir]   Build and package agent to spec tar.gz
    clean                         Clean build artifacts
    package                       Validate default publish/agent package metadata
    publish                       Publish agent package (stub)
    help                          Show this help message

Examples:
    ./agent_build.sh build publish/agent build
    ./agent_build.sh build examples/agent/basic_agent build
EOF2
}

ensure_agent_name() {
    local n="$1"
    if [[ "$n" == *Agent ]]; then
        echo "$n"
    else
        echo "${n}Agent"
    fi
}

read_json_field() {
    local file="$1"
    local expr="$2"
    python - "$file" "$expr" <<'PY'
import json, sys
f, expr = sys.argv[1], sys.argv[2]
try:
    with open(f, 'r', encoding='utf-8') as fp:
        data = json.load(fp)
except Exception:
    print("")
    raise SystemExit(0)
obj = data
for p in expr.split('.'):
    if isinstance(obj, dict):
        obj = obj.get(p)
    else:
        obj = None
        break
print(obj if isinstance(obj, str) else "")
PY
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

    [ -z "$builtins" ] && return 0
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
    local output_dir="${2:-$PROJECT_ROOT/build}"

    if [ ! -d "$source" ]; then
        print_error "Source directory not found: $source"
        return 1
    fi
    if [ ! -f "$source/package.json" ]; then
        print_error "package.json not found in $source"
        return 1
    fi

    mkdir -p "$output_dir"

    local raw_name
    raw_name=$(read_json_field "$source/package.json" "name")
    [ -z "$raw_name" ] && raw_name="Sample"
    local agent_name
    agent_name=$(ensure_agent_name "$raw_name")

    local pkg_type pkg_ver agt_ver
    pkg_type=$(read_json_field "$source/package.json" "package_type")
    pkg_ver=$(read_json_field "$source/package.json" "version")
    agt_ver=$(read_json_field "$source/agent.json" "version")
    [ -z "$pkg_type" ] && pkg_type="release"
    [ -z "$pkg_ver" ] && pkg_ver="1.0.0"
    [ -z "$agt_ver" ] && agt_ver="$pkg_ver"

    local stage_dir="$PROJECT_ROOT/build/.agent_stage_$(basename "$source")"
    local inner_dir="$PROJECT_ROOT/build/.agent_inner_$(basename "$source")"
    local outer_dir="$PROJECT_ROOT/build/.agent_outer_$(basename "$source")"
    local inner_name="${agent_name}_${agt_ver}.tar.gz"
    local outer_name="${agent_name}_${pkg_type}_${pkg_ver}.tar.gz"

    stage_source_with_builtins "$source" "$stage_dir"

    rm -rf "$inner_dir" "$outer_dir"
    mkdir -p "$inner_dir" "$outer_dir"

    cp -R "$stage_dir"/. "$inner_dir"/
    rm -f "$inner_dir/package.json"
    tar -czf "$output_dir/$inner_name" -C "$inner_dir" .

    cp "$output_dir/$inner_name" "$outer_dir/$inner_name"
    cp "$source/package.json" "$outer_dir/package.json"
    tar -czf "$output_dir/$outer_name" -C "$outer_dir" .

    rm -rf "$stage_dir" "$inner_dir" "$outer_dir"

    print_info "Inner package created: $output_dir/$inner_name"
    print_info "Outer package created: $output_dir/$outer_name"
}

clean_artifacts() {
    print_info "Cleaning build artifacts..."
    rm -rf "$PROJECT_ROOT/build/.agent_stage_"* "$PROJECT_ROOT/build/.agent_inner_"* "$PROJECT_ROOT/build/.agent_outer_"* || true
    print_info "Cleanup completed"
}

package_agents() {
    if [ ! -f "$PUBLISH_DIR/agent/package.json" ]; then
        print_error "package.json not found in publish/agent"
        return 1
    fi
    if [ ! -f "$PUBLISH_DIR/agent/agent.json" ]; then
        print_error "agent.json not found in publish/agent"
        return 1
    fi
    print_info "Package metadata ready: $PUBLISH_DIR/agent"
}

publish_agents() {
    print_info "Publishing agents..."
    print_warn "Publish functionality is a stub (not yet implemented)"
}

case "${1:-help}" in
    build) build_agents "$2" "$3" ;;
    clean) clean_artifacts ;;
    package) package_agents ;;
    publish) publish_agents ;;
    help) print_usage ;;
    *)
        print_error "Unknown command: $1"
        print_usage
        exit 1
        ;;
esac

exit 0
