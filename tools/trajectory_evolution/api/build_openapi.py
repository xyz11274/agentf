"""Maintainer tool: derive OpenAPI from the canonical request schema (no runtime dependency)."""
import argparse
import copy
import json
from pathlib import Path


def object_schema(properties):
    return {"type": "object", "properties": properties, "required": list(properties), "additionalProperties": False}


def boundary_schema():
    identifier = {"type": "string", "minLength": 1, "maxLength": 128}
    extensions = {"type": "object", "additionalProperties": True}
    causes = {"type": "array", "items": identifier, "maxItems": 64, "uniqueItems": True}
    status = {"enum": ["completed", "failed", "cancelled", "unknown"]}
    decision = {"enum": ["continue", "replan", "stop"]}
    cases = []

    def add(operation, properties, required, run=True):
        base = {"operation": {"const": operation}, "request_id": identifier}
        if run:
            base["run_id"] = {"type": "string", "pattern": "^[0-9a-f]{40}$"}
        case = object_schema(dict(base, **properties))
        case["required"] = list(base) + required
        cases.append(case)

    add("contract.describe", {}, [], False)
    add("memory.status", {}, [], False)
    add("run.begin", {"input": {}, "conversation_id": identifier, "contract_version": {"const": 1},
        "level": {"enum": ["L0", "L1", "L2", "L3"]}, "capabilities": {"type": "array", "uniqueItems": True,
        "items": {"enum": ["model", "actions", "loop"]}}, "extensions": extensions}, ["input"], False)
    metadata = {"actor_id": identifier, "scope_id": identifier, "caused_by": causes, "extensions": extensions}
    binding = object_schema({"pointer": {"type": "string", "pattern": "^(|/.*)$"}, "mode": {"enum": ["text", "system_message"]}})
    metrics = {"type": "object", "properties": {key: {"type": "integer", "minimum": 0, "maximum": 1000000000}
        for key in ["input_tokens", "output_tokens", "tokens", "cached_input_tokens"]}, "additionalProperties": False}
    add("activity.begin", dict(metadata, kind=identifier, name=identifier, input={}, stream_id=identifier,
        control_id=identifier, state={}, binding=binding, apply_policy={"type": "boolean", "default": True}), ["kind", "name", "input"])
    for operation in ["activity.end", "activity.abort"]:
        error = object_schema({"code": identifier, "retryability": {"enum": ["unknown", "retryable", "non_retryable"]}, "message": {"type": "string"}})
        error["required"] = ["code", "retryability"]
        outcome = {"enum": ["failed", "cancelled", "unknown"]} if operation.endswith("abort") else status
        add(operation, {"activity_id": identifier, "status": outcome, "output": {}, "actual_input": {},
            "executed": {"type": "boolean"}, "error": error, "extensions": extensions, "caused_by": causes, "metrics": metrics},
            ["activity_id", "status", "output"])
    add("activity.status", {"activity_id": identifier}, ["activity_id"])
    add("loop.check", dict(metadata, loop_id=identifier, state={}, apply_policy={"type": "boolean", "default": True}), ["loop_id", "state"])
    add("loop.commit", {"control_id": identifier, "decision": decision, "extensions": extensions, "caused_by": causes}, ["control_id", "decision"])
    add("loop.abort", {"control_id": identifier, "decision": decision, "extensions": extensions, "caused_by": causes}, ["control_id", "decision"])
    add("event.record", {"type": identifier, "data": {}, "activity_id": identifier, "caused_by": causes,
        "extensions": extensions}, ["type", "data"])
    for operation in ["run.end", "run.abort"]:
        outcome = {"enum": ["failed", "cancelled", "unknown"]} if operation.endswith("abort") else status
        add(operation, {"status": outcome, "business_status": {"enum": ["succeeded", "failed", "partial", "unknown"]},
            "output": {}, "capture_complete": {"type": "boolean"}, "defer_if_pending": {"type": "boolean"}, "extensions": extensions, "caused_by": causes}, ["status", "output"])
    add("point", {"phase": {"enum": ["before", "after", "abort", "event", "heartbeat", "guard", "gap"]}, "kind": identifier,
        "capture_gap": {"type": "boolean"},
        "ref": {"type": "object", "additionalProperties": False, "required": ["run"], "properties": {
            "run": {"type": "string", "minLength": 1, "maxLength": 256},
            "key": {"type": "string", "minLength": 1, "maxLength": 256},
            "parent": {"type": "string", "minLength": 1, "maxLength": 256},
            "after": {"type": "array", "maxItems": 64, "items": {"type": "string", "minLength": 1, "maxLength": 256}}}},
        "context": extensions, "causes": {"type": "array", "items": extensions, "maxItems": 64},
        "name": identifier, "input": {}, "output": {}, "status": status,
        "business_status": {"enum": ["succeeded", "failed", "partial", "unknown"]}, "executed": {"type": "boolean"},
        "actual_input": {}, "decision": decision, "loop_id": identifier, "actor_id": identifier, "scope_id": identifier,
        "state": {}, "binding": binding, "metrics": metrics, "error": error, "extensions": extensions,
        "conversation_id": identifier, "level": {"enum": ["L0", "L1", "L2", "L3"]},
        "capabilities": {"type": "array", "items": {"enum": ["model", "actions", "loop"]}},
        "apply_policy": {"type": "boolean"}}, ["phase"], run=False)
    add("run.status", {}, [])
    add("run.heartbeat", {}, [])
    for operation in ("agent_evaluation.status", "agent_evaluation.feedback"):
        add(operation, {}, [], run=False)
    add("agent_evaluation.submit", {"artifact": extensions}, ["artifact"], run=False)
    for operation in ("agent_evaluation.retry", "agent_evaluation.cancel", "agent_evaluation.tree"):
        add(operation, {"job_id": {"type": "string", "pattern": "^[0-9a-f]{64}$"}}, ["job_id"], run=False)
    return {"$schema": "https://json-schema.org/draft/2020-12/schema", "$id": "urn:trajectory-evolution:boundary:v1",
        "title": "Framework-neutral boundary protocol v1",
        "description": "Lifecycle-dependent requirements (model binding, actual_input, executed, capabilities) are checked by the service. Any JSON input/output; custom data belongs in extensions. capture.gap and evaluation.retry are shared with integration v1.",
        "oneOf": cases}


def build(schema):
    string, boolean = {"type": "string"}, {"type": "boolean"}
    natural = {"type": "integer", "minimum": 0}
    level = {"type": "string", "enum": ["L0", "L1", "L2", "L3"]}
    free_object = {"type": "object", "additionalProperties": True}
    capabilities = {"type": "array", "items": {"type": "string", "enum": ["model", "actions", "loop"]}}
    schemas = {}
    mapping = {}
    for case in schema["oneOf"]:
        request = copy.deepcopy(case)
        operation = case["properties"]["operation"]["const"]
        name = "".join(part.title() for part in operation.split(".")) + "Request"
        request["properties"]["operation"].update({"type": "string", "enum": [operation]})
        schemas[name] = request
        mapping[operation] = "#/components/schemas/" + name
    schemas["InvokeRequest"] = {"oneOf": [{"$ref": ref} for ref in mapping.values()],
        "discriminator": {"propertyName": "operation", "mapping": mapping}}
    execution_context = object_schema({"run_id": string, "execution_id": string, "actor_id": string})
    action_plan = object_schema({"id": string, "call_id": string, "event_id": string, "reason": string,
        "decision": {"type": "string", "enum": ["allow", "reject"]}})
    action_plan["properties"]["context"] = execution_context  # Only delegated actions return this field.
    results = {
        "TaskBeginResult": {"run_id": string, "level": level, "capabilities": capabilities},
        "TaskStatusResult": {"run_id": string, "closed": boolean, "level": level, "capture_complete": boolean,
            "event_count": natural, "evaluation_status": string, "evaluation_error": string,
            "observed_boundaries": {"type": "object", "additionalProperties": boolean},
            "receipts": {"type": "object", "additionalProperties": natural}},
        "EvaluationRetryResult": {"evaluation_status": {"type": "string", "enum": ["pending"]}},
        "MemoryStatusResult": {"application_id": string, "user_id": string, "private_revision": natural,
            "private_entries": {"type": "object", "additionalProperties": natural},
            "public_entries": {"type": "object", "additionalProperties": natural}, "public_revision": natural,
            "cross_relations": {"type": "object", "additionalProperties": natural},
            "sharing_enabled": boolean, "sharing_status": string, "public_available": boolean,
            "retired_public_entries": natural},
        "CaptureGapResult": {"capture_complete": {"type": "boolean", "const": False}},
        "StepBeginResult": {"step_id": string, "event_id": string, "reason": string, "iteration": natural,
            "decision": {"type": "string", "enum": ["continue", "replan", "stop"]}},
        "TaskEndResult": {"closed": boolean, "capture_complete": boolean, "evaluation_status": string},
        "StateRecordResult": {"event_id": string},
        "CallEndResult": {"event_id": string, "ended": boolean},
        "ModelPrepareResult": {"call_id": string, "event_id": string, "request": free_object},
        "ActionsPrepareResult": {"actions": {"type": "array", "items": action_plan}},
        "ExecutionStatusResult": {"context": execution_context, "ended": boolean, "event_id": string,
            "status": {"type": "string", "enum": ["queued", "running", "completed", "failed", "cancelled", "unknown"]},
            "output": {}},
        "RunBeginResult": {"run_id": string, "level": level, "capabilities": capabilities, "protocol": {"const": "evolution.boundary.v1"}},
        "HeartbeatResult": {"alive": {"const": True}},
        "ActivityBeginResult": {"activity_id": string, "event_id": string, "decision": {"enum": ["allow", "reject"]},
            "reason": string, "semantic": {"enum": ["model", "action", "custom"]}, "input": {},
            "context": object_schema({"run_id": string, "activity_id": string})},
        "ActivityStatusResult": {"activity_id": string, "event_id": string, "ended": boolean, "status": string, "output": {}},
        "LoopCheckResult": {"control_id": string, "decision": {"enum": ["continue", "replan", "stop"]},
            "reason": string, "context": string, "iteration": natural, "event_id": string},
        "ContractDescribeResult": {"protocol": {"const": "evolution.boundary.v1"}, "contract_version": {"const": 1},
            "max_level": level, "operations": {"type": "array", "items": string}, "required_fields": free_object,
            "optional_fields": {"type": "array", "items": string}, "extension_field": {"const": "extensions"},
            "policy_scopes": {"type": "array", "items": string}, "activity_types": free_object, "limits": free_object},
    }
    schemas.update({name: object_schema(fields) for name, fields in results.items()})
    schemas["TaskEndResult"]["properties"].update(metrics=free_object, pending=boolean)
    for name in results:
        schemas[name]["properties"]["persisted"] = boolean
    schemas["PointResult"] = {"type": "object", "required": ["context"], "properties": {
        "context": {"type": "object", "required": ["kind", "run_id"], "properties": {"kind": string, "run_id": string}}},
        "additionalProperties": True}
    for name in ("RunBeginResult", "TaskBeginResult"):
        schemas[name]["properties"].update(policy_gate=string, agent_version_id=string)
    schemas["AgentEvaluationStatusResult"] = object_schema({"jobs": free_object, "publications": free_object})
    schemas["AgentEvaluationStatusResult"]["properties"]["worker_error"] = string
    schemas["AgentEvaluationFeedbackResult"] = {"type": "object", "propertyNames": {"pattern": "^[0-9a-f]{64}$"}, "additionalProperties": free_object}
    schemas["AgentEvaluationSubmitResult"] = object_schema({"job_id": string})
    schemas["AgentEvaluationUpdateResult"] = object_schema({"accepted": boolean})
    schemas["InvokeResponse"] = object_schema({"ok": {"type": "boolean", "const": True},
        "result": {"oneOf": [{"$ref": "#/components/schemas/" + name} for name in list(results) +
            ["PointResult", "AgentEvaluationStatusResult", "AgentEvaluationFeedbackResult", "AgentEvaluationSubmitResult", "AgentEvaluationUpdateResult"]]}})
    schemas["ErrorResponse"] = object_schema({"ok": {"type": "boolean", "const": False},
        "error": {"type": "string", "pattern": "^[a-z_]{1,96}$"}})
    schemas["HealthResponse"] = object_schema({"ok": {"type": "boolean", "const": True},
        "protocol": {"type": "string", "const": "trajectory-integration.v1"}, "max_level": level})
    def response(name, description):
        return {"description": description, "content": {"application/json": {"schema": {"$ref": "#/components/schemas/" + name}}}}
    return {
        "openapi": "3.1.0", "info": {"title": "Trajectory Evolution Integration", "version": "1.0.0",
            "description": "Framework- and language-independent L0-L3 boundaries. The service collects traces and evaluates/curates memory; hosts apply prepared requests, per-action plans and loop decisions. No L4 support."},
        "servers": [{"url": "http://127.0.0.1:8766", "description": "Local sidecar; remote hosts must use an HTTPS gateway."}],
        "security": [{"bearerAuth": []}],
        "paths": {
            "/v1/health": {"get": {"operationId": "health", "security": [], "tags": ["Integration"],
                "responses": {"200": response("HealthResponse", "Protocol availability, not an Agent capability certification.")}}},
            "/v1/invoke": {"post": {"operationId": "invoke", "tags": ["Integration"],
                "description": "Use run.begin/activity.*/loop.*/event.record for the framework-neutral profile; task.begin/step.* remain a compatibility profile. Use a stable request_id per logical RPC. Reuse that ID and identical contents after a network failure; never retry the host's side effect. Except task.begin, run.begin, contract.describe, memory.status and agent_evaluation.*, send run_id. memory.status and agent_evaluation.* are scoped management operations, outside the native invoke hot path. IDs stay with callbacks/queue messages, not credentials. HTTP/1.1 Content-Length is required; chunked bodies and redirects are not supported.",
                "requestBody": {"required": True, "content": {"application/json": {"schema": {"$ref": "#/components/schemas/InvokeRequest"}}}},
                "responses": {"200": response("InvokeResponse", "Durably accepted RPC; result shape depends on operation."),
                    "default": response("ErrorResponse", "Fixed error code. Never include a credential in diagnostics. See README.md#operations for lifecycle and privacy handling.")}}},
        },
        "components": {"securitySchemes": {"bearerAuth": {"type": "http", "scheme": "bearer", "bearerFormat": "opaque service token"}}, "schemas": schemas},
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true", help="Fail if the generated contract is stale.")
    args = parser.parse_args()
    directory = Path(__file__).resolve().parent
    schema = json.loads((directory / "integration-v1.schema.json").read_text(encoding="utf-8"))
    boundary = boundary_schema()
    schema["oneOf"].extend(boundary["oneOf"])
    schema["$id"] = "urn:trajectory-evolution:invoke:v1"
    schema["title"] = "Trajectory Evolution v1: neutral and compatibility requests"
    for name, document in {"boundary-v1.schema.json": boundary, "invoke-v1.schema.json": schema, "openapi.json": build(schema)}.items():
        expected = json.dumps(document, indent=2, ensure_ascii=False) + "\n"
        target = directory / name
        if args.check:
            if not target.exists() or target.read_text(encoding="utf-8") != expected:
                raise SystemExit(name + " is stale; run api/build_openapi.py")
        else:
            target.write_text(expected, encoding="utf-8")


if __name__ == "__main__":
    main()
