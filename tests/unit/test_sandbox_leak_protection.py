#!/usr/bin/env python3
"""单元测试 — 沙箱泄漏防护（FR-023/FR-024/FR-025）

覆盖：
- sandbox_http_server.py 的 --parent-pid 参数解析
- _parent_monitor() 函数检测父进程死亡逻辑
- DAEMON_PID 环境变量回退
"""
import os
import subprocess
import sys
import time
import unittest
from pathlib import Path

# sandbox_http_server.py 路径
_SERVER_PATH = Path(__file__).resolve().parents[2] / "docker" / "sandbox" / "http_server" / "sandbox_http_server.py"


class TestParentMonitor(unittest.TestCase):
    """测试 _parent_monitor 函数（通过子进程验证退出行为）。"""

    def setUp(self):
        self.assertTrue(_SERVER_PATH.exists(), f"server not found: {_SERVER_PATH}")

    def _run_monitor_in_subprocess(self, monitor_script, timeout=5):
        """辅助：在子进程中执行 monitor_script，返回 Popen 对象。"""
        return subprocess.Popen(
            [sys.executable, "-c", monitor_script],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )

    def test_parent_monitor_exits_on_orphan(self):
        """_parent_monitor 在父进程死亡后检测到 ppid 不匹配并退出。

        模拟孤儿场景：启动子进程，父进程退出，子进程的 _parent_monitor
        检测到 getppid() != parent_pid 后调用 os._exit(1)。
        """
        monitor_script = """
import sys, os
sys.path.insert(0, '%s')
import importlib.util
spec = importlib.util.spec_from_file_location("srv", "%s")
mod = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mod)
mod._parent_monitor(parent_pid=999999, check_interval=0.1)
""" % (str(_SERVER_PATH.parent), str(_SERVER_PATH))

        proc = subprocess.Popen(
            [sys.executable, "-c", monitor_script],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        try:
            proc.wait(timeout=5)
            self.assertEqual(proc.returncode, 1,
                             "_parent_monitor 应 exit(1)（ppid 不匹配）")
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()
            self.fail("_parent_monitor 未在 5s 内退出（可能未检测到 ppid 不匹配）")

    def test_parent_monitor_stays_on_match(self):
        """_parent_monitor 在 getppid() == parent_pid 时不会退出。"""
        parent_pid = os.getpid()
        monitor_script = """
import sys, os, time, threading
sys.path.insert(0, '%s')
import importlib.util
spec = importlib.util.spec_from_file_location("srv", "%s")
mod = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mod)
result = {"exited": False}
def check():
    time.sleep(1)
    os._exit(0 if not result["exited"] else 2)
threading.Thread(target=check, daemon=True).start()
mod._parent_monitor(parent_pid=%d, check_interval=0.1)
""" % (str(_SERVER_PATH.parent), str(_SERVER_PATH), parent_pid)

        proc = subprocess.Popen(
            [sys.executable, "-c", monitor_script],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        try:
            proc.wait(timeout=5)
            self.assertEqual(proc.returncode, 0,
                             "_parent_monitor 不应在 ppid 匹配时退出")
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()
            self.fail("_parent_monitor 未在 5s 内退出")

    def test_parent_monitor_returns_immediately_on_zero(self):
        """_parent_monitor(parent_pid=0) 立即返回（未启用检测）。

        覆盖缺陷 #6 修复后的边界保护：parent_pid == 0 时线程立即返回，
        不进入轮询循环，避免无意义的 CPU 占用。
        """
        monitor_script = """
import sys, os
sys.path.insert(0, '%s')
import importlib.util
spec = importlib.util.spec_from_file_location("srv", "%s")
mod = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mod)
mod._parent_monitor(parent_pid=0, check_interval=0.1)
os._exit(0)  # 如果 _parent_monitor 正确返回，执行到这里
""" % (str(_SERVER_PATH.parent), str(_SERVER_PATH))

        proc = subprocess.Popen(
            [sys.executable, "-c", monitor_script],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        try:
            proc.wait(timeout=3)
            self.assertEqual(proc.returncode, 0,
                             "parent_pid=0 时 _parent_monitor 应立即返回")
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()
            self.fail("parent_pid=0 时 _parent_monitor 未立即返回（可能进入了轮询）")

    def test_parent_monitor_exits_immediately_on_init_ppid(self):
        """_parent_monitor(parent_pid=1) 立即退出（fork 后被 init 收养）。

        覆盖缺陷 #6 修复后的边界保护：parent_pid == 1 说明 daemon 在 fork
        后立即死亡，子进程被 init 收养，此时应立即退出避免孤儿残留。
        """
        monitor_script = """
import sys, os
sys.path.insert(0, '%s')
import importlib.util
spec = importlib.util.spec_from_file_location("srv", "%s")
mod = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mod)
mod._parent_monitor(parent_pid=1, check_interval=0.1)
os._exit(2)  # 不应执行到这里
""" % (str(_SERVER_PATH.parent), str(_SERVER_PATH))

        proc = subprocess.Popen(
            [sys.executable, "-c", monitor_script],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        try:
            proc.wait(timeout=3)
            self.assertEqual(proc.returncode, 1,
                             "parent_pid=1 时 _parent_monitor 应立即 exit(1)")
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()
            self.fail("parent_pid=1 时 _parent_monitor 未立即退出（可能进入了轮询）")


class TestServerStartup(unittest.TestCase):
    """测试 server 启动时 --parent-pid 参数处理。"""

    def setUp(self):
        self.assertTrue(_SERVER_PATH.exists(), f"server not found: {_SERVER_PATH}")

    def _start_server_check_alive(self, args, extra_env=None):
        """启动 server，验证启动后进程存活（不崩溃），然后 kill。"""
        cmd = [sys.executable, "-u", str(_SERVER_PATH)] + args
        env = os.environ.copy()
        if extra_env:
            env.update(extra_env)
        proc = subprocess.Popen(
            cmd,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            env=env,
        )
        try:
            # 等 1s 让 server 完成初始化（端口绑定、监控线程启动）
            time.sleep(1)
            # 检查进程是否存活（未崩溃）
            poll = proc.poll()
            self.assertIsNone(poll,
                              f"server 应保持运行，但已退出: returncode={poll}")
        finally:
            if proc.poll() is None:
                proc.kill()
                proc.wait()

    def test_parent_pid_argument_accepted(self):
        """--parent-pid 参数被接受，server 不崩溃。"""
        self._start_server_check_alive(
            ["--port", "0", "--parent-pid", str(os.getpid())]
        )

    def test_daemon_pid_env_fallback(self):
        """DAEMON_PID 环境变量作为 --parent-pid 的回退，server 不崩溃。"""
        self._start_server_check_alive(
            ["--port", "0"],
            extra_env={"DAEMON_PID": str(os.getpid())},
        )

    def test_no_parent_pid_no_env(self):
        """无 --parent-pid 且无 DAEMON_PID 时 server 正常启动。"""
        self._start_server_check_alive(["--port", "0"])


if __name__ == "__main__":
    unittest.main()