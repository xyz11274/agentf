"""AgentFlow skill-scoped adapter for wisdomagent."""
from pathlib import Path
import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[3]))
from business_sim.contracts import contract
from business_sim.services import dispatch

TOOL_NAME, TOOL_DESCRIPTION, TOOL_PARAMS = contract('user-memory')


def execute(args, sandbox_endpoint=None):
    # Business simulation state resides on the host; no shell or remote device I/O.
    return dispatch('user-memory', args)
