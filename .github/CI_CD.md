# AgentFlow CI/CD Automation Pipeline

## 📚 Documentation Moved to docs/ Directory

**All CI/CD documentation has been consolidat into the `docs` directory for better organization.**

Please refer to:
- **Complete Pipeline Documentation**: [../../docs/CI_CD_PIPELINE.md](../../docs/CI_CD_PIPELINE.md)
- **Setup & Configuration Guide**: [../../docs/CI_CD_SETUP.md](../../docs/CI_CD_SETUP.md)
- **Implementation Details**: [../../docs/CI_CD_IMPLEMENTATION.md](../../docs/CI_CD_IMPLEMENTATION.md)
- **Quick Reference**: [../../docs/QUICK_REFERENCE.md](../../docs/QUICK_REFERENCE.md)

## Overview

The CI/CD pipeline automatically builds and tests on multiple platforms:

### Hardware Architectures
- **AMD64 (x86_64)**: Full support
- **ARM64 (aarch64)**: Full support with QEMU

### Operating Systems
- **Linux**: Ubuntu 24.04 LTS
- **macOS**: 12 (Intel) and 14 (Apple Silicon)

## Workflow Files

Located in `.github/workflows/`:
- `build.yml` - Multi-platform builds
- `code-quality.yml` - Format, analysis, style checks
- `tests.yml` - Unit/integration tests, coverage, memory checks
- `release.yml` - Automated release publishing
- `manual_build.yml` - Manual build dispatch
- `badge_status.yml` - Status badge updates

---

For complete information, see the documentation in the `docs/` directory.
