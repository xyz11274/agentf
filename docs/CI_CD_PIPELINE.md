# AgentFlow CI/CD Automation Pipeline

## Overview

This document outlines the Continuous Integration and Continuous Deployment (CI/CD) automation pipelines for the AgentFlow project. The pipelines are implemented using GitHub Actions and provide comprehensive automated testing, code quality checks, and release management.

## Supported Platforms

The CI/CD pipeline automatically builds and tests on the following platforms:

### Hardware Architectures
- **AMD64 (x86_64)**: Intel/AMD 64-bit processors
- **ARM64 (aarch64)**: ARM 64-bit processors (Apple Silicon, AWS Graviton, etc.)

### Operating Systems
- **Linux**: Ubuntu 24.04 LTS (latest)
  - AMD64 support: Running on native Ubuntu runners
  - ARM64 support: Cross-compilation with QEMU user-space emulation
- **macOS**: Multiple versions for better coverage
  - AMD64: macOS 12 and later (Intel-based)
  - ARM64: macOS 14 (Apple Silicon - M1/M2/M3)

## Workflows

### 1. Build Workflow (`.github/workflows/build.yml`)

**Trigger Events:**
- Push to `main` or `develop` branches
- Pull requests to `main` or `develop` branches
- On changes to build-related files

**Matrix Configuration:**
- Linux AMD64 (Ubuntu Latest)
- Linux ARM64 (Ubuntu Latest with QEMU)
- macOS AMD64 (macOS 12)
- macOS ARM64 (macOS 14)

**Steps:**
1. Checkout source code
2. Install platform-specific dependencies
3. Configure CMake with Release build type
4. Build with parallel jobs (`-j` flag)
5. Upload build artifacts for 7 days retention

**Build Configuration:**
```cmake
-DCMAKE_BUILD_TYPE=Release
-DCMAKE_CXX_STANDARD=17
-DAGENTFLOW_WITH_PROTOACTOR=OFF
-DAGENTFLOW_WITH_OPENSSL=ON
-DAGENTFLOW_WITH_PROTOBUF=ON
-DAGENTFLOW_WITH_LIBWEBSOCKETS=ON
```

### 2. Code Quality Workflow (`.github/workflows/code-quality.yml`)

**Trigger Events:**
- Push to `main` or `develop` branches
- Pull requests to `main` or `develop` branches

**Code Quality Checks:**

#### a. Clang Format Check
- Verifies code formatting consistency
- Uses `.clang-format` configuration file
- Reports files that need formatting

#### b. Clang Tidy Analysis
- Performs static analysis for code quality issues
- Checks for performance anti-patterns
- Identifies modernization opportunities
- Generates compile_commands.json for analysis

#### c. Cppcheck Static Analysis
- Comprehensive static analysis tool
- Detects memory leaks and logic errors
- Configurable checkers for different severity levels

#### d. Cpplint Style Check
- Google C++ style guide enforcement
- Customizable line length (100 characters)
- Filters for legal/copyright notices

#### e. Sanitizer Compilation Check
- Verifies code compiles with AddressSanitizer
- Early detection of potential memory issues
- Validates sanitizer compatibility

### 3. Test Workflow (`.github/workflows/tests.yml`)

**Trigger Events:**
- Push to `main` or `develop` branches
- Pull requests to `main` or `develop` branches

**Test Jobs:**

#### Unit Tests (Multi-platform)
- Runs on Linux AMD64, macOS AMD64, macOS ARM64
- Builds with Debug configuration
- Executes all GTest test suites
- Timeout: 60 seconds per test
- Parallel execution: Multiple jobs

#### Code Coverage Analysis
- Generates combined coverage report
- Uses LCOV and Gcovr tools
- Uploads to Codecov for tracking
- Generates HTML coverage report
- Artifacts retained for 30 days

#### Integration Tests
- Runs built examples:
  - `example_basic`
  - `example_adapter`
  - `example_control_flow`
- 10-second timeout per example
- Validates end-to-end workflows

#### Memory Leak Detection
- Uses Valgrind tool
- Runs with memcheck tool
- Detects heap and stack memory leaks
- Configurable suppression rules

### 4. Code Owners Configuration (`.github/CODEOWNERS`)

Specifies code ownership and automatic review assignment:
- Default owner: @DanielKernel
- Component-specific owners for:
  - Core framework (src/, include/agentflow/)
  - Actor system
  - DSL and workflow
  - Event system
  - Network communication
  - Storage and persistence
  - Serialization
  - Runtime system

### 5. Release Workflow (`.github/workflows/release.yml`)

**Trigger:** Git tags matching `v*` pattern (e.g., `v0.1.0`)

**Release Jobs:**

#### Create Release
- Generates GitHub Release page
- Auto-populates release notes
- Extracts changelog information

#### Multi-platform Build & Package
- Builds for all supported platforms
- Creates distribution packages:
  - `agentflow-VERSION-PLATFORM.tar.gz`
  - Contains headers, libraries, CMake files, docs
- Generates SHA256 checksums
- Uploads to GitHub Release Assets

#### Artifact Publishing
- Aggregates all release artifacts
- Creates release summary
- Makes artifacts publicly available

### 6. Manual Build Workflow (`.github/workflows/manual_build.yml`)

**Trigger:** Manual dispatch via GitHub Actions UI

**Input Parameters:**
- Build type: Debug, Release, RelWithDebInfo
- Enable code coverage: Yes/No
- Run Valgrind checks: Yes/No
- Slack notification: Yes/No

**Customizable Build Options:**
- Selective feature compilation
- Performance profiling
- Memory analysis integration
- Third-party notifications

## Automated Checks

### Pull Request Checks (Required)

All PRs must pass these checks:

1. ✅ **Build Success** - All platforms compile without errors/warnings
2. ✅ **Test Coverage** - All tests pass with required coverage
3. ✅ **Code Format** - `clang-format` validation passes
4. ✅ **Static Analysis** - `clang-tidy` produces no critical issues
5. ✅ **Type Safety** - No compiler warnings

### Optional Checks

1. 📊 **Code Coverage Report** - Generated for tracking trends
2. 🔍 **Static Code Analysis** - Cppcheck and cpplint results
3. 💾 **Memory Safety** - Valgrind leak detection
4. 🏗️ **Build Artifacts** - Generated for download

## Dependencies Management

### Automatic Dependency Updates (Dependabot)

**GitHub Actions:** Weekly updates on Monday at 03:00 UTC
**Docker Images:** Weekly updates on Wednesday

**Features:**
- Automatic version checking
- Pull request creation for updates
- Configurable review assignment
- Automated commit messages

## Getting Started with CI/CD

### Local Testing Before Push

```bash
# Build locally
./build.sh build

# Run tests
./build.sh test

# Check code formatting
clang-format -i -style=file $(find src include examples tests -name '*.cpp' -o -name '*.h')

# Run static analysis
clang-tidy -p build src/*.cpp
```

### Creating a Release

```bash
# Tag a release
git tag v0.2.0 -m "Release version 0.2.0"
git push origin v0.2.0

# GitHub Actions will automatically:
# 1. Build for all platforms
# 2. Run all tests
# 3. Create release assets
# 4. Generate checksums
# 5. Publish release notes
```

### Monitoring Build Status

1. **GitHub Actions Dashboard**: https://github.com/DanielKernel/AgentFlow/actions
2. **Branch Protections**: Set status check requirements
3. **Status Badges**: Add to README:
   ```markdown
   ![Build](https://github.com/DanielKernel/AgentFlow/workflows/Build/badge.svg)
   ![Tests](https://github.com/DanielKernel/AgentFlow/workflows/Tests/badge.svg)
   ![Code Quality](https://github.com/DanielKernel/AgentFlow/workflows/Code%20Quality/badge.svg)
   ```

## Build Matrix

| Platform | OS | Arch | Runner | Status |
|----------|-----|------|--------|--------|
| Linux | Ubuntu 24.04 LTS | AMD64 | ubuntu-latest | ✅ |
| Linux | Ubuntu 24.04 LTS | ARM64 | ubuntu-latest + QEMU | ✅ |
| macOS | 12 (Intel) | AMD64 | macos-12 | ✅ |
| macOS | 14 (Apple Silicon) | ARM64 | macos-14 | ✅ |

## Quality Gates

### Code Coverage

- **Target**: > 80% code coverage
- **Tool**: LCOV + Gcovr
- **Upload**: Codecov.io integration
- **Reporting**: HTML reports on artifact storage

### Code Quality Standards

| Check | Tool | Threshold | Action |
|-------|------|-----------|--------|
| Format | Clang Format | 100% pass | Block merge |
| Static Analysis | Clang Tidy | No critical | Warning |
| Memory Safety | Valgrind | No real leaks | Warning |
| Code Style | Cpplint | 95%+ pass | Info |

## Known Limitations & Workarounds

### Linux ARM64 Build

- Uses QEMU user-space emulation instead of native ARM64 machines
- Slower build times compared to native runners
- Suitable for verification, not recommended for performance testing

### macOS ARM64 (Apple Silicon)

- Requires macOS 14 or later
- Some dependencies may need Apple Silicon native builds
- Homebrew handles architecture detection automatically

## Troubleshooting

### Build Failures

1. Check build logs in GitHub Actions UI
2. Reproduce locally: `./build.sh build`
3. Verify dependencies are installed
4. Check CMake configuration messages

### Test Failures

1. Review test output in Actions
2. Run locally: `./build.sh test`
3. Check for platform-specific issues
4. Review recent code changes

### Code Quality Issues

1. Run formatters locally: `clang-format -i -style=file src/*`
2. Address clang-tidy warnings
3. Review static analysis results
4. Update configuration if needed

## Maintenance

### Regular Tasks

- **Weekly**: Review Dependabot update PRs
- **Monthly**: Analyze code coverage trends
- **Quarterly**: Update build tool versions
- **Annually**: Review workflow efficiency

### Configuration Files

- `.github/workflows/build.yml` - Primary build pipeline
- `.github/workflows/code-quality.yml` - Code quality checks
- `.github/workflows/tests.yml` - Test execution
- `.github/workflows/release.yml` - Release automation
- `.github/workflows/manual_build.yml` - Manual build trigger
- `.github/dependabot.yml` - Dependency updates
- `.github/CODEOWNERS` - Code ownership rules

## Integration with External Services

### Codecov Integration
- Automatically uploads coverage reports
- Tracks coverage trends over time
- Provides PR annotations for coverage changes

### Badge Support
Add badges to your README:
```markdown
[![codecov](https://codecov.io/gh/DanielKernel/AgentFlow/branch/main/graph/badge.svg)](https://codecov.io/gh/DanielKernel/AgentFlow)
```

## Future Enhancements

- [ ] Performance benchmarking in CI/CD
- [ ] Docker image building and publishing
- [ ] Automated security scanning (SAST)
- [ ] Dependency vulnerability checking
- [ ] Binary artifact caching for faster builds
- [ ] Parallel cross-platform builds
- [ ] Automated changelog generation
- [ ] Multi-architecture container support

## Support & Questions

For issues or questions about the CI/CD pipeline:
1. Check this documentation first
2. Review GitHub Actions logs
3. Open an issue with `[CI/CD]` label
4. Contact: @DanielKernel
