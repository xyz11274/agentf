"""pytest 骨架——read_file.py 沙箱代理改造后的测试。"""


def test_path_escape_rejected():
    """TC-SBOX-023: 越界路径 ../../../etc/passwd 被沙箱拒绝含路径名。

    业务规则:
    - B4: 路径双检拒绝越界访问工作区之外

    错误模型:
    - 路径折叠——string prefix 通过但 realpath 逃逸

    失败意味着:
    - 越界读取宿主机敏感文件，密钥泄漏
    """
    pytest.skip("实现期驱动——read_file.py 改造后填充")


def test_contract_file_content_shape():
    """TC-SBOX-061: 契约测试——read_file.py 期望沙箱内 server 返回文件内容。

    Consumer: read_file.py
    Provider: sandbox_http_server.py
    接口: POST /exec（tool=read_file）

    契约内容:
    - 请求: tool=read_file + path
    - 响应: stdout=文件内容

    失败意味着:
    - server 返回格式变更导致 read_file.py 无法提取文件内容
    """
    pytest.skip("实现期驱动——契约测试骨架")
