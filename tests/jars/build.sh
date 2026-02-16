#!/bin/bash
# Build script for Hot Reload Test JAR
# This script compiles the Java source and creates a JAR file

set -e

# Get script directory
SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
cd "$SCRIPT_DIR"

echo "[BUILD] Building Hot Reload Test JAR..."

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# Check if javac is available
if ! command -v javac &> /dev/null; then
    echo -e "${RED}[ERROR] javac not found. Please install JDK.${NC}"
    echo -e "${YELLOW}[INFO] Install with: brew install openjdk (macOS)${NC}"
    exit 1
fi

echo -e "${GREEN}[INFO] Found javac: $(javac -version 2>&1 | head -1)${NC}"

# Compile Java source
echo "[BUILD] Compiling Java source..."
javac -d . HotReloadTest.java

if [ $? -ne 0 ]; then
    echo -e "${RED}[ERROR] Compilation failed${NC}"
    exit 1
fi

echo -e "${GREEN}[SUCCESS] Compilation successful${NC}"

# Create JAR
JAR_NAME="HotReloadTest.jar"
echo "[BUILD] Creating JAR: $JAR_NAME"

# Remove existing JAR if present
if [ -f "$JAR_NAME" ]; then
    rm "$JAR_NAME"
    echo "[BUILD] Removed existing JAR"
fi

# Create JAR with manifest
jar cvfm "$JAR_NAME" MANIFEST.MF HotReloadTest.class

if [ $? -ne 0 ]; then
    echo -e "${RED}[ERROR] JAR creation failed${NC}"
    exit 1
fi

echo -e "${GREEN}[SUCCESS] JAR created: $JAR_NAME${NC}"

# Print JAR info
echo ""
echo "[BUILD] JAR Information:"
jar tf "$JAR_NAME" | head -20

# Test JAR
echo ""
echo "[BUILD] Testing JAR..."
echo "[BUILD] Running: java -jar $JAR_NAME version"
java -jar "$JAR_NAME" version

if [ $? -ne 0 ]; then
    echo -e "${YELLOW}[WARN] JAR test failed (Java might not be available)${NC}"
else
    echo -e "${GREEN}[SUCCESS] JAR test passed${NC}"
fi

echo ""
echo -e "${GREEN}[BUILD] Build complete!${NC}"
echo ""
echo "[BUILD] Usage examples:"
echo "  java -jar $JAR_NAME execute"
echo "  java -jar $JAR_NAME add 5 3"
echo "  java -jar $JAR_NAME multiply 4 7"
echo "  java -jar $JAR_NAME echo hello world"
echo "  java -jar $JAR_NAME version"
echo "  java -jar $JAR_NAME info"
echo "  java -jar $JAR_NAME sleep 100"
echo "  java -jar $JAR_NAME file-read /path/to/file.txt"
echo "  java -jar $JAR_NAME file-write hello world"
