#!/bin/bash
set -euo pipefail
"$(dirname "$0")/read_prompt_template.sh"
"$(dirname "$0")/read_history.sh"
"$(dirname "$0")/summarize_history.sh"
"$(dirname "$0")/fill_context.sh"
