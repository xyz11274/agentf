# AgentFlow GitHub Configuration

This directory contains GitHub-specific configuration files for the AgentFlow project, including CI/CD pipelines, issue templates, and contribution guidelines.

## 📁 Directory Structure

```
.github/
├── workflows/              # GitHub Actions workflows
│   ├── build.yml          # Multi-platform build pipeline
│   ├── code-quality.yml   # Code quality and style checks
│   ├── tests.yml          # Comprehensive test suite
│   ├── release.yml        # Release automation
│   ├── manual_build.yml   # Manual build dispatch
│   └── badge_status.yml   # Status badge refresh
│
├── ISSUE_TEMPLATE/        # Issue report templates
│   ├── bug_report.md      # Bug report template
│   ├── feature_request.md # Feature request template
│   ├── performance.md     # Performance improvement template
│   └── config.md          # Configuration issue template
│
├── CODEOWNERS             # Code ownership for auto-review
├── dependabot.yml         # Dependency update automation
├── pull_request_template.md  # PR template with checklist
│
├── CI_CD.md              # Complete CI/CD documentation
├── SETUP.md              # Setup and quick reference guide
└── README.md (this file) # This directory's overview
```

## 🚀 Quick Start

### For Contributors

1. **Read the guidelines**: [CONTRIBUTING.md](../CONTRIBUTING.md)
2. **Understand the CI/CD pipeline**: [.github/CI_CD.md](./CI_CD.md)
3. **Follow the development workflow**: [docs/development.md](../docs/development.md)

### For Maintainers

1. **Review CI/CD setup**: [.github/SETUP.md](./SETUP.md)
2. **Configure branch protection**: See GitHub Settings
3. **Monitor workflows**: https://github.com/DanielKernel/AgentFlow/actions

## 📋 Workflows Overview

### Build (`workflows/build.yml`)

**Triggers**: Push/PR to main/develop, or manual dispatch

**Builds**:
- ✅ Linux AMD64 (Ubuntu latest)
- ✅ Linux ARM64 (Ubuntu latest with QEMU)
- ✅ macOS AMD64 (macOS 12)
- ✅ macOS ARM64 (macOS 14)

**Output**: Build artifacts, platform-specific binaries

### Tests (`workflows/tests.yml`)

**Triggers**: Push/PR to main/develop

**Test Jobs**:
- ✅ Unit Tests (GTest)
- ✅ Code Coverage (LCOV/Gcovr → Codecov)
- ✅ Integration Tests (Examples)
- ✅ Memory Leak Detection (Valgrind)

**Coverage**: Target > 80%

### Code Quality (`workflows/code-quality.yml`)

**Triggers**: Push/PR to main/develop

**Checks**:
- ✅ Code Formatting (clang-format)
- ✅ Static Analysis (clang-tidy)
- ✅ Code Analysis (cppcheck)
- ✅ Style Check (cpplint)
- ✅ Sanitizer Compilation

### Release (`workflows/release.yml`)

**Triggers**: Push of git tag `v*`

**Steps**:
1. Creates GitHub Release
2. Builds on all platforms
3. Runs all tests
4. Packages artifacts
5. Generates checksums
6. Publishes releases

### Manual Build (`workflows/manual_build.yml`)

**Triggers**: Manual dispatch from Actions tab

**Options**:
- Build Type: Debug / Release / RelWithDebInfo
- Code Coverage: Enable/Disable
- Valgrind Checks: Enable/Disable
- Slack Notifications: Enable/Disable

## 📧 Templates

### Issue Templates

Located in `ISSUE_TEMPLATE/`:

- **bug_report.md**: For reporting bugs
  - Environment details
  - Reproduction steps
  - Expected vs actual behavior

- **feature_request.md**: For feature requests
  - Use cases
  - Proposed solution
  - Alternative approaches

- **performance.md**: For performance improvements
  - Profiling data
  - Optimization proposal
  - Impact assessment

- **config.md**: For configuration issues
  - Affected component
  - Environment details
  - Configuration used

### Pull Request Template

File: `pull_request_template.md`

Includes checklist for:
- ✅ Testing (unit, integration, manual)
- ✅ Code Quality (style, warnings, memory)
- ✅ Platform Support (Linux/macOS, AMD64/ARM64)
- ✅ Documentation (comments, README)
- ✅ Breaking Changes

## 🔐 Configuration Files

### CODEOWNERS

Specifies code review assignments:
- Default: @DanielKernel
- Component-specific owners by directory
- Auto-requested for relevant PRs

### dependabot.yml

Automated dependency updates:
- GitHub Actions: Weekly Monday
- Docker images: Weekly Wednesday

## 📊 Status & Monitoring

### View Build Status

**GitHub Actions Dashboard**:
https://github.com/DanielKernel/AgentFlow/actions

**Status Badges** (use in README):
```markdown
[![Build](https://github.com/DanielKernel/AgentFlow/workflows/Build/badge.svg?branch=main)](...)
[![Tests](https://github.com/DanielKernel/AgentFlow/workflows/Tests/badge.svg?branch=main)](...)
[![Code Quality](https://github.com/DanielKernel/AgentFlow/workflows/Code%20Quality/badge.svg?branch=main)](...)
```

### Coverage Tracking

Code coverage is automatically uploaded to Codecov:
https://codecov.io/gh/DanielKernel/AgentFlow

## 🔧 Configuration Requirements

### Repository Settings

Recommended configuration for `main` and `develop` branches:

1. **Require status checks**:
   - Build must pass
   - Tests must pass
   - Code Quality must pass

2. **Require code reviews**:
   - At least 1 approval
   - Code owners must approve
   - Dismiss stale on push

3. **Require up-to-date branches**:
   - Before merging

4. **Auto-delete head branches**:
   - Clean up after merge

### Secrets & Tokens

Required for full functionality:

- `GITHUB_TOKEN`: Auto-provided (for releases and artifacts)
- `CODECOV_TOKEN`: Optional (for coverage uploads)
- `SLACK_WEBHOOK`: Optional (for notifications)

## 📚 Documentation Files

### CONTRIBUTING.md (root level)

Comprehensive contribution guide including:
- Setup instructions
- Development workflow
- Code quality standards
- Commit message format
- PR process
- Issue reporting

### CI_CD.md (this directory)

Complete CI/CD pipeline documentation:
- Workflow descriptions
- Platform support matrix
- Build configuration
- Quality gates
- Troubleshooting

### SETUP.md (this directory)

Quick reference and setup checklist:
- Badges configuration
- Branch protection rules
- Release process
- Codecov setup

## 🎯 Key Features

### Multi-Platform Support
- Linux x86_64 and ARM64
- macOS x86_64 and ARM64 (Apple Silicon)
- Automatic platform detection
- Cross-platform dependency handling

### Comprehensive Testing
- Unit tests (GTest)
- Integration tests (Examples)
- Code coverage tracking
- Memory leak detection (Valgrind)
- Thread sanitizer support

### Code Quality
- Format validation (clang-format)
- Static analysis (clang-tidy)
- Code analysis (cppcheck)
- Style checking (cpplint)
- Sanitizer compilation checks

### Automation
- Automatic dependency updates (Dependabot)
- Scheduled status checks
- Manual build dispatch
- Release automation
- Artifact management

## 🔄 Workflow Examples

### Creating a Feature Pull Request

```bash
# Create feature branch
git checkout -b feature/new-feature

# Make changes, commit with proper messages
git commit -m "feat(actor): add message priority"

# Push and create PR
git push origin feature/new-feature

# GitHub Actions automatically:
# 1. Builds on all platforms
# 2. Runs tests
# 3. Checks code quality
# 4. Reports coverage changes
```

### Creating a Release

```bash
# Tag the release
git tag v0.2.0 -m "Release version 0.2.0"
git push origin v0.2.0

# GitHub Actions automatically:
# 1. Creates release page
# 2. Builds binaries for all platforms
# 3. Runs full test suite
# 4. Packages and signs artifacts
# 5. Publishes to GitHub Release
```

## 📞 Support & Documentation

- **Development Guide**: `docs/development.md`
- **Testing Guide**: `docs/testing.md`
- **Architecture**: `docs/ARCHITECTURE.md`
- **Dependencies**: `docs/dependencies.md`
- **CI/CD Details**: `.github/CI_CD.md`

## ✅ Implementation Checklist

- ✅ Build pipeline (Linux & macOS, AMD64 & ARM64)
- ✅ Test automation (unit, integration, memory)
- ✅ Code quality checks (format, analysis, style)
- ✅ Release automation
- ✅ Code coverage tracking
- ✅ Dependency management
- ✅ Issue templates
- ✅ PR template
- ✅ Contribution guidelines
- ✅ Documentation

---

For detailed information on any component, see the appropriate documentation:
- CI/CD Pipelines → `CI_CD.md`
- Setup Instructions → `SETUP.md`
- Contributing → `../CONTRIBUTING.md`
- Development → `../docs/development.md`
