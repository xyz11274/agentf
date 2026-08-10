"""bridge/tests pytest 配置 — 注册 e2e 相关 marker。

运行方式（从项目根或本目录均可）:
    pytest bridge/tests/test_llm_call_chain_e2e.py -v -m e2e --timeout 300
"""


def pytest_configure(config):
    config.addinivalue_line("markers", "e2e: end-to-end tests requiring live daemon + bridge (deselect with -m 'not e2e')")
    config.addinivalue_line("markers", "slow: tests that take >10s (deselect with -m 'not slow')")
