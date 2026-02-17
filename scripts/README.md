# 项目脚本指南

此目录包含项目相关的所有辅助脚本。

## 📋 脚本列表

### 系统脚本

#### `verify_cicd_setup.sh`
验证 CI/CD 流水线完整性的脚本。

**用途**：
- 检查所有 GitHub Actions 工作流文件
- 验证 CI/CD 配置文件
- 检查问题模板
- 验证文档文件

**使用方式**：
```bash
# 从项目根目录执行
./scripts/verify_cicd_setup.sh

# 或使用根目录的包装脚本
./verify_cicd_setup.sh
```

**输出示例**：
```
🔍 Verifying CI/CD Implementation...

📁 Directory Structure
✓ .github/
✓ .github/workflows/
...

📊 Summary
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
Total checks:  28
Passed:        28
Failed:        0
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

✓ All CI/CD components verified successfully!
```

**成功条件**：
- 所有检查项通过（Failed = 0）
- 返回退出码 0

**失败排查**：
如果有检查项失败，请：
1. 查看失败的具体文件/目录
2. 参考 [CI/CD 实现指南](../docs/CI_CD_IMPLEMENTATION.md)
3. 按照指南检查相应的配置

#### `install_dependencies.sh`
安装项目依赖的脚本。

**用途**：
- 安装系统级依赖
- 配置开发环境
- 验证依赖满足要求

**使用方式**：
```bash
./scripts/install_dependencies.sh
```

**支持平台**：
- Ubuntu/Debian (apt)
- macOS (brew)
- 其他 Linux 发行版可能需要手动安装

## 📁 Docker 脚本

`docker/` 目录包含 Docker 相关的配置文件：
- `Dockerfile` - 主要镜像定义
- `Dockerfile.agent` - 代理容器镜像
- `docker-compose.yml` - 多容器编排
- `prometheus.yml` - Prometheus 监控配置

## 📦 Helm 脚本

`helm/` 目录包含 Kubernetes Helm 配置：
- AgentFlow Helm chart 定义
- 用于 Kubernetes 部署

## 🚀 常用命令

```bash
# 验证 CI/CD 设置
./verify_cicd_setup.sh

# 安装依赖
./scripts/install_dependencies.sh

# 构建 Docker 镜像
docker-compose -f scripts/docker/docker-compose.yml build

# 启动 Docker 容器
docker-compose -f scripts/docker/docker-compose.yml up
```

## 📖 更多信息

- 详细的构建说明：查看 [build.sh](../build.sh)
- CI/CD 配置：参考 [docs/CI_CD_SETUP.md](../docs/CI_CD_SETUP.md)
- 依赖管理：参考 [docs/dependencies.md](../docs/dependencies.md)
- 开发文档：参考 [docs/development.md](../docs/development.md)

## 🔧 维护

添加新脚本时：
1. 将脚本放在此目录
2. 确保脚本有明确的用途和说明
3. 在本 README 中记录脚本
4. 确保脚本具有执行权限
5. 在脚本顶部添加说明注释

## ⚙️ 脚本约定

- 所有 shell 脚本使用 `#!/bin/bash` 开头
- 建议使用输出颜色提高可读性
- 脚本应该能从项目根目录或脚本目录执行
- 脚本处理相对路径时，应该正确计算项目根目录
- 脚本失败时应该返回非零退出码

---

**最后更新**：2026 年 2 月 17 日
