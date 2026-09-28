# BFCL function calling

Use this skill for BFCL v3 multi-turn function-calling tasks. The runtime
injects the exact functions available in the current benchmark turn into this
skill's tool list. Call the listed function by its original name and pass
schema-valid arguments. Use the returned value when a later call depends on
it. Emit only the minimum calls required by the current request: do not probe
with pwd/cd/ls, repeat a previous-turn call, or invent a state transition.
When a request names a directory or file, pass that path directly to the
requested operation. Never invent a function, argument, result, credential,
or fixture value. If a tool reports a failure, stop retrying the same call and
explain the failure briefly.

The helper `bfcl_call` is only a compatibility fallback. Use it only when it
is explicitly present in the current tool list; otherwise call the generated
function tool directly.

The benchmark host owns the fixture and strict checker.  It exposes only the
current turn's schemas to this skill; the fixture file and expected answer are
never part of the skill prompt.
