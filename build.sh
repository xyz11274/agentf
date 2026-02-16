#!/bin/bash

# AgentFlow Build Script
# Usage: ./build.sh [clean|build|test|example]

set -e

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${PROJECT_DIR}/build"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

print_info() {
    echo -e "${GREEN}[INFO]${NC} $1"
}

print_error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

print_warning() {
    echo -e "${YELLOW}[WARNING]${NC} $1"
}

# Determine action
ACTION=${1:-build}

case $ACTION in
    clean)
        print_info "Cleaning build directory..."
        rm -rf "${BUILD_DIR}"
        print_info "Clean completed"
        ;;
    
    build)
        print_info "Building AgentFlow..."
        mkdir -p "${BUILD_DIR}"
        cd "${BUILD_DIR}"
        cmake .. -DCMAKE_BUILD_TYPE=Debug
        make -j$(nproc)
        print_info "Build completed successfully"
        ;;
    
    test)
        print_info "Running tests..."
        cd "${BUILD_DIR}"
        if [ ! -f test_agentflow ]; then
            print_error "Test executable not found. Run './build.sh build' first"
            exit 1
        fi
        ./test_agentflow
        print_info "Tests completed"
        ;;
    
    example)
        print_info "Running basic example..."
        cd "${BUILD_DIR}"
        if [ ! -f example_basic ]; then
            print_error "Example executable not found. Run './build.sh build' first"
            exit 1
        fi
        ./example_basic
        print_info "Example completed"
        ;;
    
    *)
        print_error "Unknown action: $ACTION"
        echo "Usage: ./build.sh [clean|build|test|example]"
        exit 1
        ;;
esac
