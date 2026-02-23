#!/bin/bash

# AgentFlow Build Script
# Usage: ./build.sh [clean|build|test|example|rebuild]
# Supports macOS (Darwin) and Linux with automatic platform detection.

set -e

# ============================================================================
# Platform Detection
# ============================================================================
OS="$(uname -s)"
ARCH="$(uname -m)"

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${PROJECT_DIR}/build"

# Colors for output (auto-detect terminal color support)
if [ -t 1 ] && command -v tput &>/dev/null && [ "$(tput colors 2>/dev/null || echo 0)" -ge 8 ]; then
    RED='\033[0;31m'
    GREEN='\033[0;32m'
    YELLOW='\033[1;33m'
    CYAN='\033[0;36m'
    NC='\033[0m'
else
    RED='' GREEN='' YELLOW='' CYAN='' NC=''
fi

print_info()    { echo -e "${GREEN}[INFO]${NC} $1"; }
print_error()   { echo -e "${RED}[ERROR]${NC} $1"; }
print_warning() { echo -e "${YELLOW}[WARNING]${NC} $1"; }

# ============================================================================
# Cross-platform Helpers
# ============================================================================

# Detect number of CPU cores
detect_cores() {
    local cores=1
    case "$OS" in
        Darwin)  cores=$(sysctl -n hw.ncpu 2>/dev/null || echo 1) ;;
        Linux)   cores=$(nproc 2>/dev/null || grep -c ^processor /proc/cpuinfo 2>/dev/null || echo 1) ;;
        *)       cores=1 ;;
    esac
    echo "$cores"
}

# Detect cmake generator (prefer Ninja if available)
detect_generator() {
    if command -v ninja &>/dev/null; then
        echo "Ninja"
    else
        echo "Unix Makefiles"
    fi
}

# Build the cmake command with cross-platform flags
build_cmake_args() {
    local build_type="${1:-Debug}"
    local generator
    generator="$(detect_generator)"

    local cmake_args=(
        -G "$generator"
        -DCMAKE_BUILD_TYPE="$build_type"
        -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
    )

    # Conda environment detection (works on both macOS and Linux)
    if [ -n "$CONDA_PREFIX" ]; then
        print_info "Detected conda environment: $CONDA_PREFIX"
        cmake_args+=(
            -DCMAKE_PREFIX_PATH="$CONDA_PREFIX"
        )
    fi

    echo "${cmake_args[@]}"
}

# Set dynamic library search path for running test/example binaries
setup_runtime_library_path() {
    local extra_paths=("${BUILD_DIR}" "${BUILD_DIR}/lib")

    # Add conda lib path if present
    if [ -n "$CONDA_PREFIX" ]; then
        extra_paths+=("$CONDA_PREFIX/lib")
    fi

    case "$OS" in
        Darwin)
            # macOS uses DYLD_LIBRARY_PATH (and DYLD_FALLBACK_LIBRARY_PATH)
            extra_paths+=("/usr/local/lib" "/opt/homebrew/lib")
            local joined
            joined=$(IFS=:; echo "${extra_paths[*]}")
            export DYLD_LIBRARY_PATH="${joined}${DYLD_LIBRARY_PATH:+:$DYLD_LIBRARY_PATH}"
            export DYLD_FALLBACK_LIBRARY_PATH="${joined}${DYLD_FALLBACK_LIBRARY_PATH:+:$DYLD_FALLBACK_LIBRARY_PATH}"
            ;;
        Linux)
            # Linux uses LD_LIBRARY_PATH
            extra_paths+=("/usr/local/lib" "/usr/lib")
            # Add multiarch lib path
            local multiarch
            multiarch=$(gcc -dumpmachine 2>/dev/null || echo "")
            if [ -n "$multiarch" ] && [ -d "/usr/lib/$multiarch" ]; then
                extra_paths+=("/usr/lib/$multiarch")
            fi
            local joined
            joined=$(IFS=:; echo "${extra_paths[*]}")
            export LD_LIBRARY_PATH="${joined}${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
            ;;
    esac
}

# Run the build tool (cmake --build for both make and ninja)
run_build() {
    local cores
    cores="$(detect_cores)"
    cmake --build "${BUILD_DIR}" --parallel "$cores"
}

# ============================================================================
# Print platform summary
# ============================================================================
print_platform_info() {
    print_info "Platform: ${OS} ${ARCH}"
    print_info "CPU cores: $(detect_cores)"
    print_info "Generator: $(detect_generator)"
    [ -n "$CONDA_PREFIX" ] && print_info "Conda: $CONDA_PREFIX"
}

# ============================================================================
# Actions
# ============================================================================
ACTION=${1:-build}
BUILD_TYPE=${2:-Debug}

case $ACTION in
    clean)
        print_info "Cleaning build directory..."
        rm -rf "${BUILD_DIR}"
        print_info "Clean completed"
        ;;

    build)
        print_platform_info
        print_info "Building AgentFlow (${BUILD_TYPE})..."
        mkdir -p "${BUILD_DIR}"
        cd "${BUILD_DIR}"

        local_cmake_args=$(build_cmake_args "$BUILD_TYPE")
        # shellcheck disable=SC2086
        cmake .. $local_cmake_args

        run_build
        print_info "Build completed successfully"
        ;;

    rebuild)
        print_info "Rebuilding AgentFlow (clean + build)..."
        rm -rf "${BUILD_DIR}"
        exec "$0" build "$BUILD_TYPE"
        ;;

    test)
        print_info "Running tests..."
        if [ ! -d "${BUILD_DIR}" ]; then
            print_error "Build directory not found. Run './build.sh build' first"
            exit 1
        fi

        setup_runtime_library_path
        cd "${BUILD_DIR}"

        # Prefer ctest if available (runs all registered tests)
        if command -v ctest &>/dev/null; then
            print_info "Using ctest to run all registered tests..."
            ctest --output-on-failure --test-dir "${BUILD_DIR}" -j "$(detect_cores)" || {
                _exit_code=$?
                print_warning "Some tests failed (exit code: $_exit_code)"
                exit $_exit_code
            }
        else
            # Fallback: run test executables directly
            _found=0
            for test_bin in test_agentflow_gtest test_agentflow_simple test_agentflow; do
                if [ -f "${BUILD_DIR}/${test_bin}" ]; then
                    print_info "Running ${test_bin}..."
                    "${BUILD_DIR}/${test_bin}"
                    _found=1
                    break
                fi
            done
            if [ $_found -eq 0 ]; then
                print_error "No test executable found. Run './build.sh build' first"
                exit 1
            fi
        fi
        print_info "Tests completed"
        ;;

    example)
        EXAMPLE_NAME=${2:-databus}
        EXAMPLE_BIN="example_${EXAMPLE_NAME}"

        setup_runtime_library_path
        cd "${BUILD_DIR}"

        if [ ! -f "${EXAMPLE_BIN}" ]; then
            print_error "Example executable '${EXAMPLE_BIN}' not found."
            print_info "Available examples:"
            # shellcheck disable=SC2012
            ls -1 "${BUILD_DIR}"/example_* 2>/dev/null | sed 's|.*/||' || echo "  (none built yet)"
            exit 1
        fi

        print_info "Running ${EXAMPLE_BIN}..."
        "./${EXAMPLE_BIN}"
        print_info "Example completed"
        ;;

    *)
        echo "AgentFlow Build Script"
        echo ""
        echo "Usage: ./build.sh [action] [options]"
        echo ""
        echo "Actions:"
        echo "  clean              Remove build directory"
        echo "  build [type]       Build the project (type: Debug|Release|RelWithDebInfo, default: Debug)"
        echo "  rebuild [type]     Clean and rebuild"
        echo "  test               Run all tests via ctest"
        echo "  example [name]     Run an example (default: databus)"
        echo ""
        echo "Examples:"
        echo "  ./build.sh build Release"
        echo "  ./build.sh test"
        echo "  ./build.sh example databus"
        exit 1
        ;;
esac
