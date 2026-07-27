"""TDD Red: sandbox_http_server.py 会话级上传路径校验测试。

当前代码拒绝 `workspace/{user_id}_{client_id}/upload/` 格式，
只接受 `workspace/upload/`。测试验证当前行为（Red → 确认失败）。
"""
import os
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from sandbox_http_server import validate_upload_path


class TestUploadPathValidation(unittest.TestCase):
    """上传路径前缀校验测试。

    Red 阶段：先确认当前 validate_upload_path 的会话级检查行为。
    """

    def test_session_upload_path_accepted(self):
        """workspace/{user_id}_{client_id}/upload/ 应被接受。"""
        path = "workspace/user1_client-a/upload/image.png"
        result = validate_upload_path(path)
        self.assertIsNone(result, f"会话级路径应被接受，但返回: {result}")

    def test_legacy_upload_path_rejected(self):
        """旧格式 workspace/upload/（无会话目录）应被拒绝。"""
        path = "workspace/upload/image.png"
        result = validate_upload_path(path)
        self.assertIsNotNone(result, "无会话目录的旧路径应被拒绝")
        self.assertIn("PATH_NOT_IN_UPLOAD_DIR", result)

    def test_empty_session_rejected(self):
        """空会话目录 workspace//upload/ 应被拒绝。"""
        path = "workspace//upload/image.png"
        result = validate_upload_path(path)
        self.assertIsNotNone(result)

    def test_non_workspace_path_rejected(self):
        """非 workspace/ 前缀的路径应被拒绝。"""
        path = "tmp/upload/image.png"
        result = validate_upload_path(path)
        self.assertIsNotNone(result)

    def test_absolute_path_rejected(self):
        """宿主机绝对路径应被拒绝。"""
        path = "/tmp/agentflow/workspace/user1/upload/image.png"
        result = validate_upload_path(path)
        self.assertIsNotNone(result)

    def test_path_outside_upload_rejected(self):
        """workspace/{session}/ 下但不在 upload/ 内的路径应被拒绝。"""
        path = "workspace/user1_client-a/other/file.png"
        result = validate_upload_path(path)
        self.assertIsNotNone(result)


if __name__ == "__main__":
    unittest.main()
