# Node Development Specification

## Scope
This document defines conventions for custom node implementation used by workflow JSON under `publish/workflow`.

## 1. Universal entry contract
All node implementations should follow:
- Input: JSON payload from runtime/databus
- Output: JSON payload to stdout or output file
- Exit code: `0` success, non-zero failure

Recommended shell entry:

```bash
node_xxx.sh --input <input.json> --output <output.json> [--context <context.json>]
```

Recommended Python entry:

```bash
python node_xxx.py --input <input.json> --output <output.json>
```

## 2. Input/Output schema (minimum)
Input JSON:
- `trace_id` (string)
- `workflow_id` (string)
- `node_id` (string)
- `payload` (object)

Output JSON:
- `status` (`SUCCESS`/`FAILED`)
- `payload` (object)
- `error` (string, optional)

## 3. Workflow binding
Workflow node fields should include:
- `execution_mode`
- `command`
- `inputs`/`outputs`
- timeout/retry related controls

## 4. Error handling
- Return non-zero on hard failure.
- Emit structured `error` in output payload.
- Keep stderr readable for debugging.
