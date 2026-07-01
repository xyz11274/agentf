"""profile_manager tool — read/write user style profile via DataBus.

Stores user body type, preferences, and style history in DataBus
ctx namespace for cross-turn persistence.

Environment variables:
  AF_SESSION_ID       — used as profile key
  AF_DATABUS_UDS_FILE — UDS socket for DataBus access
"""
import json
import os
import sys

TOOL_NAME = "profile_manager"
TOOL_DESCRIPTION = "读取或更新用户穿搭画像。包括身材、偏好、历史选择等。操作类型：read（读取画像）/update（更新画像）。"
TOOL_PARAMS = {
    "type": "object",
    "properties": {
        "action": {
            "type": "string",
            "enum": ["read", "update"],
            "description": "read=读取当前画像, update=更新画像字段"
        },
        "fields": {
            "type": "object",
            "description": "要更新的字段（仅 update 操作时使用）。如 {\"preferred_style\": \"商务休闲\", \"body_type\": \"标准\"}",
            "properties": {}
        }
    },
    "required": ["action"]
}

PROFILE_KEY_TEMPLATE = "ctx/user_profile"


def _get_databus_client():
    """Try to connect to DataBus via UDS SDK."""
    uds_file = os.environ.get("AF_DATABUS_UDS_FILE", "")
    if not uds_file or not os.path.exists(uds_file):
        return None
    try:
        # Try to use the official DataBus client
        sys.path.insert(0, os.path.join(
            os.path.dirname(os.path.abspath(__file__)),
            "..", "..", "..", "..", "..", "src", "runtime", "databus", "clients", "python"
        ))
        from agentflow_databus import DataBusClient
        session_id = os.environ.get("AF_SESSION_ID", "default")
        client = DataBusClient()
        client._session_id = session_id
        return client
    except Exception:
        return None


def _read_profile(client):
    """Read user profile from DataBus."""
    if client:
        try:
            data = client.get(PROFILE_KEY_TEMPLATE, "{}")
            return json.loads(data) if data else {}
        except Exception:
            pass
    # Fallback: read from local file
    profile_path = os.path.join(
        os.path.dirname(os.path.abspath(__file__)),
        "..", "..", "..", "data", "user_profile.json"
    )
    if os.path.exists(profile_path):
        with open(profile_path, "r", encoding="utf-8") as f:
            return json.load(f)
    return {}


def _update_profile(client, fields):
    """Update user profile in DataBus."""
    # Read current profile first
    profile = _read_profile(client)

    # Merge new fields
    profile.update(fields)
    profile["update_count"] = profile.get("update_count", 0) + 1

    if client:
        try:
            client.put(PROFILE_KEY_TEMPLATE, json.dumps(profile, ensure_ascii=False))
            return {"status": "updated", "profile": profile}
        except Exception as e:
            return {"status": "error", "message": str(e)}

    # Fallback: write to local file
    profile_path = os.path.join(
        os.path.dirname(os.path.abspath(__file__)),
        "..", "..", "..", "data", "user_profile.json"
    )
    os.makedirs(os.path.dirname(profile_path), exist_ok=True)
    with open(profile_path, "w", encoding="utf-8") as f:
        json.dump(profile, f, ensure_ascii=False, indent=2)
    return {"status": "updated", "profile": profile, "storage": "local_file"}


def execute(params):
    action = params.get("action", "read")
    fields = params.get("fields", {})

    client = _get_databus_client()

    if action == "read":
        profile = _read_profile(client)
        return json.dumps({
            "profile": profile,
            "has_profile": bool(profile)
        }, ensure_ascii=False)
    elif action == "update":
        result = _update_profile(client, fields)
        return json.dumps(result, ensure_ascii=False)
    else:
        return json.dumps({
            "error": f"Unknown action: {action}. Use 'read' or 'update'."
        }, ensure_ascii=False)
