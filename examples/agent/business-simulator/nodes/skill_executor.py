"""Use the standard executor with this simulation's business tool scope."""
from pathlib import Path
import sys
from agentflow_runtime import skill_executor as runtime

package = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(package / "harness"))
from business_sim.tool_scope import BusinessToolRegistry

runtime.AGENT_PACKAGE_DIR = str(package)
runtime.AGENT_MD_PATH = str(package / "agent.md")
runtime.NODES_DIR = str(package / "nodes")
runtime._LocalToolRegistry = BusinessToolRegistry

if __name__ == "__main__":
    runtime.execute()
