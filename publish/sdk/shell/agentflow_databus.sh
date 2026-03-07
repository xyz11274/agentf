#!/bin/sh

_af_db_root() {
  if [ -z "$AF_DATABUS_DIR" ]; then
    echo "AF_DATABUS_DIR is not set" >&2
    return 1
  fi
  printf '%s' "$AF_DATABUS_DIR"
}

_af_db_b64enc() {
  printf '%s' "$1" | base64 | tr -d '\n'
}

_af_db_b64dec() {
  if printf '' | base64 -D >/dev/null 2>&1; then
    printf '%s' "$1" | base64 -D
  else
    printf '%s' "$1" | base64 --decode
  fi
}

_af_db_key_path() {
  python3 - "$(_af_db_root)/kv" "$1" <<'PY'
import os, sys, urllib.parse
print(os.path.join(sys.argv[1], urllib.parse.quote(sys.argv[2], safe='._-') + '.kv'))
PY
}

_af_db_event_seen() {
  subscriber="$1"
  file="$2"
  state_dir="$(_af_db_root)/subscribers"
  mkdir -p "$state_dir"
  seen_file="$state_dir/${subscriber}.seen"
  touch "$seen_file"
  if grep -qxF "$file" "$seen_file" 2>/dev/null; then
    return 0
  fi
  echo "$file" >> "$seen_file"
  return 1
}

_af_db_now_ms() {
  python3 - <<'PY'
import time
print(int(time.time() * 1000))
PY
}

af_databus_set() {
  key="$1"
  value="$2"
  type="${3:-string}"
  path="$(_af_db_key_path "$key")" || return 1
  mkdir -p "$(dirname "$path")" "$(_af_db_root)/events"
  encoded="$(_af_db_b64enc "$value")"
  cat > "$path" <<DATA
key=$key
type=$type
value_b64=$encoded
timestamp_ms=$(_af_db_now_ms)
DATA
  af_databus_publish "$key" "$value" "$type" set
}

af_databus_get() {
  key="$1"
  path="$(_af_db_key_path "$key")" || return 1
  [ -f "$path" ] || return 1
  encoded=$(sed -n 's/^value_b64=//p' "$path" | head -n1)
  _af_db_b64dec "$encoded"
}

af_databus_publish() {
  topic="$1"
  value="$2"
  type="${3:-string}"
  kind="${4:-pub}"
  root="$(_af_db_root)" || return 1
  mkdir -p "$root/events"
  nonce=$(python3 - <<'PY2'
import os, random, time
print(f"{time.time_ns()}_{os.getpid()}_{random.randint(0, 999999)}")
PY2
)
  file="$root/events/${nonce}.evt"
  encoded="$(_af_db_b64enc "$value")"
  cat > "$file" <<DATA
topic=$topic
kind=$kind
type=$type
value_b64=$encoded
timestamp_ms=$(_af_db_now_ms)
DATA
}

af_databus_next_event() {
  subscriber="$1"
  topic="$2"
  timeout_ms="${3:-5000}"
  deadline=$(( $(_af_db_now_ms) + timeout_ms ))
  root="$(_af_db_root)" || return 1
  while [ "$(_af_db_now_ms)" -le "$deadline" ]; do
    for f in "$root"/events/*.evt; do
      [ -e "$f" ] || break
      base=$(basename "$f")
      if _af_db_event_seen "$subscriber" "$base"; then
        continue
      fi
      evt_topic=$(sed -n 's/^topic=//p' "$f" | head -n1)
      [ "$evt_topic" = "$topic" ] || continue
      encoded=$(sed -n 's/^value_b64=//p' "$f" | head -n1)
      _af_db_b64dec "$encoded"
      return 0
    done
    sleep 0.05
  done
  return 1
}

af_databus_request_llm() {
  request_id="$1"
  prompt="$2"
  channel="${3:-agentflow.llm.chunk.$request_id}"
  payload=$(python3 - "$request_id" "$prompt" "$channel" <<'PY'
import json, sys
print(json.dumps({"request_id": sys.argv[1], "prompt": sys.argv[2], "channel": sys.argv[3]}))
PY
)
  af_databus_publish "agentflow.llm.request" "$payload" string pub
}
