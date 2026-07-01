"""AgentFlow Bridge — HTTP route modules.

Each route module owns a FastAPI ``APIRouter`` and is mounted by
``main.py``. Concrete routers (packages, agents, sessions, chat, daemon
health) are populated by Wave 2 tasks; this package exists today so that
imports stay stable from the start of the project.
"""

from __future__ import annotations