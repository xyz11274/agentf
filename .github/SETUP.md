# GitHub Actions CI/CD Infrastructure Guide

## 📚 Documentation Moved to docs/ Directory

**All setup documentation has been consolidated into the `docs` directory for better organization.**

Please refer to:
- **Setup & Configuration Guide**: [../../docs/CI_CD_SETUP.md](../../docs/CI_CD_SETUP.md)
- **Complete Pipeline Documentation**: [../../docs/CI_CD_PIPELINE.md](../../docs/CI_CD_PIPELINE.md)
- **Contributing Guidelines**: [../../docs/CONTRIBUTING.md](../../docs/CONTRIBUTING.md)

## Quick Reference

### View Build Status
https://github.com/DanielKernel/AgentFlow/actions

### Supported Platforms
- Ubuntu x86_64 (AMD64) ✅
- Ubuntu ARM64 ✅
- macOS 12 x86_64 (AMD64) ✅
- macOS 14 ARM64 (Apple Silicon) ✅

### Creating a Release
```bash
git tag v0.2.0 -m "Release 0.2.0"
git push origin v0.2.0
```

---

For complete setup instructions, see [../../docs/CI_CD_SETUP.md](../../docs/CI_CD_SETUP.md)
