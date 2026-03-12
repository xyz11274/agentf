#!/usr/bin/env python3
"""
Simple calculator agent example
This script adds two numbers passed as command-line arguments.
"""

import sys


def main():
    """Add two numbers from command-line arguments."""
    if len(sys.argv) < 3:
        print("Usage: calc.py <num1> <num2>")
        print("Example: calc.py 5 3")
        sys.exit(1)

    try:
        num1 = float(sys.argv[1])
        num2 = float(sys.argv[2])
        result = num1 + num2
        print(f"{num1} + {num2} = {result}")
        return 0
    except ValueError as e:
        print(f"Error: Invalid number format - {e}", file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()
