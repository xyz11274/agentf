"""SysAgent AISF 适配层 — 已接入主 Bridge。

协议解析、HaContext 管理、回传报文构造等纯函数由 server.py 提供，
被 routes/aisf_tasks.py 复用。原独立 ASGI app (server.py:app) 保留作
历史/独立启动兼容，但生产链路走主 Bridge 的 /v1/tasks 路由。
"""
