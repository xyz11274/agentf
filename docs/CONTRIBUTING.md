# Contributing to AgentFlow

Thank you for your interest in contributing to AgentFlow! This document provides guidelines and instructions for contributing to the project.

## Table of Contents

1. [Code of Conduct](#code-of-conduct)
2. [Getting Started](#getting-started)
3. [Development Workflow](#development-workflow)
4. [Testing Requirements](#testing-requirements)
5. [Code Quality Standards](#code-quality-standards)
6. [Commit Guidelines](#commit-guidelines)
7. [Pull Request Process](#pull-request-process)
8. [Reporting Issues](#reporting-issues)

## Code of Conduct

We are committed to providing a welcoming and inclusive environment for all contributors. Please:

- Be respectful and professional
- Welcome new contributors
- Accept constructive criticism
- Focus on what is best for the community
- Show empathy towards other community members

## Getting Started

### Prerequisites

- C++17 compatible compiler (GCC 7+, Clang 5+, MSVC 2017+)
- CMake 3.10 or later
- Git
- Platform-specific development tools (see [Development Guide](development.md))

### Setup Development Environment

```bash
# Clone the repository
git clone https://github.com/DanielKernel/AgentFlow.git
cd AgentFlow

# Create a feature branch
git checkout -b feature/your-feature-name

# Install dependencies
# On Ubuntu/Debian:
sudo apt-get update
sudo apt-get install -y build-essential cmake pkg-config \
  libssl-dev libwebsockets-dev libprotobuf-dev protobuf-compiler \
  rapidjson-dev libevent-dev libgtest-dev

# On macOS:
brew install cmake openssl@3 libwebsockets protobuf rapidjson \
  libevent googletest ninja

# Build the project
./build.sh build

# Run tests to verify setup
./build.sh test
```

## Development Workflow

### 1. Create a Feature Branch

```bash
# Update main branch
git checkout main
git pull origin main

# Create feature branch (use descriptive names)
git checkout -b feature/add-new-feature
# or for bug fixes:
git checkout -b fix/resolve-issue-123
# or for documentation:
git checkout -b docs/update-readme
```

### 2. Make Your Changes

- Keep commits atomic and focused
- Follow the [Code Quality Standards](#code-quality-standards)
- Update documentation as needed
- Add tests for new features

### 3. Testing

```bash
# Build locally
./build.sh build

# Run all tests
./build.sh test

# Run specific test suite
cd build && ctest -R test_name --output-on-failure

# Run with sanitizers (if enabled)
# See build/cmake/CompilerOptions.cmake for available options
```

### 4. Code Quality Checks

```bash
# Format code
find src include examples tests -name '*.cpp' -o -name '*.h' -o -name '*.hpp' | \
  xargs clang-format -i -style=file

# Run static analysis (requires clang-tidy)
clang-tidy -p build src/your_file.cpp

# Check with cppcheck
cppcheck --enable=all src/

# Run cpplint
cpplint --linelength=100 src/your_file.cpp
```

## Testing Requirements

### Unit Tests

All new features must include unit tests:

```cpp
#include <gtest/gtest.h>
#include "agentflow/framework/your_component.h"

TEST(YourComponentTest, BasicFunctionality) {
    // Arrange
    
    // Act
    
    // Assert
}
```

### Test Coverage

- Target: > 80% code coverage
- Use `./build.sh` to generate coverage reports
- Coverage reports are automatically generated in CI/CD

### Platform-Specific Testing

Test your changes on multiple platforms:

- Linux x86_64
- macOS AMD64 (Intel)
- macOS ARM64 (Apple Silicon)

## Code Quality Standards

### C++ Style Guide

We follow a modified Google C++ style guide with these specifics:

#### Naming Conventions

```cpp
// Classes and types: CamelCase
class MyComponent { };

// Functions and methods: snake_case
void process_data() { }

// Member variables: snake_case_ with trailing underscore
class MyClass {
private:
    int member_var_;
};

// Constants: UPPER_SNAKE_CASE
const int MAX_BUFFER_SIZE = 1024;

// Template parameters: CamelCase
template<typename Iterator>
void process(Iterator begin, Iterator end) { }
```

#### Code Formatting

Configuration: `.clang-format`

```bash
# Automatic formatting
clang-format -i -style=file your_file.cpp

# Check formatting without modifying
clang-format --dry-run --Werror your_file.cpp
```

#### Common Standards

1. **Line Length**: Maximum 100 characters
2. **Indentation**: 2 spaces (no tabs)
3. **Braces**: Allman style (opening brace on new line)
4. **Namespaces**: Use `agentflow` namespace
5. **Comments**: Document public APIs and complex logic

#### Include Order

```cpp
// 1. System includes
#include <iostream>
#include <vector>

// 2. Third-party includes
#include <gtest/gtest.h>

// 3. Project includes
#include "agentflow/framework/agent.h"
```

### Documentation

1. **Header Comments**: Document public interfaces
2. **Function Comments**: Describe purpose, parameters, return value
3. **Complex Logic**: Explain non-obvious implementations
4. **Update README**: For user-facing changes

Example:

```cpp
/// Processes the given workflow configuration.
///
/// @param config The workflow configuration to process
/// @param timeout Maximum time to wait for processing (milliseconds)
/// @return true if processing succeeded, false otherwise
/// @throw std::invalid_argument if config is invalid
bool process_workflow(const WorkflowConfig& config, int timeout) {
    // Implementation
}
```

### Error Handling

Use appropriate error handling:

```cpp
// For recoverable errors
if (!validate_input(data)) {
    LOG(ERROR) << "Invalid input data";
    return Status::INVALID_INPUT;
}

// For programming errors
assert(ptr != nullptr);  // Debug checks

// For runtime errors that shouldn't happen
if (unlikely_error) {
    throw std::runtime_error("Critical error occurred");
}
```

## Commit Guidelines

### Commit Message Format

```
<type>(<scope>): <subject>

<body>

<footer>
```

### Type

- **feat**: A new feature
- **fix**: A bug fix
- **docs**: Documentation only changes
- **style**: Changes that don't affect code meaning (formatting, missing semicolons, etc.)
- **refactor**: A code change that neither fixes a bug nor adds a feature
- **perf**: A code change that improves performance
- **test**: Adding missing tests or correcting existing tests
- **chore**: Changes to build process, dependencies, or tools

### Scope

Component that is affected:
- framework
- actor
- dsl
- event
- network
- storage
- serialization
- runtime
- build
- ci

### Subject

- Use imperative, present tense: "add feature" not "added feature"
- Don't capitalize first letter
- No period (.) at the end
- Limit to 50 characters

### Body

- Explain what and why, not how
- Wrap at 72 characters
- Separate from subject with blank line

### Footer

Reference issues and breaking changes:

```
Fixes #123
BREAKING CHANGE: description of breaking change
```

### Example Commits

```
feat(actor): add message priority queue

Implement priority-based message processing in the actor
system to support high-priority control messages.

Fixes #456
```

```
fix(serialization): handle null pointers correctly

Prevent null pointer dereference when serializing
optional fields that are not set.

Fixes #789
```

## Pull Request Process

### Before Creating a PR

1. ✅ Fork the repository and create a feature branch
2. ✅ Make your changes following code standards
3. ✅ Format code: `clang-format -i -style=file src/*`
4. ✅ Run tests locally: `./build.sh test`
5. ✅ Run static analysis: `clang-tidy` and `cppcheck`
6. ✅ Update documentation
7. ✅ Update CHANGELOG.md

### Creating a PR

1. Push your branch to your fork
2. Create a PR against `main` or `develop` branch
3. Fill in the PR template completely
4. Link related issues using "Fixes #123"
5. Request review from code owners

### PR Checklist

Our Pull Request template includes:

- [ ] Code follows the style guidelines
- [ ] Documentation is updated
- [ ] Tests are included and passing
- [ ] No new compiler warnings
- [ ] Memory usage is appropriate
- [ ] Breaking changes are documented
- [ ] Backward compatibility maintained

### Review Process

1. **Automated Checks** (GitHub Actions):
   - Build succeeds on all platforms
   - All tests pass
   - Code formatting is correct
   - Static analysis passes
   - Code coverage maintained

2. **Code Review**:
   - At least one approval from code owner
   - Discussion of design and implementation
   - Suggestions for improvements

3. **Merge**:
   - All checks pass
   - Code review approved
   - Branch is up to date with main
   - Squash commits if appropriate

## Reporting Issues

### Bug Reports

1. Use the bug report template
2. Include:
   - OS and architecture
   - Build environment details
   - Minimal reproducible example
   - Expected vs actual behavior
   - Error messages/logs
   - Steps to reproduce

### Feature Requests

1. Use the feature request template
2. Describe:
   - The problem statement
   - Proposed solution
   - Use cases
   - Alternative approaches
   - Implementation complexity

### Questions & Discussions

- Use GitHub Discussions
- Join community chat (if available)
- Check documentation first

## Setup Code Owners

Repository maintainers are specified in `.github/CODEOWNERS`:

```
* @DanielKernel
/src/actor/ @DanielKernel
/src/dsl/ @DanielKernel
```

## Resources

- [Architecture Overview](ARCHITECTURE.md)
- [Development Guide](development.md)
- [Testing Guide](testing.md)
- [CI/CD Pipeline Documentation](CI_CD_PIPELINE.md)
- [Dependencies Management](dependencies.md)

## Getting Help

- 📖 Check documentation
- 🔍 Search existing issues
- 💬 Open a discussion
- 📧 Contact maintainers

## License

By contributing, you agree that your contributions will be licensed under the same license as AgentFlow.

---

Thank you for contributing to AgentFlow! 🚀
