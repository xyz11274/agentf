#!/usr/bin/env python3
"""
DataBus CLI - Command-line tool for DataBus access via Unix Domain Socket.

Usage:
    # Set value
    databus-cli -s {session-id} -ns user put "my_key" "my_value"

    # Get value
    databus-cli -s {session-id} -ns user get "my_key"

    # With dialogue_id
    databus-cli -s {session-id} -d {dialogue-id} -ns user put "result" '{"status":"ok"}'

    # Delete key
    databus-cli -s {session-id} -ns user remove "my_key"

    # Check existence
    databus-cli -s {session-id} -ns user contains "my_key"

    # List keys
    databus-cli -s {session-id} -ns user list

    # Publish event
    databus-cli -s {session-id} -ns sys publish "topic.event" "payload"

    # Subscribe (blocking)
    databus-cli -s {session-id} -ns user subscribe "my_key"
"""

import argparse
import json
import os
import sys
import time
from pathlib import Path

# Add parent directory to path for importing the SDK
sys.path.insert(0, str(Path(__file__).parent.parent / "python"))

try:
    from agentflow_databus import DataBusClient, DataBusError, JSONRPCError
except ImportError:
    print("Error: agentflow_databus module not found", file=sys.stderr)
    print("Make sure src/runtime/databus/clients/python/agentflow_databus.py is available", file=sys.stderr)
    sys.exit(1)


def get_client_from_env_or_args(args):
    """Get a DataBus client from environment or arguments."""
    socket_path = args.socket or os.environ.get("AF_DATABUS_UDS_FILE")

    if not socket_path:
        print("Error: No socket path specified. Use -S/--socket or set AF_DATABUS_UDS_FILE", file=sys.stderr)
        sys.exit(1)

    return DataBusClient(socket_path, args.session)


def cmd_put(args):
    """Put a value into the DataBus."""
    client = get_client_from_env_or_args(args)
    ns_client = client.user(args.dialogue) if args.namespace == "user" else \
                client.sys(args.dialogue) if args.namespace == "sys" else \
                client.ctx(args.dialogue) if args.namespace == "ctx" else \
                client.llm(args.dialogue) if args.namespace == "llm" else \
                client.tool(args.dialogue)

    # Parse value
    try:
        value = json.loads(args.value)
    except json.JSONDecodeError:
        value = args.value

    try:
        success = ns_client.put(args.key, value)
        if success:
            print(f"OK: Set {args.key}")
        else:
            print(f"Error: Failed to set {args.key}", file=sys.stderr)
            sys.exit(1)
    except DataBusError as e:
        print(f"Error: {e}", file=sys.stderr)
        sys.exit(1)
    finally:
        client.close()


def cmd_get(args):
    """Get a value from the DataBus."""
    client = get_client_from_env_or_args(args)
    ns_client = client.user(args.dialogue) if args.namespace == "user" else \
                client.sys(args.dialogue) if args.namespace == "sys" else \
                client.ctx(args.dialogue) if args.namespace == "ctx" else \
                client.llm(args.dialogue) if args.namespace == "llm" else \
                client.tool(args.dialogue)

    try:
        value = ns_client.get(args.key)
        if value is not None:
            if args.raw:
                print(value)
            else:
                print(json.dumps(value, indent=2, ensure_ascii=False))
        else:
            if not args.quiet:
                print(f"Key not found: {args.key}", file=sys.stderr)
            sys.exit(1)
    except DataBusError as e:
        print(f"Error: {e}", file=sys.stderr)
        sys.exit(1)
    finally:
        client.close()


def cmd_remove(args):
    """Remove a key from the DataBus."""
    client = get_client_from_env_or_args(args)
    ns_client = client.user(args.dialogue) if args.namespace == "user" else \
                client.sys(args.dialogue) if args.namespace == "sys" else \
                client.ctx(args.dialogue) if args.namespace == "ctx" else \
                client.llm(args.dialogue) if args.namespace == "llm" else \
                client.tool(args.dialogue)

    try:
        success = ns_client.remove(args.key)
        if success:
            print(f"OK: Removed {args.key}")
        else:
            print(f"Error: Failed to remove {args.key}", file=sys.stderr)
            sys.exit(1)
    except DataBusError as e:
        print(f"Error: {e}", file=sys.stderr)
        sys.exit(1)
    finally:
        client.close()


def cmd_contains(args):
    """Check if a key exists in the DataBus."""
    client = get_client_from_env_or_args(args)
    ns_client = client.user(args.dialogue) if args.namespace == "user" else \
                client.sys(args.dialogue) if args.namespace == "sys" else \
                client.ctx(args.dialogue) if args.namespace == "ctx" else \
                client.llm(args.dialogue) if args.namespace == "llm" else \
                client.tool(args.dialogue)

    try:
        exists = ns_client.contains(args.key)
        if args.raw:
            print("true" if exists else "false")
        else:
            print(f"{'exists' if exists else 'not found'}: {args.key}")
        sys.exit(0 if exists else 1)
    except DataBusError as e:
        print(f"Error: {e}", file=sys.stderr)
        sys.exit(1)
    finally:
        client.close()


def cmd_list(args):
    """List keys in the DataBus."""
    client = get_client_from_env_or_args(args)

    # Note: list_keys is not fully implemented in the UDS server yet
    print("Warning: list_keys is not fully implemented", file=sys.stderr)
    print("Namespace:", args.namespace)
    if args.dialogue:
        print("Dialogue:", args.dialogue)
    client.close()


def cmd_publish(args):
    """Publish an event to the DataBus."""
    client = get_client_from_env_or_args(args)
    ns_client = client.user(args.dialogue) if args.namespace == "user" else \
                client.sys(args.dialogue) if args.namespace == "sys" else \
                client.ctx(args.dialogue) if args.namespace == "ctx" else \
                client.llm(args.dialogue) if args.namespace == "llm" else \
                client.tool(args.dialogue)

    # Parse value
    try:
        value = json.loads(args.value)
    except json.JSONDecodeError:
        value = args.value

    try:
        success = ns_client.publish(args.topic, value)
        if success:
            print(f"OK: Published to {args.topic}")
        else:
            print(f"Error: Failed to publish to {args.topic}", file=sys.stderr)
            sys.exit(1)
    except DataBusError as e:
        print(f"Error: {e}", file=sys.stderr)
        sys.exit(1)
    finally:
        client.close()


def cmd_subscribe(args):
    """Subscribe to key changes."""
    client = get_client_from_env_or_args(args)
    ns_client = client.user(args.dialogue) if args.namespace == "user" else \
                client.sys(args.dialogue) if args.namespace == "sys" else \
                client.ctx(args.dialogue) if args.namespace == "ctx" else \
                client.llm(args.dialogue) if args.namespace == "llm" else \
                client.tool(args.dialogue)

    def on_change(key, value):
        timestamp = time.strftime("%Y-%m-%d %H:%M:%S")
        print(f"[{timestamp}] {key}: {json.dumps(value, ensure_ascii=False)}")

    try:
        sub_id = ns_client.subscribe(args.key, on_change)
        print(f"Subscribed to {args.key} (subscription_id: {sub_id})")
        print("Press Ctrl+C to stop...")

        # Keep running until interrupted
        while True:
            time.sleep(1)

    except KeyboardInterrupt:
        print("\nUnsubscribing...")
        ns_client.unsubscribe(sub_id)
    except DataBusError as e:
        print(f"Error: {e}", file=sys.stderr)
        sys.exit(1)
    finally:
        client.close()


def main():
    parser = argparse.ArgumentParser(
        description="DataBus CLI - Command-line tool for DataBus access",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__
    )

    # Global options
    parser.add_argument("-S", "--socket", help="Unix Domain Socket path (or set AF_DATABUS_UDS_FILE)")
    parser.add_argument("-s", "--session", help="Session ID")
    parser.add_argument("-d", "--dialogue", help="Dialogue ID for dialogue-scoped access")
    parser.add_argument("-ns", "--namespace", default="user",
                        choices=["sys", "ctx", "llm", "tool", "user"],
                        help="Namespace (default: user)")
    parser.add_argument("-q", "--quiet", action="store_true", help="Suppress non-essential output")
    parser.add_argument("--raw", action="store_true", help="Output raw values without formatting")

    subparsers = parser.add_subparsers(dest="command", help="Command to execute")

    # put command
    put_parser = subparsers.add_parser("put", help="Set a value")
    put_parser.add_argument("key", help="Key name")
    put_parser.add_argument("value", help="Value (JSON or string)")
    put_parser.set_defaults(func=cmd_put)

    # get command
    get_parser = subparsers.add_parser("get", help="Get a value")
    get_parser.add_argument("key", help="Key name")
    get_parser.set_defaults(func=cmd_get)

    # remove command
    remove_parser = subparsers.add_parser("remove", help="Remove a key")
    remove_parser.add_argument("key", help="Key name")
    remove_parser.set_defaults(func=cmd_remove)

    # contains command
    contains_parser = subparsers.add_parser("contains", help="Check if a key exists")
    contains_parser.add_argument("key", help="Key name")
    contains_parser.set_defaults(func=cmd_contains)

    # list command
    list_parser = subparsers.add_parser("list", help="List keys in namespace")
    list_parser.set_defaults(func=cmd_list)

    # publish command
    publish_parser = subparsers.add_parser("publish", help="Publish an event")
    publish_parser.add_argument("topic", help="Topic name")
    publish_parser.add_argument("value", help="Event payload (JSON or string)")
    publish_parser.set_defaults(func=cmd_publish)

    # subscribe command
    subscribe_parser = subparsers.add_parser("subscribe", help="Subscribe to key changes")
    subscribe_parser.add_argument("key", help="Key name")
    subscribe_parser.set_defaults(func=cmd_subscribe)

    args = parser.parse_args()

    if not args.command:
        parser.print_help()
        sys.exit(1)

    args.func(args)


if __name__ == "__main__":
    main()