"""共享沙箱 HTTP 客户端，屏蔽 local/docker 差异。

所有沙箱 E2E 测试通过此客户端访问沙箱，确保测试代码在两种 mode 下完全一致。
"""

import requests


class SandboxClient:
    """统一沙箱 HTTP 客户端。

    封装 POST /exec 契约，提供类型安全的 exec 方法。
    local 和 docker 两个 fixture 都返回此客户端，测试代码无需区分 mode。
    """

    def __init__(self, endpoint: str):
        self.endpoint = endpoint.rstrip("/")

    def exec(self, tool: str, tool_params: dict,
             opts: dict | None = None) -> dict:
        """执行沙箱工具。

        Args:
            tool: 工具名（bash / read_file / write_file / edit_file / …）
            tool_params: 工具参数 dict
            opts: 执行选项（timeout_ms, working_dir, output_max_bytes, …）

        Returns:
            ExecResult JSON dict: {exit_code, stdout, stderr, truncated, duration_ms}
        """
        resp = requests.post(
            f"{self.endpoint}/exec",
            json={
                "tool": tool,
                "tool_params": tool_params,
                "opts": opts or {},
            },
            timeout=30,
        )
        resp.raise_for_status()
        return resp.json()

    def bash(self, command: str, opts: dict | None = None) -> dict:
        """快捷方式：执行 bash 命令。"""
        return self.exec("bash", {"command": command}, opts)

    def read_file(self, path: str, opts: dict | None = None) -> dict:
        """快捷方式：读文件。"""
        return self.exec("read_file", {"path": path}, opts)

    def write_file(self, path: str, content: str,
                   opts: dict | None = None) -> dict:
        """快捷方式：写文件。"""
        return self.exec("write_file", {"path": path, "content": content}, opts)

    def edit_file(self, path: str, old_str: str, new_str: str,
                  replace_all: bool = False,
                  opts: dict | None = None) -> dict:
        """快捷方式：编辑文件。"""
        return self.exec("edit_file", {
            "path": path,
            "old_str": old_str,
            "new_str": new_str,
            "replace_all": replace_all,
        }, opts)

    def load_skill(self, name: str, opts: dict | None = None) -> dict:
        """快捷方式：加载技能文档。"""
        return self.exec("load_skill", {"name": name}, opts)

    def list_skills(self, opts: dict | None = None) -> dict:
        """快捷方式：列出可用技能。"""
        return self.exec("list_skills", {}, opts)

    def health(self) -> bool:
        """健康检查：执行 echo ready 确认可达。"""
        try:
            result = self.bash("echo ready", {"timeout_ms": 3000})
            return result.get("exit_code") == 0
        except Exception:
            return False