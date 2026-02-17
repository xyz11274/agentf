# CI/CD Quick Reference Card

## 🚀 Local Development

```bash
# Clone and setup
git clone https://github.com/DanielKernel/AgentFlow.git
cd AgentFlow

# Build
./build.sh build

# Test
./build.sh test

# Run example
./build.sh example
```

## 📝 Commit Guidelines

```bash
# Format: <type>(<scope>): <subject>
git commit -m "feat(actor): add message priority queue"

# Types: feat, fix, docs, style, refactor, perf, test, chore
# Scopes: framework, actor, dsl, event, network, storage, serialization, runtime, build, ci
```

## 🔍 Code Quality Before Push

```bash
# Format check and fix
clang-format --dry-run --Werror src/*.cpp          # Check
clang-format -i -style=file src/*.cpp               # Fix all

# Static analysis
clang-tidy -p build src/your_file.cpp

# Code analysis
cppcheck --enable=all src/

# Style check
cpplint src/your_file.cpp
```

## 🌿 Git Workflow

```bash
# Create feature branch
git checkout -b feature/my-feature

# Make changes and commit
git add src/
git commit -m "feat(actor): add feature"

# Push and create PR
git push origin feature/my-feature
# Then create PR on GitHub

# After approval, merge with squash (optional)
```

## 📊 Automated Checks

Every PR triggers:
- ✅ **Build** - All 4 platforms (Linux/macOS × AMD64/ARM64)
- ✅ **Tests** - Unit, integration, memory checks
- ✅ **Quality** - Format, analysis, style
- ✅ **Coverage** - Code coverage report

## 🏷️ Creating a Release

```bash
# Create and push tag
git tag v0.2.0 -m "Release 0.2.0"
git push origin v0.2.0

# GitHub Actions automatically:
# 1. Builds for all platforms
# 2. Runs all tests
# 3. Creates release page
# 4. Publishes artifacts
```

## 📋 PR Checklist

- [ ] Code formatted: `clang-format -i -style=file`
- [ ] Tests pass: `./build.sh test`
- [ ] No warnings
- [ ] Documentation updated
- [ ] Commits follow message format
- [ ] Tested on multiple platforms (if possible)

## 🔗 Useful Links

| Resource | URL |
|----------|-----|
| Actions Dashboard | https://github.com/DanielKernel/AgentFlow/actions |
| Code Coverage | https://codecov.io/gh/DanielKernel/AgentFlow |
| Issues | https://github.com/DanielKernel/AgentFlow/issues |
| Pull Requests | https://github.com/DanielKernel/AgentFlow/pulls |
| Releases | https://github.com/DanielKernel/AgentFlow/releases |

## ✅ Verify CI/CD Setup

```bash
# Verify all CI/CD components are installed
./scripts/verify_cicd_setup.sh

# Or from any directory
cd /path/to/AgentFlow
./scripts/verify_cicd_setup.sh
```

This script checks:
- GitHub Actions workflow files
- CI/CD configuration
- Issue templates
- Documentation completeness

**Expected output**: 26/26 checks passed ✓

## ❓ Troubleshooting

### Build Fails Locally
```bash
./build.sh clean
./build.sh build
# Check CMake output for missing dependencies
```

### CI/CD Setup Issues
```bash
# First verify all components are present
./scripts/verify_cicd_setup.sh

# If any checks fail, see docs/CI_CD_SETUP.md for detailed instructions
```

### Format Issues
```bash
# Auto-fix formatting
find src include tests -name '*.cpp' -o -name '*.h' -o -name '*.hpp' | \
  xargs clang-format -i -style=file
```

### Test Failures
```bash
cd build
ctest --output-on-failure -V
```

## 📚 Documentation

- **CI/CD Details**: `docs/CI_CD_PIPELINE.md`
- **Setup**: `docs/CI_CD_SETUP.md`
- **Contributing**: `docs/CONTRIBUTING.md`
- **Development**: `docs/development.md`
- **Testing**: `docs/testing.md`

## 🎯 Platform Matrix

| Platform | Build | Test | Status |
|----------|-------|------|--------|
| Linux AMD64 | ✅ | ✅ | CI/CD |
| Linux ARM64 | ✅ | ✅ | CI/CD |
| macOS AMD64 | ✅ | ✅ | CI/CD |
| macOS ARM64 | ✅ | ✅ | CI/CD |

---

**Quick Help**: `docs/CONTRIBUTING.md` | **Support**: @DanielKernel
