"""AgentFlow Bridge — FastAPI bridge service.

Exposes a REST + WebSocket API to the AgentFlow Vue frontend and proxies /
forwards calls to the local ``agentflowd`` daemon over WebSocket JSON-RPC.

Layout:
    main.py      FastAPI application entrypoint (CORS + /health).
    config.py    Pydantic-settings configuration (env vars).
    client.py    DaemonClient (WebSocket JSON-RPC) — added by T4.
    routes/      HTTP route modules — populated by Wave 2 tasks.
    scripts/     Operational / verification scripts.
"""

__version__ = "0.1.0"