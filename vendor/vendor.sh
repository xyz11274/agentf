#!/bin/bash
# Vendor dependencies download script

set -e

# Color codes
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# Script directory
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
VENDOR_DIR="$SCRIPT_DIR"
CONFIG_FILE="$SCRIPT_DIR/vendor.conf"

# Create vendor directory
mkdir -p "$VENDOR_DIR"

# Function to print colored output
print_info() {
    echo -e "${GREEN}[INFO]${NC} $1"
}

print_warn() {
    echo -e "${YELLOW}[WARN]${NC} $1"
}

print_error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

# Function to download a single dependency
download_dependency() {
    local name=$1
    local url=$2
    local branch=$3
    local target_dir=$4

    print_info "Downloading $name..."
    print_info "  URL: $url"
    print_info "  Branch: $branch"

    local target_path="$VENDOR_DIR/$target_dir"

    if [ -d "$target_path" ]; then
        print_warn "Directory $target_path already exists. Skipping download."
        print_warn "Use '$0 --clean' to remove existing vendors."
        return
    fi

    # Clone with depth 1 for shallow clone
    if git clone --depth 1 --branch "$branch" "$url" "$target_path"; then
        print_info "Successfully downloaded $name to $target_path"
    else
        print_error "Failed to download $name"
        # Clean up failed download
        rm -rf "$target_path"
        return 1
    fi
}

# Function to clean vendor directory
clean_vendor() {
    print_warn "Cleaning vendor directory: $VENDOR_DIR"
    read -p "Are you sure? (y/N): " -n 1 -r
    echo
    if [[ $REPLY =~ ^[Yy]$ ]]; then
        rm -rf "$VENDOR_DIR"
        mkdir -p "$VENDOR_DIR"
        print_info "Vendor directory cleaned"
    else
        print_info "Cleanup cancelled"
    fi
}

# Function to list configured dependencies
list_dependencies() {
    print_info "Configured vendor dependencies:"
    echo ""

    local index=0
    while IFS='|' read -r name url branch target_dir; do
        # Skip comments and empty lines
        [[ "$name" =~ ^#.*$ ]] && continue
        [[ -z "$name" ]] && continue

        index=$((index + 1))
        echo -e "${GREEN}$index. $name${NC}"
        echo "   URL: $url"
        echo "   Branch: $branch"
        echo "   Target: vendor/$target_dir"

        # Check if already downloaded
        if [ -d "$VENDOR_DIR/$target_dir" ]; then
            echo -e "   Status: ${GREEN}Installed${NC}"
        else
            echo -e "   Status: ${YELLOW}Not installed${NC}"
        fi
        echo ""
    done < "$CONFIG_FILE"

    if [ "$index" -eq 0 ]; then
        print_warn "No dependencies configured in $CONFIG_FILE"
    fi
}

# Function to update a specific dependency
update_dependency() {
    local name=$1
    local url=$2
    local branch=$3
    local target_dir=$4

    local target_path="$VENDOR_DIR/$target_dir"

    if [ ! -d "$target_path" ]; then
        print_error "Dependency $name is not installed. Run '$0' first."
        return 1
    fi

    print_info "Updating $name..."
    cd "$target_path"

    if git fetch origin "$branch" && git reset --hard "origin/$branch"; then
        print_info "Successfully updated $name"
    else
        print_error "Failed to update $name"
        return 1
    fi
}

# Parse command line arguments
if [ "$1" = "--clean" ]; then
    clean_vendor
    exit 0
elif [ "$1" = "--list" ]; then
    list_dependencies
    exit 0
elif [ "$1" = "--update" ]; then
    print_info "Updating all vendor dependencies..."
    while IFS='|' read -r name url branch target_dir; do
        [[ "$name" =~ ^#.*$ ]] && continue
        [[ -z "$name" ]] && continue
        update_dependency "$name" "$url" "$branch" "$target_dir"
    done < "$CONFIG_FILE"
    exit 0
elif [ "$1" = "--help" ] || [ "$1" = "-h" ]; then
    echo "Vendor Dependencies Manager"
    echo ""
    echo "Usage: $0 [options]"
    echo ""
    echo "Options:"
    echo "  (no args)     Download all configured dependencies"
    echo "  --clean       Remove vendor directory"
    echo "  --list        List configured dependencies"
    echo "  --update      Update all downloaded dependencies"
    echo "  --help, -h    Show this help message"
    echo ""
    echo "Configuration file: $CONFIG_FILE"
    echo "Vendor directory: $VENDOR_DIR"
    exit 0
fi

# Main download logic
print_info "Vendor directory: $VENDOR_DIR"
print_info "Configuration file: $CONFIG_FILE"
echo ""

local count=0
local failed=0

while IFS='|' read -r name url branch target_dir; do
    # Skip comments and empty lines
    [[ "$name" =~ ^#.*$ ]] && continue
    [[ -z "$name" ]] && continue

    count=$((count + 1))

    if download_dependency "$name" "$url" "$branch" "$target_dir"; then
        # Success
        :
    else
        failed=$((failed + 1))
    fi
    echo ""
done < "$CONFIG_FILE"

if [ "$count" -eq 0 ]; then
    print_warn "No dependencies configured in $CONFIG_FILE"
    echo "Edit $CONFIG_FILE to add dependencies"
elif [ "$failed" -eq 0 ]; then
    print_info "All dependencies downloaded successfully ($count total)"
else
    print_error "Failed to download $failed out of $count dependencies"
    exit 1
fi
