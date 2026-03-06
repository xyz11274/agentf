#!/usr/bin/env bash
# Copyright (c) 2026 AgentFlow Authors
# Build script for SkillDelegateNode.java → skill_delegate.jar
# Usage: bash build.sh [--clean]
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUT_DIR="${SCRIPT_DIR}"
JAR_NAME="skill_delegate.jar"
CLASS_NAME="SkillDelegateNode"

if [[ "${1:-}" == "--clean" ]]; then
    rm -f "${OUT_DIR}/${CLASS_NAME}.class" "${OUT_DIR}/${JAR_NAME}"
    echo "Cleaned build artifacts."
    exit 0
fi

echo "Compiling ${CLASS_NAME}.java ..."
javac -encoding UTF-8 -d "${OUT_DIR}" "${SCRIPT_DIR}/${CLASS_NAME}.java"

echo "Packaging ${JAR_NAME} ..."
(cd "${OUT_DIR}" && jar cfe "${JAR_NAME}" "${CLASS_NAME}" "${CLASS_NAME}.class")

echo "Built: ${OUT_DIR}/${JAR_NAME}"
echo "Test run:"
java -jar "${OUT_DIR}/${JAR_NAME}" 2>&1 | head -3
