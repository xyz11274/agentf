# GitHub Actions CI/CD Infrastructure Guide

## ✅ Complete Setup Checklist

This guide walks you through all the CI/CD components that have been set up for AgentFlow.

### Workflow Files Created

- ✅ `.github/workflows/build.yml` - Multi-platform build pipeline
- ✅ `.github/workflows/code-quality.yml` - Code quality and style checks
- ✅ `.github/workflows/tests.yml` - Comprehensive testing framework
- ✅ `.github/workflows/release.yml` - Automated release publishing
- ✅ `.github/workflows/manual_build.yml` - Manual build trigger option
- ✅ `.github/workflows/badge_status.yml` - Status badge generation

### Configuration Files Created

- ✅ `.github/CODEOWNERS` - Code ownership and review assignment
- ✅ `.github/dependabot.yml` - Automated dependency updates
- ✅ `.github/pull_request_template.md` - PR template with quality checklist
- ✅ `.github/ISSUE_TEMPLATE/bug_report.md` - Bug report template
- ✅ `.github/ISSUE_TEMPLATE/feature_request.md` - Feature request template
- ✅ `.github/ISSUE_TEMPLATE/performance.md` - Performance improvement template
- ✅ `.github/ISSUE_TEMPLATE/config.md` - Configuration issue template

### Documentation Files Created

- ✅ `docs/CI_CD_PIPELINE.md` - Complete CI/CD documentation
- ✅ `docs/CONTRIBUTING.md` - Contribution guidelines

## 🔧 Quick Reference

### View Build Status

Visit the Actions page: https://github.com/DanielKernel/AgentFlow/actions

### Supported Platforms

| Platform | Architecture | Status |
|----------|--------------|--------|
| Ubuntu | x86_64 (AMD64) | ✅ |
| Ubuntu | ARM64 | ✅ |
| macOS 12 | x86_64 (AMD64) | ✅ |
| macOS 14 | ARM64 (Apple Silicon) | ✅ |

### Automated Checks

Every push and pull request triggers:

1. **Build Check** - All platforms compile successfully
2. **Test Execution** - Unit tests, integration tests, memory leak detection
3. **Code Quality** - Format check, static analysis, style validation
4. **Coverage Analysis** - Code coverage reporting to Codecov

### Run Manual Builds

1. Go to Actions tab → Manual Build & Test
2. Click "Run workflow"
3. Select options:
   - Build Type (Debug/Release)
   - Enable Code Coverage
   - Run Valgrind Memory Checks
   - Slack Notifications

## 📊 Status Badges

Add these badges to your README.md for visual status:

```markdown
## CI/CD Status

[![Build](https://github.com/DanielKernel/AgentFlow/workflows/Build/badge.svg?branch=main)](https://github.com/DanielKernel/AgentFlow/actions/workflows/build.yml)
[![Tests](https://github.com/DanielKernel/AgentFlow/workflows/Tests/badge.svg?branch=main)](https://github.com/DanielKernel/AgentFlow/actions/workflows/tests.yml)
[![Code Quality](https://github.com/DanielKernel/AgentFlow/workflows/Code%20Quality/badge.svg?branch=main)](https://github.com/DanielKernel/AgentFlow/actions/workflows/code-quality.yml)
[![codecov](https://codecov.io/gh/DanielKernel/AgentFlow/branch/main/graph/badge.svg)](https://codecov.io/gh/DanielKernel/AgentFlow)
```

## 🔐 Branch Protection Rules

Recommended settings for main/develop branches:

1. **Require status checks to pass before merging**:
   - Build must pass on all platforms
   - Tests must pass on all platforms
   - Code quality checks must pass

2. **Require pull request reviews**:
   - At least 1 approval required
   - Code owners must approve (based on CODEOWNERS)

3. **Require branches to be up to date before merging**:
   - Automatic requirement of latest main branch

4. **Require conversation resolution**:
   - Comments must be resolved before merge

To set these up:
1. Go to Repository Settings
2. Branches → Branch protection rules
3. Add rule for `main` and `develop`
4. Configure as described above

## 🚀 Creating a Release

```bash
# Create and push a version tag
git tag v0.2.0 -m "Release version 0.2.0"
git push origin v0.2.0

# GitHub Actions will automatically:
# - Build for all platforms
# - Run all tests
# - Package artifacts
# - Create GitHub Release
# - Generate checksums
# - Publish release notes
```

Release artifacts will be available at:
https://github.com/DanielKernel/AgentFlow/releases

## 📝 Next Steps

### 1. Configure Codecov (Optional but Recommended)

The coverage reports are automatically uploaded to Codecov. You can:

1. Visit https://codecov.io
2. Sign in with GitHub
3. Enable AgentFlow repository
4. Codecov will start tracking coverage automatically

### 2. Set Up Branch Protection Rules

```bash
# Using GitHub CLI
gh repo edit --enable-discussions --enable-wiki false
gh rule create -b main --require-status-checks
```

### 3. Add Badges to README

Include the status badges in your main README.md file (between title and content):

```markdown
# AgentFlow

[![Build](https://github.com/DanielKernel/AgentFlow/workflows/Build/badge.svg?branch=main)](...)
[![Tests](https://github.com/DanielKernel/AgentFlow/workflows/Tests/badge.svg?branch=main)](...)
...

基于 C++ Actor 反应式编程框架...
```

### 4. Update CHANGELOG

Maintain a CHANGELOG.md file with release notes:

```markdown
# Changelog

## [0.2.0] - 2026-02-17

### Added
- New feature description
- Another new feature

### Fixed
- Bug fix description

### Changed
- Breaking change description

## [0.1.0] - 2026-01-01

### Initial Release
- Core framework implementation
- Basic workflow support
```

### 5. Configure Dependabot Alerts

The dependabot.yml is already configured to:
- Check GitHub Actions for updates weekly
- Check Dockerfile base images weekly
- Create PRs automatically
- Request review from @DanielKernel

To view and manage alerts:
1. Go to Repository Settings → Security & analysis
2. Enable/configure Dependabot security alerts
3. Review and merge dependency update PRs

## 🐛 Troubleshooting

### Build Fails on Specific Platform

1. Check the Actions tab for detailed logs
2. The workflow matrix shows which platform failed
3. Possible causes:
   - Missing dependencies (check pre-installed packages)
   - Platform-specific code issues
   - CMake configuration problems

### Code Quality Check Fails

Run locally to reproduce:

```bash
# Format check
clang-format --dry-run --Werror src/file.cpp

# Static analysis
clang-tidy -p build src/file.cpp

# Code style
cpplint src/file.cpp

# Fix automatically
clang-format -i -style=file src/file.cpp
```

### Tests Fail

1. Run locally: `./build.sh test`
2. Check for race conditions (especially on ARM64)
3. Ensure test data files exist
4. Check for platform-specific issues

## 📚 Documentation

- See `docs/CONTRIBUTING.md` for contribution guidelines
- See `docs/CI_CD_PIPELINE.md` for complete pipeline documentation
- See `docs/development.md` for local development setup
- See `docs/testing.md` for testing guidelines

## 🎯 Architecture Overview

```
GitHub Actions Workflows
├── Build Pipeline (build.yml)
│   ├── Linux AMD64
│   ├── Linux ARM64
│   ├── macOS AMD64
│   └── macOS ARM64
│
├── Testing Pipeline (tests.yml)
│   ├── Unit Tests → GTest
│   ├── Code Coverage → LCOV/Gcovr
│   ├── Integration Tests → Examples
│   └── Memory Leaks → Valgrind
│
├── Code Quality (code-quality.yml)
│   ├── Formatting → clang-format
│   ├── Static Analysis → clang-tidy
│   ├── Code Analysis → cppcheck
│   └── Style Check → cpplint
│
└── Release Pipeline (release.yml)
    ├── Create GitHub Release
    ├── Build Release Binaries
    ├── Package Artifacts
    └── Publish to GitHub
```

## 🔄 Continuous Monitoring

Each commit triggers automated checks. Key metrics to monitor:

1. **Build Status**: Should be passing on all platforms
2. **Test Coverage**: Aim for > 80%
3. **Code Quality**: No critical warnings
4. **Performance**: Track memory usage and speed

## 👥 Team Collaboration

With CODEOWNERS configured:
- Code owners are automatically assigned to PRs
- Different teams can own different components
- Review requirements are enforced
- Expertise-based code reviews

## 🎓 Learning Resources

- GitHub Actions Documentation: https://docs.github.com/actions
- CMake Documentation: https://cmake.org/cmake/help/
- Modern C++ Best Practices: https://isocpp.org/wiki/faq

## 📞 Support

For questions about CI/CD:
1. Check this guide
2. Review workflow files
3. Open an issue with [CI/CD] label
4. Contact @DanielKernel

---

## Summary

Your AgentFlow project now has a complete, professional-grade CI/CD infrastructure supporting:

✅ Multi-platform builds (Linux/macOS, AMD64/ARM64)
✅ Comprehensive testing with coverage tracking
✅ Code quality checks and analysis
✅ Automated releases and artifact publishing
✅ Dependency management and security updates
✅ Clear contribution guidelines
✅ Automatic code review assignment

This ensures code quality, reduces manual testing burden, and enables confident releases!
