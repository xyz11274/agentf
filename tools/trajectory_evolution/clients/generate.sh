#!/usr/bin/env bash
# Optional developer tool; never needed by a deployed Agent or the C++ service.
set -euo pipefail
if [[ $# -ne 2 || ! "$1" =~ ^[a-z][a-z0-9-]*$ ]]; then
  echo 'Usage: bash clients/generate.sh <generator-language> <new-output-directory>' >&2
  exit 2
fi
if [[ -e "$2" ]]; then
  echo 'Output must be a new directory; existing files are not overwritten.' >&2
  exit 2
fi
client_source_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
generator_command="${OPENAPI_GENERATOR_CLI:-openapi-generator-cli}"
if ! command -v "$generator_command" >/dev/null 2>&1; then
  echo 'Install OpenAPI Generator CLI, or set OPENAPI_GENERATOR_CLI to its executable path.' >&2
  exit 2
fi
exec "$generator_command" generate -i "$client_source_dir/../api/openapi.json" -g "$1" -o "$2"
